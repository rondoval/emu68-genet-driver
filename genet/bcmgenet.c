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

#include <debug.h>
#include <phy/phy.h>
#include <device.h>
#include <compat.h>

/* Register definitions derived from Linux source */
#define SYS_REV_CTRL 0x00

#define SYS_PORT_CTRL 0x04
#define PORT_MODE_EXT_GPHY 3

#define GENET_SYS_OFF 0x0000
#define SYS_RBUF_FLUSH_CTRL (GENET_SYS_OFF + 0x08)
#define SYS_TBUF_FLUSH_CTRL (GENET_SYS_OFF + 0x0c)

#define GENET_EXT_OFF 0x0080
#define EXT_RGMII_OOB_CTRL (GENET_EXT_OFF + 0x0c)
#define RGMII_LINK BIT(4)
#define OOB_DISABLE BIT(5)
#define RGMII_MODE_EN BIT(6)
#define ID_MODE_DIS BIT(16)

#define GENET_RBUF_OFF 0x0300
#define RBUF_TBUF_SIZE_CTRL (GENET_RBUF_OFF + 0xb4)
#define RBUF_CTRL (GENET_RBUF_OFF + 0x00)
#define RBUF_ALIGN_2B BIT(1)

#define GENET_UMAC_OFF 0x0800
#define UMAC_MIB_CTRL (GENET_UMAC_OFF + 0x580)
#define UMAC_MAX_FRAME_LEN (GENET_UMAC_OFF + 0x014)
#define UMAC_MAC0 (GENET_UMAC_OFF + 0x00c)
#define UMAC_MAC1 (GENET_UMAC_OFF + 0x010)
#define UMAC_CMD (GENET_UMAC_OFF + 0x008)
#define UMAC_TX_FLUSH (GENET_UMAC_OFF + 0x334)
#define MDIO_CMD (GENET_UMAC_OFF + 0x614)

#define CMD_TX_EN BIT(0)
#define CMD_RX_EN BIT(1)
#define UMAC_SPEED_10 0
#define UMAC_SPEED_100 1
#define UMAC_SPEED_1000 2
#define UMAC_SPEED_2500 3
#define CMD_SPEED_SHIFT 2
#define CMD_SPEED_MASK 3
#define CMD_SW_RESET BIT(13)
#define CMD_LCL_LOOP_EN BIT(15)
#define CMD_TX_EN BIT(0)
#define CMD_RX_EN BIT(1)

#define MIB_RESET_RX BIT(0)
#define MIB_RESET_RUNT BIT(1)
#define MIB_RESET_TX BIT(2)

/* total number of Buffer Descriptors, same for Rx/Tx */
#define TOTAL_DESCS 256
#define RX_DESCS TOTAL_DESCS
#define TX_DESCS TOTAL_DESCS

#define DEFAULT_Q 0x10

/* Body(1500) + EH_SIZE(14) + VLANTAG(4) + BRCMTAG(6) + FCS(4) = 1528.
 * 1536 is multiple of 256 bytes
 */
#define ENET_BRCM_TAG_LEN 6
#define ENET_PAD 8
#define ENET_MAX_MTU_SIZE (ETH_DATA_LEN + ETH_HLEN +       \
						   VLAN_HLEN + ENET_BRCM_TAG_LEN + \
						   ETH_FCS_LEN + ENET_PAD)

/* Tx/Rx Dma Descriptor common bits */
#define DMA_EN BIT(0)
#define DMA_RING_BUF_EN_SHIFT 0x01
#define DMA_RING_BUF_EN_MASK 0xffff
#define DMA_BUFLENGTH_MASK 0x0fff
#define DMA_BUFLENGTH_SHIFT 16
#define DMA_RING_SIZE_SHIFT 16
#define DMA_OWN 0x8000
#define DMA_EOP 0x4000
#define DMA_SOP 0x2000
#define DMA_WRAP 0x1000
#define DMA_MAX_BURST_LENGTH 0x8
/* Tx specific DMA descriptor bits */
#define DMA_TX_UNDERRUN 0x0200
#define DMA_TX_APPEND_CRC 0x0040
#define DMA_TX_OW_CRC 0x0020
#define DMA_TX_DO_CSUM 0x0010
#define DMA_TX_QTAG_SHIFT 7

