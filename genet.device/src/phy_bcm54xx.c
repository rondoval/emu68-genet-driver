// SPDX-License-Identifier: GPL-2.0+
/*
 * Broadcom BCM54xx vendor register access and bring-up. Ported from Linux
 * drivers/net/phy/bcm-phy-lib.c and broadcom.c.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <debug.h>
#include <errors.h>
#include <types.h>
#include <device.h>

#include <genet/phy.h>
#include <genet/phy_priv.h>
#include <genet/mii.h>
#include <genet/brcmphy.h>

/*
 * Broadcom BCM54xx vendor register access.
 *
 * These reach private register windows layered on top of the standard MII set,
 * so they must only ever be issued to a Broadcom PHY — see phy_is_bcm54xx().
 * Ported from Linux drivers/net/phy/bcm-phy-lib.c.
 */

static s32 bcm54xx_auxctl_read(struct phy_device *phydev, u16 regnum)
{
	/* The register must be written to both the Shadow Register Select and
	 * the Shadow Read Register Selector */
	mdio_write(phydev, MII_BCM54XX_AUX_CTL,
			   (u16)(MII_BCM54XX_AUXCTL_SHDWSEL_MASK |
					 (regnum << MII_BCM54XX_AUXCTL_SHDWSEL_READ_SHIFT)));
	return mdio_read(phydev, MII_BCM54XX_AUX_CTL);
}

static s32 bcm54xx_auxctl_write(struct phy_device *phydev, u16 regnum, u16 val)
{
	/* The shadow select rides in the low bits of the written value. */
	return mdio_write(phydev, MII_BCM54XX_AUX_CTL, (u16)(regnum | val));
}

static s32 bcm_phy_read_shadow(struct phy_device *phydev, u16 shadow)
{
	mdio_write(phydev, MII_BCM54XX_SHD, MII_BCM54XX_SHD_VAL(shadow));
	s32 ret = mdio_read(phydev, MII_BCM54XX_SHD);
	if (ret < 0)
		return ret;

	return MII_BCM54XX_SHD_DATA(ret);
}

static s32 bcm_phy_write_shadow(struct phy_device *phydev, u16 shadow, u16 val)
{
	return mdio_write(phydev, MII_BCM54XX_SHD,
					  (u16)(MII_BCM54XX_SHD_WRITE | MII_BCM54XX_SHD_VAL(shadow) |
							MII_BCM54XX_SHD_DATA(val)));
}

#ifdef DEBUG
/* Only needed for the post-config read-back below: the RMW path uses
 * bcm_phy_modify_exp(), which cannot reuse this (it resets EXP_SEL). */
static s32 bcm_phy_read_exp(struct phy_device *phydev, u16 reg)
{
	s32 ret = mdio_write(phydev, MII_BCM54XX_EXP_SEL, reg);
	if (ret < 0)
		return ret;

	ret = mdio_read(phydev, MII_BCM54XX_EXP_DATA);

	/* Restore default value.  It's O.K. if this write fails. */
	mdio_write(phydev, MII_BCM54XX_EXP_SEL, 0);

	return ret;
}
#endif

static s32 bcm_phy_modify_exp(struct phy_device *phydev, u16 reg, u16 mask, u16 set)
{
	s32 ret = mdio_write(phydev, MII_BCM54XX_EXP_SEL, reg);
	if (ret < 0)
		return ret;

	ret = mdio_read(phydev, MII_BCM54XX_EXP_DATA);
	if (ret < 0)
		return ret;

	u16 new_val = (u16)((ret & ~mask) | set);
	if (new_val == (u16)ret)
		return 0;

	return mdio_write(phydev, MII_BCM54XX_EXP_DATA, new_val);
}

/* The vendor register windows above are Broadcom-specific; writing them to a
 * foreign PHY would poke unrelated registers. OUI_4 covers the BCM54210E /
 * BCM54213PE family, i.e. the PHY fitted to the Pi 4 B and CM4. */
BOOL phy_is_bcm54xx(const struct phy_device *phydev)
{
	return (phydev->phy_id & PHY_BCM_OUI_MASK) == PHY_BCM_OUI_4;
}

