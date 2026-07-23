// SPDX-License-Identifier: GPL-2.0+
/*
 * Broadcom GENETv5 — netdev RX datapath: the ring drain that hands frames up to
 * the stack and swaps fresh pool buffers into the descriptors, plus RX ring
 * init.
 */

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/gic400_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>

#define GIC400_BASE_NAME unit->device->gic400Base
#include <proto/gic400.h>
#endif

#include <exec/types.h>
#include <limits.h>

#include <debug.h>
#include <bits.h>
#include <cache_ops.h>
#include <errors.h>
#include <iomem.h>
#include <memory.h>
#include <timing.h>
#include <types.h>
#include <device.h>

#include <genet/phy.h>
#include <genet/unimac.h>
#include <genet/bcmgenet-mib.h>
#include <genet/bcmgenet-regs.h>
#include <genet/bcmgenet-irq.h>
#include <genet/bcmgenet-priv.h>

/* netdev zero-copy RX: hand completed buffers to the stack and swap fresh
 * pool buffers into the ring slots. A batch of descriptors per nso_RxInput
 * call; refused or unswappable frames recycle in place (counted). Unit task
 * context only. */
static void netdev_rx_flush(struct GenetUnit *unit, struct NetDevRxDesc *batch, ULONG n)
{
	PERF_T0(t_flush);
	ULONG consumed = unit->ndStackOps->nso_RxInput(unit->ndStackCtx, batch, n);
	PERF_ADD(&unit->gu_Perf, GP_RX_FLUSH, t_flush);

	unit->ndRxHeld += consumed;
	unit->internalStats.rx_packets += consumed;
	for (ULONG i = 0; i < consumed; i++)
		unit->internalStats.rx_bytes += batch[i].nrd_Len;

	/* backpressure: the tail goes straight back to the pool */
	for (ULONG i = consumed; i < n; i++)
	{
		netdev_rx_push(unit, batch[i].nrd_Cookie);
		unit->internalStats.rx_dropped++;
	}
}

