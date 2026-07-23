// SPDX-License-Identifier: GPL-2.0+
/*
 * Broadcom GENETv5 Ethernet controller — bring-up and teardown.
 *
 * Based on the Linux driver drivers/net/ethernet/broadcom/genet/bcmgenet.c
 * (Copyright (c) 2014-2017 Broadcom). Only GENET v5, as on the Raspberry Pi 4,
 * is supported, and all 256 descriptors go to the single default queue (#16).
 *
 * The datapath and the register plumbing live in the sibling files:
 *   bcmgenet-rx.c   RX drain and RX ring
 *   bcmgenet-tx.c   TX submit and harvest
 *   bcmgenet-dma.c  DMA enable/disable, ring init, coalescing
 *   bcmgenet-mac.c  UMAC/MII programming and the address filter
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

/* Datapath timing ([genet] rx_drain/rx_flush/tx_submit/tx_harvest), reported
 * every ~2 s off the ~200 ms housekeeping tick and reduced by
 * emu68-common/scripts/perf-report.py. The MAC counters are not swept here:
 * they are read on demand behind NETDEV_CMD_GET_COUNTERS (bcmgenet-mib.c). */
#ifdef PROFILE
void bcmgenet_perf_tick(struct GenetUnit *unit)
{
	if (++unit->gu_ProfTicks >= 10)
	{
		unit->gu_ProfTicks = 0;
		perf_report(&unit->gu_Perf);
	}
}
#endif /* PROFILE */

u32 bcmgenet_gmac_eth_start(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Starting GENET\n", __func__);
	u32 ret;

	bcmgenet_umac_reset(unit);

	bcmgenet_gmac_write_hwaddr(unit, unit->currentMacAddress);

	/* Not ported: the hardware filter block. See docs/offloads.md. */

	ret = bcmgenet_init_dma(unit);
	if (ret != GENET_OK)
	{
		Kprintf("[genet] %s: Failed to initialize DMA: %ld\n", __func__, ret);
		return ret;
	}

	unit->irq0_isr.is_Node.ln_Type = NT_INTERRUPT;
	unit->irq0_isr.is_Node.ln_Name = "bcmgenet_isr0";
	unit->irq0_isr.is_Data = (APTR)unit;
	unit->irq0_isr.is_Code = (APTR)bcmgenet_isr0;

	s32 irq_result = AddIntServerEx(unit->irq0_number, 0, FALSE, &unit->irq0_isr);
	if (irq_result < 0)
	{
		Kprintf("[genet] %s: can't register IRQ %ld\n", __func__, unit->irq0_number);
		ret = EIO;
		return ret;
	}
	unit->irq0_installed = TRUE;
	Kprintf("[genet] %s: Interrupt server for IRQ %ld registered\n", __func__, unit->irq0_number);

	bcmgenet_set_rx_mode(unit);

	bcmgenet_mii_config(unit);

	/* Sample the link without blocking. If it is not up yet, the MAC stays in
	 * soft reset and the periodic link poll brings it up — and tells the
	 * stack — once the PHY has negotiated. Starting the device must never
	 * stall waiting for a cable. */
	s32 phy_result = genphy_read_status(unit->phydev, FALSE);
	if (phy_result < 0)
	{
		Kprintf("[genet] %s: PHY status read failed: %ld\n", __func__, phy_result);
		ret = EIO;
		return ret;
	}

	if (unit->phydev->link)
	{
		/* Enables the datapath as a side effect (releases CMD_SW_RESET). */
		ret = bcmgenet_mac_config(unit);
		if (ret != GENET_OK)
		{
			Kprintf("[genet] %s: MAC link config failed: %ld\n", __func__, ret);
			return ret;
		}
	}

	/* Monitor link, RX and TX-done interrupts. TXDMA_DONE drives nso_TxDone,
	 * the stack's memory-reclaim and ring-space signal; the MBUF_DONE threshold
	 * batches it, and the hardware also interrupts when the ring drains, so
	 * latency is bounded. One MASK_CLEAR write covers the lot.
	 */
	bcmgenet_irq0_enable(unit, bcmgenet_link_irq_mask(unit) |
								   UMAC_IRQ_RXDMA_DONE | UMAC_IRQ_TXDMA_DONE);
	Kprintf("[genet] %s: UMAC started, link %s\n", __func__,
			unit->phydev->link ? "up" : "down (waiting for poll)");

	return GENET_OK;
}

