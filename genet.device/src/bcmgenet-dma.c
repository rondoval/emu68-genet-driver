// SPDX-License-Identifier: GPL-2.0+
/*
 * Broadcom GENETv5 — DMA engine setup: enable/disable, ring and queue init for
 * both directions, and interrupt-moderation programming.
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

/* Stopping the engine means the ring's own enable as well: a descriptor index is
 * only writable while its ring is disabled (the ring inits rely on that). */
#define GENET_DMA_OFF (DMA_EN | (1u << (DEFAULT_Q + DMA_RING_BUF_EN_SHIFT)))

void bcmgenet_disable_dma(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Disabling DMA\n", __func__);
	mmio_clear32(BCMGENET_REG(unit, TDMA_REG_BASE + DMA_CTRL), GENET_DMA_OFF);
	for (u32 timeout = 0; timeout < DMA_TIMEOUT_VAL; timeout++)
	{
		u32 tdma = mmio_read32(unit->genetBase + TDMA_REG_BASE + DMA_CTRL);
		if (!(tdma & DMA_EN))
		{
			break;
		}
		delay_us(1);
	}

	/* Let both engines drain whatever they had in flight. A busy wait, but
	 * this runs only at start, stop and the pre-reset quiesce. */
	delay_ms(10);

	mmio_clear32(BCMGENET_REG(unit, RDMA_REG_BASE + DMA_CTRL), GENET_DMA_OFF);
	for (u32 timeout = 0; timeout < DMA_TIMEOUT_VAL; timeout++)
	{
		u32 rdma = mmio_read32(unit->genetBase + RDMA_REG_BASE + DMA_CTRL);
		if (!(rdma & DMA_EN))
		{
			break;
		}
		delay_us(1);
	}
	KprintfT("[genet] %s: DMA disabled\n", __func__);

	/* Flush TX queues */
	mmio_write32(1, BCMGENET_REG(unit, UMAC_TX_FLUSH));
	delay_us(10);
	mmio_write32(0, BCMGENET_REG(unit, UMAC_TX_FLUSH));
}

static void bcmgenet_enable_dma(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Enabling DMA\n", __func__);
	mmio_set32(BCMGENET_REG(unit, RDMA_REG_BASE + DMA_CTRL), DMA_EN);
	mmio_set32(BCMGENET_REG(unit, TDMA_REG_BASE + DMA_CTRL), DMA_EN);
}

/* Are these settings programmable? Pure test, so a stopped unit can accept and
 * store values it will only apply at the next start.
 *
 * Base system clock is 125MHz and the DMA timeout counts that reference clock
 * divided by 1024, roughly 8.192us per tick, so the usec figure has to reduce
 * to something that fits DMA_TIMEOUT_MASK.
 */
u32 bcmgenet_coalesce_valid(u32 tx_max_coalesced_frames, u32 rx_max_coalesced_frames,
							u32 rx_coalesce_usecs)
{
	if (tx_max_coalesced_frames > DMA_INTR_THRESHOLD_MASK ||
		tx_max_coalesced_frames == 0 ||
		rx_max_coalesced_frames > DMA_INTR_THRESHOLD_MASK ||
		rx_coalesce_usecs > (DMA_TIMEOUT_MASK * 8) + 1)
		return EINVAL;

	if (rx_coalesce_usecs == 0 && rx_max_coalesced_frames == 0)
		return EINVAL;

	return GENET_OK;
}

