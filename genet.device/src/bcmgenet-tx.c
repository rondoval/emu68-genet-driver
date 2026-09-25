// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom GENET (Gigabit Ethernet) controller driver
 *
 * Copyright (c) 2014-2025 Broadcom
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <cache_ops.h>
#include <iomem.h>
#include <types.h>
#include <debug.h>
#include <device.h>
#include <runtime_config.h>

#include <genet/bcmgenet.h>
#include <genet/bcmgenet-regs.h>
#include <genet/bcmgenet-irq.h>

/*
 * Transmit: one Forbid() per write, everything ring-related inside it.
 *
 * Any task may call BeginIO(CMD_WRITE) at any time - several openers, or one
 * opener with several sender tasks - so the ring has no single producer, and
 * the claim, the fill and the publish of a slot have to be one critical
 * section: a slot is claimed by advancing tx_prod_index, which only happens
 * once it is filled, so two writers can never publish out of order. Forbid()
 * does not block interrupts, and the section is a few microseconds: the copy
 * of one frame and a handful of register stores.
 *
 * Staging is ring-bound: slot k of txbuffer belongs to BD k for the life of
 * the ring, so nothing is allocated on the way in and nothing freed on the
 * way out. A slot is reusable exactly when its BD is, and the free-BD check
 * against the cached consumer index already guarantees the hardware has read
 * it. "Reclaim" is therefore one TDMA_CONS_INDEX read when the cached count
 * runs low - every ~240 frames at line rate. Requests are replied at submit,
 * so a stale cache only under-reports free space, never a frame's fate.
 *
 * The frame is built at slot + TX_STAGE_OFFSET: the 14-byte Ethernet header
 * then leaves the body 4-aligned, the alignment a stack's packet buffer has,
 * so the opener's CopyFromBuff moves longwords on both sides. The descriptor
 * stores are relaxed (iomem.h): the one barrier before the doorbell
 * orders them together with the NoSync cache cleans.
 */

static inline void dmadesc_set(APTR descriptor_address, dma_addr_t addr, u32 val)
{
	mmio_write32_relaxed((u32)addr, descriptor_address + DMA_DESC_ADDRESS_LO);
	mmio_write32_relaxed(val, descriptor_address + DMA_DESC_LENGTH_STATUS);
}

/* TX_DESCS == 256: the low byte of the producer index is the slot. */
static inline u32 tx_slot(const struct bcmgenet_tx_ring *ring)
{
	return (u8)ring->tx_prod_index;
}

static inline APTR tx_desc(const struct GenetUnit *unit, const struct bcmgenet_tx_ring *ring)
{
	return BCMGENET_REG(unit, GENET_TX_OFF) + tx_slot(ring) * DMA_DESC_SIZE;
}

static inline u8 *tx_staging(const struct GenetUnit *unit, const struct bcmgenet_tx_ring *ring)
{
	return (u8 *)unit->txbuffer + tx_slot(ring) * RX_BUF_LENGTH + TX_STAGE_OFFSET;
}

static inline u16 tx_free_bds(const struct bcmgenet_tx_ring *ring)
{
	return (u16)(TX_DESCS - ((u32)(ring->tx_prod_index - ring->hw_cons_cache) & DMA_P_INDEX_MASK));
}

/* Program the BD at the producer slot and claim it. */
static inline void tx_publish(struct GenetUnit *unit, struct bcmgenet_tx_ring *ring, dma_addr_t dma, u32 len_stat)
{
	dmadesc_set(tx_desc(unit, ring), dma, len_stat);
	ring->tx_prod_index = (u16)(((u32)ring->tx_prod_index + 1U) & DMA_P_INDEX_MASK);
}

/* Build the 14-byte Ethernet II header into `buf`. */
static inline void build_eth_header(u8 *buf, const u8 *dst_mac,
									const u8 *src_mac, u16 ethertype)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
	*(ULONG *)buf = *(const ULONG *)dst_mac;
	*(UWORD *)(buf + 4) = *(const UWORD *)&dst_mac[4];
	*(ULONG *)(buf + 6) = *(const ULONG *)src_mac;
	*(UWORD *)(buf + 10) = *(const UWORD *)&src_mac[4];
	*(UWORD *)(buf + 12) = ethertype;