/*
 * Translate the LINK_MODE / AUTONEG / FLOW_CONTROL prefs into the PHY's
 * advertisement and autoneg state, between the default "advertise everything"
 * assignment and phy_config(), which masks the result against what the PHY
 * actually reports it can do.
 *
 * LINK_MODE narrows the advertisement rather than forcing the BMCR, so pinning
 * a link to one speed works at every speed and stays inside the standard.
 * AUTONEG=off is the separate case of a partner that will not negotiate at all;
 * runtime_config.c has already refused the combinations that cannot work, so a
 * cleared link_autoneg here always carries a usable 10/100 speed.
 */
static void bcmgenet_apply_link_config(struct GenetUnit *unit, struct phy_device *phydev)
{
	const struct GenetRuntimeConfig *config = &unit->device->runtimeConfig;

	if (!config->flow_control)
		phydev->advertising &= ~(u32)PHY_PAUSE_FEATURES;

	if (config->link_speed != LINK_MODE_NONE)
	{
		u32 mode = 0;
		switch (config->link_speed)
		{
		case 10:
			mode = (config->link_duplex == DUPLEX_FULL_CFG) ? ADVERTISED_10baseT_Full
															: ADVERTISED_10baseT_Half;
			break;
		case 100:
			mode = (config->link_duplex == DUPLEX_FULL_CFG) ? ADVERTISED_100baseT_Full
															: ADVERTISED_100baseT_Half;
			break;
		default:
			mode = ADVERTISED_1000baseT_Full;
			break;
		}
		/* Keep the pause and medium bits: they qualify the link rather than
		 * selecting one, and dropping them would withdraw flow control as a
		 * side effect of pinning a speed. */
		phydev->advertising &= (u32)(PHY_PAUSE_FEATURES | SUPPORTED_Autoneg |
									 SUPPORTED_TP | SUPPORTED_MII);
		phydev->advertising |= mode;

		phydev->speed = config->link_speed;
		phydev->duplex = (config->link_duplex == DUPLEX_FULL_CFG) ? DUPLEX_FULL : DUPLEX_HALF;
	}

	if (!config->link_autoneg)
		phydev->autoneg = AUTONEG_DISABLE;

	Kprintf("[genet] %s: link %s, advertising 0x%08lx, flow control %s\n", __func__,
			config->link_autoneg
				? (config->link_speed == LINK_MODE_NONE ? (ULONG) "autoneg, all modes"
														: (ULONG) "autoneg, one mode")
				: (ULONG) "forced",
			phydev->advertising, config->flow_control ? (ULONG) "on" : (ULONG) "off");
}

static u32 bcmgenet_phy_init(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Initializing PHY interface %s\n", __func__, phy_string_for_interface(unit->phy_interface));
	struct phy_device *phydev;

	phydev = phy_create(unit, unit->phy_interface);
	if (!phydev)
		return EIO;

	phydev->supported &= PHY_GBIT_FEATURES;
	phydev->advertising = phydev->supported;

	bcmgenet_apply_link_config(unit, phydev);

	unit->phydev = phydev;
	s32 result = phy_config(phydev);
	if (result < 0)
	{
		Kprintf("[genet] %s: PHY config failed: %ld\n", __func__, result);
		phy_destroy(phydev);
		unit->phydev = NULL;
		return EIO;
	}

	return GENET_OK;
}

