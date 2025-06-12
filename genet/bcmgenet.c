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

#define __NOLIBBASE__

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <exec/types.h>
#include <limits.h>

#include <debug.h>
#include <phy/phy.h>
#include <device.h>
#include <compat.h>
#include <unimac.h>
#include <bcmgenet-regs.h>

static void bcmgenet_umac_reset(struct GenetUnit *priv)
{
	Kprintf("[genet] %s: Resetting UMAC\n", __func__);

	setbits_32((APTR)((ULONG)priv->genetBase + SYS_RBUF_FLUSH_CTRL), BIT(1));
	delay_us(10);

	clrbits_32((APTR)((ULONG)priv->genetBase + SYS_RBUF_FLUSH_CTRL), BIT(1));
	delay_us(10);

	/* Reset UMAC */
	writel(0, (ULONG)priv->genetBase + SYS_RBUF_FLUSH_CTRL);
	delay_us(10);

	writel(CMD_SW_RESET | CMD_LCL_LOOP_EN, (ULONG)priv->genetBase + UMAC_CMD);
	delay_us(2);

	/* clear tx/rx counter */
	writel(MIB_RESET_RX | MIB_RESET_TX | MIB_RESET_RUNT, (ULONG)priv->genetBase + UMAC_MIB_CTRL);
	writel(0, (ULONG)priv->genetBase + UMAC_MIB_CTRL);

	writel(ENET_MAX_MTU_SIZE, (ULONG)priv->genetBase + UMAC_MAX_FRAME_LEN);

	/* init rx registers, enable ip header optimization */
	// reg = readl((ULONG)priv->genetBase + RBUF_CTRL);
	// reg |= RBUF_ALIGN_2B;
	// // RBUF_64B_EN would be set here, but we don't use Receive Status Block
	// writel(reg, ((ULONG)priv->genetBase + RBUF_CTRL));

	writel(1, ((ULONG)priv->genetBase + RBUF_TBUF_SIZE_CTRL));
}

