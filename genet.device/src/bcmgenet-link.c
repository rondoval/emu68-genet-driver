// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * Broadcom GENETv5 — link-state coordinator. Ties the PHY (phy.c), the MAC
 * (bcmgenet-mac.c) and the stack (netdev_api.c) together: read the PHY, push
 * any change out to the MAC, then notify the stack. Runs in the unit-task
 * context, the only one that calls the stack's nso_* callbacks.
 */

#include <types.h>
#include <device.h>
#include <genet/bcmgenet.h>
#include <genet/phy.h>

/*
 * Re-read the PHY and push any change out to the MAC and the stack.
 *
 * Link state is level-triggered, never edge-replayed: the link interrupt only
 * says "something changed", so the PHY is always the source of truth. That
 * matters twice over. GENET v5 (BCM2711) fails to signal link-up at 10 Mbps
 * at all, which is why the periodic poll — not the interrupt — is what makes
 * the state converge; and reading current state means a down->up bounce
 * between two wakeups cannot strand us reporting "down".
 *
 * @polling distinguishes the two callers for the benefit of BMSR's latched
 * link bit — see genphy_update_link().
 */
void bcmgenet_link_poll(struct GenetUnit *unit, BOOL polling)
{
    struct phy_device *phy = unit->phydev;
    if (phy == NULL)
        return;

    BOOL was_up = phy->link;
    u16 was_speed = phy->speed;
    u8 was_duplex = phy->duplex;

    if (genphy_read_status(phy, polling) < 0)
        return;

    if (phy->link == was_up && phy->speed == was_speed && phy->duplex == was_duplex)
        return;

    /* Never advertise a link the MAC could not be programmed for: fold an
     * unusable speed back into "down" so it stays self-consistent and the
     * next good negotiation still reads as a change. */
    if (phy->link && bcmgenet_mac_config(unit) != GENET_OK)
        phy->link = FALSE;

    if (!phy->link)
        bcmgenet_mac_link_down(unit);

    /* Only notifies the stack on a real change; safe to call either way. */
    netdev_link_update(unit, phy->link, FALSE);
}

/*
 * Re-apply forced PHY settings after the PHY has (re)appeared.
 *
 * Only meaningful with autonegotiation off: a PHY that reset — which is what
 * UMAC_IRQ_PHY_DET_R reports — comes back at its power-on defaults, so the
 * forced speed/duplex has to be pushed again. Gating on "link is currently
 * down" keeps it bounce-free: rewriting BMCR on a live forced link would
 * drop it, on a dead one it cannot.
 */
void bcmgenet_phy_refresh_forced(struct GenetUnit *unit)
{
    struct phy_device *phy = unit->phydev;
    if (phy != NULL && phy->autoneg != AUTONEG_ENABLE && !phy->link)
        genphy_config_aneg(phy);
}
