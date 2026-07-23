// SPDX-License-Identifier: GPL-2.0+
/*
 * Broadcom GENETv5 — UMAC and MII programming: reset, station address, RGMII
 * mode, the link-driven CMD register, and the MDF address filter.
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

void bcmgenet_umac_reset(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Resetting UMAC\n", __func__);

	mmio_set32(BCMGENET_REG(unit, SYS_RBUF_FLUSH_CTRL), BIT(1));
	delay_us(10);

	mmio_clear32(BCMGENET_REG(unit, SYS_RBUF_FLUSH_CTRL), BIT(1));
	delay_us(10);

	/* Reset UMAC */
	mmio_write32(0, BCMGENET_REG(unit, SYS_RBUF_FLUSH_CTRL));
	delay_us(10);

	/* Hold the MAC in soft reset while its registers are reprogrammed. It
	 * stays in reset until the link comes up: bcmgenet_mac_config() is what
	 * releases CMD_SW_RESET and enables TX/RX, so a start with no cable
	 * leaves the MAC quiescent instead of half-configured. */
	mmio_write32(CMD_SW_RESET, BCMGENET_REG(unit, UMAC_CMD));
	delay_us(2);

	bcmgenet_mib_reset(unit); /* one bring-up, one counter baseline */

	mmio_write32(ENET_MAX_MTU_SIZE, BCMGENET_REG(unit, UMAC_MAX_FRAME_LEN));

	/* Status blocks on, both directions: RX frames arrive with a 64-byte
	 * RSB (RXCHK checksum), TX frames carry a 64-byte TSB (checksum
	 * offload parameters). ALIGN_2B is deliberately OFF with the RSB: the
	 * 2-byte IP-alignment pad would make the frame offset 66. */
	u32 reg = mmio_read32(BCMGENET_REG(unit, RBUF_CTRL));
	reg |= RBUF_64B_EN;
	reg &= ~(u32)RBUF_ALIGN_2B;
	mmio_write32(reg, BCMGENET_REG(unit, RBUF_CTRL));

	mmio_set32(BCMGENET_REG(unit, TBUF_CTRL), TBUF_64B_EN);

	/* EEE and buffer-block power management off in both directions. These
	 * registers survive CMD_SW_RESET, so their state is whatever ran before us;
	 * RBUF EEE/PM in particular breaks reception. */
	mmio_clear32(BCMGENET_REG(unit, RBUF_ENERGY_CTRL), ENERGY_EEE_EN | ENERGY_PM_EN);
	mmio_clear32(BCMGENET_REG(unit, TBUF_ENERGY_CTRL), ENERGY_EEE_EN | ENERGY_PM_EN);

	/* RXCHK with the L3 parser off checksums the whole frame past the
	 * Ethernet header into the RSB (CHECKSUM_COMPLETE style) — the raw
	 * sum the netdev RX contract expects. The UMAC strips the FCS
	 * (CMD_CRC_FWD unset), so no RBUF_SKIP_FCS. */
	mmio_set32(BCMGENET_REG(unit, RBUF_CHK_CTRL), RBUF_RXCHK_EN | RBUF_L3_PARSE_DIS);
	Kprintf("[genet] %s: RBUF_CHK_CTRL 0x%08lx\n", __func__,
			mmio_read32(BCMGENET_REG(unit, RBUF_CHK_CTRL)));

	mmio_write32(1, BCMGENET_REG(unit, RBUF_TBUF_SIZE_CTRL));

	bcmgenet_intr_disable(unit);
}

