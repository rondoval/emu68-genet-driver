// SPDX-License-Identifier: GPL-2.0+
#ifndef __BCMGENET_H
#define __BCMGENET_H

#include <types.h>

struct GenetUnit;
struct IOSana2Req;

#define BCMGENET_REG(unit, offset) ((unit)->genetBase + (offset))

u32 bcmgenet_eth_probe(struct GenetUnit *unit);
u32 bcmgenet_gmac_eth_start(struct GenetUnit *unit);
void bcmgenet_gmac_eth_stop(struct GenetUnit *unit);
void bcmgenet_reset_quiesce(struct GenetUnit *unit);
u32 bcmgenet_set_coalesce(struct GenetUnit *unit, u32 tx_max_coalesced_frames, u32 rx_max_coalesced_frames, u32 rx_coalesce_usecs);
void bcmgenet_set_rx_mode(struct GenetUnit *unit); /* Updates PROMISC flag and sets up MDF if possible */

/* RX functions */
s32 bcmgenet_gmac_eth_rx(struct GenetUnit *unit, u16 budget);

/* Periodic [genet] datapath perf report — profile tier only. */
#ifdef PROFILE
void bcmgenet_perf_tick(struct GenetUnit *unit);
#else
#define bcmgenet_perf_tick(unit) ((void)0)
#endif

/* TX functions */
u32 bcmgenet_xmit(struct IOSana2Req *io, struct GenetUnit *unit);
void bcmgenet_tx_reclaim(struct GenetUnit *unit, u16 budget);

#endif