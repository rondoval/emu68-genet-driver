// SPDX-License-Identifier: GPL-2.0+
/*
 * Cross-file interface within the bcmgenet hardware layer. These functions are
 * defined in one of the bcmgenet-*.c files and called from another; they are
 * not part of the driver's public surface, which is <genet/bcmgenet.h>.
 */
#ifndef _BCMGENET_PRIV_H
#define _BCMGENET_PRIV_H

#include <types.h>

struct GenetUnit;

/* bcmgenet-mac.c */
void bcmgenet_umac_reset(struct GenetUnit *unit);
void bcmgenet_gmac_write_hwaddr(struct GenetUnit *unit, const u8 *addr);
void bcmgenet_mii_config(struct GenetUnit *unit);

/* bcmgenet-dma.c */
void bcmgenet_disable_dma(struct GenetUnit *unit);
u32 bcmgenet_init_dma(struct GenetUnit *unit);

/* bcmgenet-rx.c — RX queue init, driven by bcmgenet_init_dma() */
u32 bcmgenet_init_rx_queues(struct GenetUnit *unit);

#endif /* _BCMGENET_PRIV_H */
