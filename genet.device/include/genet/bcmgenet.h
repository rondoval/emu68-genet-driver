// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifndef __BCMGENET_H
#define __BCMGENET_H

#include <exec/types.h>

int bcmgenet_eth_probe(struct GenetUnit *unit);
int bcmgenet_gmac_eth_start(struct GenetUnit *unit);
void bcmgenet_gmac_eth_stop(struct GenetUnit *unit);
int bcmgenet_set_coalesce(struct GenetUnit *unit, ULONG tx_max_coalesced_frames, ULONG rx_max_coalesced_frames, ULONG rx_coalesce_usecs);
void bcmgenet_set_rx_mode(struct GenetUnit *unit); /* Updates PROMISC flag and sets up MDF if possible */

/* RX functions */
int bcmgenet_gmac_eth_rx(struct GenetUnit *unit, unsigned int budget);

/* TX functions */
int bcmgenet_xmit(struct IOSana2Req *io, struct GenetUnit *unit);
unsigned int bcmgenet_tx_reclaim(struct GenetUnit *unit, unsigned int budget);

#endif