s32 bcmgenet_netdev_rx(struct GenetUnit *unit, u16 budget)
{
	/* Released buffers become swap candidates before anything else — this
	 * runs on every unit-task wakeup, so the pool is replenished (and the
	 * re-arm cache maintenance paid) even on a tick with no traffic. */
	netdev_drain_recycle(unit);

	u32 rx_prod_reg = mmio_read32(BCMGENET_REG(unit, RDMA_PROD_INDEX));
	u16 discards = (u16)((rx_prod_reg >> DMA_P_INDEX_DISCARD_CNT_SHIFT) & DMA_P_INDEX_DISCARD_CNT_MASK);
	u16 rx_prod_index = (u16)(rx_prod_reg & DMA_P_INDEX_MASK);

	if (rx_prod_index == unit->rx_ring.rx_cons_index)
		return -EAGAIN;

	if (unlikely(discards > unit->rx_ring.old_discards))
	{
		u16 new_discards = (u16)(discards - unit->rx_ring.old_discards);
		/* TRACE tier only: at sustained overrun this fires once per drain
		 * pass, and the print cost at drain entry — the moment the ring is
		 * near-full — feeds the very overrun it reports. The mib line carries
		 * rdmadrop deltas anyway. */
		KprintfT("[genet] %s: RDMA discarded %lu frame(s) — ring overrun\n",
				 __func__, (ULONG)new_discards);
		unit->internalStats.rx_overruns += new_discards;
		unit->rx_ring.old_discards = (u16)(unit->rx_ring.old_discards + new_discards);

		/* Clear HW register when we reach 75% of maximum 0xFFFF */
		if (unit->rx_ring.old_discards >= 0xC000)
		{
			unit->rx_ring.old_discards = 0;
			mmio_write32(0, BCMGENET_REG(unit, RDMA_PROD_INDEX));
		}
	}

	PERF_T0(t_drain);
	struct NetDevRxDesc batch[ND_RX_BATCH];
	ULONG batched = 0;

	u16 rx_cons_index = unit->rx_ring.rx_cons_index;
	u16 to_process = (u16)((u32)(rx_prod_index - rx_cons_index) & DMA_C_INDEX_MASK);
	if (to_process > budget)
		to_process = budget;
	rx_prod_index = (u16)((u32)(rx_cons_index + to_process) & DMA_C_INDEX_MASK);
	while (rx_cons_index != rx_prod_index)
	{
		struct enet_cb *rx_cb = &unit->rx_ring.rx_control_block[(u8)rx_cons_index];
		u8 *desc_base = (u8 *)rx_cb->descriptor_address;
		u32 length = mmio_read32(desc_base + DMA_DESC_LENGTH_STATUS);
		u16 dma_flags = length & 0xffffu;
		length = (length >> DMA_BUFLENGTH_SHIFT) & DMA_BUFLENGTH_MASK;
		u8 *addr = (u8 *)rx_cb->data_buffer;

		if (unlikely(length > RX_BUF_LENGTH))
		{
			KprintfT("[genet] %s: len %lu exceeds RX_BUF_LENGTH %lu\n", __func__, (ULONG)length, (ULONG)RX_BUF_LENGTH);
			unit->internalStats.rx_length_errors++;
			goto next;
		}

		if (unlikely(!(dma_flags & DMA_EOP) || !(dma_flags & DMA_SOP)))
		{
			KprintfT("[genet] %s: dropping fragmented packet, dma_flags=0x%lx\n", __func__, (ULONG)dma_flags);
			unit->internalStats.rx_fragmented_errors++;
			goto next;
		}

		/* report errors */
		if (unlikely(dma_flags & (DMA_RX_CRC_ERROR |
								  DMA_RX_OV |
								  DMA_RX_NO |
								  DMA_RX_LG |
								  DMA_RX_RXER)))
		{
			KprintfT("[genet] %s: Packet error, length=%lu, dma_flag=0x%lx\n",
					 __func__, (ULONG)length, (ULONG)dma_flags);
			if (dma_flags & DMA_RX_CRC_ERROR)
				unit->internalStats.rx_crc_errors++;
			if (dma_flags & DMA_RX_OV)
				unit->internalStats.rx_over_errors++;
			if (dma_flags & DMA_RX_NO)
				unit->internalStats.rx_frame_errors++;
			if (dma_flags & DMA_RX_LG)
				unit->internalStats.rx_length_errors++;
			if ((dma_flags & (DMA_RX_CRC_ERROR |
							  DMA_RX_OV |
							  DMA_RX_NO |
							  DMA_RX_LG |
							  DMA_RX_RXER)) == DMA_RX_RXER)
				unit->internalStats.rx_other_errors++;
			goto next;
		} /* error packet */

		if (unlikely(length <= GENET_STATUS64_LEN))
		{
			unit->internalStats.rx_length_errors++;
			goto next;
		}

		if (likely(unit->ndStarted))
		{
			APTR fresh = netdev_rx_pop(unit);
			if (unlikely(fresh == NULL))
			{
				/* Pool dry — usually releases parked in the recycle ring:
				 * on one CPU the app frees buffers whenever we block on
				 * ns_Core mid-flush, after this pass's entry drain. Reclaim
				 * and retry before declaring a drop, since pool-dry from parked
				 * releases dominates genuine ring overrun. */
				netdev_drain_recycle(unit);
				fresh = netdev_rx_pop(unit);
			}
			if (unlikely(fresh == NULL))
			{
				/* genuinely exhausted: keep the buffer in the ring, drop
				 * the frame (the peer is window-limited anyway) */
				unit->internalStats.rx_dropped++;
				unit->internalStats.rx_pool_dry++;
				goto next;
			}

			/* Invalidate the DMA-written buffer, now that the frame is
			 * accepted. Every drop above — error, oversize, !ndStarted,
			 * pool-dry — reads nothing from the buffer and re-arms it via the
			 * recycle-path pre-DMA clean, so it skips the ~32-line ivac for
			 * free. Must precede the first buffer read: the RSB below. */
			cache_post_dma(addr, length, 0);

			/* The 64-byte RSB precedes the frame. RXCHK's result (L3
			 * parser off) is the PLAIN 1's-complement sum over the frame
			 * past the Ethernet header, in the low half of the LE word.
			 * The halfword needs NO further swap. Zero
			 * means the block produced no result: pass the frame up
			 * unvalidated (the Ethernet FCS covered the wire). The OK/FR
			 * bits are L3-parser-only and stay clear in this mode. */
			const struct genet_status_64 *rsb = (const struct genet_status_64 *)addr;
			u16 rx_csum = (u16)le32(rsb->rx_csum);

			/* the buffer leaves the ring: hand it up, swap the slot */
			struct NetDevRxDesc *d = &batch[batched++];
			d->nrd_Data = addr + GENET_STATUS64_LEN;
			d->nrd_Len = length - GENET_STATUS64_LEN;
			d->nrd_Flags = (rx_csum != 0) ? NDRF_CSUM_RAW : 0;
			d->nrd_CsumRaw = rx_csum;
			d->nrd_Cookie = addr;

			rx_cb->data_buffer = (dma_addr_t)fresh;
			mmio_write32((u32)(dma_addr_t)fresh, desc_base + DMA_DESC_ADDRESS_LO);

			if (batched == ND_RX_BATCH)
			{
				netdev_rx_flush(unit, batch, batched);
				batched = 0;
			}
		}
	next:
		rx_cons_index++;
	}

	if (batched != 0)
		netdev_rx_flush(unit, batch, batched);

	unit->rx_ring.rx_cons_index = rx_cons_index;
	mmio_write32(rx_cons_index, BCMGENET_REG(unit, RDMA_CONS_INDEX));

	PERF_ADD(&unit->gu_Perf, GP_RX_DRAIN, t_drain);
	return to_process;
}

