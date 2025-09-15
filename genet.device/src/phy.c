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
#include <proto/exec.h>
#endif

#include <debug.h>
#include <compat.h>
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
static inline int wait_for_bit_32(APTR reg,
								  const ULONG mask,
								  const UBYTE set,
								  const ULONG timeout_ms)
{
	// Kprintf("[genet] %s: reg=%ld mask=0x%lx set=%ld timeout=%ld\n", __func__, reg, mask, set, timeout_ms);
	ULONG val;
	ULONG start = LE32(*(volatile ULONG *)0xf2003004); // TODO get from device tree
	ULONG end = start + timeout_ms * 1000;

	while (1)
	{
		val = readl(reg);

		if (!set)
			val = ~val;

		if ((val & mask) == mask)
			return 0;

		if (end < LE32(*(volatile ULONG *)0xf2003004))
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
	setbits_32(unit->genetBase + MDIO_CMD, MDIO_START_BUSY);
}

static int mdio_write(struct phy_device *phy, int reg, UWORD value)
{
	// Kprintf("[genet] %s: phy=%ld reg=%ld value=0x%04lx\n", __func__, phy->addr, reg, value);
	struct GenetUnit *unit = phy->unit;
	ULONG val;

	/* Prepare the read operation */
	val = MDIO_WR | (phy->addr << MDIO_PMD_SHIFT) |
		  (reg << MDIO_REG_SHIFT) | (0xffff & value);
	writel(val, unit->genetBase + MDIO_CMD);

	/* Start MDIO transaction */
	mdio_start(unit);

	return wait_for_bit_32(unit->genetBase + MDIO_CMD,
						   MDIO_START_BUSY, FALSE, 20);
}

static int mdio_read(struct phy_device *phy, int reg)
{
	// Kprintf("[genet] %s: phy=%ld reg=%ld\n", __func__, phy->addr, reg);
	struct GenetUnit *unit = phy->unit;
	ULONG val;
	int ret;

	/* Prepare the read operation */
	val = MDIO_RD | (phy->addr << MDIO_PMD_SHIFT) | (reg << MDIO_REG_SHIFT);
	writel(val, unit->genetBase + MDIO_CMD);

	/* Start MDIO transaction */
	mdio_start(unit);

	ret = wait_for_bit_32(unit->genetBase + MDIO_CMD,
						  MDIO_START_BUSY, FALSE, 20);
	if (ret)
		return ret;

	val = readl(unit->genetBase + MDIO_CMD);
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
static int genphy_config_advert(struct phy_device *phydev)
{
	Kprintf("[genet] %s: phy=%ld autoneg=%ld\n", __func__, phydev->addr, phydev->autoneg);
	ULONG advertise;
	int oldadv, adv, bmsr;
	int err, changed = 0;

	/* Only allow advertising what this PHY supports */
	phydev->advertising &= phydev->supported;
	advertise = phydev->advertising;

	/* Setup standard advertisement */
	adv = mdio_read(phydev, MII_ADVERTISE);
	oldadv = adv;

	if (adv < 0)
		return adv;

	adv &= ~(ADVERTISE_ALL | ADVERTISE_100BASE4 | ADVERTISE_PAUSE_CAP |
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
		err = mdio_write(phydev, MII_ADVERTISE, adv);

		if (err < 0)
			return err;
		changed = 1;
	}

	bmsr = mdio_read(phydev, MII_BMSR);
	if (bmsr < 0)
		return bmsr;

	/* Per 802.3-2008, Section 22.2.4.2.16 Extended status all
	 * 1000Mbits/sec capable PHYs shall have the BMSR_ESTATEN bit set to a
	 * logical 1.
	 */
	if (!(bmsr & BMSR_ESTATEN))
		return changed;

	/* Configure gigabit if it's supported */
	adv = mdio_read(phydev, MII_CTRL1000);
	oldadv = adv;

	if (adv < 0)
		return adv;

	adv &= ~(ADVERTISE_1000FULL | ADVERTISE_1000HALF);

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

	err = mdio_write(phydev, MII_CTRL1000, adv);
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
static int genphy_setup_forced(struct phy_device *phydev)
{
	Kprintf("[genet] %s: phy=%ld speed=%ld duplex=%ld\n", __func__, phydev->addr, phydev->speed, phydev->duplex);
	int err;
	int ctl = BMCR_ANRESTART;

	if (phydev->speed == SPEED_1000)
		ctl |= BMCR_SPEED1000;
	else if (phydev->speed == SPEED_100)
		ctl |= BMCR_SPEED100;

	if (phydev->duplex == DUPLEX_FULL)
		ctl |= BMCR_FULLDPLX;

	err = mdio_write(phydev, MII_BMCR, ctl);

	return err;
}

/**
 * genphy_restart_aneg - Enable and Restart Autonegotiation
 * @phydev: target phy_device struct
 */
static int genphy_restart_aneg(struct phy_device *phydev)
{
	Kprintf("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	int ctl;

	ctl = mdio_read(phydev, MII_BMCR);

	if (ctl < 0)
		return ctl;

	ctl |= (BMCR_ANENABLE | BMCR_ANRESTART);

	/* Don't isolate the PHY if we're negotiating */
	ctl &= ~(BMCR_ISOLATE);

	ctl = mdio_write(phydev, MII_BMCR, ctl);

	return ctl;
}

/**
 * genphy_config_aneg - restart auto-negotiation or write BMCR
 * @phydev: target phy_device struct
 *
 * Description: If auto-negotiation is enabled, we configure the
 *   advertising, and then restart auto-negotiation.  If it is not
 *   enabled, then we write the BMCR.
 */
static int genphy_config_aneg(struct phy_device *phydev)
{
	Kprintf("[genet] %s: phy=%ld autoneg=%ld\n", __func__, phydev->addr, phydev->autoneg);
	int result;

	if (phydev->autoneg != AUTONEG_ENABLE)
		return genphy_setup_forced(phydev);

	result = genphy_config_advert(phydev);

	if (result < 0) /* error */
		return result;

	if (result == 0)
	{
		/*
		 * Advertisment hasn't changed, but maybe aneg was never on to
		 * begin with?  Or maybe phy was isolated?
		 */
		int ctl = mdio_read(phydev, MII_BMCR);

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
static int genphy_update_link(struct phy_device *phydev)
{
	Kprintf("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	unsigned int mii_reg;

	/*
	 * Wait if the link is up, and autonegotiation is in progress
	 * (ie - we're capable and it's not done)
	 */
	mii_reg = mdio_read(phydev, MII_BMSR);

	/*
	 * If we already saw the link up, and it hasn't gone down, then
	 * we don't need to wait for autoneg again
	 */
	if (phydev->link && mii_reg & BMSR_LSTATUS)
		return 0;

	if ((phydev->autoneg == AUTONEG_ENABLE) &&
		!(mii_reg & BMSR_ANEGCOMPLETE))
	{
		int i = 0;

		Kprintf("[genet] %s: Waiting for PHY auto negotiation to complete", __func__);
		while (!(mii_reg & BMSR_ANEGCOMPLETE))
		{
			/*
			 * Timeout reached ?
			 */
			if (i > (CONFIG_PHY_ANEG_TIMEOUT / 50))
			{
				Kprintf(" TIMEOUT !\n");
				phydev->link = 0;
				return -ETIMEDOUT;
			}

			if ((i++ % 10) == 0)
			{
				Kprintf(".");
			}

			mii_reg = mdio_read(phydev, MII_BMSR);
			delay_us(50 * 1000); /* 50 ms */
		}
		Kprintf(" done\n");
		phydev->link = 1;
	}
	else
	{
		/* Read the link a second time to clear the latched state */
		mii_reg = mdio_read(phydev, MII_BMSR);

		if (mii_reg & BMSR_LSTATUS)
			phydev->link = 1;
		else
			phydev->link = 0;
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
static int genphy_parse_link(struct phy_device *phydev)
{
	Kprintf("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	int mii_reg = mdio_read(phydev, MII_BMSR);

	/* We're using autonegotiation */
	if (phydev->autoneg == AUTONEG_ENABLE)
	{
		ULONG lpa = 0;
		int gblpa = 0;
		ULONG estatus = 0;

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
		if (gblpa & (PHY_1000BTSR_1000FD | PHY_1000BTSR_1000HD))
		{
			phydev->speed = SPEED_1000;

			if (gblpa & PHY_1000BTSR_1000FD)
				phydev->duplex = DUPLEX_FULL;

			/* We're done! */
			return 0;
		}

		lpa = mdio_read(phydev, MII_ADVERTISE);
		lpa &= mdio_read(phydev, MII_LPA);

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
		if ((mii_reg & BMSR_ESTATEN) && !(mii_reg & BMSR_ERCAP))
			estatus = mdio_read(phydev, MII_ESTATUS);

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
		ULONG bmcr = mdio_read(phydev, MII_BMCR);

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

int phy_config(struct phy_device *phydev)
{
	Kprintf("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	int val;
	ULONG features;

	features = (SUPPORTED_TP | SUPPORTED_MII | SUPPORTED_AUI | SUPPORTED_FIBRE |
				SUPPORTED_BNC);

	/* Do we support autonegotiation? */
	val = mdio_read(phydev, MII_BMSR);

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

int phy_startup(struct phy_device *phydev)
{
	Kprintf("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	int ret;

	ret = genphy_update_link(phydev);
	if (ret)
		return ret;

	return genphy_parse_link(phydev);
}

int phy_reset(struct phy_device *phydev)
{
	Kprintf("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	int reg;
	int timeout = 500;

	if (phydev->flags & PHY_FLAG_BROKEN_RESET)
		return 0;

	reg = mdio_write(phydev, MII_BMCR, BMCR_RESET);
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
		delay_us(1000);
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
static int get_phy_id(struct phy_device *phydev)
{
	Kprintf("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	/*
	 * Grab the bits from PHYIR1, and put them
	 * in the upper half
	 */
	int phy_reg = mdio_read(phydev, MII_PHYSID1);

	if (phy_reg < 0)
		return phy_reg;

	phydev->phy_id = (phy_reg & 0xffff) << 16;

	/* Grab the bits from PHYIR2, and put them in the lower half */
	phy_reg = mdio_read(phydev, MII_PHYSID2);

	if (phy_reg < 0)
		return phy_reg;

	phydev->phy_id |= (phy_reg & 0xffff);

	return 0;
}

struct phy_device *phy_create(struct GenetUnit *dev, phy_interface_t interface)
{
	Kprintf("[genet] %s: base=0x%lx phyaddr=%ld\n", __func__, dev->genetBase, dev->phyaddr);
	struct phy_device *phydev;

	phydev = AllocPooled(dev->memoryPool, sizeof(*phydev));
	if (!phydev)
	{
		Kprintf("[genet] %s: Failed to allocate MDIO bus\n", __func__);
		return NULL;
	}
	phydev->features = PHY_GBIT_FEATURES | SUPPORTED_MII |
					   SUPPORTED_AUI | SUPPORTED_FIBRE |
					   SUPPORTED_BNC;
	phydev->unit = dev;
	phydev->duplex = -1;
	phydev->link = 0;
	phydev->interface = PHY_INTERFACE_MODE_NA;
	phydev->autoneg = AUTONEG_ENABLE;
	phydev->addr = dev->phyaddr;
	phydev->advertising = phydev->features;
	phydev->supported = phydev->features;

	int result = get_phy_id(phydev);
	if (result == 0)
	{
		if (phydev->phy_id != 0 && (phydev->phy_id & 0x1fffffff) != 0x1fffffff)
		{
			Kprintf("[genet] %s: PHY ID: %08lx\n", __func__, phydev->phy_id);
			phydev->interface = interface;
			/* Soft Reset the PHY */
			phy_reset(phydev);

			return phydev;
		}
	}

	FreePooled(dev->memoryPool, phydev, sizeof(*phydev));
	Kprintf("[genet] %s: Could not get PHY\n", __func__);
	return NULL;
}

void phy_destroy(struct phy_device *phydev)
{
	Kprintf("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	FreePooled(phydev->unit->memoryPool, phydev, sizeof(*phydev));
}