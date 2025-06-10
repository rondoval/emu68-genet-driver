// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifndef __BCMGENET_H
#define __BCMGENET_H

#include <exec/types.h>

int bcmgenet_eth_probe(struct GenetUnit *priv);
int bcmgenet_gmac_eth_start(struct GenetUnit *priv);
void bcmgenet_gmac_eth_stop(struct GenetUnit *priv);
int bcmgenet_set_coalesce(struct GenetUnit *priv, ULONG tx_max_coalesced_frames, ULONG rx_max_coalesced_frames, ULONG rx_coalesce_usecs);
void bcmgenet_set_rx_mode(struct GenetUnit *priv);

/* RX functions */
int bcmgenet_gmac_eth_recv(struct GenetUnit *priv, int flags, UBYTE **packetp);
void bcmgenet_gmac_free_pkt(struct GenetUnit *priv, UBYTE *packet, ULONG length);

/* TX functions */
int bcmgenet_tx_poll(struct GenetUnit *unit, struct IOSana2Req *io);
void bcmgenet_timeout(struct GenetUnit *priv);

#endif