/*
 * RX moderation by profile.
 *
 * The RDMA timeout counts from the first pending frame, and frames that keep
 * arriving do not postpone it. So a lone small frame - the reply somebody is
 * blocked on - always waits the full timeout, and no static value serves both
 * that frame and a line-rate burst: a short timeout costs a third of the bulk
 * throughput, one interrupt and one wakeup of this task per few frames.
 *
 * Which of the two matters is not something a NIC driver can see; the stack
 * says so (NETDEV_CMD_SET_RX_PROFILE). What the driver knows is its ring:
 *
 *   static      no profile stated, or the operator pinned values with
 *               SET_COALESCE: the unit's coalesce settings, a compromise
 *               that has to serve both.
 *   THROUGHPUT  stated: nobody waits for a single frame, so the longer bulk
 *               timeout; at line rate the frame threshold fires first.
 *   LATENCY     the burst ladder. Whenever an RX pass is about to re-arm its
 *               interrupt it looks at the ring once more. Quiet: level 0, one
 *               frame, so whatever comes next interrupts at once. Frames
 *               already waiting again: a burst is in progress, one level up -
 *               a timeout that doubles per level up to the static setting. A
 *               burst thus pays a few quick passes while it proves itself and
 *               is batched as usual from then on, and the frame after it is a
 *               lone frame again. The top of the ladder is the static timeout,
 *               not the bulk one: the end of a large reply waits for it, and
 *               under this profile somebody waits for that end.
 *               One thing overrules a quiet ring: the consumer being behind.
 *               When the stack still holds two poll batches or more of our
 *               buffers while the wire is idle, whoever this traffic is for is
 *               busy with something else - a file copied window by window
 *               while the application writes the previous one out - and a
 *               burst gains nothing from early interrupts that cost it CPU
 *               (measured: 6 % of such a copy). The ring is then re-armed at
 *               the bulk level, and the next burst is batched from its first
 *               frame, until a quiet ring finds the buffers returned. Measured
 *               at quiet moments: 161-192 buffers held in 99 % of them during
 *               such a copy, at most 96 in 96 % of them when the reader waits.
 *
 * The registers are written only when the level changes. Going down to one
 * frame is the only change that could strand a frame (it lands between the
 * look at the ring and the write, under a threshold it did not reach and a
 * timeout nobody can vouch for across a rewrite), so that one is followed by
 * another look, and a poll if the ring is no longer empty. Every other change
 * leaves frames under a threshold and a timeout that both still fire.
 */
static const struct
{
	u16 frames;
	u16 usecs;
} genet_rx_ladder[] = {
	{1, 8}, /* the hardware's minimum: one timer tick */
	{GENET_COAL_RX_FRAMES, 100},
	{GENET_COAL_RX_FRAMES, 200},
	{GENET_COAL_RX_FRAMES, 400},
	{GENET_COAL_RX_FRAMES, GENET_COAL_RX_USECS},	  /* GENET_RX_LADDER_TOP: as far as a burst climbs */
	{GENET_COAL_RX_FRAMES, GENET_COAL_RX_BULK_USECS}, /* GENET_RX_LADDER_BULK: only a consumer that is behind gets here */
};

#define GENET_RX_LADDER_BULK ((u32)(sizeof(genet_rx_ladder) / sizeof(genet_rx_ladder[0])) - 1)
#define GENET_RX_LADDER_TOP (GENET_RX_LADDER_BULK - 1)

/* The consumer is behind: it holds this many poll batches unread while the wire is idle. */
#define GENET_RX_BEHIND_BATCHES 2

static inline BOOL bcmgenet_rx_pending(struct GenetUnit *unit)
{
	u16 prod = (u16)(mmio_read32(BCMGENET_REG(unit, RDMA_PROD_INDEX)) & DMA_P_INDEX_MASK);
	return prod != unit->rx_ring.rx_cons_index;
}

/* RDMA has a frame threshold and a timeout. The timeout register counts the
 * 125MHz reference divided by 1024, so ~8.192us per tick. */
static void bcmgenet_rx_coalesce_write(struct GenetUnit *unit, u32 frames, u32 usecs)
{
	mmio_write32(frames, BCMGENET_REG(unit, RDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH));

	u32 reg = mmio_read32(BCMGENET_REG(unit, RDMA_REG_BASE + DMA_RING16_TIMEOUT));
	reg &= ~DMA_TIMEOUT_MASK;
	reg |= DIV_CEIL(usecs * 1000, 8192);
	mmio_write32(reg, BCMGENET_REG(unit, RDMA_REG_BASE + DMA_RING16_TIMEOUT));
}

static void bcmgenet_rx_level_write(struct GenetUnit *unit)
{
	bcmgenet_rx_coalesce_write(unit, genet_rx_ladder[unit->rxLevel].frames,
							   genet_rx_ladder[unit->rxLevel].usecs);
}

