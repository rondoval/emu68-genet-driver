// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifndef __BCMGENET_H
#define __BCMGENET_H

#include <exec/types.h>

int bcmgenet_gmac_eth_send(struct GenetUnit *priv, void *packet, ULONG length);
int bcmgenet_gmac_eth_recv(struct GenetUnit *priv, int flags, UBYTE **packetp);
int bcmgenet_gmac_free_pkt(struct GenetUnit *priv, UBYTE *packet, ULONG length);
int bcmgenet_gmac_eth_start(struct GenetUnit *priv);
int bcmgenet_eth_probe(struct GenetUnit *priv);
void bcmgenet_gmac_eth_stop(struct GenetUnit *priv);

#endif