/*
 * Program the PHY's internal RGMII clock delays for the configured interface.
 * Ported from Linux bcm54xx_config_clock_delay().
 *
 * RGMII needs ~2 ns of clock-to-data skew in each direction, contributed
 * exactly once. The phy-mode names the PHY's share and the MAC covers the
 * rest, so this must be kept consistent with the MAC-side ID_MODE_DIS bit in
 * bcmgenet_mii_config(). For the Pi's "rgmii-rxid": the PHY delays RXC, and
 * the MAC delays TXC.
 */
static s32 bcm54xx_config_clock_delay(struct phy_device *phydev)
{
	/* PHY's internal RX clock delay */
	s32 val = bcm54xx_auxctl_read(phydev, MII_BCM54XX_AUXCTL_SHDWSEL_MISC);
	if (val < 0)
		return val;

	u16 misc = (u16)val | MII_BCM54XX_AUXCTL_MISC_WREN;
	if (phydev->interface == PHY_INTERFACE_MODE_RGMII ||
		phydev->interface == PHY_INTERFACE_MODE_RGMII_TXID)
		/* Disable RGMII RXC-RXD skew */
		misc &= (u16)~MII_BCM54XX_AUXCTL_SHDWSEL_MISC_RGMII_SKEW_EN;
	if (phydev->interface == PHY_INTERFACE_MODE_RGMII_ID ||
		phydev->interface == PHY_INTERFACE_MODE_RGMII_RXID)
		/* Enable RGMII RXC-RXD skew */
		misc |= MII_BCM54XX_AUXCTL_SHDWSEL_MISC_RGMII_SKEW_EN;

	s32 ret = bcm54xx_auxctl_write(phydev, MII_BCM54XX_AUXCTL_SHDWSEL_MISC, misc);
	if (ret < 0)
		return ret;

	/* PHY's internal TX clock delay */
	val = bcm_phy_read_shadow(phydev, BCM54810_SHD_CLK_CTL);
	if (val < 0)
		return val;

	u16 clk = (u16)val;
	if (phydev->interface == PHY_INTERFACE_MODE_RGMII ||
		phydev->interface == PHY_INTERFACE_MODE_RGMII_RXID)
		/* Disable internal TX clock delay */
		clk &= (u16)~BCM54810_SHD_CLK_CTL_GTXCLK_EN;
	if (phydev->interface == PHY_INTERFACE_MODE_RGMII_ID ||
		phydev->interface == PHY_INTERFACE_MODE_RGMII_TXID)
		/* Enable internal TX clock delay */
		clk |= BCM54810_SHD_CLK_CTL_GTXCLK_EN;

	return bcm_phy_write_shadow(phydev, BCM54810_SHD_CLK_CTL, clk);
}

/*
 * Broadcom-specific PHY bring-up, mirroring the subset of Linux's
 * bcm54xx_config_init() that applies here. Must run after the PHY soft reset,
 * which would otherwise clear these shadows.
 */
void bcm54xx_config_init(struct phy_device *phydev)
{
	s32 ret = bcm54xx_config_clock_delay(phydev);
	if (ret < 0)
		Kprintf("[genet] %s: RGMII clock delay config failed: %ld\n", __func__, ret);

	/* Disable AutogrEEEn: the PHY would otherwise enter low-power idle on its
	 * own, with no MAC involvement. We implement no EEE, so leaving the PHY to
	 * power-manage the link behind the MAC's back risks idle->active glitches. */
	ret = bcm_phy_modify_exp(phydev, BCM54XX_TOP_MISC_MII_BUF_CNTL0,
							 BCM54XX_MII_BUF_CNTL0_AUTOGREEEN_EN, 0);
	if (ret < 0)
		Kprintf("[genet] %s: AutogrEEEn disable failed: %ld\n", __func__, ret);

#ifdef DEBUG
	/* Read back: a bad shadow select or a missing MISC_WREN makes the writes
	 * above no-ops, which is indistinguishable from "nothing changed". */
	Kprintf("[genet] %s: AUXCTL MISC=0x%04lx CLK_CTL=0x%04lx MII_BUF_CNTL0=0x%04lx\n", __func__,
			(ULONG)bcm54xx_auxctl_read(phydev, MII_BCM54XX_AUXCTL_SHDWSEL_MISC),
			(ULONG)bcm_phy_read_shadow(phydev, BCM54810_SHD_CLK_CTL),
			(ULONG)bcm_phy_read_exp(phydev, BCM54XX_TOP_MISC_MII_BUF_CNTL0));
#endif
}
