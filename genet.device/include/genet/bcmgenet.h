// SPDX-License-Identifier: GPL-2.0+
#ifndef __BCMGENET_H
#define __BCMGENET_H

#include <types.h>

struct GenetUnit;
struct NetDevTxDesc;

#define BCMGENET_REG(unit, offset) ((unit)->genetBase + (offset))

/* Controller lifecycle. probe/unconfigure own the PHY and the ring
 * bookkeeping; start/stop only run and quiesce the hardware, so a unit can be
 * stopped and started again without being reconfigured. */
u32 bcmgenet_eth_probe(struct GenetUnit *unit);
void bcmgenet_eth_unconfigure(struct GenetUnit *unit);
u32 bcmgenet_gmac_eth_start(struct GenetUnit *unit);
void bcmgenet_gmac_eth_stop(struct GenetUnit *unit);
void bcmgenet_reset_quiesce(struct GenetUnit *unit);
/* Interrupt moderation. The unit holds the settings (coalTxFrames and friends);
 * _valid() tests a candidate set without touching hardware, so a stopped unit
 * can accept one, and _apply() programs whatever the unit currently holds. */
u32 bcmgenet_coalesce_valid(u32 tx_max_coalesced_frames, u32 rx_max_coalesced_frames,
							u32 rx_coalesce_usecs);
void bcmgenet_apply_coalesce(struct GenetUnit *unit);
/* RX moderation by profile (unit task context). _reset() re-derives the mode
 * from the unit's profile, pin and prefs and programs it, quietly: the stack
 * changes profile often. _moderate() is the burst ladder's step, run when an RX
 * pass is about to re-arm its interrupt and unit->rxLadder is set. */
void bcmgenet_rx_moderation_reset(struct GenetUnit *unit);
void bcmgenet_rx_moderate(struct GenetUnit *unit);
void bcmgenet_set_rx_mode(struct GenetUnit *unit); /* Updates PROMISC flag and sets up MDF if possible */

/* Link state -> MAC (unit task context). bcmgenet_mac_config() programs the
 * negotiated speed/duplex and, on the first link-up after reset, releases the
 * UMAC and enables TX/RX. */
u32 bcmgenet_mac_config(struct GenetUnit *unit);
void bcmgenet_mac_link_down(struct GenetUnit *unit);

/* Link-state coordinator (bcmgenet-link.c, unit task context): reads the PHY
 * and pushes any change out to the MAC and the stack. bcmgenet_phy_refresh_forced()
 * re-applies forced speed/duplex after a PHY reset. */
void bcmgenet_link_poll(struct GenetUnit *unit, BOOL polling);
void bcmgenet_phy_refresh_forced(struct GenetUnit *unit);
/* Record the link (up per @up, speed and duplex from the PHY) in
 * unit->linkState and notify whoever holds the unit, on a change or always
 * with @force — which a netdev START needs, since the ABI requires one
 * callback after it whether or not anything changed. */
void bcmgenet_link_publish(struct GenetUnit *unit, BOOL up, BOOL force);

/* RX buffer pool (bcmgenet-rxpool.c, unit task context): one DMA slab of
 * RX_BUF_LENGTH buffers and the free stack the ring slots draw from. The pool
 * must cover the ring; a personality that lends buffers out sizes it larger. */
u32 bcmgenet_rx_pool_create(struct GenetUnit *unit, u32 buffers);
void bcmgenet_rx_pool_destroy(struct GenetUnit *unit);
APTR bcmgenet_rx_pool_pop(struct GenetUnit *unit);
void bcmgenet_rx_pool_push(struct GenetUnit *unit, APTR buffer);
#ifdef DEBUG
BOOL bcmgenet_rx_buf_ok(const struct GenetUnit *unit, APTR buffer); /* a slot-aligned pool buffer */
#endif

/* RX functions (unit task context): one drain per personality, at most
 * @limit frames, the count drained or -EAGAIN for an empty ring. */
s32 bcmgenet_netdev_rx(struct GenetUnit *unit, u16 limit);
s32 bcmgenet_sana2_rx(struct GenetUnit *unit, u16 limit);

/* Periodic datapath perf report — profile tier only, and the only work the
 * housekeeping tick does beyond the PHY poll. */
#ifdef PROFILE
void bcmgenet_perf_tick(struct GenetUnit *unit);
#else
#define bcmgenet_perf_tick(unit) ((void)0)
#endif

/* TX functions */
LONG bcmgenet_netdev_tx_submit(struct GenetUnit *unit, const struct NetDevTxDesc *descs, ULONG count);
void bcmgenet_netdev_tx_kick(struct GenetUnit *unit);	 /* publish the staged TX batch (doorbell) */
ULONG bcmgenet_tx_harvest(struct GenetUnit *unit);		 /* unit task; cookies delivered */
void bcmgenet_tx_irq_arm(struct GenetUnit *unit);		 /* unit task; one TX-done interrupt, armed */
void bcmgenet_tx_done_arm(struct GenetUnit *unit);		 /* unit task; netdev: one for the cookies in flight */
void bcmgenet_netdev_tx_quiesce(struct GenetUnit *unit); /* unit task, DMA off */

#endif
