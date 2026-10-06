/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Broadcom BCM54xx PHY register definitions.
 *
 * This file pretty much stolen from Linux's include/linux/brcmphy.h — only the
 * subset genet.device needs to reach the vendor register windows (RGMII clock
 * delay and autonomous EEE) is reproduced here.
 *
 * The BCM54xx exposes three private register spaces on top of the standard MII
 * set, each reached through a different select/data dance:
 *   - AUXCTL  (reg 0x18)         : auxiliary control shadows
 *   - SHADOW  (reg 0x1c)         : "shadow" registers
 *   - EXP     (reg 0x17 + 0x15)  : expansion registers
 */

#ifndef _BRCMPHY_H
#define _BRCMPHY_H

#include <bits.h>

/* Auxiliary control register (0x18) and its MISC shadow */
#define MII_BCM54XX_AUX_CTL 0x18
#define MII_BCM54XX_AUXCTL_SHDWSEL_MASK 0x0007
#define MII_BCM54XX_AUXCTL_SHDWSEL_READ_SHIFT 12
#define MII_BCM54XX_AUXCTL_SHDWSEL_MISC 0x07
#define MII_BCM54XX_AUXCTL_SHDWSEL_MISC_RGMII_SKEW_EN 0x0100
/* Write-enable: without this bit a MISC shadow write is silently discarded */
#define MII_BCM54XX_AUXCTL_MISC_WREN 0x8000

/* Shadow register access (0x1c) */
#define MII_BCM54XX_SHD 0x1c
#define MII_BCM54XX_SHD_WRITE 0x8000
#define MII_BCM54XX_SHD_VAL(x) (((x) & 0x1f) << 10)
#define MII_BCM54XX_SHD_DATA(x) (((x) & 0x3ff) << 0)

/* Shadow 0x03: clock control — PHY-internal transmit clock (GTXCLK) delay */
#define BCM54810_SHD_CLK_CTL 0x3
#define BCM54810_SHD_CLK_CTL_GTXCLK_EN BIT(9)

/* Expansion register access (select 0x17, data 0x15) */
#define MII_BCM54XX_EXP_DATA 0x15
#define MII_BCM54XX_EXP_SEL 0x17
#define MII_BCM54XX_EXP_SEL_TOP 0x0d00 /* TOP_MISC expansion select */

/* TOP_MISC: autonomous EEE ("AutogrEEEn"). Disabled so the MAC owns LPI. */
#define BCM54XX_TOP_MISC_MII_BUF_CNTL0 (MII_BCM54XX_EXP_SEL_TOP + 0x00)
#define BCM54XX_MII_BUF_CNTL0_AUTOGREEEN_EN BIT(0)

/* PHY identification. OUI_4 covers BCM54210E and BCM54213PE — the latter is
 * the PHY on the Raspberry Pi 4 B and CM4. */
#define PHY_BCM_OUI_MASK 0xfffffc00
#define PHY_BCM_OUI_4 0x600d8400

#endif /* _BRCMPHY_H */
