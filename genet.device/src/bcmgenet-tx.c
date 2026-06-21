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

#include <exec/execbase.h> /* DMA_ReadFromRAM for CachePreDMA(); older NDKs don't pull it in transitively */
#include <iomem.h>
#include <types.h>
#include <debug.h>
#include <device.h>
#include <runtime_config.h>

#include <genet/bcmgenet.h>
#include <genet/bcmgenet-regs.h>
#include <genet/bcmgenet-irq.h>

/* Combined address + length/status setter */
static inline void dmadesc_set(APTR descriptor_address, dma_addr_t addr, u32 val)
{
	mmio_write32((u32)addr, descriptor_address + DMA_DESC_ADDRESS_LO);
	mmio_write32(val, descriptor_address + DMA_DESC_LENGTH_STATUS);
}

/* Slab cache wrappers — Forbid is brief and recursion-safe, so callers may
 * be either inside or outside the ring lock. */
static inline APTR tx_staging_alloc(struct GenetUnit *unit)
{
	Forbid();
	APTR p = slab_alloc(&unit->tx_buffer_cache);
	Permit();
	return p;
}

static inline void tx_staging_free(struct GenetUnit *unit, APTR p)
{
	Forbid();
	slab_free(&unit->tx_buffer_cache, p);
	Permit();
}

static inline u16 tx_free_bds(const struct bcmgenet_tx_ring *ring)
{
	return (u16)(TX_DESCS - ((u32)(ring->tx_prod_index - ring->tx_cons_index) & DMA_P_INDEX_MASK));
}

static inline void tx_advance_prod(struct bcmgenet_tx_ring *ring)
{
	ring->tx_prod_index = (u16)(((u32)ring->tx_prod_index + 1U) & DMA_P_INDEX_MASK);
}