/* We only support RGMII (as used on the RPi4). */
static u32 bcmgenet_interface_set(struct GenetUnit *unit)
{
	Kprintf("[genet] %s: Setting PHY interface %s\n", __func__, phy_string_for_interface(unit->phy_interface));
	switch (unit->phy_interface)
	{
	/* The external-GPHY RGMII variants GENET supports. "rgmii-id" is not one:
	 * the MAC has no internal RX delay to pair with the PHY's. */
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		KprintfT("[genet] %s: Setting PHY mode to RGMII\n", __func__);
		mmio_write32(PORT_MODE_EXT_GPHY, BCMGENET_REG(unit, SYS_PORT_CTRL));
		break;
	default:
		Kprintf("[genet] %s: unknown phy mode: %ld\n", __func__, unit->phy_interface);
		return EINVAL;
	}

	return GENET_OK;
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
		return EIO;
	}
	Kprintf("[genet] %s: GENET v%ld.%ld\n", __func__, major, (reg >> 16) & 0x0f);

	u32 ret = bcmgenet_interface_set(unit);
	if (ret != GENET_OK)
		return ret;

	/* Ring slot bookkeeping: fixed size, so it outlives start/stop and is
	 * released by bcmgenet_eth_unconfigure(). */
	unit->rx_ring.rx_control_block =
		pool_zalloc(unit->metaPool, RX_DESCS * sizeof(struct enet_cb));
	if (unit->rx_ring.rx_control_block == NULL)
	{
		Kprintf("[genet] %s: Failed to allocate RX control block\n", __func__);
		return ENOMEM;
	}

	mmio_write32(0, BCMGENET_REG(unit, SYS_RBUF_FLUSH_CTRL));
	delay_us(10);
	/* issue soft reset with (rg)mii loopback to ensure a stable rxclk */
	mmio_write32(CMD_SW_RESET | CMD_LCL_LOOP_EN, BCMGENET_REG(unit, UMAC_CMD));
	delay_us(2);

	ret = bcmgenet_phy_init(unit);
	if (ret != GENET_OK)
		bcmgenet_eth_unconfigure(unit);

	return ret;
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

/* Stop the datapath. Hardware only: the PHY and the ring bookkeeping belong to
 * the probe/unconfigure pair, so a subsequent start finds them intact. Also the
 * unwind for a failed start, hence the guards — every stage is optional. */
void bcmgenet_gmac_eth_stop(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Stopping GENET\n", __func__);

	/* Publish any TX batch the stack staged but has not kicked, before the DMA
	 * is disabled below — otherwise those frames would be completed as dropped
	 * without ever reaching the wire. The stack normally kicks at the unlock
	 * preceding STOP, so this is an idempotent backstop. */
	bcmgenet_netdev_tx_kick(unit);

	bcmgenet_reset_quiesce(unit);

	if (unit->irq0_installed)
	{
		RemIntServerEx(unit->irq0_number, &unit->irq0_isr);
		unit->irq0_installed = FALSE;
	}

	/* DMA is off: complete every in-flight TX cookie (the stack's memory
	 * accounting must close), then return the ring's RX buffers to the pool.
	 * Buffers the stack still holds stay valid until it releases them. */
	bcmgenet_netdev_tx_quiesce(unit);

	struct bcmgenet_rx_ring *rx_ring = &unit->rx_ring;
	if (rx_ring->rx_control_block != NULL)
	{
		for (u32 i = 0; i < RX_DESCS; i++)
		{
			if (rx_ring->rx_control_block[i].data_buffer != 0)
			{
				netdev_rx_push(unit, (APTR)rx_ring->rx_control_block[i].data_buffer);
				rx_ring->rx_control_block[i].data_buffer = 0;
			}
		}
	}

	KprintfT("[genet] %s: GENET stopped\n", __func__);
}

/* Release what bcmgenet_eth_probe() acquired. Called from UnitClose, not from
 * the stop path: START after STOP must find the PHY where it left it. */
void bcmgenet_eth_unconfigure(struct GenetUnit *unit)
{
	if (unit->phydev != NULL)
	{
		phy_destroy(unit->phydev);
		unit->phydev = NULL;
	}

	if (unit->rx_ring.rx_control_block != NULL)
	{
		pool_free(unit->metaPool, unit->rx_ring.rx_control_block);
		unit->rx_ring.rx_control_block = NULL;
	}
	KprintfT("[genet] %s: GENET unconfigured\n", __func__);
}
