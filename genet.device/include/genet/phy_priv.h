/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Cross-file interface within the PHY layer: the MDIO transport (phy_mdio.c)
 * and the Broadcom vendor bring-up (phy_bcm54xx.c) that the generic phylib
 * (phy.c) drives. Not part of the driver's public PHY interface, which is
 * <genet/phy.h>.
 */
#ifndef _GENET_PHY_PRIV_H
#define _GENET_PHY_PRIV_H

#include <types.h>

struct phy_device;

/* MDIO transport (phy_mdio.c). Both return a register value (0..0xffff) or a
 * negative errno; the sign is what tells them apart. */
s32 mdio_read(struct phy_device *phy, u8 reg);
s32 mdio_write(struct phy_device *phy, u8 reg, u16 value);

/* Broadcom BCM54xx vendor layer (phy_bcm54xx.c). config_init runs the vendor
 * bring-up (clock delays, AutogrEEEn off); it is a no-op on a non-Broadcom PHY,
 * so the caller need not gate it on phy_is_bcm54xx(). */
BOOL phy_is_bcm54xx(const struct phy_device *phydev);
void bcm54xx_config_init(struct phy_device *phydev);

#endif /* _GENET_PHY_PRIV_H */
