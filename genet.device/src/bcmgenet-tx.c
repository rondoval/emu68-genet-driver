// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom GENET (Gigabit Ethernet) controller driver — netdev TX path.
 *
 * Copyright (c) 2014-2025 Broadcom
 *
 * Zero-copy scatter-gather transmit: the stack submits descriptor batches
 * whose segments live in memory from ndo_DmaAlloc (guaranteed reachable);
 * one hardware doorbell per batch. No staging buffers, no memcpy.
 *
 * NO LOCK. Two single-writer relations carry the whole path:
 *
 *   BD ring      producer (this file's submit, foreign task, serialized by
 *                the stack's core lock) -> hardware. Descriptors live in the
 *                controller's BD RAM and are read by the DMA engine alone,
 *                so free space comes straight off TDMA_CONS_INDEX and no
 *                consumer state takes part.
 *   ndTxDone     producer (submit) -> consumer (unit task, tx_harvest), an
 *                SPSC FIFO of {cookie, end-of-packet BD index}. It is what
 *                decouples cookie delivery from BD reuse, and the only
 *                object the two contexts share.
 *
 * nso_TxDone is called from the unit task only, never from a submitter.
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

#include <genet/bcmgenet.h>
#include <genet/bcmgenet-regs.h>
#include <dma_mem.h>

/* Cookies delivered per nso_TxDone call. */
#define TX_DONE_BATCH 32u

/* Combined address + length/status setter */
static inline void dmadesc_set(APTR descriptor_address, dma_addr_t addr, u32 val)
{
	mmio_write32((u32)addr, descriptor_address + DMA_DESC_ADDRESS_LO);
	mmio_write32(val, descriptor_address + DMA_DESC_LENGTH_STATUS);
}

/* The ring slot the producer is about to fill. TX_DESCS is 256, so the
 * 16-bit BD index truncates straight to it — no separate write pointer. */
static inline u8 tx_slot(const struct bcmgenet_tx_ring *ring)
{
	return (u8)ring->tx_prod_index;
}

/* BD RAM address of that slot. */
static inline APTR tx_desc(const struct GenetUnit *unit, const struct bcmgenet_tx_ring *ring)
{
	return BCMGENET_REG(unit, GENET_TX_OFF) + (u32)tx_slot(ring) * DMA_DESC_SIZE;
}

static inline u16 tx_free_bds(const struct bcmgenet_tx_ring *ring)
{
	return (u16)(TX_DESCS - ((u32)(ring->tx_prod_index - ring->hw_cons_cache) & DMA_P_INDEX_MASK));
}

static inline void tx_advance_prod(struct bcmgenet_tx_ring *ring)
{
	ring->tx_prod_index = (u16)(((u32)ring->tx_prod_index + 1U) & DMA_P_INDEX_MASK);
}

/* FIFO slots the producer may still fill. */
static inline u32 txdone_free(const struct GenetUnit *unit)
{
	return ND_TXDONE_RING_N - (unit->ndTxDoneProd - unit->ndTxDoneCons);
}

/* Does the unit task have completions waiting as of `hw_cons`? Producer-side
 * peek at the consumer's head entry: the consumer only ever advances, and an
 * entry is not rewritten until the producer wraps onto it (which needs the
 * consumer to have passed it), so a stale read costs at most one redundant
 * Signal to a task that is by definition already awake. */
static inline BOOL txdone_pending(const struct GenetUnit *unit, u16 hw_cons)
{
	u32 cons = unit->ndTxDoneCons;
	if (cons == unit->ndTxDoneProd)
		return FALSE;
	/* Read the producer's published index before the entry it publishes. */
	asm volatile("" ::: "memory");
	return (u16)(hw_cons - unit->ndTxDone[cons & ND_TXDONE_RING_MASK].gtd_End) < 0x8000u;
}

/* Queue a cookie for the unit task, complete once the hardware consumer
 * reaches `end`. Producer side; the caller has checked txdone_free().
 * `bytes` is 0 for a packet the sanity gate rejected. */
static inline void txdone_push(struct GenetUnit *unit, APTR cookie, u16 end, u16 bytes)
{
	u32 prod = unit->ndTxDoneProd;
	struct GenetTxDone *e = &unit->ndTxDone[prod & ND_TXDONE_RING_MASK];
	e->gtd_Cookie = cookie;
	e->gtd_End = end;
	e->gtd_Bytes = bytes;
	asm volatile("" ::: "memory");
	unit->ndTxDoneProd = prod + 1;
}

/*
 * Deliver every TX cookie the hardware has finished with. Unit task only.
 *
 * The hardware consumer index is sampled ONCE, which bounds the pass: a
 * packet nso_TxDone submits from inside this call cannot complete into it.
 * Completion is a modular comparison against that snapshot — safe because an
 * unharvested entry's distance can never approach the 16-bit half-window
 * (the FIFO holds at most ND_TXDONE_RING_N packets of ND_TX_MAX_SEGS + 1
 * BDs each, i.e. well under 32768 BDs of producer advance).
 */
void bcmgenet_tx_harvest(struct GenetUnit *unit)
{
	u32 cons = unit->ndTxDoneCons;
	if (cons == unit->ndTxDoneProd)
		return; /* nothing outstanding: no MMIO, no work */
	if (unlikely(unit->ndStackOps == NULL))
	{
		/* Detached with entries still queued — a close that skipped DETACH.
		 * Retire them silently; there is no longer a stack to tell. */
		unit->ndTxDoneCons = unit->ndTxDoneProd;
		return;
	}

	PERF_T0(t_harvest);
	u16 hw_cons = (u16)(mmio_read32(BCMGENET_REG(unit, TDMA_CONS_INDEX)) & DMA_C_INDEX_MASK);

	APTR batch[TX_DONE_BATCH];
	for (;;)
	{
		ULONG n = 0;
		/* re-read the producer index every pass: nso_TxDone below may
		 * re-enter ndo_TxSubmit and extend the FIFO */
		while (n < TX_DONE_BATCH && cons != unit->ndTxDoneProd)
		{
			const struct GenetTxDone *e = &unit->ndTxDone[cons & ND_TXDONE_RING_MASK];
			if ((u16)(hw_cons - e->gtd_End) >= 0x8000u)
				break; /* still in flight — and so is everything behind it */
			/* Tallied here, where the hardware consumer has passed the packet,
			 * so the counters mean "transmitted". A zero length marks a
			 * sanity-gate reject, already counted in tx_bad. */
			if (likely(e->gtd_Bytes != 0))
			{
				unit->internalStats.tx_packets++;
				unit->internalStats.tx_bytes += e->gtd_Bytes;
			}
			batch[n++] = e->gtd_Cookie;
			cons++;
		}
		if (n == 0)
			break;
		/* Publish before the callback: a re-entrant submit. */
		asm volatile("" ::: "memory");
		unit->ndTxDoneCons = cons;
		unit->ndStackOps->nso_TxDone(unit->ndStackCtx, batch, n);
	}
	PERF_ADD(&unit->gu_Perf, GP_TX_HARVEST, t_harvest);
}

/*
 * STOP-time quiesce: hardware DMA is already disabled — complete everything
 * still queued so the stack's memory accounting closes exactly. Unit task
 * context, and the ABI requires it to finish before the STOP op replies.
 *
 * ndStarted is cleared before this runs, but a submitter that read the flag
 * just before then can still be part-way through queueing its cookies, and
 * anything it pushes after the drain below would never be completed — a
 * permanent pbuf leak in the stack. So wait it out first. ndo_TxSubmit never
 * blocks, so an in-flight submitter needs nothing but CPU to finish: dropping
 * below every other task hands it exactly that, and the wait is bounded by one
 * submit call.
 */
void bcmgenet_netdev_tx_quiesce(struct GenetUnit *unit)
{
	if (unit->ndTxBusy != 0)
	{
		struct Task *self = FindTask(NULL);
		BYTE pri = SetTaskPri(self, -128);
		while (unit->ndTxBusy != 0)
			;
		SetTaskPri(self, pri);
	}

	bcmgenet_tx_harvest(unit); /* whatever did make it onto the wire */

	if (unit->ndStackOps == NULL)
		return; /* no stack left to complete cookies to; harvest retired them */

	APTR batch[TX_DONE_BATCH];
	u32 cons = unit->ndTxDoneCons;
	while (cons != unit->ndTxDoneProd)
	{
		ULONG n = 0;
		while (n < TX_DONE_BATCH && cons != unit->ndTxDoneProd)
		{
			const struct GenetTxDone *e = &unit->ndTxDone[cons & ND_TXDONE_RING_MASK];
			batch[n++] = e->gtd_Cookie;
			cons++;
			/* Zero length is a sanity-gate reject: already in tx_bad, and
			 * counting it here too would report one lost frame as two. */
			if (e->gtd_Bytes != 0)
				unit->internalStats.tx_dropped++;
		}
		asm volatile("" ::: "memory");
		unit->ndTxDoneCons = cons;
		unit->ndStackOps->nso_TxDone(unit->ndStackCtx, batch, n);
	}
}

/* A descriptor is submittable iff its segment count is in range and every
 * segment is a non-empty, in-bounds, DMA-reachable buffer. Checked before the
 * segment reaches CachePreDMA: a trashed pointer would fault Emu68's ARM-side
 * cache-clean loop (dc cvac on an unmapped address). */
static BOOL tx_desc_ok(struct GenetUnit *unit, const struct NetDevTxDesc *d)
{
	if (d->ntd_NumSegs == 0 || d->ntd_NumSegs > ND_TX_MAX_SEGS)
		return FALSE;
	for (UWORD s = 0; s < d->ntd_NumSegs; s++)
	{
		const struct NetDevSg *sg = &d->ntd_Segs[s];
		/* per-segment ceiling: RX_BUF_LENGTH (2048) is a generous sanity bound
		 * that holds only because MTU is negotiated at ETH_DATA_LEN (1500) — no
		 * stack segment approaches it. Revisit (tie to ndc_Mtu) if jumbo lands. */
		if (sg->nsg_Len == 0 || sg->nsg_Len > RX_BUF_LENGTH ||
		    !dma_addr_reachable(&unit->dma_ctx, sg->nsg_Data, sg->nsg_Len))
			return FALSE;
	}
	return TRUE;
}

/* The ndo_TxSubmit implementation: accept a prefix of the batch, one
 * doorbell. Foreign task context (the stack's core lock serializes callers).
 */
LONG bcmgenet_netdev_tx_submit(struct GenetUnit *unit, const struct NetDevTxDesc *descs, ULONG count)
{
	KprintfT("[genet] %s: count %lu len %lu\n", __func__, count,
			 (ULONG)(count != 0 ? descs[0].ntd_Segs[0].nsg_Len : 0));
	if (unlikely(!unit->ndStarted))
		return 0;

	struct bcmgenet_tx_ring *ring = &unit->tx_ring;

	/*
	 * One pass over the batch: per descriptor, validate -> reserve -> prime ->
	 * write, with a single prod-index doorbell for the whole accepted prefix.
	 *
	 * Validation (tx_desc_ok) precedes the CachePreDMA prime because a trashed
	 * segment pointer would fault Emu68's ARM-side cache-clean loop. The prime
	 * follows the ring-space check, so nothing is cleaned that the ring will
	 * not accept — the rejected tail is never touched. Every clean carries
	 * DMAF_NoSync; the single closing barrier is the emu68_barrier() before the
	 * doorbell, the only point where the hardware starts reading. Free space is
	 * read from the hardware consumer index, so a slow harvest never costs ring
	 * capacity.
	 */
	PERF_T0(t_submit); /* driver-internal submit: validate + cache prime + ring writes + doorbell */
	ULONG accepted = 0;
	BOOL kick = FALSE;

	while (accepted < count)
	{
		const struct NetDevTxDesc *d = &descs[accepted];

		if (unlikely(!tx_desc_ok(unit, d)))
		{
			/* A bad descriptor deeper in the batch truncates to the good
			 * prefix, which still proceeds to the doorbell; the bad one
			 * returns on the caller's retry. A bad head has no good prefix to
			 * hide behind: consume and complete it as a zero-BD, zero-length
			 * drop so the stack's accounting closes and the caller advances. */
			if (accepted != 0)
				break;
			if (txdone_free(unit) == 0)
				return 0; /* no room to report it — retry after the next harvest */
			Kprintf("[genet] %s: BAD SG in head descriptor (segs %lu, seg0 data 0x%08lx len %lu) — frame dropped\n",
					__func__, (ULONG)d->ntd_NumSegs,
					(ULONG)d->ntd_Segs[0].nsg_Data, (ULONG)d->ntd_Segs[0].nsg_Len);
			unit->internalStats.tx_bad++;
			txdone_push(unit, d->ntd_Cookie, ring->tx_prod_index, 0);
			Signal(unit->task, 1UL << unit->tx_signal);
			return 1;
		}

		u16 need = d->ntd_NumSegs;
		/* +1: with TBUF_64B_EN every frame's stream starts with a 64-byte
		 * TSB, submitted as its own SOP descriptor. */
		u16 need_bds = (u16)(need + 1);

		u16 free_bds = tx_free_bds(ring);
		if (unlikely(free_bds < TX_CONS_REFRESH_BDS))
		{
			ring->hw_cons_cache =
				(u16)(mmio_read32(BCMGENET_REG(unit, TDMA_CONS_INDEX)) & DMA_C_INDEX_MASK);
			free_bds = tx_free_bds(ring);
			/* Space came back straight off the hardware index, so the unit
			 * task may not have been woken for any of it: TXDMA_DONE needs
			 * TX_COALESCE_FRAMES buffers, which a bursty low-rate stream can
			 * go a long time without reaching. Wake it, or those cookies —
			 * and the stack's pbufs — wait for the housekeeping tick. */
			kick = TRUE;
		}
		if (free_bds <= need_bds || txdone_free(unit) == 0)
			break; /* full — the stack retries after nso_TxDone */

		/* Validated and known to fit: prime the segments for DMA. */
		for (u16 s = 0; s < need; s++)
			cache_pre_dma(d->ntd_Segs[s].nsg_Data, d->ntd_Segs[s].nsg_Len,
						  DMA_ReadFromRAM | DMAF_NoSync);

		/* TSB: one block per ring slot, keyed by the TSB descriptor's own
		 * slot. Only tx_csum_info is consumed by the hardware. */
		struct genet_status_64 *tsb = (struct genet_status_64 *)
			((u8 *)unit->ndTxTsb + ((u32)tx_slot(ring) * GENET_STATUS64_LEN));
		u32 csum_info = 0;
		if (d->ntd_Flags & NDTF_L4CSUM)
		{
			csum_info = STATUS_TX_CSUM_LV |
						(((u32)d->ntd_CsumStart & STATUS_TX_CSUM_START_MASK)
						 << STATUS_TX_CSUM_START_SHIFT) |
						((u32)d->ntd_CsumOffset & STATUS_TX_CSUM_OFFSET_MASK);
			if (d->ntd_Flags & NDTF_L4_UDP)
				csum_info |= STATUS_TX_CSUM_PROTO_UDP;
		}
		tsb->tx_csum_info = le32(csum_info);
		cache_pre_dma(tsb, GENET_STATUS64_LEN, DMA_ReadFromRAM | DMAF_NoSync);

		/* the TSB says WHERE to checksum; the SOP descriptor's
		 * DMA_TX_DO_CSUM is what makes the engine do it at all */
		u32 sop_stat = ((u32)GENET_STATUS64_LEN << DMA_BUFLENGTH_SHIFT) |
					   (GENET_QTAG_MASK << DMA_TX_QTAG_SHIFT) |
					   DMA_TX_APPEND_CRC | DMA_SOP;
		if (d->ntd_Flags & NDTF_L4CSUM)
			sop_stat |= DMA_TX_DO_CSUM;
		dmadesc_set(tx_desc(unit, ring), (dma_addr_t)tsb, sop_stat);
		tx_advance_prod(ring);

		u16 bytes = 0;
		for (u16 s = 0; s < need; s++)
		{
			const struct NetDevSg *sg = &d->ntd_Segs[s];

			u32 len_stat = ((u32)sg->nsg_Len << DMA_BUFLENGTH_SHIFT) |
						   (GENET_QTAG_MASK << DMA_TX_QTAG_SHIFT) |
						   DMA_TX_APPEND_CRC;
			if (s == need - 1)
				len_stat |= DMA_EOP;

			dmadesc_set(tx_desc(unit, ring), (dma_addr_t)sg->nsg_Data, len_stat);
			tx_advance_prod(ring);
			bytes = (u16)(bytes + sg->nsg_Len);
		}

		/* The length travels with the cookie: the packet and byte counters are
		 * tallied by the harvest, on the unit task, so nothing outside that
		 * task writes them and no snapshot can catch a 64-bit carry half
		 * applied. */
		txdone_push(unit, d->ntd_Cookie, ring->tx_prod_index, bytes);
		accepted++;
	}

	/* The doorbell is deferred: staged descriptors (tx_prod_index advanced,
	 * segments/TSBs cleaned NoSync) are published later by a single
	 * bcmgenet_netdev_tx_kick() per burst — one barrier + one TDMA_PROD_INDEX
	 * write for the whole locked run instead of one per frame. */
	if (accepted == 0 && count != 0)
		unit->internalStats.tx_rejected++; /* ring full */

	if (unlikely(kick) && txdone_pending(unit, ring->hw_cons_cache))
		Signal(unit->task, 1UL << unit->tx_signal);

	PERF_ADD(&unit->gu_Perf, GP_TX_SUBMIT, t_submit);
	return (LONG)accepted;
}

/* Publish every descriptor staged by prior bcmgenet_netdev_tx_submit() calls:
 * close the accumulated DMAF_NoSync clean batch with one barrier, then hand the
 * producer index to the hardware. Idempotent — re-writing the current index is
 * harmless, so this is safe to call with nothing staged (STOP backstop). */
void bcmgenet_netdev_tx_kick(struct GenetUnit *unit)
{
	emu68_barrier();
	mmio_write32(unit->tx_ring.tx_prod_index, BCMGENET_REG(unit, TDMA_PROD_INDEX));
}