void bcmgenet_rx_moderate(struct GenetUnit *unit)
{
	u32 level;

	if (bcmgenet_rx_pending(unit))
	{
		/* a burst in progress climbs to the top; one that began batched stays so */
		level = unit->rxLevel < GENET_RX_LADDER_TOP ? unit->rxLevel + 1 : unit->rxLevel;
	}
	else
	{
		PERF_HIST_ADD(&unit->gu_RxHeldHist, unit->ndRxHeld);

		level = 0;
		if (unit->ndRxHeld >= GENET_RX_BEHIND_BATCHES * (u32)unit->ndRxBatch)
		{
			level = GENET_RX_LADDER_BULK;
			unit->internalStats.rx_bulk_arms++;
		}
	}
	if (level == unit->rxLevel)
		return;

	unit->rxLevel = level;
	unit->internalStats.rx_level_changes++;
	bcmgenet_rx_level_write(unit);

	if (level == 0 && bcmgenet_rx_pending(unit))
		Signal(unit->task, 1UL << unit->rx_signal);
}

void bcmgenet_rx_moderation_reset(struct GenetUnit *unit)
{
	/* Two sources, and the pin decides which one is in force:
	 *
	 *   pinned     the operator named the numbers with an explicit
	 *              SET_COALESCE, and they outrank the stack's intent until an
	 *              all-default RX setting releases them. coalRxFrames and
	 *              coalRxUsecs are read HERE AND NOWHERE ELSE.
	 *   latency    the burst ladder.
	 *   throughput the driver's frame threshold with the longer bulk timeout.
	 *   unstated   the driver's own static pair, which has to serve both.
	 *
	 * The unpinned arms take the driver's constants rather than the unit's
	 * coal* fields even though the two are equal today */
	unit->rxLadder = !unit->coalPinned && unit->rxProfile == NDRP_LATENCY;
	unit->rxLevel = 0;

	if (unit->rxLadder)
	{
		bcmgenet_rx_level_write(unit);
		if (unit->task != NULL && bcmgenet_rx_pending(unit))
			Signal(unit->task, 1UL << unit->rx_signal);
	}
	else if (unit->coalPinned)
	{
		bcmgenet_rx_coalesce_write(unit, unit->coalRxFrames, unit->coalRxUsecs);
	}
	else
	{
		bcmgenet_rx_coalesce_write(unit, GENET_COAL_RX_FRAMES,
								   unit->rxProfile == NDRP_THROUGHPUT
									   ? GENET_COAL_RX_BULK_USECS
									   : GENET_COAL_RX_USECS);
	}
}

/*
 * The one place interrupt moderation is programmed from the unit's settings,
 * in both directions: from bcmgenet_init_dma() once the rings exist, and again
 * whenever NETDEV_CMD_SET_COALESCE lands on a running unit. The settings live
 * on the unit, so both callers program the same thing and neither has to be
 * told what it is. (A change of RX profile only needs the quiet RX half.)
 */
void bcmgenet_apply_coalesce(struct GenetUnit *unit)
{
	Kprintf("[genet] %s: tx_frames=%lu rx_frames=%lu rx_usecs=%lu pinned=%lu\n", __func__,
			(ULONG)unit->coalTxFrames, (ULONG)unit->coalRxFrames, (ULONG)unit->coalRxUsecs,
			(ULONG)unit->coalPinned);

	/* TDMA has no configurable timeout: it interrupts after this many buffers
	 * have been transmitted, or when the ring drains. */
	mmio_write32(unit->coalTxFrames,
				 BCMGENET_REG(unit, TDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH));

	bcmgenet_rx_moderation_reset(unit);
}