void bcmgenet_gmac_write_hwaddr(struct GenetUnit *unit, const u8 *addr)
{
	Kprintf("[genet] %s: Setting MAC address to %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
			__func__, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	u32 reg;

	reg = ((u32)addr[0] << 24) | ((u32)addr[1] << 16) | ((u32)addr[2] << 8) | (u32)addr[3];
	mmio_write32(reg, BCMGENET_REG(unit, UMAC_MAC0));

	reg = ((u32)addr[4] << 8) | (u32)addr[5];
	mmio_write32(reg, BCMGENET_REG(unit, UMAC_MAC1));
}

/* RGMII pad/mode setup for the configured PHY interface. This describes the
 * board wiring, not the link, so it runs once per start; only RGMII_LINK
 * tracks link state (bcmgenet_mac_config/_mac_link_down below).
 *
 * Delay contract, split across two files: RGMII needs ~2 ns of clock-to-data
 * skew in each direction, contributed exactly once. The phy-mode names the
 * PHY's share (programmed by bcm54xx_config_clock_delay() in phy.c) and the MAC
 * supplies the rest — ID_MODE_DIS turns the MAC's internal TX delay *off*, so
 * it is set only for plain "rgmii", where neither end delays and the board is
 * expected to. The Pi's "rgmii-rxid" means the PHY delays RXC and the MAC
 * delays TXC, so the bit stays clear. Clear-then-set keeps the result
 * independent of whatever the bootloader left behind.
 */
void bcmgenet_mii_config(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Configuring PHY interface %s\n", __func__, phy_string_for_interface(unit->phy_interface));

	u32 id_mode_dis = (unit->phydev->interface == PHY_INTERFACE_MODE_RGMII) ? ID_MODE_DIS : 0;

	mmio_update32(BCMGENET_REG(unit, EXT_RGMII_OOB_CTRL),
				  OOB_DISABLE | ID_MODE_DIS,	  /* clear */
				  RGMII_MODE_EN | id_mode_dis); /* set   */
}

/* Program the MAC for the currently negotiated link. Must be a
 * read-modify-write: a blind store here would drop CMD_TX_EN/CMD_RX_EN and
 * silently kill the datapath on every link change. */
u32 bcmgenet_mac_config(struct GenetUnit *unit)
{
	struct phy_device *phy_dev = unit->phydev;
	u32 cmd_bits;

	switch (phy_dev->speed)
	{
	case SPEED_1000:
		cmd_bits = CMD_SPEED_1000;
		break;
	case SPEED_100:
		cmd_bits = CMD_SPEED_100;
		break;
	case SPEED_10:
		cmd_bits = CMD_SPEED_10;
		break;
	default:
		Kprintf("[genet] %s: Unsupported PHY speed: %ld\n", __func__, phy_dev->speed);
		return EINVAL;
	}
	cmd_bits <<= CMD_SPEED_SHIFT;

	/* Flow control follows the negotiated result, so the MAC only honours and
	 * emits PAUSE on a link where both ends agreed to it. Half duplex has no
	 * PAUSE at all — collision detection is the backpressure there. */
	if (phy_dev->duplex != DUPLEX_FULL)
	{
		cmd_bits |= CMD_HD_EN | CMD_RX_PAUSE_IGNORE | CMD_TX_PAUSE_IGNORE;
	}
	else
	{
		if (!phy_dev->pause_rx)
			cmd_bits |= CMD_RX_PAUSE_IGNORE;
		if (!phy_dev->pause_tx)
			cmd_bits |= CMD_TX_PAUSE_IGNORE;
	}

	/* The speed programmed here also selects the RGMII transmit clock
	 * (25 MHz @ 100, 125 MHz @ 1000); the receive clock comes from the PHY. */
	mmio_set32(BCMGENET_REG(unit, EXT_RGMII_OOB_CTRL), RGMII_LINK);

	u32 reg = mmio_read32(BCMGENET_REG(unit, UMAC_CMD));
	reg &= ~((u32)(CMD_SPEED_MASK << CMD_SPEED_SHIFT) | CMD_HD_EN |
			 CMD_RX_PAUSE_IGNORE | CMD_TX_PAUSE_IGNORE | CMD_LCL_LOOP_EN);
	reg |= cmd_bits;
	if (reg & CMD_SW_RESET)
	{
		/* First link-up since the UMAC reset: release the MAC, then bring
		 * the datapath up. This is the only place TX/RX are enabled, so a
		 * start with no cable leaves the MAC safely in reset. */
		reg &= ~(u32)CMD_SW_RESET;
		mmio_write32(reg, BCMGENET_REG(unit, UMAC_CMD));
		delay_us(2);
		reg |= CMD_TX_EN | CMD_RX_EN;
	}
	mmio_write32(reg, BCMGENET_REG(unit, UMAC_CMD));

	return GENET_OK;
}

/* Link lost: drop the RGMII link indication. TX/RX stay enabled so the
 * datapath resumes as soon as the link returns. */
void bcmgenet_mac_link_down(struct GenetUnit *unit)
{
	mmio_clear32(BCMGENET_REG(unit, EXT_RGMII_OOB_CTRL), RGMII_LINK);
}

static inline void bcmgenet_set_mdf_addr(struct GenetUnit *unit, const u8 *addr, u8 *i)
{
	mmio_write32(((u32)addr[0] << 8) | (u32)addr[1], unit->genetBase + UMAC_MDF_ADDR + (*i * 4));
	mmio_write32(((u32)addr[2] << 24) | ((u32)addr[3] << 16) | ((u32)addr[4] << 8) | (u32)addr[5],
				 unit->genetBase + UMAC_MDF_ADDR + ((*i + 1) * 4));
	*i = (u8)(*i + 2U);
}

void bcmgenet_set_rx_mode(struct GenetUnit *unit)
{
	/*
	 * netdev reception policy (NETDEV_CMD_SET_RXFILTER):
	 *  - unit->ndPromisc covers explicit promiscuity, all-multi, and a
	 *    multicast list too long for the MDF slots. The UniMAC has no
	 *    multicast-only accept bit, so CMD_PROMISC is our all-multi — a
	 *    correct superset.
	 *  - otherwise the MDF exact-match filter passes broadcast, own MAC and
	 *    the joined multicast groups (NDCF_MCAST_FILTER).
	 *
	 * Runs on the unit task, the same context as the RX drain and the link
	 * poll, so the UMAC_CMD read-modify-write below needs no lock. RX stays
	 * enabled throughout: slots 0/1 are rewritten with identical values (a
	 * bit-for-bit no-op, so unicast/broadcast never blink) and the enable
	 * mask is written last, after the addresses it covers.
	 */
	u32 reg = mmio_read32(BCMGENET_REG(unit, UMAC_CMD));
	if (unit->ndPromisc)
	{
		Kprintf("[genet] %s: Enabling promiscuous mode\n", __func__);
		reg |= CMD_PROMISC;
		mmio_write32(reg, BCMGENET_REG(unit, UMAC_CMD));
		mmio_write32(0, BCMGENET_REG(unit, UMAC_MDF_CTRL));
		return;
	}

	Kprintf("[genet] %s: Setting filtered RX mode (broadcast + own MAC + %lu multicast)\n",
			__func__, (ULONG)unit->ndMcastCount);

	reg &= ~CMD_PROMISC;
	mmio_write32(reg, BCMGENET_REG(unit, UMAC_CMD));

	/* update MDF filter */
	u8 i = 0;
	static const u8 broadcast[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	bcmgenet_set_mdf_addr(unit, broadcast, &i);
	bcmgenet_set_mdf_addr(unit, unit->currentMacAddress, &i);
	for (UWORD m = 0; m < unit->ndMcastCount; m++)
		bcmgenet_set_mdf_addr(unit, unit->ndMcastList[m], &i);

	/* Enable filters: slot k occupies MDF_CTRL bit MAX_MDF_FILTER-1-k, so the
	 * filters in use light the top nfilter bits. Slots past them keep stale
	 * addresses, masked off. */
	u32 nfilter = 2U + unit->ndMcastCount;
	reg = GENMASK(MAX_MDF_FILTER - 1, MAX_MDF_FILTER - nfilter);
	mmio_write32(reg, BCMGENET_REG(unit, UMAC_MDF_CTRL));
}
