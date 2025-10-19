/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2011 Freescale Semiconductor, Inc.
 *	Andy Fleming <afleming@gmail.com>
 *
 * This file pretty much stolen from Linux's mii.h/ethtool.h/phy.h
 */

#ifndef _PHY_H
#define _PHY_H

#include <exec/types.h>
#include <genet/phy_interface.h>
#include <genet/ethtool.h>

struct GenetUnit;

#define CONFIG_PHY_ANEG_TIMEOUT 6000

/* MDIO registers, BCM2711 */
#define MDIO_START_BUSY BIT(29)
#define MDIO_READ_FAIL BIT(28)
#define MDIO_RD (2 << 26)
#define MDIO_WR BIT(26)
#define MDIO_PMD_SHIFT 21
#define MDIO_PMD_MASK 0x1f
#define MDIO_REG_SHIFT 16
#define MDIO_REG_MASK 0x1f

#define GENET_UMAC_OFF 0x0800
#define MDIO_CMD (GENET_UMAC_OFF + 0x614)

/* MII_STAT1000 masks */
#define PHY_1000BTSR_1000FD 0x0800
#define PHY_1000BTSR_1000HD 0x0400

#define PHY_FLAG_BROKEN_RESET (1 << 0) /* soft reset not supported */

#define PHY_DEFAULT_FEATURES (SUPPORTED_Autoneg | \
							  SUPPORTED_TP |      \
							  SUPPORTED_MII)

#define PHY_10BT_FEATURES (SUPPORTED_10baseT_Half | \
						   SUPPORTED_10baseT_Full)

#define PHY_100BT_FEATURES (SUPPORTED_100baseT_Half | \
							SUPPORTED_100baseT_Full)

#define PHY_1000BT_FEATURES (SUPPORTED_1000baseT_Half | \
							 SUPPORTED_1000baseT_Full)

#define PHY_BASIC_FEATURES (PHY_10BT_FEATURES |  \
							PHY_100BT_FEATURES | \
							PHY_DEFAULT_FEATURES)

#define PHY_GBIT_FEATURES (PHY_BASIC_FEATURES | \
						   PHY_1000BT_FEATURES)

struct phy_device
{
	struct GenetUnit *unit;

	/* forced speed & duplex (no autoneg)
	 * partner speed & duplex & pause (autoneg)
	 */
	int speed;
	int duplex;

	/* The most recently read link state */
	int link;
	phy_interface_t interface;

	ULONG features;
	ULONG advertising;
	ULONG supported;

	int autoneg;
	int addr;
	ULONG phy_id;
	ULONG flags;
};

/**
 * phy_reset() - Resets the specified PHY
 * Issues a reset of the PHY and waits for it to complete
 *
 * @phydev:	PHY to reset
 * @return: 0 if OK, -ve on error
 */
int phy_reset(struct phy_device *phydev);

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

int phy_config(struct phy_device *phydev);
int phy_startup(struct phy_device *phydev);
void phy_destroy(struct phy_device *phydev);
int genphy_config_aneg(struct phy_device *phydev);

#endif