/* DMA rings size */
#define DMA_RING_SIZE 0x40
#define DMA_RINGS_SIZE (DMA_RING_SIZE * (DEFAULT_Q + 1))

/* DMA descriptor */
#define DMA_DESC_LENGTH_STATUS 0x00
#define DMA_DESC_ADDRESS_LO 0x04
#define DMA_DESC_ADDRESS_HI 0x08
#define DMA_DESC_SIZE 12

#define GENET_RX_OFF 0x2000
#define GENET_RDMA_REG_OFF \
	(GENET_RX_OFF + TOTAL_DESCS * DMA_DESC_SIZE)
#define GENET_TX_OFF 0x4000
#define GENET_TDMA_REG_OFF \
	(GENET_TX_OFF + TOTAL_DESCS * DMA_DESC_SIZE)

#define DMA_FC_THRESH_HI (RX_DESCS >> 4)
#define DMA_FC_THRESH_LO 5
#define DMA_FC_THRESH_VALUE ((DMA_FC_THRESH_LO << 16) | \
							 DMA_FC_THRESH_HI)

#define DMA_XOFF_THRESHOLD_SHIFT 16

#define TDMA_RING_REG_BASE \
	(GENET_TDMA_REG_OFF + DEFAULT_Q * DMA_RING_SIZE)
#define TDMA_READ_PTR (TDMA_RING_REG_BASE + 0x00)
#define TDMA_CONS_INDEX (TDMA_RING_REG_BASE + 0x08)
#define TDMA_PROD_INDEX (TDMA_RING_REG_BASE + 0x0c)
#define DMA_RING_BUF_SIZE 0x10
#define DMA_START_ADDR 0x14
#define DMA_END_ADDR 0x1c
#define DMA_MBUF_DONE_THRESH 0x24
#define TDMA_FLOW_PERIOD (TDMA_RING_REG_BASE + 0x28)
#define TDMA_WRITE_PTR (TDMA_RING_REG_BASE + 0x2c)

#define RDMA_RING_REG_BASE \
	(GENET_RDMA_REG_OFF + DEFAULT_Q * DMA_RING_SIZE)
#define RDMA_WRITE_PTR (RDMA_RING_REG_BASE + 0x00)
#define RDMA_PROD_INDEX (RDMA_RING_REG_BASE + 0x08)
#define RDMA_CONS_INDEX (RDMA_RING_REG_BASE + 0x0c)
#define RDMA_XON_XOFF_THRESH (RDMA_RING_REG_BASE + 0x28)
#define RDMA_READ_PTR (RDMA_RING_REG_BASE + 0x2c)

#define TDMA_REG_BASE (GENET_TDMA_REG_OFF + DMA_RINGS_SIZE)
#define RDMA_REG_BASE (GENET_RDMA_REG_OFF + DMA_RINGS_SIZE)
#define DMA_RING_CFG 0x00
#define DMA_CTRL 0x04
#define DMA_SCB_BURST_SIZE 0x0c

#define RX_BUF_LENGTH 2048
#define RX_TOTAL_BUFSIZE (RX_BUF_LENGTH * RX_DESCS)
#define RX_BUF_OFFSET 2