#pragma GCC diagnostic pop
}

u32 bcmgenet_xmit(struct IOSana2Req *io, struct GenetUnit *unit)
{
	KprintfT("[genet] %s: unit %lu, io 0x%lx, flags 0x%lx\n", __func__, unit->unitNumber, io, io->ios2_Req.io_Flags);

	PERF_T0(t_submit);
	struct Opener *opener = io->ios2_BufferManagement;
	struct bcmgenet_tx_ring *ring = &unit->tx_ring;
	const BOOL is_raw = (io->ios2_Req.io_Flags & SANA2IOF_RAW) != 0;
	const u32 data_len = io->ios2_DataLength;

	if (unlikely(data_len == 0))
	{
		KprintfT("[genet] %s: No data to send\n", __func__);
		goto err_no_buf;
	}
	/* A slot is RX_BUF_LENGTH bytes: a write must never run into the next one. */
	if (unlikely(data_len > TX_MAX_DATALEN))
		goto err_mtu;

	/* Outside the lock: nothing below touches the ring or a slot. */
	u32 copy_len = data_len;
	if (unit->use_miami_workaround)
		copy_len = (copy_len + 3U) & ~3U;

	dma_addr_t user_dma = 0;
	if (unlikely(opener->DMACopyFromBuff))
	{
		dma_addr_t d = (dma_addr_t)opener->DMACopyFromBuff(io->ios2_Data);
		/* GENET DMA can only reach Emu68 (Pi-DRAM) RAM; Chip RAM and any
		 * Zorro/accelerator Fast RAM are unreachable -> fall back to copy. */
		if (likely(dma_addr_reachable(&unit->dma_ctx, (APTR)d, data_len)))
			user_dma = d;
		else
			KprintfT("[genet] %s: buffer not DMA-reachable, falling back to copy\n", __func__);
	}
	const u16 bds_required = (user_dma != 0 && !is_raw) ? 2U : 1U;
	const u32 ls_common = (GENET_QTAG_MASK << DMA_TX_QTAG_SHIFT) | DMA_TX_APPEND_CRC;

	PERF_T0(t_claim);
	Forbid();
	/* UnitOffline flips the state under Forbid: a writer that sees ONLINE
	 * here owns txbuffer and the ring until its Permit(). */
	if (unlikely(unit->state != STATE_ONLINE))
	{
		Permit();
		goto err_offline;
	}
	u16 free_bds = tx_free_bds(ring);
	if (unlikely(free_bds < TX_RECLAIM_THRESHOLD))
	{
		ring->hw_cons_cache = (u16)(mmio_read32(BCMGENET_REG(unit, TDMA_CONS_INDEX)) & DMA_C_INDEX_MASK);
		free_bds = tx_free_bds(ring);
	}
	if (unlikely(free_bds <= bds_required))
	{
		KprintfT("[genet] %s: Not enough free BDs\n", __func__);
		Permit();
		goto err_no_buf;
	}
	/* Tell abortIO this request is committed to the ring. */
	io->ios2_Req.io_Message.mn_Node.ln_Pred = NULL;
	u8 *slot = tx_staging(unit, ring);
	PERF_ADD(&unit->perf, GP_TX_CLAIM, t_claim);

	u32 wire_len;
	if (likely(!is_raw))
	{
		build_eth_header(slot, io->ios2_DstAddr, unit->currentMacAddress, (u16)io->ios2_PacketType);
		wire_len = ETH_HLEN + data_len;
		if (unlikely(user_dma != 0))
		{
			/* Two descriptors: the header from the slot, the body from the client's buffer. */
			KprintfT("[genet] %s: DMA path, body 0x%lx\n", __func__, user_dma);
			cache_pre_dma(slot, ETH_HLEN, DMA_ReadFromRAM | DMAF_NoSync);
			cache_pre_dma((APTR)user_dma, data_len, DMA_ReadFromRAM | DMAF_NoSync);
			tx_publish(unit, ring, (dma_addr_t)slot, ((u32)ETH_HLEN << DMA_BUFLENGTH_SHIFT) | ls_common | DMA_SOP);
			tx_publish(unit, ring, user_dma, (data_len << DMA_BUFLENGTH_SHIFT) | ls_common | DMA_EOP);
			unit->internalStats.tx_dma++;
		}
		else
		{
			PERF_T0(t_copy);
			if (unlikely(!opener->CopyFromBuff ||
						 opener->CopyFromBuff(slot + ETH_HLEN, io->ios2_Data, copy_len) == 0))
			{
				/* nothing published: the next write simply reuses the slot */
				KprintfT("[genet] %s: Failed to copy packet data\n", __func__);
				Permit();
				goto err_no_buf;
			}
			PERF_ADD(&unit->perf, GP_TX_COPY, t_copy);
			PERF_T0(t_clean);
			cache_pre_dma(slot, wire_len, DMA_ReadFromRAM | DMAF_NoSync);
			PERF_ADD(&unit->perf, GP_TX_CLEAN, t_clean);
			PERF_T0(t_ring);
			tx_publish(unit, ring, (dma_addr_t)slot, (wire_len << DMA_BUFLENGTH_SHIFT) | ls_common | DMA_SOP | DMA_EOP);
			PERF_ADD(&unit->perf, GP_TX_RING, t_ring);
			unit->internalStats.tx_copy++;
		}
	}
	else
	{
		/* RAW: the caller's frame is complete - one descriptor from wherever it is. */
		dma_addr_t src = user_dma;
		if (likely(src == 0))
		{
			if (unlikely(!opener->CopyFromBuff ||
						 opener->CopyFromBuff(slot, io->ios2_Data, copy_len) == 0))
			{
				KprintfT("[genet] %s: Failed to copy RAW packet data\n", __func__);
				Permit();
				goto err_no_buf;
			}
			src = (dma_addr_t)slot;
		}
		wire_len = data_len;
		cache_pre_dma((APTR)src, wire_len, DMA_ReadFromRAM | DMAF_NoSync);
		tx_publish(unit, ring, src, (wire_len << DMA_BUFLENGTH_SHIFT) | ls_common | DMA_SOP | DMA_EOP);
		if (user_dma != 0)
			unit->internalStats.tx_dma++;
		else
			unit->internalStats.tx_copy++;
	}
	unit->internalStats.tx_packets++;
	unit->internalStats.tx_bytes += wire_len;

	KprintfT("[genet] %s: Scheduled, tx_prod_index %ld\n", __func__, ring->tx_prod_index);
	PERF_T0(t_pub);
	emu68_barrier(); /* the one dsb: closes the NoSync cleans and orders the relaxed BD stores ahead of the doorbell */
	mmio_write32(ring->tx_prod_index, BCMGENET_REG(unit, TDMA_PROD_INDEX));
	PERF_ADD(&unit->perf, GP_TX_PUBLISH, t_pub);
	Permit();
	PERF_ADD(&unit->perf, GP_TX_SUBMIT, t_submit);
	return COMMAND_PROCESSED;

err_offline:
	io->ios2_WireError = S2WERR_UNIT_OFFLINE;
	io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
	PERF_ADD(&unit->perf, GP_TX_SUBMIT, t_submit);
	return COMMAND_PROCESSED;

err_mtu:
	unit->internalStats.tx_dropped++;
	io->ios2_WireError = S2WERR_GENERIC_ERROR;
	io->ios2_Req.io_Error = S2ERR_MTU_EXCEEDED;
	PERF_ADD(&unit->perf, GP_TX_SUBMIT, t_submit);
	return COMMAND_PROCESSED;

err_no_buf:
	unit->internalStats.tx_dropped++;
	io->ios2_WireError = S2WERR_BUFF_ERROR;
	io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
	UnitSubmitControlAsync(unit, UNIT_CTRL_EVENT_REPORT, (union UnitControlPayload){.eventSet = S2EVENT_BUFF | S2EVENT_TX | S2EVENT_SOFTWARE | S2EVENT_ERROR});
	PERF_ADD(&unit->perf, GP_TX_SUBMIT, t_submit);
	return COMMAND_PROCESSED;
}
