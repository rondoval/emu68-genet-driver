// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Amit Singh Tomar <amittomer25@gmail.com>
 *
 * Driver for Broadcom GENETv5 Ethernet controller (as found on the RPi4)
 * This driver is based on the Linux driver:
 *      drivers/net/ethernet/broadcom/genet/bcmgenet.c
 *      which is: Copyright (c) 2014-2017 Broadcom
 *
 * The hardware supports multiple queues (16 priority queues and one
 * default queue), both for RX and TX. There are 256 DMA descriptors (both
 * for TX and RX), and they live in MMIO registers. The hardware allows
 * assigning descriptor ranges to queues, but we choose the most simple setup:
 * All 256 descriptors are assigned to the default queue (#16).
 * Also the Linux driver supports multiple generations of the MAC, whereas
 * we only support v5, as used in the Raspberry Pi 4.
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

#include <cache_ops.h>
#include <debug.h>
#include <bits.h>
#include <errors.h>
#include <iomem.h>
#include <memory.h>
#include <timing.h>
#include <types.h>
#include <device.h>

#include <genet/phy.h>
#include <genet/unimac.h>
#include <genet/bcmgenet-regs.h>
#include <genet/bcmgenet-irq.h>

static void bcmgenet_umac_reset(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Resetting UMAC\n", __func__);

	mmio_set32(BCMGENET_REG(unit, SYS_RBUF_FLUSH_CTRL), BIT(1));
	delay_us(10);

	mmio_clear32(BCMGENET_REG(unit, SYS_RBUF_FLUSH_CTRL), BIT(1));
	delay_us(10);

	/* Reset UMAC */
	mmio_write32(0, BCMGENET_REG(unit, SYS_RBUF_FLUSH_CTRL));
	delay_us(10);

	mmio_write32(CMD_SW_RESET | CMD_LCL_LOOP_EN, BCMGENET_REG(unit, UMAC_CMD));
	delay_us(2);

	/* clear tx/rx counter */
	mmio_write32(MIB_RESET_RX | MIB_RESET_TX | MIB_RESET_RUNT, BCMGENET_REG(unit, UMAC_MIB_CTRL));
	mmio_write32(0, BCMGENET_REG(unit, UMAC_MIB_CTRL));

	mmio_write32(ENET_MAX_MTU_SIZE, BCMGENET_REG(unit, UMAC_MAX_FRAME_LEN));

	/* init rx registers, enable ip header optimization */
	u32 reg = mmio_read32(BCMGENET_REG(unit, RBUF_CTRL));
	reg |= RBUF_ALIGN_2B;
	// // RBUF_64B_EN would be set here, but we don't use Receive Status Block
	mmio_write32(reg, BCMGENET_REG(unit, RBUF_CTRL));

	mmio_write32(1, BCMGENET_REG(unit, RBUF_TBUF_SIZE_CTRL));

	bcmgenet_intr_disable(unit);

	// u32 int0_enable |= UMAC_IRQ_MDIO_EVENT;
	// bcmgenet_intrl2_0_writel(priv, int0_enable, INTRL2_CPU_MASK_CLEAR);
}

static void bcmgenet_gmac_write_hwaddr(struct GenetUnit *unit, const u8 *addr)
{
	Kprintf("[genet] %s: Setting MAC address to %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
			__func__, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	u32 reg;

	reg = ((u32)addr[0] << 24) | ((u32)addr[1] << 16) | ((u32)addr[2] << 8) | (u32)addr[3];
	mmio_write32(reg, BCMGENET_REG(unit, UMAC_MAC0));

	reg = ((u32)addr[4] << 8) | (u32)addr[5];
	mmio_write32(reg, BCMGENET_REG(unit, UMAC_MAC1));
}

static void bcmgenet_disable_dma(struct GenetUnit *unit)
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

	/* Wait 10ms for packet drain in both tx and rx dma */
	// TODO timer?
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

s32 bcmgenet_gmac_eth_rx(struct GenetUnit *unit, u16 budget)
{
	PERF_T0(t_drain);
	u32 rx_prod_reg = mmio_read32(BCMGENET_REG(unit, RDMA_PROD_INDEX));
	u16 discards = (u16)((rx_prod_reg >> DMA_P_INDEX_DISCARD_CNT_SHIFT) & DMA_P_INDEX_DISCARD_CNT_MASK);
	u16 rx_prod_index = (u16)(rx_prod_reg & DMA_P_INDEX_MASK);

	if (rx_prod_index == unit->rx_ring.rx_cons_index)
		return -EAGAIN;

	if (unlikely(discards > unit->rx_ring.old_discards))
	{
		u16 new_discards = (u16)(discards - unit->rx_ring.old_discards);
		unit->internalStats.rx_overruns += new_discards; // dropped packets?
		unit->rx_ring.old_discards = (u16)(unit->rx_ring.old_discards + new_discards);

		/* Clear HW register when we reach 75% of maximum 0xFFFF */
		if (unit->rx_ring.old_discards >= 0xC000)
		{
			unit->rx_ring.old_discards = 0;
			mmio_write32(0, BCMGENET_REG(unit, RDMA_PROD_INDEX));
		}
	}

	KprintfT("[genet] %s: rx_prod_index=%lu, rx_cons_index=%lu\n", __func__, (ULONG)rx_prod_index, (ULONG)unit->rx_ring.rx_cons_index);

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

		KprintfT("[genet] %s: packet=%08lx length=%lu\n", __func__, addr + RX_BUF_OFFSET, (ULONG)(length - RX_BUF_OFFSET));

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

		/* Invalidate only after the descriptor passed the sanity checks: the
		 * inline op drops whole lines with no end-of-range concession, so a
		 * corrupt length must never widen it — and rejected frames are never
		 * read, so they need no invalidate at all. */
		cache_post_dma(addr, length, 0);
		ReceiveFrame(unit, addr + RX_BUF_OFFSET, length - RX_BUF_OFFSET, dma_flags);
	next:
		rx_cons_index++;
	}

	unit->rx_ring.rx_cons_index = rx_cons_index;
	mmio_write32(rx_cons_index, BCMGENET_REG(unit, RDMA_CONS_INDEX));

	PERF_ADD(&unit->perf, GP_RX_DRAIN, t_drain);
	return to_process;
}

/* Periodic [genet] datapath perf report, ~2 s at the default 200 ms tick;
 * lines feed emu68-common/scripts/perf-report.py. */
#ifdef PROFILE
void bcmgenet_perf_tick(struct GenetUnit *unit)
{
	if (++unit->perfTicks >= 10)
	{
		unit->perfTicks = 0;
		perf_report(&unit->perf);
	}
}
#endif /* PROFILE */

static void bcmgenet_set_rx_coalesce(struct GenetUnit *unit, u32 usecs, u32 pkts)
{
	Kprintf("[genet] %s: Setting RX coalesce parameters: usecs=%lu, pkts=%lu\n", __func__, (ULONG)usecs, (ULONG)pkts);
	unit->rx_ring.rx_coalesce_usecs = usecs;
	unit->rx_ring.rx_max_coalesced_frames = pkts;

	mmio_write32(pkts, unit->genetBase + RDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH);

	u32 reg = mmio_read32(unit->genetBase + RDMA_REG_BASE + DMA_RING16_TIMEOUT);
	reg &= ~DMA_TIMEOUT_MASK;
	reg |= DIV_CEIL(usecs * 1000, 8192);
	mmio_write32(reg, unit->genetBase + RDMA_REG_BASE + DMA_RING16_TIMEOUT);
}

u32 bcmgenet_set_coalesce(struct GenetUnit *unit, u32 tx_max_coalesced_frames, u32 rx_max_coalesced_frames, u32 rx_coalesce_usecs)
{
	Kprintf("[genet] %s: Setting coalesce parameters: tx_max_coalesced_frames=%lu, rx_max_coalesced_frames=%lu, rx_coalesce_usecs=%lu\n",
			__func__, (ULONG)tx_max_coalesced_frames, (ULONG)rx_max_coalesced_frames, (ULONG)rx_coalesce_usecs);
	/* Base system clock is 125Mhz, DMA timeout is this reference clock
	 * divided by 1024, which yields roughly 8.192us, our maximum value
	 * has to fit in the DMA_TIMEOUT_MASK (16 bits)
	 */
	if (tx_max_coalesced_frames > DMA_INTR_THRESHOLD_MASK ||
		tx_max_coalesced_frames == 0 ||
		rx_max_coalesced_frames > DMA_INTR_THRESHOLD_MASK ||
		rx_coalesce_usecs > (DMA_TIMEOUT_MASK * 8) + 1)
		return S2ERR_BAD_ARGUMENT;

	if (rx_coalesce_usecs == 0 && rx_max_coalesced_frames == 0)
		return S2ERR_BAD_ARGUMENT;

	/* GENET TDMA hardware does not support a configurable timeout, but will
	 * always generate an interrupt either after MBDONE packets have been
	 * transmitted, or when the ring is empty.
	 */
	mmio_write32(tx_max_coalesced_frames, BCMGENET_REG(unit, TDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH));

	bcmgenet_set_rx_coalesce(unit, rx_coalesce_usecs, rx_max_coalesced_frames);

	return S2ERR_NO_ERROR;
}

static u32 bcmgenet_init_rx_ring(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Initializing RX ring\n", __func__);
	struct bcmgenet_rx_ring *ring = &unit->rx_ring;

	/* Initialize common Rx ring structures */
	const APTR desc_base = unit->genetBase + GENET_RX_OFF;
	ring->rx_control_block = pool_alloc(unit->metaPool, RX_DESCS * sizeof(struct enet_cb));
	if (!ring->rx_control_block)
	{
		return S2ERR_NO_RESOURCES;
	}

	memset(ring->rx_control_block, 0, RX_DESCS * sizeof(struct enet_cb));

	const u32 len_stat = (RX_BUF_LENGTH << DMA_BUFLENGTH_SHIFT); // | DMA_OWN;

	for (u32 i = 0; i < RX_DESCS; i++)
	{
		dma_addr_t buffer = unit->rxbuffer + (dma_addr_t)(i * RX_BUF_LENGTH);
		APTR descriptor_address = desc_base + i * DMA_DESC_SIZE;

		ring->rx_control_block[i].descriptor_address = descriptor_address;
		ring->rx_control_block[i].data_buffer = buffer;

		mmio_write32((dma_addr_t)buffer, descriptor_address + DMA_DESC_ADDRESS_LO);
		mmio_write32(len_stat, descriptor_address + DMA_DESC_LENGTH_STATUS);
	}

	bcmgenet_set_rx_coalesce(unit, unit->device->runtimeConfig.rx_coalesce_usecs, unit->device->runtimeConfig.rx_coalesce_frames);

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

	return S2ERR_NO_ERROR;
}

static u32 bcmgenet_init_rx_queues(struct GenetUnit *unit)
{
	u32 ret = bcmgenet_init_rx_ring(unit);
	if (ret != S2ERR_NO_ERROR)
	{
		return ret;
	}

	/* Configure Rx queues as descriptor rings */
	mmio_write32(1 << DEFAULT_Q, unit->genetBase + RDMA_REG_BASE + DMA_RING_CFG);

	/* Enable Rx rings */
	u32 dma_ctrl = 1 << (DEFAULT_Q + DMA_RING_BUF_EN_SHIFT);
	mmio_write32(dma_ctrl, unit->genetBase + RDMA_REG_BASE + DMA_CTRL);
	return S2ERR_NO_ERROR;
}

static u32 bcmgenet_init_tx_ring(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Initializing TX ring\n", __func__);
	struct bcmgenet_tx_ring *ring = &unit->tx_ring;

	/* Initialize common TX ring structures */
	APTR desc_base = unit->genetBase + GENET_TX_OFF;
	ring->tx_control_block = pool_alloc(unit->metaPool, TX_DESCS * sizeof(struct enet_cb));
	if (!ring->tx_control_block)
	{
		return S2ERR_NO_RESOURCES;
	}

	memset(ring->tx_control_block, 0, TX_DESCS * sizeof(struct enet_cb));
	for (u32 i = 0; i < TX_DESCS; i++)
	{
		ring->tx_control_block[i].descriptor_address = desc_base + i * DMA_DESC_SIZE;
	}

	slab_cache_init(&unit->tx_buffer_cache, unit->metaPool, unit->dmaPool,
	                RX_BUF_LENGTH, DMA_ALIGN_MIN, TX_DESCS);

	/* Cannot init TDMA_CONS_INDEX to 0, so align TDMA_PROD_INDEX on it instead */
	ring->tx_cons_index = mmio_read32(BCMGENET_REG(unit, TDMA_CONS_INDEX)) & DMA_C_INDEX_MASK;
	mmio_write32(ring->tx_cons_index, BCMGENET_REG(unit, TDMA_PROD_INDEX));
	ring->tx_prod_index = ring->tx_cons_index;
	ring->write_ptr = (u8)ring->tx_cons_index;
	ring->clean_ptr = (u8)ring->tx_cons_index;

	/* Default, can be overridden using coalesce settings */
	mmio_write32(unit->device->runtimeConfig.tx_coalesce_frames, BCMGENET_REG(unit, TDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH));

	/* Disable rate control for now */
	mmio_write32(0x0, BCMGENET_REG(unit, TDMA_FLOW_PERIOD));
	mmio_write32((TX_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH, BCMGENET_REG(unit, TDMA_RING_REG_BASE + DMA_RING_BUF_SIZE));

	/* Set start and end address, read and write pointers */
	mmio_write32(0x0, BCMGENET_REG(unit, TDMA_RING_REG_BASE + DMA_START_ADDR));
	mmio_write32(0x0, BCMGENET_REG(unit, TDMA_READ_PTR));
	mmio_write32(0x0, BCMGENET_REG(unit, TDMA_WRITE_PTR));
	mmio_write32(TX_DESCS * DMA_DESC_SIZE / 4 - 1, BCMGENET_REG(unit, TDMA_RING_REG_BASE + DMA_END_ADDR));

	return S2ERR_NO_ERROR;
}

static u32 bcmgenet_init_tx_queues(struct GenetUnit *unit)
{
	// We'll only setup queue 0

	/* Enable strict priority arbiter mode */
	mmio_write32(DMA_ARBITER_SP, unit->genetBase + TDMA_REG_BASE + DMA_ARB_CTRL);

	/* Initialize Tx priority queues */
	u32 ret = bcmgenet_init_tx_ring(unit);
	if (ret != S2ERR_NO_ERROR)
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
	return S2ERR_NO_ERROR;
}

static u32 bcmgenet_adjust_link(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Adjusting link for PHY interface %s\n", __func__, phy_string_for_interface(unit->phy_interface));
	struct phy_device *phy_dev = unit->phydev;
	u32 speed;

	switch (phy_dev->speed)
	{
	case SPEED_1000:
		speed = CMD_SPEED_1000;
		break;
	case SPEED_100:
		speed = CMD_SPEED_100;
		break;
	case SPEED_10:
		speed = CMD_SPEED_10;
		break;
	default:
		Kprintf("[genet] %s: Unsupported PHY speed: %ld\n", __func__, phy_dev->speed);
		return S2ERR_BAD_ARGUMENT;
	}

	mmio_update32(BCMGENET_REG(unit, EXT_RGMII_OOB_CTRL), OOB_DISABLE,
				  RGMII_LINK | RGMII_MODE_EN);

	if (phy_dev->interface == PHY_INTERFACE_MODE_RGMII || phy_dev->interface == PHY_INTERFACE_MODE_RGMII_RXID)
		mmio_set32(BCMGENET_REG(unit, EXT_RGMII_OOB_CTRL), ID_MODE_DIS);

	mmio_write32(speed << CMD_SPEED_SHIFT, BCMGENET_REG(unit, UMAC_CMD));

	return S2ERR_NO_ERROR;
}

#define MAX_MDF_FILTER 17

static inline void bcmgenet_set_mdf_addr(struct GenetUnit *unit, const u8 *addr, u8 *i)
{
	mmio_write32(((u32)addr[0] << 8) | (u32)addr[1], unit->genetBase + UMAC_MDF_ADDR + (*i * 4));
	mmio_write32(((u32)addr[2] << 24) | ((u32)addr[3] << 16) | ((u32)addr[4] << 8) | (u32)addr[5],
				 unit->genetBase + UMAC_MDF_ADDR + ((*i + 1) * 4));
	*i = (u8)(*i + 2U);
}

static u32 bcmgenet_init_dma(struct GenetUnit *unit)
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
	if (ret != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: Failed to initialize RX queues: %ld\n", __func__, ret);
		return ret;
	}

	/* Init tDma */
	mmio_write32(DMA_MAX_BURST_LENGTH, unit->genetBase + TDMA_REG_BASE + DMA_SCB_BURST_SIZE);
	ret = bcmgenet_init_tx_queues(unit);
	if (ret != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: Failed to initialize TX queues: %ld\n", __func__, ret);
		return ret;
	}

	/* Enable RX/TX DMA */
	bcmgenet_enable_dma(unit);
	return S2ERR_NO_ERROR;
}

void bcmgenet_set_rx_mode(struct GenetUnit *unit)
{
	/* Number of filters needed */
	u32 nfilter = 2 + unit->multicastCount; // 2 for broadcast and own address

	/*
	 * Turn on promicuous mode for two scenarios
	 * 1. SANA2OPF_PROM flag is set
	 * 2. The number of filters needed exceeds the number of filters supported by the hardware.
	 */
	u32 reg = mmio_read32(BCMGENET_REG(unit, UMAC_CMD));
	if ((unit->flags & SANA2OPF_PROM) || nfilter > MAX_MDF_FILTER)
	{
		Kprintf("[genet] %s: Enabling promiscuous mode, nfilter=%ld\n", __func__, nfilter);
		reg |= CMD_PROMISC;
		mmio_write32(reg, BCMGENET_REG(unit, UMAC_CMD));
		mmio_write32(0, BCMGENET_REG(unit, UMAC_MDF_CTRL));

		unit->mdfEnabled = FALSE;
		return;
	}

	Kprintf("[genet] %s: Setting RX mode, nfilter=%ld\n", __func__, nfilter);

	reg &= ~CMD_PROMISC;
	mmio_write32(reg, BCMGENET_REG(unit, UMAC_CMD));

	/* update MDF filter */
	u8 i = 0;
	/* Broadcast */
	static const u8 broadcast[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	bcmgenet_set_mdf_addr(unit, broadcast, &i);
	/* my own address.*/
	bcmgenet_set_mdf_addr(unit, unit->currentMacAddress, &i);

	/* Multicast */
	/* Go through registered multicast ranges. Add each address to MDF */
	for (struct MinNode *node = unit->multicastRanges.mlh_Head; node->mln_Succ; node = node->mln_Succ)
	{
		struct MulticastRange *range = (struct MulticastRange *)node;

		for (u64 addr = range->lowerBound; addr <= range->upperBound; addr++)
		{
			union
			{
				u64 u64;
				u8 u8[8];
			} u;

			u.u64 = addr;
			bcmgenet_set_mdf_addr(unit, u.u8 + 2, &i);
		}
	}

	/* Enable filters */
	reg = GENMASK(MAX_MDF_FILTER - 1, MAX_MDF_FILTER - nfilter);
	mmio_write32(reg, BCMGENET_REG(unit, UMAC_MDF_CTRL));

	unit->mdfEnabled = TRUE;
}

u32 bcmgenet_gmac_eth_start(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Starting GENET\n", __func__);
	u32 ret;

	unit->rxbuffer = (dma_addr_t)dma_zalloc(unit->dmaPool, DMA_ALIGN_MIN, RX_TOTAL_BUFSIZE);
	if (!unit->rxbuffer)
	{
		Kprintf("[genet] %s: Failed to allocate RX buffer\n", __func__);
		ret = S2ERR_NO_RESOURCES;
		goto rx_buf_allocated;
	}

	cache_pre_dma((APTR)unit->rxbuffer, RX_TOTAL_BUFSIZE, 0);

	bcmgenet_umac_reset(unit);

	bcmgenet_gmac_write_hwaddr(unit, unit->currentMacAddress);

	// bcmgenet_hfb_init()

	ret = bcmgenet_init_dma(unit);
	if (ret != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: Failed to initialize DMA: %ld\n", __func__, ret);
		goto init_dma;
	}

	unit->irq0_isr.is_Node.ln_Type = NT_INTERRUPT;
	unit->irq0_isr.is_Node.ln_Name = "bcmgenet_isr0";
	unit->irq0_isr.is_Data = (APTR)unit;
	unit->irq0_isr.is_Code = (APTR)bcmgenet_isr0;

	s32 irq_result = AddIntServerEx(unit->irq0_number, 0, FALSE, &unit->irq0_isr);
	if (irq_result < 0)
	{
		Kprintf("[genet] %s: can't register IRQ %ld\n", __func__, unit->irq0_number);
		ret = S2ERR_SOFTWARE;
		goto init_dma;
	}
	Kprintf("[genet] %s: Interrupt server for IRQ %ld registered\n", __func__, unit->irq0_number);

	// bcmgenet_mii_probe(unit);
	//  rx_pause=1, tx_pause=1
	// bcmgenet_phy_pause_set(unit, unit->rx_pause, unit->tx_pause);
	bcmgenet_set_rx_mode(unit);
	//  enable rx/tx
	// phy_start()
	s32 phy_result = phy_startup(unit->phydev);
	if (phy_result < 0)
	{
		Kprintf("[genet] %s: PHY startup failed: %ld\n", __func__, phy_result);
		ret = S2ERR_SOFTWARE;
		goto err_irq;
	}

	/* Update MAC registers based on PHY property */
	ret = bcmgenet_adjust_link(unit);
	if (ret != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: adjust PHY link failed: %ld\n", __func__, ret);
		goto err_irq;
	}

	/* Monitor link interrupts now.
	 *
	 * TXDMA_DONE is intentionally NOT enabled: the SANA-II stack 
	 * is too slow to keep up with the DMA, and we would get an interrupt for every packet, which
	 * would cause excessive CPU overhead.
	 * Coalescing cannot batch anything and a per-completion interrupt is
	 * pure overhead. We reclaim TX descriptors at the top of bcmgenet_xmit
	 * instead.
	 */
	bcmgenet_irq0_enable(unit, UMAC_IRQ_LINK_EVENT | UMAC_IRQ_PHY_DET_R);
	bcmgenet_irq0_enable(unit, UMAC_IRQ_RXDMA_DONE /* | UMAC_IRQ_TXDMA_DONE */);
	KprintfT("[genet] %s: Enabled link and RX DMA interrupts (TX reclaimed in xmit/watchdog)\n", __func__);

	/* Enable Rx/Tx */
	mmio_set32(BCMGENET_REG(unit, UMAC_CMD), CMD_TX_EN | CMD_RX_EN);
	Kprintf("[genet] %s: UMAC started, RX/TX enabled\n", __func__);

	return S2ERR_NO_ERROR;

err_irq:
	RemIntServerEx(unit->irq0_number, &unit->irq0_isr);

init_dma:
	slab_cache_destroy(&unit->tx_buffer_cache);

rx_buf_allocated:
	dma_free(unit->dmaPool, (APTR)unit->rxbuffer);
	unit->rxbuffer = 0;

	return ret;
}

static u32 bcmgenet_phy_init(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Initializing PHY interface %s\n", __func__, phy_string_for_interface(unit->phy_interface));
	struct phy_device *phydev;

	phydev = phy_create(unit, unit->phy_interface);
	if (!phydev)
		return S2ERR_SOFTWARE;

	phydev->supported &= PHY_GBIT_FEATURES;
	phydev->advertising = phydev->supported;

	unit->phydev = phydev;
	s32 result = phy_config(phydev);
	if (result < 0)
	{
		Kprintf("[genet] %s: PHY config failed: %ld\n", __func__, result);
		phy_destroy(phydev);
		unit->phydev = NULL;
		return S2ERR_SOFTWARE;
	}

	return S2ERR_NO_ERROR;
}

/* We only support RGMII (as used on the RPi4). */
static u32 bcmgenet_interface_set(struct GenetUnit *unit)
{
	Kprintf("[genet] %s: Setting PHY interface %s\n", __func__, phy_string_for_interface(unit->phy_interface));
	switch (unit->phy_interface)
	{
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_RXID:
		KprintfT("[genet] %s: Setting PHY mode to RGMII\n", __func__);
		mmio_write32(PORT_MODE_EXT_GPHY, BCMGENET_REG(unit, SYS_PORT_CTRL));
		break;
	default:
		Kprintf("[genet] %s: unknown phy mode: %ld\n", __func__, unit->phy_interface);
		return S2ERR_BAD_ARGUMENT;
	}

	return S2ERR_NO_ERROR;
}

u32 bcmgenet_eth_probe(struct GenetUnit *unit)
{
	/* Read GENET HW version */
	u32 reg = mmio_read32(BCMGENET_REG(unit, SYS_REV_CTRL));
	u8 major = (reg >> 24) & 0x0f;
	if (major == 6 || major == 7)
		major = 5;
	else if (major == 5)
		major = 4;
	else if (major == 0)
		major = 1;

	if (major != 5)
	{
		Kprintf("[genet] %s: Unsupported GENET v%ld.%ld\n", __func__, major, (reg >> 16) & 0x0f);
		return S2ERR_SOFTWARE;
	}
	Kprintf("[genet] %s: GENET v%ld.%ld\n", __func__, major, (reg >> 16) & 0x0f);

	u32 ret = bcmgenet_interface_set(unit);
	if (ret != S2ERR_NO_ERROR)
		return ret;

	mmio_write32(0, BCMGENET_REG(unit, SYS_RBUF_FLUSH_CTRL));
	delay_us(10);
	/* issue soft reset with (rg)mii loopback to ensure a stable rxclk */
	mmio_write32(CMD_SW_RESET | CMD_LCL_LOOP_EN, BCMGENET_REG(unit, UMAC_CMD));
	delay_us(2);

	// bcmgenet_mii_init()
	return bcmgenet_phy_init(unit);
}

/* Stop all bus-master activity (RX/TX DMA, MAC, interrupts) without
 * releasing any resources.  Also the pre-reset quiesce: the RX ring keeps
 * receiving into RAM the next OS session reuses unless this runs before
 * the machine resets. */
void bcmgenet_reset_quiesce(struct GenetUnit *unit)
{
	/* Disable MAC receive */
	mmio_clear32(BCMGENET_REG(unit, UMAC_CMD), CMD_RX_EN);
	delay_ms(1);
	bcmgenet_disable_dma(unit);
	/* Disable MAC transmit. TX DMA disabled must be done before this */
	mmio_clear32(BCMGENET_REG(unit, UMAC_CMD), CMD_TX_EN);
	delay_ms(1);

	bcmgenet_intr_disable(unit);
}

void bcmgenet_gmac_eth_stop(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Stopping GENET\n", __func__);

	bcmgenet_reset_quiesce(unit);
	RemIntServerEx(unit->irq0_number, &unit->irq0_isr);

	/* tx reclaim */
	bcmgenet_tx_reclaim(unit, TX_DESCS);
	// /* Really kill the PHY state machine and disconnect from it */
	// phy_disconnect(dev->phydev);

	if (unit->rxbuffer)
	{
		dma_free(unit->dmaPool, (APTR)unit->rxbuffer);
		unit->rxbuffer = 0;
	}
	slab_cache_destroy(&unit->tx_buffer_cache);

	if (unit->phydev)
	{
		phy_destroy(unit->phydev);
		unit->phydev = NULL;
	}
	KprintfT("[genet] %s: PHY destroyed. GENET stopped.\n", __func__);
}