static void bcmgenet_umac_reset(struct GenetUnit *priv)
{
	Kprintf("[genet] %s: Resetting UMAC\n", __func__);
	ULONG reg;

	reg = readl((ULONG)priv->genetBase + SYS_RBUF_FLUSH_CTRL);
	reg |= BIT(1);
	writel(reg, ((ULONG)priv->genetBase + SYS_RBUF_FLUSH_CTRL));
	delay_us(10);

	reg &= ~BIT(1);
	writel(reg, ((ULONG)priv->genetBase + SYS_RBUF_FLUSH_CTRL));
	delay_us(10);

	writel(0, ((ULONG)priv->genetBase + SYS_RBUF_FLUSH_CTRL));
	delay_us(10);

	writel(0, (ULONG)priv->genetBase + UMAC_CMD);

	writel(CMD_SW_RESET | CMD_LCL_LOOP_EN, (ULONG)priv->genetBase + UMAC_CMD);
	delay_us(2);
	writel(0, (ULONG)priv->genetBase + UMAC_CMD);

	/* clear tx/rx counter */
	writel(MIB_RESET_RX | MIB_RESET_TX | MIB_RESET_RUNT,
		   (ULONG)priv->genetBase + UMAC_MIB_CTRL);
	writel(0, (ULONG)priv->genetBase + UMAC_MIB_CTRL);

	writel(ENET_MAX_MTU_SIZE, (ULONG)priv->genetBase + UMAC_MAX_FRAME_LEN);

	/* init rx registers, enable ip header optimization */
	reg = readl((ULONG)priv->genetBase + RBUF_CTRL);
	reg |= RBUF_ALIGN_2B;
	writel(reg, ((ULONG)priv->genetBase + RBUF_CTRL));

	writel(1, ((ULONG)priv->genetBase + RBUF_TBUF_SIZE_CTRL));
}

