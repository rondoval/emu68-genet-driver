// SPDX-License-Identifier: GPL-2.0+
/*
 * Generic PHY management. Ported from U-Boot's phylib (drivers/net/phy/phy.c),
 * which is itself derived from Linux's phy_device.c. The MDIO transport and the
 * Broadcom vendor layer live in phy_mdio.c and phy_bcm54xx.c.
 *
 * Copyright 2011 Freescale Semiconductor, Inc.
 * author Andy Fleming
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
#include <errors.h>
#include <iomem.h>
#include <timing.h>
#include <types.h>
#include <device.h>

#include <genet/phy.h>
#include <genet/phy_priv.h>
#include <genet/mii.h>
#include <genet/brcmphy.h>

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
 *
 * The blind write is what disables autonegotiation: BMCR_ANENABLE is simply not
 * among the bits set. BMCR_ANRESTART must stay out of it too — it only
 * self-clears while autonegotiation is enabled, and genphy_update_link() reads
 * a set ANRESTART as "negotiating, link not yet meaningful", which would pin a
 * forced link to down forever.
 */
static s32 genphy_setup_forced(struct phy_device *phydev)
{
	Kprintf("[genet] %s: phy=%ld speed=%lu duplex=%lu\n", __func__, phydev->addr, (ULONG)phydev->speed, (ULONG)phydev->duplex);
	u16 ctl = 0;

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
 * @polling: TRUE from the periodic poll, FALSE from the link interrupt
 *
 * Description: Update phydev->link to reflect the current link state.
 *   Never blocks: this runs from the unit task, so it must not wait for
 *   autonegotiation to finish (a stalled poll would stall RX and TX with it).
 *   An incomplete negotiation simply reports link down; the next poll picks it
 *   up. Mirrors Linux genphy_update_link().
 */
static s32 genphy_update_link(struct phy_device *phydev, BOOL polling)
{
	s32 bmcr = mdio_read(phydev, MII_BMCR);
	if (bmcr < 0)
		return bmcr;

	/* Autoneg is being restarted, so BMSR is meaningless — report down. */
	if (bmcr & BMCR_ANRESTART)
	{
		phydev->link = FALSE;
		return 0;
	}

	/*
	 * BMSR_LSTATUS latches low, so a drop since the previous read is still
	 * visible in the next one. Which of the two readings we want depends on why
	 * we are here:
	 *
	 *   polling  — take the latched value. It is the only way a drop shorter
	 *              than the poll period is ever seen at all.
	 *   interrupt — discard the latch and read again for the current state.
	 *              The interrupt already reported that something changed; a
	 *              drop that has since recovered must not be published as a
	 *              link-down the next poll would have to undo.
	 *
	 * Either way, a link that was already down needs the second read: the first
	 * carries nothing but old news.
	 */
	s32 status;
	if (!polling || !phydev->link)
	{
		status = mdio_read(phydev, MII_BMSR);
		if (status < 0)
			return status;
		if (status & BMSR_LSTATUS)
			goto done;
	}

	status = mdio_read(phydev, MII_BMSR);
	if (status < 0)
		return status;

done:
	phydev->link = (status & BMSR_LSTATUS) ? TRUE : FALSE;

	/* Autoneg may have restarted with ANEGCOMPLETE already cleared but
	 * LSTATUS not yet — do not report a link we cannot describe. */
	if (phydev->autoneg == AUTONEG_ENABLE && !(status & BMSR_ANEGCOMPLETE))
		phydev->link = FALSE;

	return 0;
}

/*
 * Generic function which updates the speed and duplex.  If
 * autonegotiation is enabled, it uses the AND of the link
 * partner's advertised capabilities and our advertised
 * capabilities.  If autonegotiation is disabled, we use the
 * appropriate bits in the control register.
 *
 * Ported from U-Boot's genphy_parse_link().
 */
static s32 genphy_parse_link(struct phy_device *phydev)
{
	KprintfT("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	s32 mii_reg = mdio_read(phydev, MII_BMSR);
	if (mii_reg < 0)
		return mii_reg;

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
			s32 ctrl1000 = mdio_read(phydev, MII_CTRL1000);
			if (ctrl1000 < 0)
			{
				Kprintf("[genet] %s: Could not read MII_CTRL1000. Ignoring gigabit capability\n", __func__);
				gblpa = 0;
			}
			else
			{
				gblpa &= ctrl1000 << 2;
			}
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

		if (estatus & (ESTATUS_1000_TFULL | ESTATUS_1000_THALF))
		{
			phydev->speed = SPEED_1000;
			if (estatus & ESTATUS_1000_TFULL)
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

	/* Vendor bring-up first: phy_create() has just soft-reset the PHY, which
	 * clears the shadow registers these settings live in. Nothing below
	 * resets it again (genphy_config_aneg only sets BMCR_ANRESTART). */
	if (phy_is_bcm54xx(phydev))
		bcm54xx_config_init(phydev);

	/* Everything below is discovered from BMSR/ESTATUS; the pause bits are not,
	 * because pause capability is a property of the MAC and has no status bit
	 * to read. They are asserted here so the mask at the end of this function
	 * does not strip an advertisement the driver deliberately made. */
	u32 features = (SUPPORTED_TP | SUPPORTED_MII | PHY_PAUSE_FEATURES);

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
	}

	phydev->supported &= features;
	phydev->advertising &= features;

	return genphy_config_aneg(phydev);
}

/*
 * Resolve negotiated flow control from the two advertisements, per 802.3 Annex 31B.
 */
static s32 genphy_read_pause(struct phy_device *phydev)
{
	phydev->pause_rx = FALSE;
	phydev->pause_tx = FALSE;

	/* Nothing was negotiated, so nothing is agreed. */
	if (phydev->autoneg != AUTONEG_ENABLE)
		return 0;

	s32 adv_read = mdio_read(phydev, MII_ADVERTISE);
	if (adv_read < 0)
		return adv_read;
	s32 lpa_read = mdio_read(phydev, MII_LPA);
	if (lpa_read < 0)
		return lpa_read;

	u32 adv = (u32)adv_read;
	u32 lpa = (u32)lpa_read;
	u32 both = adv & lpa;

	if (both & ADVERTISE_PAUSE_CAP)
	{
		/* Symmetric: each end may pause the other. */
		phydev->pause_rx = TRUE;
		phydev->pause_tx = TRUE;
	}
	else if (both & ADVERTISE_PAUSE_ASYM)
	{
		/* Asymmetric: pause flows only towards whichever end asked to be able
		 * to send it. */
		phydev->pause_tx = (lpa & LPA_PAUSE_CAP) ? TRUE : FALSE;
		phydev->pause_rx = (adv & ADVERTISE_PAUSE_CAP) ? TRUE : FALSE;
	}

	return 0;
}

/**
 * genphy_read_status - refresh link, speed, duplex and pause from the PHY
 * @phydev: target phy_device struct
 * @polling: TRUE from the periodic poll, FALSE from the link interrupt
 *
 * The authoritative "what is the link doing right now" read, and the only one
 * the driver uses: both the periodic link poll and the link-change interrupt
 * funnel through here. Non-blocking, so it is safe from the unit task.
 * Speed, duplex and pause are only meaningful while the link is up, so they are
 * read only in that case.
 */
s32 genphy_read_status(struct phy_device *phydev, BOOL polling)
{
	s32 ret = genphy_update_link(phydev, polling);
	if (ret < 0)
		return ret;

	if (!phydev->link)
		return 0;

	ret = genphy_parse_link(phydev);
	if (ret < 0)
		return ret;

	return genphy_read_pause(phydev);
}

static s32 phy_reset(struct phy_device *phydev)
{
	KprintfT("[genet] %s: phy=%ld\n", __func__, phydev->addr);
	u16 timeout = 500;

	s32 reg = mdio_write(phydev, MII_BMCR, BMCR_RESET);
	if (reg < 0)
	{
		Kprintf("[genet] %s: PHY reset failed\n", __func__);
		return reg;
	}

	/*
	 * Poll the control register for the reset bit to go to 0 (it is
	 * auto-clearing).  This should happen within 0.5 seconds per the
	 * IEEE spec.  Every read is checked before it is used as a bitmask: an
	 * error return is negative, and its sign bit sits where BMCR_RESET does.
	 */
	do
	{
		reg = mdio_read(phydev, MII_BMCR);
		if (reg < 0)
		{
			Kprintf("[genet] %s: PHY status read failed\n", __func__);
			return reg;
		}
		if (!(reg & BMCR_RESET))
			break;
		delay_ms(1);
	} while (timeout--);

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
	struct phy_device *phydev = pool_zalloc(dev->metaPool, sizeof(*phydev));
	if (!phydev)
	{
		Kprintf("[genet] %s: Failed to allocate MDIO bus\n", __func__);
		return NULL;
	}
	phydev->features = PHY_GBIT_FEATURES;
	phydev->unit = dev;
	phydev->speed = SPEED_10;
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

			/* Soft reset the PHY. A failure here leaves the vendor shadow
			 * registers at whatever the previous session programmed, which
			 * phy_config()'s bring-up assumes it owns. */
			result = phy_reset(phydev);
			if (result < 0)
			{
				Kprintf("[genet] %s: PHY reset failed: %ld\n", __func__, result);
				pool_free(dev->metaPool, phydev);
				return NULL;
			}

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