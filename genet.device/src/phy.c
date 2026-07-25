// SPDX-License-Identifier: GPL-2.0+
/*
 * Generic PHY Management code
 *
 * Copyright 2011 Freescale Semiconductor, Inc.
 * author Andy Fleming
 *
 * Based loosely off of Linux's PHY Lib
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <debug.h>
#include <bits.h>
#include <byteorder.h>
#include <errors.h>
#include <iomem.h>
#include <timing.h>
#include <types.h>
#include <device.h>

#include <genet/phy.h>
#include <genet/mii.h>

/**
 * wait_for_bit_x()	waits for bit set/cleared in register
 *
 * Function polls register waiting for specific bit(s) change
 * (either 0->1 or 1->0). It can fail under two conditions:
 * - Timeout
 * Function succeeds only if all bits of masked register are set/cleared
 * (depending on set option).
 *
 * @param reg		Register that will be read (using read_x())
 * @param mask		Bit(s) of register that must be active
 * @param set		Selects wait condition (bit set or clear)
 * @param timeout_ms	Timeout (in milliseconds)
 * Return:		0 on success, ETIMEDOUT on failure
 */
static inline s32 wait_for_bit_32(APTR reg,
								  const u32 mask,
								  const BOOL set,
								  const u32 timeout_ms)
{
	// Kprintf("[genet] %s: reg=%ld mask=0x%lx set=%ld timeout=%ld\n", __func__, reg, mask, set, timeout_ms);
	u32 start = le32(*(volatile u32 *)0xf2003004); // TODO get from device tree
	u32 end = start + timeout_ms * 1000u;

	while (1)
	{
		u32 val = mmio_read32(reg);

		if (!set)
			val = ~val;

		if ((val & mask) == mask)
			return 0;

		if (end < le32(*(volatile u32 *)0xf2003004))
			break;

		delay_us(1);
		// schedule();
	}

	// Kprintf("[genet] %s: Timeout (reg=%ld mask=0x%lx wait_set=%ld)\n", __func__, reg, mask, set);
	return -ETIMEDOUT;
}

static inline void mdio_start(struct GenetUnit *unit)
{
	// Kprintf("%s\n", __func__);
	mmio_set32(unit->genetBase + MDIO_CMD, MDIO_START_BUSY);
}

static s32 mdio_write(struct phy_device *phy, u8 reg, u16 value)
{
	// Kprintf("[genet] %s: phy=%ld reg=%ld value=0x%04lx\n", __func__, phy->addr, reg, value);
	struct GenetUnit *unit = phy->unit;

	/* Prepare the read operation */
	u32 val = MDIO_WR | ((u32)phy->addr << MDIO_PMD_SHIFT) |
			  ((u32)reg << MDIO_REG_SHIFT) | ((u32)value & 0xffffU);
	mmio_write32(val, unit->genetBase + MDIO_CMD);

	/* Start MDIO transaction */
	mdio_start(unit);

	return wait_for_bit_32(unit->genetBase + MDIO_CMD,
						   MDIO_START_BUSY, FALSE, 20);
}

static s32 mdio_read(struct phy_device *phy, u8 reg)
{
	// Kprintf("[genet] %s: phy=%ld reg=%ld\n", __func__, phy->addr, reg);
	struct GenetUnit *unit = phy->unit;

	/* Prepare the read operation */
	u32 val = MDIO_RD | ((u32)phy->addr << MDIO_PMD_SHIFT) | ((u32)reg << MDIO_REG_SHIFT);
	mmio_write32(val, unit->genetBase + MDIO_CMD);

	/* Start MDIO transaction */
	mdio_start(unit);

	s32 ret = wait_for_bit_32(unit->genetBase + MDIO_CMD,
							  MDIO_START_BUSY, FALSE, 20);
	if (ret)
		return ret;

	val = mmio_read32(unit->genetBase + MDIO_CMD);
	// Kprintf("[genet] %s: phy=%ld reg=%ld value=0x%lx\n", __func__, phy->addr, reg, val);

	return val & 0xffff;
}

/**
 * genphy_config_advert - sanitize and advertise auto-negotiation parameters
 * @phydev: target phy_device struct
 *
 * Description: Writes MII_ADVERTISE with the appropriate values,
 *   after sanitizing the values to make sure we only advertise
 *   what is supported.  Returns < 0 on error, 0 if the PHY's advertisement
 *   hasn't changed, and > 0 if it has changed.
 */