static void bcmgenet_gmac_write_hwaddr(struct GenetUnit *priv, const UBYTE *addr)
{
	Kprintf("[genet] %s: Setting MAC address to %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
			__func__, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	ULONG reg;

	reg = addr[0] << 24 | addr[1] << 16 | addr[2] << 8 | addr[3];
	writel_relaxed(reg, (APTR)((ULONG)priv->genetBase + UMAC_MAC0));

	reg = addr[4] << 8 | addr[5];
	writel_relaxed(reg, (APTR)((ULONG)priv->genetBase + UMAC_MAC1));
}

static void bcmgenet_disable_dma(struct GenetUnit *priv)
{
	Kprintf("[genet] %s: Disabling DMA\n", __func__);
	clrbits_32((APTR)((ULONG)priv->genetBase + TDMA_REG_BASE + DMA_CTRL), DMA_EN);
	clrbits_32((APTR)((ULONG)priv->genetBase + RDMA_REG_BASE + DMA_CTRL), DMA_EN);

	writel(1, (APTR)((ULONG)priv->genetBase + UMAC_TX_FLUSH));
	delay_us(10);
	writel(0, (APTR)((ULONG)priv->genetBase + UMAC_TX_FLUSH));
}

static void bcmgenet_enable_dma(struct GenetUnit *priv)
{
	Kprintf("[genet] %s: Enabling DMA\n", __func__);
	ULONG dma_ctrl = (1 << (DEFAULT_Q + DMA_RING_BUF_EN_SHIFT)) | DMA_EN;

	writel(dma_ctrl, (ULONG)priv->genetBase + TDMA_REG_BASE + DMA_CTRL);

	setbits_32((APTR)((ULONG)priv->genetBase + RDMA_REG_BASE + DMA_CTRL), dma_ctrl);
}

int bcmgenet_gmac_eth_send(struct GenetUnit *priv, void *packet, ULONG length)
{
	struct ExecBase *SysBase = priv->execBase;
	KprintfH("[genet] %s: packet=%08lx length=%ld\n", __func__, packet, length);

	ULONG tx_prod_index = readl((ULONG)priv->genetBase + TDMA_PROD_INDEX);
	APTR desc_base = (APTR)((ULONG)priv->tx_desc_base + (tx_prod_index & 0xff) * DMA_DESC_SIZE);

	// TODO can this shorten length?
	CachePreDMA(packet, &length, DMA_ReadFromRAM);

	ULONG len_stat = length << DMA_BUFLENGTH_SHIFT;
	len_stat |= 0x3F << DMA_TX_QTAG_SHIFT;
	len_stat |= DMA_TX_APPEND_CRC | DMA_SOP | DMA_EOP;

	/* Set-up packet for transmission */
	writel(lower_32_bits((ULONG)packet), ((ULONG)desc_base + DMA_DESC_ADDRESS_LO));
	writel(0, ((ULONG)desc_base + DMA_DESC_ADDRESS_HI));
	//	writel(upper_32_bits((ULONG)packet), (desc_base + DMA_DESC_ADDRESS_HI));
	writel(len_stat, ((ULONG)desc_base + DMA_DESC_LENGTH_STATUS));

	tx_prod_index++;

	/* Start Transmisson */
	writel(tx_prod_index, (ULONG)priv->genetBase + TDMA_PROD_INDEX);

	ULONG tx_cons_index;
	ULONG tries = 100;
	do
	{
		tx_cons_index = readl((ULONG)priv->genetBase + TDMA_CONS_INDEX);
	} while ((tx_cons_index & 0xffff) < tx_prod_index && --tries);
	CachePostDMA(packet, &length, DMA_ReadFromRAM | DMA_NoModify);
	if (!tries)
		return S2ERR_TX_FAILURE;

	return S2ERR_NO_ERROR;
}

int bcmgenet_gmac_eth_recv(struct GenetUnit *priv, int flags, UBYTE **packetp)
{
	struct ExecBase *SysBase = priv->execBase;
	ULONG rx_prod_index = readl((ULONG)priv->genetBase + RDMA_PROD_INDEX);

	if (rx_prod_index == priv->rx_cons_index)
		return EAGAIN;

	APTR desc_base = (APTR)((ULONG)priv->rx_desc_base + (priv->rx_cons_index & 0xFF) * DMA_DESC_SIZE);
	ULONG length = readl((ULONG)desc_base + DMA_DESC_LENGTH_STATUS);
	length = (length >> DMA_BUFLENGTH_SHIFT) & DMA_BUFLENGTH_MASK;
	APTR addr = (APTR)readl((ULONG)desc_base + DMA_DESC_ADDRESS_LO);

	CachePostDMA(addr, &length, 0);

	/* To cater for the IP header alignment the hardware does.
	 * This would actually not be needed if we don't program
	 * RBUF_ALIGN_2B
	 */
	*packetp = (UBYTE *)((ULONG)addr + RX_BUF_OFFSET);
	KprintfH("[genet] %s: packet=%08lx length=%ld\n", __func__, *packetp, length);

	return length - RX_BUF_OFFSET;
}

int bcmgenet_gmac_free_pkt(struct GenetUnit *priv, UBYTE *packet, ULONG length)
{
	struct ExecBase *SysBase = priv->execBase;
	KprintfH("[genet] %s: packet=%08lx length=%ld\n", __func__, packet, length);

	// Adjust back to the original address
	packet -= RX_BUF_OFFSET;

	CachePreDMA(packet, &length, 0);

	/* Tell the MAC we have consumed that last receive buffer. */
	priv->rx_cons_index = (priv->rx_cons_index + 1) & 0xFFFF;
	writel(priv->rx_cons_index, (ULONG)priv->genetBase + RDMA_CONS_INDEX);

	return S2ERR_NO_ERROR;
}

static void rx_descs_init(struct GenetUnit *priv)
{
	Kprintf("[genet] %s: Initializing RX descriptors\n", __func__);
	// translate to VC address space
	UBYTE *rxbuffs = priv->rxbuffer;
	APTR desc_base = priv->rx_desc_base;

	ULONG len_stat = (RX_BUF_LENGTH << DMA_BUFLENGTH_SHIFT) | DMA_OWN;

	for (int i = 0; i < RX_DESCS; i++)
	{
		writel(lower_32_bits(&rxbuffs[i * RX_BUF_LENGTH]),
			   (ULONG)desc_base + i * DMA_DESC_SIZE + DMA_DESC_ADDRESS_LO);
		// writel(upper_32_bits(&rxbuffs[i * RX_BUF_LENGTH]),
		//        desc_base + i * DMA_DESC_SIZE + DMA_DESC_ADDRESS_HI);
		writel(0,
			   (ULONG)desc_base + i * DMA_DESC_SIZE + DMA_DESC_ADDRESS_HI);
		writel(len_stat,
			   (ULONG)desc_base + i * DMA_DESC_SIZE + DMA_DESC_LENGTH_STATUS);
	}
}

static void rx_ring_init(struct GenetUnit *priv)
{
	Kprintf("[genet] %s: Initializing RX ring\n", __func__);
	writel(DMA_MAX_BURST_LENGTH,
		   (ULONG)priv->genetBase + RDMA_REG_BASE + DMA_SCB_BURST_SIZE);

	writel(0x0, (ULONG)priv->genetBase + RDMA_RING_REG_BASE + DMA_START_ADDR);
	writel(0x0, (ULONG)priv->genetBase + RDMA_READ_PTR);
	writel(0x0, (ULONG)priv->genetBase + RDMA_WRITE_PTR);
	writel(RX_DESCS * DMA_DESC_SIZE / 4 - 1,
		   (ULONG)priv->genetBase + RDMA_RING_REG_BASE + DMA_END_ADDR);

	/* cannot init RDMA_PROD_INDEX to 0, so align RDMA_CONS_INDEX on it instead */
	priv->rx_cons_index = readl((ULONG)priv->genetBase + RDMA_PROD_INDEX);
	writel(priv->rx_cons_index, (ULONG)priv->genetBase + RDMA_CONS_INDEX);
	writel((RX_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH,
		   (ULONG)priv->genetBase + RDMA_RING_REG_BASE + DMA_RING_BUF_SIZE);
	writel(DMA_FC_THRESH_VALUE, (ULONG)priv->genetBase + RDMA_XON_XOFF_THRESH);
	writel(1 << DEFAULT_Q, (ULONG)priv->genetBase + RDMA_REG_BASE + DMA_RING_CFG);
}

static void tx_ring_init(struct GenetUnit *priv)
{
	Kprintf("[genet] %s: Initializing TX ring\n", __func__);
	writel(DMA_MAX_BURST_LENGTH,
		   (ULONG)priv->genetBase + TDMA_REG_BASE + DMA_SCB_BURST_SIZE);

	writel(0x0, (ULONG)priv->genetBase + TDMA_RING_REG_BASE + DMA_START_ADDR);
	writel(0x0, (ULONG)priv->genetBase + TDMA_READ_PTR);
	writel(0x0, (ULONG)priv->genetBase + TDMA_WRITE_PTR);
	writel(TX_DESCS * DMA_DESC_SIZE / 4 - 1,
		   (ULONG)priv->genetBase + TDMA_RING_REG_BASE + DMA_END_ADDR);
	/* cannot init TDMA_CONS_INDEX to 0, so align TDMA_PROD_INDEX on it instead */
	ULONG tx_cons_index = readl((ULONG)priv->genetBase + TDMA_CONS_INDEX);
	writel(tx_cons_index, (ULONG)priv->genetBase + TDMA_PROD_INDEX);
	writel(0x1, (ULONG)priv->genetBase + TDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH);
	writel(0x0, (ULONG)priv->genetBase + TDMA_FLOW_PERIOD);
	writel((TX_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH,
		   (ULONG)priv->genetBase + TDMA_RING_REG_BASE + DMA_RING_BUF_SIZE);

	writel(1 << DEFAULT_Q, (ULONG)priv->genetBase + TDMA_REG_BASE + DMA_RING_CFG);
}

static int bcmgenet_adjust_link(struct GenetUnit *priv)
{
	Kprintf("[genet] %s: Adjusting link for PHY interface %s\n", __func__, phy_string_for_interface(priv->phy_interface));
	struct phy_device *phy_dev = priv->phydev;
	ULONG speed;

	switch (phy_dev->speed)
	{
	case SPEED_1000:
		speed = UMAC_SPEED_1000;
		break;
	case SPEED_100:
		speed = UMAC_SPEED_100;
		break;
	case SPEED_10:
		speed = UMAC_SPEED_10;
		break;
	default:
		Kprintf("[genet] %s: Unsupported PHY speed: %d\n", __func__, phy_dev->speed);
		return S2ERR_BAD_ARGUMENT;
	}

	clrsetbits_32((APTR)((ULONG)priv->genetBase + EXT_RGMII_OOB_CTRL), OOB_DISABLE,
				  RGMII_LINK | RGMII_MODE_EN);

	if (phy_dev->interface == PHY_INTERFACE_MODE_RGMII ||
		phy_dev->interface == PHY_INTERFACE_MODE_RGMII_RXID)
		setbits_32((APTR)((ULONG)priv->genetBase + EXT_RGMII_OOB_CTRL), ID_MODE_DIS);

	writel(speed << CMD_SPEED_SHIFT, ((ULONG)priv->genetBase + UMAC_CMD));

	return S2ERR_NO_ERROR;
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

	priv->txbuffer_not_aligned = AllocMem(RX_BUF_LENGTH + ARCH_DMA_MINALIGN, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
	if (!priv->txbuffer_not_aligned)
	{
		Kprintf("[genet] %s: Failed to allocate TX buffer\n", __func__);
		FreeMem(priv->rxbuffer_not_aligned, RX_TOTAL_BUFSIZE + ARCH_DMA_MINALIGN);
		FreeMem(priv->txbuffer_not_aligned, RX_BUF_LENGTH + ARCH_DMA_MINALIGN);
		priv->rxbuffer_not_aligned = NULL;
		priv->txbuffer_not_aligned = NULL;
		return S2ERR_NO_RESOURCES;
	}
	priv->rxbuffer = (UBYTE *)roundup(priv->rxbuffer_not_aligned, ARCH_DMA_MINALIGN);
	priv->txbuffer = (UBYTE *)roundup(priv->txbuffer_not_aligned, ARCH_DMA_MINALIGN);

	priv->rx_desc_base = (APTR)((ULONG)priv->genetBase + GENET_RX_OFF);
	priv->tx_desc_base = (APTR)((ULONG)priv->genetBase + GENET_TX_OFF);

	bcmgenet_umac_reset(priv);

	bcmgenet_gmac_write_hwaddr(priv, priv->currentMacAddress);

	/* Disable RX/TX DMA and flush TX queues */
	bcmgenet_disable_dma(priv);

	rx_ring_init(priv);
	rx_descs_init(priv);

	tx_ring_init(priv);

	/* Enable RX/TX DMA */
	bcmgenet_enable_dma(priv);

	/* read PHY properties over the wire from generic PHY set-up */
	int ret = phy_startup(priv->phydev);
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
	ULONG reg = readl_relaxed((APTR)((ULONG)priv->genetBase + SYS_REV_CTRL));
	UBYTE major = (reg >> 24) & 0x0f;
	if (major != 6)
	{
		if (major == 5)
			major = 4;
		else if (major == 0)
			major = 1;

		Kprintf("[genet] %s: Unsupported GENET v%ld.%ld\n", __func__, major, (reg >> 16) & 0x0f);
		return S2ERR_SOFTWARE;
	}
	Kprintf("[genet] %s: GENET v%ld.%ld\n", __func__, major - 1, (reg >> 16) & 0x0f);

	int ret = bcmgenet_interface_set(priv);
	if (ret != S2ERR_NO_ERROR)
		return ret;

	writel(0, (ULONG)priv->genetBase + SYS_RBUF_FLUSH_CTRL);
	delay_us(10);
	/* disable MAC while updating its registers */
	writel(0, (ULONG)priv->genetBase + UMAC_CMD);
	/* issue soft reset with (rg)mii loopback to ensure a stable rxclk */
	writel(CMD_SW_RESET | CMD_LCL_LOOP_EN, (ULONG)priv->genetBase + UMAC_CMD);

	return bcmgenet_phy_init(priv);
}

void bcmgenet_gmac_eth_stop(struct GenetUnit *priv)
{
	struct ExecBase *SysBase = priv->execBase;
	Kprintf("[genet] %s: Stopping GENET\n", __func__);
	clrbits_32((APTR)((ULONG)priv->genetBase + UMAC_CMD), CMD_TX_EN | CMD_RX_EN);
	bcmgenet_disable_dma(priv);

	priv->rxbuffer = NULL;
	if (priv->rxbuffer_not_aligned)
	{
		FreeMem(priv->rxbuffer_not_aligned, RX_TOTAL_BUFSIZE + ARCH_DMA_MINALIGN);
		priv->rxbuffer_not_aligned = NULL;
	}

	if (priv->phydev)
	{
		phy_destroy(priv->phydev);
		priv->phydev = NULL;
	}
	Kprintf("[genet] %s: PHY destroyed. GENET stopped.\n", __func__);
}
