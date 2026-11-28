// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom GENET (Gigabit Ethernet) controller driver — TX BD ring access,
 * shared by every TX path.
 *
 * Copyright (c) 2014-2025 Broadcom
 *
 * The ring is producer-owned in full (see struct bcmgenet_tx_ring): these
 * helpers run in the producer's context only.
 */
#ifndef _BCMGENET_TX_H
#define _BCMGENET_TX_H

#include <iomem.h>
#include <types.h>
#include <device.h>
#include <genet/bcmgenet.h>
#include <genet/bcmgenet-regs.h>

/* Combined address + length/status setter. Relaxed stores (iomem.h):
 * nothing reads a BD back, and the one emu68_barrier() before the doorbell
 * (bcmgenet_netdev_tx_kick) orders every BD of the burst - together with the
 * DMAF_NoSync cache cleans - ahead of it. */
static inline void dmadesc_set(APTR descriptor_address, dma_addr_t addr, u32 val)
{
	mmio_write32_relaxed((u32)addr, descriptor_address + DMA_DESC_ADDRESS_LO);
	mmio_write32_relaxed(val, descriptor_address + DMA_DESC_LENGTH_STATUS);
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

/* Free BDs as the producer sees them. The cached hardware consumer index can
 * only under-report, so it is re-read once the cached count falls below
 * TX_CONS_REFRESH_BDS - every ~240 frames at line rate - and then *refreshed
 * is set: space came straight off the hardware, which no TX-done interrupt
 * may have announced. */
static inline u16 tx_free_bds_refresh(const struct GenetUnit *unit, struct bcmgenet_tx_ring *ring,
									  BOOL *refreshed)
{
	u16 free_bds = tx_free_bds(ring);
	if (unlikely(free_bds < TX_CONS_REFRESH_BDS))
	{
		ring->hw_cons_cache =
			(u16)(mmio_read32(BCMGENET_REG(unit, TDMA_CONS_INDEX)) & DMA_C_INDEX_MASK);
		free_bds = tx_free_bds(ring);
		*refreshed = TRUE;
	}
	return free_bds;
}

#endif /* _BCMGENET_TX_H */