static u32 bcmgenet_init_rx_ring(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Initializing RX ring\n", __func__);
	struct bcmgenet_rx_ring *ring = &unit->rx_ring;

	/* Initialize common Rx ring structures. The control block is allocated once
	 * at probe time and reused across start/stop cycles. */
	const APTR desc_base = unit->genetBase + GENET_RX_OFF;
	memset(ring->rx_control_block, 0, RX_DESCS * sizeof(struct enet_cb));

	const u32 len_stat = (RX_BUF_LENGTH << DMA_BUFLENGTH_SHIFT); // | DMA_OWN;

	/* Ring slots draw their buffers from the netdev pool (allocated at
	 * ATTACH, larger than the ring); the pool must cover the whole ring. */
	for (u32 i = 0; i < RX_DESCS; i++)
	{
		dma_addr_t buffer = (dma_addr_t)netdev_rx_pop(unit);
		APTR descriptor_address = desc_base + i * DMA_DESC_SIZE;

		if (buffer == 0)
		{
			Kprintf("[genet] %s: RX pool underrun at slot %lu\n", __func__, i);
			return ENOMEM;
		}

		ring->rx_control_block[i].descriptor_address = descriptor_address;
		ring->rx_control_block[i].data_buffer = buffer;

		mmio_write32((dma_addr_t)buffer, descriptor_address + DMA_DESC_ADDRESS_LO);
		mmio_write32(len_stat, descriptor_address + DMA_DESC_LENGTH_STATUS);
	}

	/* cannot init RDMA_PROD_INDEX to 0, so align RDMA_CONS_INDEX on it instead */
	ring->rx_cons_index = mmio_read32(BCMGENET_REG(unit, RDMA_PROD_INDEX)) & DMA_P_INDEX_MASK;
	mmio_write32(ring->rx_cons_index, BCMGENET_REG(unit, RDMA_CONS_INDEX));
	KprintfT("[genet] %s: rx_cons_index=%lu\n", __func__, (ULONG)unit->rx_ring.rx_cons_index);

	mmio_write32((RX_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH, unit->genetBase + RDMA_RING_REG_BASE + DMA_RING_BUF_SIZE);
	mmio_write32((DMA_FC_THRESH_LO << DMA_XOFF_THRESHOLD_SHIFT) | DMA_FC_THRESH_HI, unit->genetBase + RDMA_XON_XOFF_THRESH);

	/* Set start and end address, read and write pointers */
	mmio_write32(0x0, unit->genetBase + RDMA_RING_REG_BASE + DMA_START_ADDR);
	mmio_write32(0x0, unit->genetBase + RDMA_READ_PTR);
	mmio_write32(0x0, unit->genetBase + RDMA_WRITE_PTR);
	mmio_write32(RX_DESCS * DMA_DESC_SIZE / 4 - 1, unit->genetBase + RDMA_RING_REG_BASE + DMA_END_ADDR);

	return GENET_OK;
}

u32 bcmgenet_init_rx_queues(struct GenetUnit *unit)
{
	u32 ret = bcmgenet_init_rx_ring(unit);
	if (ret != GENET_OK)
	{
		return ret;
	}

	/* Configure Rx queues as descriptor rings */
	mmio_write32(1 << DEFAULT_Q, unit->genetBase + RDMA_REG_BASE + DMA_RING_CFG);

	/* Enable Rx rings */
	u32 dma_ctrl = 1 << (DEFAULT_Q + DMA_RING_BUF_EN_SHIFT);
	mmio_write32(dma_ctrl, unit->genetBase + RDMA_REG_BASE + DMA_CTRL);
	return GENET_OK;
}