static s32 genphy_config_advert(struct phy_device *phydev)
{
	KprintfT("[genet] %s: phy=%ld autoneg=%lu\n", __func__, phydev->addr, (ULONG)phydev->autoneg);
	s32 changed = 0;

	/* Only allow advertising what this PHY supports */
	phydev->advertising &= phydev->supported;
	u32 advertise = phydev->advertising;

	/* Setup standard advertisement */
	s32 adv_read = mdio_read(phydev, MII_ADVERTISE);

	if (adv_read < 0)
		return adv_read;

	u16 adv = (u16)adv_read;
	u16 oldadv = adv;

	adv &= (u16)~(ADVERTISE_ALL | ADVERTISE_100BASE4 | ADVERTISE_PAUSE_CAP |
			   ADVERTISE_PAUSE_ASYM);
	if (advertise & ADVERTISED_10baseT_Half)
		adv |= ADVERTISE_10HALF;
	if (advertise & ADVERTISED_10baseT_Full)
		adv |= ADVERTISE_10FULL;
	if (advertise & ADVERTISED_100baseT_Half)
		adv |= ADVERTISE_100HALF;
	if (advertise & ADVERTISED_100baseT_Full)
		adv |= ADVERTISE_100FULL;
	if (advertise & ADVERTISED_Pause)
		adv |= ADVERTISE_PAUSE_CAP;
	if (advertise & ADVERTISED_Asym_Pause)
		adv |= ADVERTISE_PAUSE_ASYM;
	if (advertise & ADVERTISED_1000baseX_Half)
		adv |= ADVERTISE_1000XHALF;
	if (advertise & ADVERTISED_1000baseX_Full)
		adv |= ADVERTISE_1000XFULL;

	if (adv != oldadv)
	{
		s32 err = mdio_write(phydev, MII_ADVERTISE, adv);

		if (err < 0)
			return err;
		changed = 1;
	}

	s32 bmsr = mdio_read(phydev, MII_BMSR);
	if (bmsr < 0)
		return bmsr;

	/* Per 802.3-2008, Section 22.2.4.2.16 Extended status all
	 * 1000Mbits/sec capable PHYs shall have the BMSR_ESTATEN bit set to a
	 * logical 1.
	 */
	if (!(bmsr & BMSR_ESTATEN))
		return changed;

	/* Configure gigabit if it's supported */
	adv_read = mdio_read(phydev, MII_CTRL1000);

	if (adv_read < 0)
		return adv_read;

	adv = (u16)adv_read;
	oldadv = adv;

	adv &= (u16)~(ADVERTISE_1000FULL | ADVERTISE_1000HALF);

	if (phydev->supported & (SUPPORTED_1000baseT_Half |
							 SUPPORTED_1000baseT_Full))
	{
		if (advertise & SUPPORTED_1000baseT_Half)
			adv |= ADVERTISE_1000HALF;
		if (advertise & SUPPORTED_1000baseT_Full)
			adv |= ADVERTISE_1000FULL;
	}

	if (adv != oldadv)
		changed = 1;

	s32 err = mdio_write(phydev, MII_CTRL1000, adv);
	if (err < 0)
		return err;

	return changed;
}

/**
 * genphy_setup_forced - configures/forces speed/duplex from @phydev
 * @phydev: target phy_device struct
 *
 * Description: Configures MII_BMCR to force speed/duplex
 *   to the values in phydev. Assumes that the values are valid.
 */
static s32 genphy_setup_forced(struct phy_device *phydev)
{
	Kprintf("[genet] %s: phy=%ld speed=%lu duplex=%lu\n", __func__, phydev->addr, (ULONG)phydev->speed, (ULONG)phydev->duplex);
	u16 ctl = BMCR_ANRESTART;

	if (phydev->speed == SPEED_1000)
		ctl |= BMCR_SPEED1000;
	else if (phydev->speed == SPEED_100)
		ctl |= BMCR_SPEED100;

	if (phydev->duplex == DUPLEX_FULL)
		ctl |= BMCR_FULLDPLX;

	return mdio_write(phydev, MII_BMCR, ctl);
}

/**
 * genphy_restart_aneg - Enable and Restart Autonegotiation
 * @phydev: target phy_device struct
 */
