/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2011 Freescale Semiconductor, Inc.
 *	Andy Fleming <afleming@gmail.com>
 *
 * This file pretty much stolen from Linux's mii.h/ethtool.h/phy.h
 */

#ifndef _PHY_H
#define _PHY_H

#include <types.h>
#include <bits.h>
#include <genet/phy_interface.h>
#include <genet/ethtool.h>

struct GenetUnit;

/* MII_STAT1000 masks */
#define PHY_1000BTSR_1000FD BIT(11)
#define PHY_1000BTSR_1000HD BIT(10)

#define PHY_DEFAULT_FEATURES (SUPPORTED_Autoneg | \
							  SUPPORTED_TP |      \
							  SUPPORTED_MII)

/* Symmetric pause. There is no BMSR bit for pause capability — it is a property
 * of the MAC, not the PHY — so it is asserted here and negotiated through the
 * MII_ADVERTISE/MII_LPA pause bits like any other capability. */
#define PHY_PAUSE_FEATURES (SUPPORTED_Pause | SUPPORTED_Asym_Pause)

#define PHY_10BT_FEATURES (SUPPORTED_10baseT_Half | \
						   SUPPORTED_10baseT_Full)

#define PHY_100BT_FEATURES (SUPPORTED_100baseT_Half | \
							SUPPORTED_100baseT_Full)

#define PHY_1000BT_FEATURES (SUPPORTED_1000baseT_Half | \
							 SUPPORTED_1000baseT_Full)

#define PHY_BASIC_FEATURES (PHY_10BT_FEATURES |  \
							PHY_100BT_FEATURES | \
							PHY_DEFAULT_FEATURES)

#define PHY_GBIT_FEATURES (PHY_BASIC_FEATURES |  \
						   PHY_1000BT_FEATURES | \
						   PHY_PAUSE_FEATURES)

struct phy_device
{
	struct GenetUnit *unit;

	/* forced speed & duplex (no autoneg)
	 * partner speed & duplex & pause (autoneg)
	 */
	u16 speed;
	u8 duplex;

	/* The most recently read link state */
	BOOL link;
	phy_interface_t interface;

	/* Negotiated flow control, resolved by genphy_read_pause() per 802.3
	 * Annex 31B. Not the advertisement — the outcome — so the MAC can program
	 * CMD_RX_PAUSE_IGNORE / CMD_TX_PAUSE_IGNORE without touching MDIO. */
	BOOL pause_rx;
	BOOL pause_tx;

	u32 features;
	u32 advertising;
	u32 supported;

	u8 autoneg;
	u8 addr;
	u32 phy_id;
};

/**
 * phy_connect() - Creates a PHY device for the Ethernet interface
 * Creates a PHY device for the PHY at the given address, if one doesn't exist
 * already, and associates it with the Ethernet device.
 * The function may be called with addr <= 0, in this case addr value is ignored
 * and the bus is scanned to detect a PHY.  Scanning should only be used if only
 * one PHY is expected to be present on the MDIO bus, otherwise it is undefined
 * which PHY is returned.
 *
 * @dev:	Ethernet device to associate to the PHY
 * @interface:	type of MAC-PHY interface
 * @return: pointer to phy_device if a PHY is found, or NULL otherwise
 */
struct phy_device *phy_create(struct GenetUnit *dev, phy_interface_t interface);

s32 phy_config(struct phy_device *phydev);
/* Non-blocking link/speed/duplex/pause refresh. @polling selects how a latched
 * link drop is treated: TRUE (periodic poll) keeps it so short drops are seen,
 * FALSE (link interrupt) reads through it for the current state. */
s32 genphy_read_status(struct phy_device *phydev, BOOL polling);
void phy_destroy(struct phy_device *phydev);
s32 genphy_config_aneg(struct phy_device *phydev);

#endif