static inline struct enet_cb *bcmgenet_get_txcb(struct bcmgenet_tx_ring *ring)
{
	struct enet_cb *tx_cb_ptr = &ring->tx_control_block[ring->write_ptr++];
	KprintfH("[genet] %s: tx_cb_ptr 0x%lx, write_ptr %ld\n", __func__, tx_cb_ptr, ring->write_ptr - 1);
	return tx_cb_ptr;
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

/* Fill a CB and program its descriptor */
static inline void tx_cb_publish(struct enet_cb *cb, APTR staging, dma_addr_t dma, u32 len_stat)
{
	cb->staging_buffer = staging;
	cb->data_buffer = dma;
	dmadesc_set(cb->descriptor_address, dma, len_stat);
}

void bcmgenet_tx_reclaim(struct GenetUnit *unit, u16 budget)
{
	if (budget == 0U)
		return;

	struct bcmgenet_tx_ring *ring = &unit->tx_ring;

	/* Compute how many buffers are transmitted since last xmit call */
	u16 tx_cons_index = mmio_read32(BCMGENET_REG(unit, TDMA_CONS_INDEX)) & DMA_C_INDEX_MASK;
	u16 txbds_ready = (u16)((u32)(tx_cons_index - ring->tx_cons_index) & DMA_C_INDEX_MASK);
	txbds_ready = (txbds_ready > budget) ? budget : txbds_ready;

	u16 txbds_processed = 0;
	while (txbds_processed < txbds_ready)
	{
		struct enet_cb *cb = &ring->tx_control_block[ring->clean_ptr];
		if (cb->staging_buffer)
		{
			slab_free(&unit->tx_buffer_cache, cb->staging_buffer);
			cb->staging_buffer = NULL;
		}
		++ring->clean_ptr;
		++txbds_processed;
	}

	ring->tx_cons_index = (u16)((u32)(ring->tx_cons_index + txbds_processed) & DMA_C_INDEX_MASK);
}

u32 bcmgenet_xmit(struct IOSana2Req *io, struct GenetUnit *unit)
{
	KprintfH("[genet] %s: unit %lu, io 0x%lx, flags 0x%lx\n", __func__, unit->unitNumber, io, io->ios2_Req.io_Flags);

	struct Opener *opener = io->ios2_BufferManagement;
	struct bcmgenet_tx_ring *ring = &unit->tx_ring;
	BOOL is_raw = (io->ios2_Req.io_Flags & SANA2IOF_RAW) != 0;

	if (unlikely(io->ios2_DataLength == 0))
	{
		KprintfH("[genet] %s: No data to send\n", __func__);
		goto err_no_buf;
	}

	/*
	 * Phase 1 — outside the ring lock: decide DMA vs. copy, slab-allocate
	 * staging, build header, run user CopyFromBuff, prime the DMA cache.
	 * This keeps the up-to-1500-byte memcpy and the cache flush out of the
	 * critical section.
	 */
	APTR hdr_staging = NULL, body_staging = NULL;
	dma_addr_t hdr_dma = 0, body_dma = 0;
	ULONG hdr_len = 0, body_len = 0; /* ULONG so &hdr_len/&body_len match CachePreDMA on any NDK */
	u8 bds_required;
	BOOL is_dma_path = FALSE;

	if (likely(!is_raw))
	{
		if (unlikely(opener->DMACopyFromBuff))
		{
			dma_addr_t d = (dma_addr_t)opener->DMACopyFromBuff(io->ios2_Data);
			/* GENET DMA can only reach Emu68 (Pi-DRAM) RAM; Chip RAM and any
			 * Zorro/accelerator Fast RAM are unreachable -> fall back to copy. */
			if (likely(dma_addr_reachable(&unit->dma_ctx, (APTR)d, io->ios2_DataLength)))
			{
				body_dma = d;
			}
			else
			{
				KprintfH("[genet] %s: buffer not DMA-reachable, falling back to copy\n", __func__);
			}
		}

		if (unlikely(body_dma))
		{
			/* Two-descriptor DMA path: header in staging, body via user DMA. */
			KprintfH("[genet] %s: DMA path, body_dma=0x%lx\n", __func__, body_dma);
			hdr_staging = tx_staging_alloc(unit);
			if (unlikely(!hdr_staging))
				goto err_no_buf;

			build_eth_header((u8 *)hdr_staging, io->ios2_DstAddr,
							 unit->currentMacAddress, (u16)io->ios2_PacketType);
			hdr_dma = (dma_addr_t)hdr_staging;
			hdr_len = ETH_HLEN;
			body_len = io->ios2_DataLength;
			CachePreDMA((APTR)hdr_dma, &hdr_len, DMA_ReadFromRAM);
			CachePreDMA((APTR)body_dma, &body_len, DMA_ReadFromRAM);
			is_dma_path = TRUE;
			bds_required = 2;
		}
		else
		{
			/* Single-descriptor copy path: header + body into staging. */
			body_staging = tx_staging_alloc(unit);
			if (unlikely(!body_staging))
				goto err_no_buf;

			build_eth_header((u8 *)body_staging, io->ios2_DstAddr,
							 unit->currentMacAddress, (u16)io->ios2_PacketType);

			u32 copy_length = io->ios2_DataLength;
			if (unit->use_miami_workaround)
				copy_length = (copy_length + 3U) & ~3U;
			if (!opener->CopyFromBuff ||
				opener->CopyFromBuff((u8 *)body_staging + ETH_HLEN, io->ios2_Data, copy_length) == 0)
			{
				KprintfH("[genet] %s: Failed to copy packet data\n", __func__);
				tx_staging_free(unit, body_staging);
				body_staging = NULL;
				goto err_no_buf;
			}

			body_dma = (dma_addr_t)body_staging;
			body_len = ETH_HLEN + io->ios2_DataLength;
			CachePreDMA((APTR)body_dma, &body_len, DMA_ReadFromRAM);
			bds_required = 1;
		}
	}
	else
	{
		/* RAW path: caller provides a complete Ethernet frame. */
		if (unlikely(opener->DMACopyFromBuff))
		{
			dma_addr_t d = (dma_addr_t)opener->DMACopyFromBuff(io->ios2_Data);
			if (dma_addr_reachable(&unit->dma_ctx, (APTR)d, io->ios2_DataLength))
			{
				KprintfH("[genet] %s: RAW DMA copy from buffer 0x%lx\n", __func__, d);
				body_dma = d;
				is_dma_path = TRUE;
			}
		}
		if (likely(!body_dma))
		{
			KprintfH("[genet] %s: RAW software copy\n", __func__);
			body_staging = tx_staging_alloc(unit);
			if (unlikely(!body_staging))
				goto err_no_buf;

			u32 copy_length = io->ios2_DataLength;
			if (unit->use_miami_workaround)
				copy_length = (copy_length + 3U) & ~3U;
			if (!opener->CopyFromBuff ||
				opener->CopyFromBuff(body_staging, io->ios2_Data, copy_length) == 0)
			{
				KprintfH("[genet] %s: Failed to copy RAW packet data\n", __func__);
				tx_staging_free(unit, body_staging);
				body_staging = NULL;
				goto err_no_buf;
			}
			body_dma = (dma_addr_t)body_staging;
		}
		body_len = io->ios2_DataLength;
		CachePreDMA((APTR)body_dma, &body_len, DMA_ReadFromRAM);
		bds_required = 1;
	}

	/*
	 * Phase 2 — under the ring lock: free-BD check, optional lazy reclaim,
	 * descriptor writes, prod-index advance, MMIO kick. No memcpy, no
	 * CachePreDMA, no user callbacks here.
	 */
	KprintfH("[genet] %s: pre: tx_prod_index %ld, write_ptr %ld\n", __func__, ring->tx_prod_index, ring->write_ptr);
	Forbid();
	u16 free_bds = tx_free_bds(ring);

	/*
	 * Lazy reclaim: skip the MMIO read on every submit. TX reply already
	 * happened at submit time, so stale descriptors don't block the stack.
	 * Reclaim only when the ring is actually getting low.
	 */
	if (unlikely(free_bds < TX_RECLAIM_THRESHOLD))
	{
		bcmgenet_tx_reclaim(unit, TX_DESCS);
		free_bds = tx_free_bds(ring);
	}

	if (unlikely(free_bds <= bds_required))
	{
		KprintfH("[genet] %s: Not enough free BDs\n", __func__);
		Permit();
		if (hdr_staging)
			tx_staging_free(unit, hdr_staging);
		if (body_staging)
			tx_staging_free(unit, body_staging);
		goto err_no_buf;
	}

	/* Tell abortIO this request is committed to the ring. */
	io->ios2_Req.io_Message.mn_Node.ln_Pred = NULL;

	if (unlikely(!is_raw && is_dma_path))
	{
		/* Two-descriptor DMA: SOP header CB (no packet boundary) + EOP body CB. */
		struct enet_cb *hcb = bcmgenet_get_txcb(ring);
		tx_cb_publish(hcb, hdr_staging, hdr_dma,
					  (ETH_HLEN << DMA_BUFLENGTH_SHIFT) | (GENET_QTAG_MASK << DMA_TX_QTAG_SHIFT) | DMA_TX_APPEND_CRC | DMA_SOP);
		tx_advance_prod(ring);

		struct enet_cb *bcb = bcmgenet_get_txcb(ring);
		tx_cb_publish(bcb, NULL, body_dma,
					  (io->ios2_DataLength << DMA_BUFLENGTH_SHIFT) | (GENET_QTAG_MASK << DMA_TX_QTAG_SHIFT) | DMA_TX_APPEND_CRC | DMA_EOP);
		tx_advance_prod(ring);

		unit->internalStats.tx_dma++;
		unit->internalStats.tx_bytes += io->ios2_DataLength + ETH_HLEN;
	}
	else
	{
		/* Single descriptor: non-RAW copy, RAW DMA, or RAW copy. */
		struct enet_cb *cb = bcmgenet_get_txcb(ring);
		u16 wire_len = is_raw ? (u16)io->ios2_DataLength : (u16)body_len;
		tx_cb_publish(cb, body_staging, body_dma,
					  ((u32)wire_len << DMA_BUFLENGTH_SHIFT) | (GENET_QTAG_MASK << DMA_TX_QTAG_SHIFT) | DMA_TX_APPEND_CRC | DMA_SOP | DMA_EOP);
		tx_advance_prod(ring);

		if (unlikely(is_dma_path))
			unit->internalStats.tx_dma++;
		else
			unit->internalStats.tx_copy++;
		unit->internalStats.tx_bytes += wire_len;
	}
	unit->internalStats.tx_packets++;

	KprintfH("[genet] %s: Scheduled, tx_prod_index %ld\n", __func__, ring->tx_prod_index);
	mmio_write32(ring->tx_prod_index, BCMGENET_REG(unit, TDMA_PROD_INDEX));
	Permit();
	return COMMAND_PROCESSED;

err_no_buf:
	unit->internalStats.tx_dropped++;
	io->ios2_WireError = S2WERR_BUFF_ERROR;
	io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
	UnitSubmitControlAsync(unit, UNIT_CTRL_EVENT_REPORT, (union UnitControlPayload){.eventSet = S2EVENT_BUFF | S2EVENT_TX | S2EVENT_SOFTWARE | S2EVENT_ERROR});
	return COMMAND_PROCESSED;
}