static s32 genphy_restart_aneg(struct phy_device *phydev)
{
	KprintfT("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	s32 ctl_read = mdio_read(phydev, MII_BMCR);

	if (ctl_read < 0)
		return ctl_read;

	u16 ctl = (u16)ctl_read;

	ctl |= (BMCR_ANENABLE | BMCR_ANRESTART);

	/* Don't isolate the PHY if we're negotiating */
	ctl &= (u16)~BMCR_ISOLATE;

	return mdio_write(phydev, MII_BMCR, ctl);
}

/**
 * genphy_config_aneg - restart auto-negotiation or write BMCR
 * @phydev: target phy_device struct
 *
 * Description: If auto-negotiation is enabled, we configure the
 *   advertising, and then restart auto-negotiation.  If it is not
 *   enabled, then we write the BMCR.
 */
s32 genphy_config_aneg(struct phy_device *phydev)
{
	KprintfT("[genet] %s: phy=%ld autoneg=%lu\n", __func__, phydev->addr, (ULONG)phydev->autoneg);

	if (phydev->autoneg != AUTONEG_ENABLE)
		return genphy_setup_forced(phydev);

	s32 result = genphy_config_advert(phydev);

	if (result < 0) /* error */
		return result;

	if (result == 0)
	{
		/*
		 * Advertisment hasn't changed, but maybe aneg was never on to
		 * begin with?  Or maybe phy was isolated?
		 */
		s32 ctl = mdio_read(phydev, MII_BMCR);

		if (ctl < 0)
			return ctl;

		if (!(ctl & BMCR_ANENABLE) || (ctl & BMCR_ISOLATE))
			result = 1; /* do restart aneg */
	}

	/*
	 * Only restart aneg if we are advertising something different
	 * than we were before.
	 */
	if (result > 0)
		result = genphy_restart_aneg(phydev);

	return result;
}

/**
 * genphy_update_link - update link status in @phydev
 * @phydev: target phy_device struct
 *
 * Description: Update the value in phydev->link to reflect the
 *   current link value.  In order to do this, we need to read
 *   the status register twice, keeping the second value.
 */
static s32 genphy_update_link(struct phy_device *phydev)
{
	KprintfT("[genet] %s: phy=%ld\n", __func__, phydev->addr);

	/*
	 * Wait if the link is up, and autonegotiation is in progress
	 * (ie - we're capable and it's not done)
	 */
	s32 mii_reg = mdio_read(phydev, MII_BMSR);

	/*
	 * If we already saw the link up, and it hasn't gone down, then
	 * we don't need to wait for autoneg again
	 */
	if (phydev->link && mii_reg & BMSR_LSTATUS)
		return 0;

	if ((phydev->autoneg == AUTONEG_ENABLE) &&
		!(mii_reg & BMSR_ANEGCOMPLETE))
	{
		u32 i = 0;

		Kprintf("[genet] %s: Waiting for PHY auto negotiation to complete", __func__);
		while (!(mii_reg & BMSR_ANEGCOMPLETE))
		{
			/*
			 * Timeout reached ?
			 */
			if (i > (CONFIG_PHY_ANEG_TIMEOUT / 50))
			{
				Kprintf(" TIMEOUT !\n");
				phydev->link = FALSE;
				return -ETIMEDOUT;
			}

			if ((i++ % 10) == 0)
			{
				Kprintf(".");
			}

			mii_reg = mdio_read(phydev, MII_BMSR);
			delay_ms(50);
		}
		Kprintf(" done\n");
		phydev->link = TRUE;
	}
	else
	{
		/* Read the link a second time to clear the latched state */
		mii_reg = mdio_read(phydev, MII_BMSR);

		if (mii_reg & BMSR_LSTATUS)
			phydev->link = TRUE;
		else
			phydev->link = FALSE;
	}

	return 0;
}

/*
 * Generic function which updates the speed and duplex.  If
 * autonegotiation is enabled, it uses the AND of the link
 * partner's advertised capabilities and our advertised
 * capabilities.  If autonegotiation is disabled, we use the
 * appropriate bits in the control register.
 *
 * Stolen from Linux's mii.c and phy_device.c
 */
static s32 genphy_parse_link(struct phy_device *phydev)
{
	KprintfT("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	s32 mii_reg = mdio_read(phydev, MII_BMSR);

	/* We're using autonegotiation */
	if (phydev->autoneg == AUTONEG_ENABLE)
	{
		s32 gblpa = 0;

		/* Check for gigabit capability */
		if (phydev->supported & (SUPPORTED_1000baseT_Full |
								 SUPPORTED_1000baseT_Half))
		{
			/* We want a list of states supported by
			 * both PHYs in the link
			 */
			gblpa = mdio_read(phydev, MII_STAT1000);
			if (gblpa < 0)
			{
				Kprintf("[genet] %s: Could not read MII_STAT1000. Ignoring gigabit capability\n", __func__);
				gblpa = 0;
			}
			gblpa &= mdio_read(phydev, MII_CTRL1000) << 2;
		}

		/* Set the baseline so we only have to set them
		 * if they're different
		 */
		phydev->speed = SPEED_10;
		phydev->duplex = DUPLEX_HALF;

		/* Check the gigabit fields */
		if ((u32)gblpa & (PHY_1000BTSR_1000FD | PHY_1000BTSR_1000HD))
		{
			phydev->speed = SPEED_1000;

			if ((u32)gblpa & PHY_1000BTSR_1000FD)
				phydev->duplex = DUPLEX_FULL;

			/* We're done! */
			return 0;
		}

		s32 lpa_read = mdio_read(phydev, MII_ADVERTISE);
		if (lpa_read < 0)
			return lpa_read;
		u32 lpa = (u32)lpa_read;

		lpa_read = mdio_read(phydev, MII_LPA);
		if (lpa_read < 0)
			return lpa_read;
		lpa &= (u32)lpa_read;

		if (lpa & (LPA_100FULL | LPA_100HALF))
		{
			phydev->speed = SPEED_100;

			if (lpa & LPA_100FULL)
				phydev->duplex = DUPLEX_FULL;
		}
		else if (lpa & LPA_10FULL)
		{
			phydev->duplex = DUPLEX_FULL;
		}

		/*
		 * Extended status may indicate that the PHY supports
		 * 1000BASE-T/X even though the 1000BASE-T registers
		 * are missing. In this case we can't tell whether the
		 * peer also supports it, so we only check extended
		 * status if the 1000BASE-T registers are actually
		 * missing.
		 */
		u32 estatus = 0;
		if ((mii_reg & BMSR_ESTATEN) && !(mii_reg & BMSR_ERCAP))
		{
			s32 estatus_read = mdio_read(phydev, MII_ESTATUS);
			if (estatus_read < 0)
				return estatus_read;
			estatus = (u32)estatus_read;
		}

		if (estatus & (ESTATUS_1000_XFULL | ESTATUS_1000_XHALF |
					   ESTATUS_1000_TFULL | ESTATUS_1000_THALF))
		{
			phydev->speed = SPEED_1000;
			if (estatus & (ESTATUS_1000_XFULL | ESTATUS_1000_TFULL))
				phydev->duplex = DUPLEX_FULL;
		}
	}
	else
	{
		s32 bmcr_read = mdio_read(phydev, MII_BMCR);
		if (bmcr_read < 0)
			return bmcr_read;
		u32 bmcr = (u32)bmcr_read;

		phydev->speed = SPEED_10;
		phydev->duplex = DUPLEX_HALF;

		if (bmcr & BMCR_FULLDPLX)
			phydev->duplex = DUPLEX_FULL;

		if (bmcr & BMCR_SPEED1000)
			phydev->speed = SPEED_1000;
		else if (bmcr & BMCR_SPEED100)
			phydev->speed = SPEED_100;
	}

	return 0;
}

s32 phy_config(struct phy_device *phydev)
{
	KprintfT("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	u32 features = (SUPPORTED_TP | SUPPORTED_MII | SUPPORTED_AUI | SUPPORTED_FIBRE |
					SUPPORTED_BNC);

	/* Do we support autonegotiation? */
	s32 val = mdio_read(phydev, MII_BMSR);

	if (val < 0)
		return val;

	if (val & BMSR_ANEGCAPABLE)
		features |= SUPPORTED_Autoneg;

	if (val & BMSR_100FULL)
		features |= SUPPORTED_100baseT_Full;
	if (val & BMSR_100HALF)
		features |= SUPPORTED_100baseT_Half;
	if (val & BMSR_10FULL)
		features |= SUPPORTED_10baseT_Full;
	if (val & BMSR_10HALF)
		features |= SUPPORTED_10baseT_Half;

	if (val & BMSR_ESTATEN)
	{
		val = mdio_read(phydev, MII_ESTATUS);

		if (val < 0)
			return val;

		if (val & ESTATUS_1000_TFULL)
			features |= SUPPORTED_1000baseT_Full;
		if (val & ESTATUS_1000_THALF)
			features |= SUPPORTED_1000baseT_Half;
		if (val & ESTATUS_1000_XFULL)
			features |= SUPPORTED_1000baseX_Full;
		if (val & ESTATUS_1000_XHALF)
			features |= SUPPORTED_1000baseX_Half;
	}

	phydev->supported &= features;
	phydev->advertising &= features;

	return genphy_config_aneg(phydev);
}

s32 phy_startup(struct phy_device *phydev)
{
	KprintfT("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	s32 ret = genphy_update_link(phydev);
	if (ret)
		return ret;

	return genphy_parse_link(phydev);
}

static s32 phy_reset(struct phy_device *phydev)
{
	KprintfT("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	u16 timeout = 500;

	if (phydev->flags & PHY_FLAG_BROKEN_RESET)
		return 0;

	s32 reg = mdio_write(phydev, MII_BMCR, BMCR_RESET);
	if (reg < 0)
	{
		Kprintf("[genet] %s: PHY reset failed\n", __func__);
		return reg;
	}

	/*
	 * Poll the control register for the reset bit to go to 0 (it is
	 * auto-clearing).  This should happen within 0.5 seconds per the
	 * IEEE spec.
	 */
	reg = mdio_read(phydev, MII_BMCR);
	while ((reg & BMCR_RESET) && timeout--)
	{
		reg = mdio_read(phydev, MII_BMCR);

		if (reg < 0)
		{
			Kprintf("[genet] %s: PHY status read failed\n", __func__);
			return reg;
		}
		delay_ms(1);
	}

	if (reg & BMCR_RESET)
	{
		Kprintf("[genet] %s: PHY reset timed out\n", __func__);
		return -ETIMEDOUT;
	}

	return 0;
}

/**
 * get_phy_id - reads the specified addr for its ID.
 * @phydev: the target MII bus
 *
 * Description: Reads the ID registers of the PHY at @addr on the
 *   @bus, stores it in @phy_id and returns zero on success.
 */
static s32 get_phy_id(struct phy_device *phydev)
{
	KprintfT("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	/*
	 * Grab the bits from PHYIR1, and put them
	 * in the upper half
	 */
	s32 phy_reg = mdio_read(phydev, MII_PHYSID1);

	if (phy_reg < 0)
		return phy_reg;

	phydev->phy_id = ((u32)phy_reg & 0xffffU) << 16;

	/* Grab the bits from PHYIR2, and put them in the lower half */
	phy_reg = mdio_read(phydev, MII_PHYSID2);

	if (phy_reg < 0)
		return phy_reg;

	phydev->phy_id |= (u32)phy_reg & 0xffffU;

	return 0;
}

struct phy_device *phy_create(struct GenetUnit *dev, phy_interface_t interface)
{
	KprintfT("[genet] %s: base=0x%lx phyaddr=%ld\n", __func__, dev->genetBase, dev->phyaddr);
	struct phy_device *phydev = pool_alloc(dev->metaPool, sizeof(*phydev));
	if (!phydev)
	{
		Kprintf("[genet] %s: Failed to allocate MDIO bus\n", __func__);
		return NULL;
	}
	phydev->features = PHY_GBIT_FEATURES | SUPPORTED_MII |
					   SUPPORTED_AUI | SUPPORTED_FIBRE |
					   SUPPORTED_BNC;
	phydev->unit = dev;
	phydev->duplex = DUPLEX_HALF;
	phydev->link = FALSE;
	phydev->interface = PHY_INTERFACE_MODE_NA;
	phydev->autoneg = AUTONEG_ENABLE;
	phydev->addr = dev->phyaddr;
	phydev->advertising = phydev->features;
	phydev->supported = phydev->features;

	s32 result = get_phy_id(phydev);
	if (result == 0)
	{
		if (phydev->phy_id != 0 && (phydev->phy_id & 0x1fffffff) != 0x1fffffff)
		{
			KprintfT("[genet] %s: PHY ID: %08lx\n", __func__, phydev->phy_id);
			phydev->interface = interface;
			/* Soft Reset the PHY */
			phy_reset(phydev);

			return phydev;
		}
	}

	pool_free(dev->metaPool, phydev);
	Kprintf("[genet] %s: Could not get PHY\n", __func__);
	return NULL;
}

void phy_destroy(struct phy_device *phydev)
{
	KprintfT("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	pool_free(phydev->unit->metaPool, phydev);
}