static void bcmgenet_gmac_write_hwaddr(struct GenetUnit *priv, const UBYTE *addr)
{
	Kprintf("[genet] %s: Setting MAC address to %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
			__func__, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	ULONG reg;

	reg = addr[0] << 24 | addr[1] << 16 | addr[2] << 8 | addr[3];
	writel(reg, (APTR)((ULONG)priv->genetBase + UMAC_MAC0));

	reg = addr[4] << 8 | addr[5];
	writel(reg, (APTR)((ULONG)priv->genetBase + UMAC_MAC1));
}

static void bcmgenet_disable_dma(struct GenetUnit *priv)
{
	Kprintf("[genet] %s: Disabling DMA\n", __func__);
	clrbits_32((APTR)((ULONG)priv->genetBase + TDMA_REG_BASE + DMA_CTRL), DMA_EN);
	for (int timeout = 0; timeout < DMA_TIMEOUT_VAL; timeout++)
	{
		ULONG tdma = readl(priv->genetBase + TDMA_REG_BASE + DMA_CTRL);
		if (!(tdma & DMA_EN))
		{
			break;
		}
		delay_us(1);
	}

	/* Wait 10ms for packet drain in both tx and rx dma */
	// TODO timer?
	delay_us(10000);

	clrbits_32((APTR)((ULONG)priv->genetBase + RDMA_REG_BASE + DMA_CTRL), DMA_EN);
	for (int timeout = 0; timeout < DMA_TIMEOUT_VAL; timeout++)
	{
		ULONG rdma = readl(priv->genetBase + RDMA_REG_BASE + DMA_CTRL);
		if (!(rdma & DMA_EN))
		{
			break;
		}
		delay_us(1);
	}
	Kprintf("[genet] %s: DMA disabled\n", __func__);

	/* Flush TX queues */
	writel(1, (APTR)((ULONG)priv->genetBase + UMAC_TX_FLUSH));
	delay_us(10);
	writel(0, (APTR)((ULONG)priv->genetBase + UMAC_TX_FLUSH));
}

static void bcmgenet_enable_dma(struct GenetUnit *priv)
{
	Kprintf("[genet] %s: Enabling DMA\n", __func__);
	setbits_32((APTR)((ULONG)priv->genetBase + RDMA_REG_BASE + DMA_CTRL), DMA_EN);
	setbits_32((APTR)((ULONG)priv->genetBase + TDMA_REG_BASE + DMA_CTRL), DMA_EN);
}

int bcmgenet_gmac_eth_recv(struct GenetUnit *priv, int flags, UBYTE **packetp)
{
	struct ExecBase *SysBase = priv->execBase;
	UWORD rx_prod_index = readl((ULONG)priv->genetBase + RDMA_PROD_INDEX) & DMA_P_INDEX_MASK;

	if (rx_prod_index == priv->rx_ring.rx_cons_index)
		return EAGAIN;

	KprintfH("[genet] %s: rx_prod_index=%ld, rx_cons_index=%ld\n", __func__, rx_prod_index, priv->rx_ring.rx_cons_index);

	struct enet_cb *rx_cb = &priv->rx_ring.rx_control_block[priv->rx_ring.rx_cons_index & 0xff];
	APTR desc_base = rx_cb->descriptor_address;
	ULONG length = readl((ULONG)desc_base + DMA_DESC_LENGTH_STATUS);
	length = (length >> DMA_BUFLENGTH_SHIFT) & DMA_BUFLENGTH_MASK;
	APTR addr = rx_cb->internal_buffer;
	// APTR addr = (APTR)readl((ULONG)desc_base + DMA_DESC_ADDRESS_LO);

	CachePostDMA(addr, &length, 0);

	*packetp = (UBYTE *)addr;
	KprintfH("[genet] %s: packet=%08lx length=%ld\n", __func__, *packetp, length);

	return length;
}

void bcmgenet_gmac_free_pkt(struct GenetUnit *priv, UBYTE *packet, ULONG length)
{
	KprintfH("[genet] %s: packet=%08lx length=%ld\n", __func__, packet, length);

	/* Tell the MAC we have consumed that last receive buffer. */
	priv->rx_ring.rx_cons_index = (priv->rx_ring.rx_cons_index + 1) & DMA_C_INDEX_MASK;
	writel(priv->rx_ring.rx_cons_index, (ULONG)priv->genetBase + RDMA_CONS_INDEX);
}

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

static void bcmgenet_set_rx_coalesce(struct GenetUnit *unit, ULONG usecs, ULONG pkts)
{
	Kprintf("[genet] %s: Setting RX coalesce parameters: usecs=%ld, pkts=%ld\n", __func__, usecs, pkts);
	unit->rx_ring.rx_coalesce_usecs = usecs;
	unit->rx_ring.rx_max_coalesced_frames = pkts;

	writel(pkts, unit->genetBase + RDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH);

	ULONG reg = readl(unit->genetBase + RDMA_REG_BASE + DMA_RING16_TIMEOUT);
	reg &= ~DMA_TIMEOUT_MASK;
	reg |= DIV_ROUND_UP(usecs * 1000, 8192);
	writel(reg, unit->genetBase + RDMA_REG_BASE + DMA_RING16_TIMEOUT);
}

int bcmgenet_set_coalesce(struct GenetUnit *priv, ULONG tx_max_coalesced_frames, ULONG rx_max_coalesced_frames, ULONG rx_coalesce_usecs)
{
	Kprintf("[genet] %s: Setting coalesce parameters: tx_max_coalesced_frames=%ld, rx_max_coalesced_frames=%ld, rx_coalesce_usecs=%ld\n",
			__func__, tx_max_coalesced_frames, rx_max_coalesced_frames, rx_coalesce_usecs);
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

	/* Program all TX queues with the same values, as there is no
	 * ethtool knob to do coalescing on a per-queue basis
	 */
	writel(tx_max_coalesced_frames, (ULONG)priv->genetBase + TDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH);

	bcmgenet_set_rx_coalesce(priv, rx_coalesce_usecs, rx_max_coalesced_frames);

	return S2ERR_NO_ERROR;
}

static int bcmgenet_init_rx_ring(struct GenetUnit *priv)
{
	struct ExecBase *SysBase = priv->execBase;
	Kprintf("[genet] %s: Initializing RX ring\n", __func__);
	struct bcmgenet_rx_ring *ring = &priv->rx_ring;

	/* Initialize common Rx ring structures */
	const APTR desc_base = priv->genetBase + GENET_RX_OFF;
	ring->rx_control_block = AllocPooled(priv->memoryPool, RX_DESCS * sizeof(struct enet_cb));
	if (!ring->rx_control_block)
	{
		return S2ERR_NO_RESOURCES;
	}

	_memset(ring->rx_control_block, 0, RX_DESCS * sizeof(struct enet_cb));

	const ULONG len_stat = (RX_BUF_LENGTH << DMA_BUFLENGTH_SHIFT) | DMA_OWN;

	for (ULONG i = 0; i < RX_DESCS; i++)
	{
		APTR buffer = &priv->rxbuffer[i * RX_BUF_LENGTH];
		APTR descriptor_address = desc_base + i * DMA_DESC_SIZE;

		ring->rx_control_block[i].descriptor_address = descriptor_address;
		ring->rx_control_block[i].internal_buffer = buffer;

		//  TODO this is temporary until RX ring is refactored
		// translate to VC address space?
		writel((ULONG)buffer, descriptor_address + DMA_DESC_ADDRESS_LO);
		writel(len_stat, descriptor_address + DMA_DESC_LENGTH_STATUS);
	}

	bcmgenet_set_rx_coalesce(priv, 50, 1);

	/* cannot init RDMA_PROD_INDEX to 0, so align RDMA_CONS_INDEX on it instead */
	ring->rx_cons_index = readl((ULONG)priv->genetBase + RDMA_PROD_INDEX) & DMA_P_INDEX_MASK;
	writel(ring->rx_cons_index, (ULONG)priv->genetBase + RDMA_CONS_INDEX);
	Kprintf("[genet] %s: rx_cons_index=%ld\n", __func__, priv->rx_ring.rx_cons_index);
	ring->read_ptr = ring->rx_cons_index;

	writel((RX_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH, priv->genetBase + RDMA_RING_REG_BASE + DMA_RING_BUF_SIZE);
	writel((DMA_FC_THRESH_LO << DMA_XOFF_THRESHOLD_SHIFT) | DMA_FC_THRESH_HI, priv->genetBase + RDMA_XON_XOFF_THRESH);

	/* Set start and end address, read and write pointers */
	writel(0x0, priv->genetBase + RDMA_RING_REG_BASE + DMA_START_ADDR);
	writel(0x0, priv->genetBase + RDMA_READ_PTR);
	writel(0x0, priv->genetBase + RDMA_WRITE_PTR);
	writel(RX_DESCS * DMA_DESC_SIZE / 4 - 1, priv->genetBase + RDMA_RING_REG_BASE + DMA_END_ADDR);

	return S2ERR_NO_ERROR;
}

static int bcmgenet_init_rx_queues(struct GenetUnit *priv)
{
	int ret = bcmgenet_init_rx_ring(priv);
	if (ret != S2ERR_NO_ERROR)
	{
		return ret;
	}

	/* Configure Rx queues as descriptor rings */
	writel(1 << DEFAULT_Q, priv->genetBase + RDMA_REG_BASE + DMA_RING_CFG);

	/* Enable Rx rings */
	ULONG dma_ctrl = 1 << (DEFAULT_Q + DMA_RING_BUF_EN_SHIFT);
	writel(dma_ctrl, priv->genetBase + RDMA_REG_BASE + DMA_CTRL);
	return S2ERR_NO_ERROR;
}

static int bcmgenet_init_tx_ring(struct GenetUnit *priv)
{
	struct ExecBase *SysBase = priv->execBase;
	Kprintf("[genet] %s: Initializing TX ring\n", __func__);
	struct bcmgenet_tx_ring *ring = &priv->tx_ring;

	/* Initialize common TX ring structures */
	APTR desc_base = priv->genetBase + GENET_TX_OFF;
	ring->tx_control_block = AllocPooled(priv->memoryPool, TX_DESCS * sizeof(struct enet_cb));
	if (!ring->tx_control_block)
	{
		return S2ERR_NO_RESOURCES;
	}

	_memset(ring->tx_control_block, 0, TX_DESCS * sizeof(struct enet_cb));
	for (ULONG i = 0; i < TX_DESCS; i++)
	{
		ring->tx_control_block[i].descriptor_address = desc_base + i * DMA_DESC_SIZE;
		ring->tx_control_block[i].internal_buffer = &priv->txbuffer[i * RX_BUF_LENGTH];
	}

	ring->free_bds = TX_DESCS;

	/* Cannot init TDMA_CONS_INDEX to 0, so align TDMA_PROD_INDEX on it instead */
	ring->tx_cons_index = readl((ULONG)priv->genetBase + TDMA_CONS_INDEX) & DMA_C_INDEX_MASK;
	writel(ring->tx_cons_index, (ULONG)priv->genetBase + TDMA_PROD_INDEX);
	ring->tx_prod_index = ring->tx_cons_index;
	ring->write_ptr = ring->tx_cons_index;
	ring->clean_ptr = ring->tx_cons_index;

	/* Default, can be overridden using coalesce settings */
	writel(10, (ULONG)priv->genetBase + TDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH);

	/* Disable rate control for now */
	writel(0x0, (ULONG)priv->genetBase + TDMA_FLOW_PERIOD);
	writel((TX_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH, (ULONG)priv->genetBase + TDMA_RING_REG_BASE + DMA_RING_BUF_SIZE);

	/* Set start and end address, read and write pointers */
	writel(0x0, (ULONG)priv->genetBase + TDMA_RING_REG_BASE + DMA_START_ADDR);
	writel(0x0, (ULONG)priv->genetBase + TDMA_READ_PTR);
	writel(0x0, (ULONG)priv->genetBase + TDMA_WRITE_PTR);
	writel(TX_DESCS * DMA_DESC_SIZE / 4 - 1, (ULONG)priv->genetBase + TDMA_RING_REG_BASE + DMA_END_ADDR);

	return S2ERR_NO_ERROR;
}

static int bcmgenet_init_tx_queues(struct GenetUnit *priv)
{
	// We'll only setup queue 0

	/* Enable strict priority arbiter mode */
	writel(DMA_ARBITER_SP, priv->genetBase + TDMA_REG_BASE + DMA_ARB_CTRL);

	/* Initialize Tx priority queues */
	int ret = bcmgenet_init_tx_ring(priv);
	if (ret != S2ERR_NO_ERROR)
	{
		return ret;
	}

	/* Set Tx queue priorities */
	writel(0, priv->genetBase + TDMA_REG_BASE + DMA_PRIORITY_0);
	writel(0, priv->genetBase + TDMA_REG_BASE + DMA_PRIORITY_1);
	writel(0, priv->genetBase + TDMA_REG_BASE + DMA_PRIORITY_2);

	/* Configure Tx queues as descriptor rings */
	writel(1 << DEFAULT_Q, (ULONG)priv->genetBase + TDMA_REG_BASE + DMA_RING_CFG);

	/* Enable Tx rings */
	ULONG dma_ctrl = 1 << (DEFAULT_Q + DMA_RING_BUF_EN_SHIFT);
	writel(dma_ctrl, priv->genetBase + TDMA_REG_BASE + DMA_CTRL);
	return S2ERR_NO_ERROR;
}

static int bcmgenet_adjust_link(struct GenetUnit *priv)
{
	Kprintf("[genet] %s: Adjusting link for PHY interface %s\n", __func__, phy_string_for_interface(priv->phy_interface));
	struct phy_device *phy_dev = priv->phydev;
	ULONG speed;

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
		Kprintf("[genet] %s: Unsupported PHY speed: %d\n", __func__, phy_dev->speed);
		return S2ERR_BAD_ARGUMENT;
	}

	clrsetbits_32((APTR)((ULONG)priv->genetBase + EXT_RGMII_OOB_CTRL), OOB_DISABLE,
				  RGMII_LINK | RGMII_MODE_EN);

	if (phy_dev->interface == PHY_INTERFACE_MODE_RGMII || phy_dev->interface == PHY_INTERFACE_MODE_RGMII_RXID)
		setbits_32((APTR)((ULONG)priv->genetBase + EXT_RGMII_OOB_CTRL), ID_MODE_DIS);

	writel(speed << CMD_SPEED_SHIFT, ((ULONG)priv->genetBase + UMAC_CMD));

	return S2ERR_NO_ERROR;
}

#define MAX_MDF_FILTER 17

static inline void bcmgenet_set_mdf_addr(struct GenetUnit *priv, const unsigned char *addr, int *i)
{
	writel(addr[0] << 8 | addr[1], priv->genetBase + UMAC_MDF_ADDR + (*i * 4));
	writel(addr[2] << 24 | addr[3] << 16 | addr[4] << 8 | addr[5], priv->genetBase + UMAC_MDF_ADDR + ((*i + 1) * 4));
	*i += 2;
}

static int bcmgenet_init_dma(struct GenetUnit *priv)
{
	/* Disable RX/TX DMA and flush TX queues */
	bcmgenet_disable_dma(priv);

	Kprintf("[genet] %s: Initializing DMA\n", __func__);

	/* Flush RX */
	setbits_32((APTR)((ULONG)priv->genetBase + GENET_SYS_OFF + SYS_RBUF_FLUSH_CTRL), BIT(0));
	delay_us(10);
	clrbits_32((APTR)((ULONG)priv->genetBase + GENET_SYS_OFF + SYS_RBUF_FLUSH_CTRL), BIT(0));
	delay_us(10);

	/* Init rDma */
	writel(DMA_MAX_BURST_LENGTH, priv->genetBase + RDMA_REG_BASE + DMA_SCB_BURST_SIZE);

	/* Initialize Rx queues */
	int ret = bcmgenet_init_rx_queues(priv);
	if (ret != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: Failed to initialize RX queues: %ld\n", __func__, ret);
		return ret;
	}

	/* Init tDma */
	writel(DMA_MAX_BURST_LENGTH, priv->genetBase + TDMA_REG_BASE + DMA_SCB_BURST_SIZE);
	ret = bcmgenet_init_tx_queues(priv);
	if (ret != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: Failed to initialize TX queues: %ld\n", __func__, ret);
		return ret;
	}

	/* Enable RX/TX DMA */
	bcmgenet_enable_dma(priv);
	return S2ERR_NO_ERROR;
}

void bcmgenet_set_rx_mode(struct GenetUnit *priv)
{
	/* Number of filters needed */
	int nfilter = 2 + priv->multicastCount; // 2 for broadcast and own address

	/*
	 * Turn on promicuous mode for two scenarios
	 * 1. SANA2OPF_PROM flag is set
	 * 2. The number of filters needed exceeds the number of filters supported by the hardware.
	 */
	ULONG reg = readl((ULONG)priv->genetBase + UMAC_CMD);
	if ((priv->flags & SANA2OPF_PROM) || nfilter > MAX_MDF_FILTER)
	{
		Kprintf("[genet] %s: Enabling promiscuous mode, nfilter=%ld\n", __func__, nfilter);
		reg |= CMD_PROMISC;
		writel(reg, (ULONG)priv->genetBase + UMAC_CMD);
		writel(0, (ULONG)priv->genetBase + UMAC_MDF_CTRL);

		priv->mdfEnabled = FALSE;
		return;
	}

	Kprintf("[genet] %s: Setting RX mode, nfilter=%ld\n", __func__, nfilter);

	reg &= ~CMD_PROMISC;
	writel(reg, (ULONG)priv->genetBase + UMAC_CMD);

	/* update MDF filter */
	int i = 0;
	/* Broadcast */
	const UBYTE broadcast[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	bcmgenet_set_mdf_addr(priv, broadcast, &i);
	/* my own address.*/
	bcmgenet_set_mdf_addr(priv, priv->currentMacAddress, &i);

	/* Multicast */
	/* Go through registered multicast ranges. Add each address to MDF */
	for (struct MinNode *node = priv->multicastRanges.mlh_Head; node->mln_Succ; node = node->mln_Succ)
	{
		struct MulticastRange *range = (struct MulticastRange *)node;

		for (uint64_t addr = range->lowerBound; addr <= range->upperBound; addr++)
		{
			union
			{
				uint64_t u64;
				UBYTE u8[8];
			} u;

			u.u64 = addr;
			bcmgenet_set_mdf_addr(priv, u.u8 + 2, &i);
		}
	}

	/* Enable filters */
	reg = GENMASK(MAX_MDF_FILTER - 1, MAX_MDF_FILTER - nfilter);
	writel(reg, (ULONG)priv->genetBase + UMAC_MDF_CTRL);

	priv->mdfEnabled = TRUE;
}

int bcmgenet_gmac_eth_start(struct GenetUnit *priv)
{
	struct ExecBase *SysBase = priv->execBase;
	Kprintf("[genet] %s: Starting GENET\n", __func__);
	priv->rxbuffer_not_aligned = AllocMem(RX_TOTAL_BUFSIZE + ARCH_DMA_MINALIGN, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
	if (!priv->rxbuffer_not_aligned)
	{
		Kprintf("[genet] %s: Failed to allocate RX buffer\n", __func__);
		return S2ERR_NO_RESOURCES;
	}

	priv->txbuffer_not_aligned = AllocMem(TX_TOTAL_BUFSIZE + ARCH_DMA_MINALIGN, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
	if (!priv->txbuffer_not_aligned)
	{
		Kprintf("[genet] %s: Failed to allocate TX buffer\n", __func__);
		FreeMem(priv->rxbuffer_not_aligned, RX_TOTAL_BUFSIZE + ARCH_DMA_MINALIGN);
		FreeMem(priv->txbuffer_not_aligned, RX_BUF_LENGTH + ARCH_DMA_MINALIGN);
		priv->rxbuffer_not_aligned = NULL;
		priv->txbuffer_not_aligned = NULL;
		return S2ERR_NO_RESOURCES;
	}

	/* These buffers are used for DMA transfers where buffers from IP stack cannot be used */
	priv->rxbuffer = (UBYTE *)roundup(priv->rxbuffer_not_aligned, ARCH_DMA_MINALIGN);
	priv->txbuffer = (UBYTE *)roundup(priv->txbuffer_not_aligned, ARCH_DMA_MINALIGN);

	bcmgenet_umac_reset(priv);

	bcmgenet_gmac_write_hwaddr(priv, priv->currentMacAddress);

	// bcmgenet_hfb_init()

	int ret = bcmgenet_init_dma(priv);
	if (ret != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: Failed to initialize DMA: %ld\n", __func__, ret);
		FreeMem(priv->rxbuffer_not_aligned, RX_TOTAL_BUFSIZE + ARCH_DMA_MINALIGN);
		FreeMem(priv->txbuffer_not_aligned, TX_TOTAL_BUFSIZE + ARCH_DMA_MINALIGN);
		priv->rxbuffer_not_aligned = NULL;
		priv->txbuffer_not_aligned = NULL;
		priv->rxbuffer = NULL;
		priv->txbuffer = NULL;
		return ret;
	}

	// bcmgenet_mii_probe(priv);
	//  rx_pause=1, tx_pause=1
	// bcmgenet_phy_pause_set(priv, priv->rx_pause, priv->tx_pause);
	bcmgenet_set_rx_mode(priv);
	//  enable rx/tx
	// phy_start()
	ret = phy_startup(priv->phydev);
	if (ret)
	{
		Kprintf("[genet] %s: PHY startup failed: %d\n", __func__, ret);
		return S2ERR_SOFTWARE;
	}

	/* Update MAC registers based on PHY property */
	ret = bcmgenet_adjust_link(priv);
	if (ret != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: adjust PHY link failed: %d\n", __func__, ret);
		return ret;
	}

	/* Enable Rx/Tx */
	setbits_32((APTR)((ULONG)priv->genetBase + UMAC_CMD), CMD_TX_EN | CMD_RX_EN);
	Kprintf("[genet] %s: UMAC started, RX/TX enabled\n", __func__);

	return S2ERR_NO_ERROR;
}

static int bcmgenet_phy_init(struct GenetUnit *priv)
{
	Kprintf("[genet] %s: Initializing PHY interface %s\n", __func__, phy_string_for_interface(priv->phy_interface));
	struct phy_device *phydev;

	phydev = phy_create(priv, priv->phy_interface);
	if (!phydev)
		return S2ERR_SOFTWARE;

	phydev->supported &= PHY_GBIT_FEATURES;
	phydev->advertising = phydev->supported;

	priv->phydev = phydev;
	int result = phy_config(phydev);
	if (result < 0)
	{
		Kprintf("[genet] %s: PHY config failed: %d\n", __func__, result);
		phy_destroy(phydev);
		priv->phydev = NULL;
		return S2ERR_SOFTWARE;
	}

	return S2ERR_NO_ERROR;
}

/* We only support RGMII (as used on the RPi4). */
static int bcmgenet_interface_set(struct GenetUnit *priv)
{
	Kprintf("[genet] %s: Setting PHY interface %s\n", __func__, phy_string_for_interface(priv->phy_interface));
	switch (priv->phy_interface)
	{
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_RXID:
		Kprintf("[genet] %s: Setting PHY mode to RGMII\n", __func__);
		writel(PORT_MODE_EXT_GPHY, (ULONG)priv->genetBase + SYS_PORT_CTRL);
		break;
	default:
		Kprintf("[genet] %s: unknown phy mode: %d\n", __func__, priv->phy_interface);
		return S2ERR_BAD_ARGUMENT;
	}

	return S2ERR_NO_ERROR;
}

int bcmgenet_eth_probe(struct GenetUnit *priv)
{
	/* Read GENET HW version */
	ULONG reg = readl((APTR)((ULONG)priv->genetBase + SYS_REV_CTRL));
	UBYTE major = (reg >> 24) & 0x0f;
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

	int ret = bcmgenet_interface_set(priv);
	if (ret != S2ERR_NO_ERROR)
		return ret;

	writel(0, (ULONG)priv->genetBase + SYS_RBUF_FLUSH_CTRL);
	delay_us(10);
	/* issue soft reset with (rg)mii loopback to ensure a stable rxclk */
	writel(CMD_SW_RESET | CMD_LCL_LOOP_EN, (ULONG)priv->genetBase + UMAC_CMD);
	delay_us(2);

	// bcmgenet_mii_init()
	return bcmgenet_phy_init(priv);
}

void bcmgenet_gmac_eth_stop(struct GenetUnit *priv)
{
	struct ExecBase *SysBase = priv->execBase;
	Kprintf("[genet] %s: Stopping GENET\n", __func__);

	// bcmgenet_hfb_reg_writel(priv, 0, HFB_CTRL);
	/* Disable MAC receive */
	clrbits_32((APTR)((ULONG)priv->genetBase + UMAC_CMD), CMD_RX_EN);
	delay_us(1000);
	bcmgenet_disable_dma(priv);
	/* Disable MAC transmit. TX DMA disabled must be done before this */
	clrbits_32((APTR)((ULONG)priv->genetBase + UMAC_CMD), CMD_TX_EN);
	delay_us(1000);
	/* tx reclaim */
	bcmgenet_timeout(priv);
	// /* Really kill the PHY state machine and disconnect from it */
	// phy_disconnect(dev->phydev);

	// TODO cancel pending requests in ring buffers

	priv->rxbuffer = NULL;
	if (priv->rxbuffer_not_aligned)
	{
		FreeMem(priv->rxbuffer_not_aligned, RX_TOTAL_BUFSIZE + ARCH_DMA_MINALIGN);
		priv->rxbuffer_not_aligned = NULL;
	}
	priv->txbuffer = NULL;
	if (priv->txbuffer_not_aligned)
	{
		FreeMem(priv->txbuffer_not_aligned, TX_TOTAL_BUFSIZE + ARCH_DMA_MINALIGN);
		priv->txbuffer_not_aligned = NULL;
	}

	if (priv->phydev)
	{
		phy_destroy(priv->phydev);
		priv->phydev = NULL;
	}
	Kprintf("[genet] %s: PHY destroyed. GENET stopped.\n", __func__);
}
