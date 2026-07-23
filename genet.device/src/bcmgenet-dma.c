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

void bcmgenet_disable_dma(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Disabling DMA\n", __func__);
	mmio_clear32(BCMGENET_REG(unit, TDMA_REG_BASE + DMA_CTRL), DMA_EN);
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

	mmio_clear32(BCMGENET_REG(unit, RDMA_REG_BASE + DMA_CTRL), DMA_EN);
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
 * The one place interrupt moderation is programmed, in both directions: from
 * bcmgenet_init_dma() once the rings exist, and again whenever
 * NETDEV_CMD_SET_COALESCE lands on a running unit. The settings live on the
 * unit, so both callers program the same thing and neither has to be told
 * what it is.
 */
void bcmgenet_apply_coalesce(struct GenetUnit *unit)
{
	Kprintf("[genet] %s: tx_frames=%lu rx_frames=%lu rx_usecs=%lu\n", __func__,
			(ULONG)unit->coalTxFrames, (ULONG)unit->coalRxFrames, (ULONG)unit->coalRxUsecs);

	/* TDMA has no configurable timeout: it interrupts after this many buffers
	 * have been transmitted, or when the ring drains. */
	mmio_write32(unit->coalTxFrames,
				 BCMGENET_REG(unit, TDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH));

	/* RDMA has both. The timeout register counts the 125MHz reference divided
	 * by 1024, so ~8.192us per tick. */
	mmio_write32(unit->coalRxFrames,
				 BCMGENET_REG(unit, RDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH));

	u32 reg = mmio_read32(BCMGENET_REG(unit, RDMA_REG_BASE + DMA_RING16_TIMEOUT));
	reg &= ~DMA_TIMEOUT_MASK;
	reg |= DIV_CEIL(unit->coalRxUsecs * 1000, 8192);
	mmio_write32(reg, BCMGENET_REG(unit, RDMA_REG_BASE + DMA_RING16_TIMEOUT));
}

static u32 bcmgenet_init_tx_ring(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Initializing TX ring\n", __func__);
	struct bcmgenet_tx_ring *ring = &unit->tx_ring;

	/* No control-block array: TX descriptors are addressed straight from the
	 * BD index, and cookies live in the ndTxDone FIFO, not beside the ring. */

	/* Cannot init TDMA_CONS_INDEX to 0, so align TDMA_PROD_INDEX on it instead */
	ring->hw_cons_cache = (u16)(mmio_read32(BCMGENET_REG(unit, TDMA_CONS_INDEX)) & DMA_C_INDEX_MASK);
	mmio_write32(ring->hw_cons_cache, BCMGENET_REG(unit, TDMA_PROD_INDEX));
	ring->tx_prod_index = ring->hw_cons_cache;

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