static u32 bcmgenet_init_tx_ring(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Initializing TX ring\n", __func__);
	struct bcmgenet_tx_ring *ring = &unit->tx_ring;

	/* No control-block array: TX descriptors are addressed straight from the
	 * BD index, and cookies live in the ndTxDone FIFO, not beside the ring. */

	mmio_write32(0, BCMGENET_REG(unit, TDMA_PROD_INDEX));
	mmio_write32(0, BCMGENET_REG(unit, TDMA_CONS_INDEX));
	ring->hw_cons_cache = 0;
	ring->tx_prod_index = 0;

	/* Disable rate control for now */
	mmio_write32(0x0, BCMGENET_REG(unit, TDMA_FLOW_PERIOD));
	mmio_write32((TX_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH, BCMGENET_REG(unit, TDMA_RING_REG_BASE + DMA_RING_BUF_SIZE));

	/* Set start and end address, read and write pointers */
	mmio_write32(0x0, BCMGENET_REG(unit, TDMA_RING_REG_BASE + DMA_START_ADDR));
	mmio_write32(0x0, BCMGENET_REG(unit, TDMA_READ_PTR));
	mmio_write32(0x0, BCMGENET_REG(unit, TDMA_WRITE_PTR));
	mmio_write32(TX_DESCS * DMA_DESC_SIZE / 4 - 1, BCMGENET_REG(unit, TDMA_RING_REG_BASE + DMA_END_ADDR));

	return GENET_OK;
}

static u32 bcmgenet_init_tx_queues(struct GenetUnit *unit)
{
	// We'll only setup queue 0

	/* Enable strict priority arbiter mode */
	mmio_write32(DMA_ARBITER_SP, unit->genetBase + TDMA_REG_BASE + DMA_ARB_CTRL);

	/* Initialize Tx priority queues */
	u32 ret = bcmgenet_init_tx_ring(unit);
	if (ret != GENET_OK)
	{
		return ret;
	}

	/* Set Tx queue priorities */
	mmio_write32(0, unit->genetBase + TDMA_REG_BASE + DMA_PRIORITY_0);
	mmio_write32(0, unit->genetBase + TDMA_REG_BASE + DMA_PRIORITY_1);
	mmio_write32(0, unit->genetBase + TDMA_REG_BASE + DMA_PRIORITY_2);

	/* Configure Tx queues as descriptor rings */
	mmio_write32(1 << DEFAULT_Q, BCMGENET_REG(unit, TDMA_REG_BASE + DMA_RING_CFG));

	/* Enable Tx rings */
	u32 dma_ctrl = 1 << (DEFAULT_Q + DMA_RING_BUF_EN_SHIFT);
	mmio_write32(dma_ctrl, unit->genetBase + TDMA_REG_BASE + DMA_CTRL);
	return GENET_OK;
}

u32 bcmgenet_init_dma(struct GenetUnit *unit)
{
	/* Disable RX/TX DMA and flush TX queues */
	bcmgenet_disable_dma(unit);

	KprintfT("[genet] %s: Initializing DMA\n", __func__);

	/* Flush RX */
	mmio_set32(BCMGENET_REG(unit, GENET_SYS_OFF + SYS_RBUF_FLUSH_CTRL), BIT(0));
	delay_us(10);
	mmio_clear32(BCMGENET_REG(unit, GENET_SYS_OFF + SYS_RBUF_FLUSH_CTRL), BIT(0));
	delay_us(10);

	/* Init rDma */
	mmio_write32(DMA_MAX_BURST_LENGTH, unit->genetBase + RDMA_REG_BASE + DMA_SCB_BURST_SIZE);

	/* Initialize Rx queues */
	u32 ret = bcmgenet_init_rx_queues(unit);
	if (ret != GENET_OK)
	{
		Kprintf("[genet] %s: Failed to initialize RX queues: %ld\n", __func__, ret);
		return ret;
	}

	/* Init tDma */
	mmio_write32(DMA_MAX_BURST_LENGTH, unit->genetBase + TDMA_REG_BASE + DMA_SCB_BURST_SIZE);
	ret = bcmgenet_init_tx_queues(unit);
	if (ret != GENET_OK)
	{
		Kprintf("[genet] %s: Failed to initialize TX queues: %ld\n", __func__, ret);
		return ret;
	}

	/* Both rings exist and DMA is still off: the moderation thresholds go in
	 * once, here, for both directions. */
	bcmgenet_apply_coalesce(unit);

	/* Enable RX/TX DMA */
	bcmgenet_enable_dma(unit);
	return GENET_OK;
}
