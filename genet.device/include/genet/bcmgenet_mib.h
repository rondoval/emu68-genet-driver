// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifndef _GENET_BCMGENET_MIB_H
#define _GENET_BCMGENET_MIB_H

#include <types.h>

struct GenetUnit;

struct mib_snapshot
{
    /* RX */
    u32 rx_pkts;
    u32 rx_bytes;
    u32 rx_mca;
    u32 rx_bca;
    u32 rx_uc;
    u32 rx_fcs;
    u32 rx_aln;
    u32 rx_pf;
    u32 rx_ovr;
    u32 rx_jbr;
    u32 rx_pok;
    u32 rx_runt;

    /* TX */
    u32 tx_pkts;
    u32 tx_bytes;
    u32 tx_mca;
    u32 tx_bca;
    u32 tx_uc;
    u32 tx_fcs;
    u32 tx_pf;
    u32 tx_ovr;
    u32 tx_drf;
    u32 tx_edf;
    u32 tx_scl;
    u32 tx_mcl;
    u32 tx_lcl;
    u32 tx_ecl;
    u32 tx_ncl;
    u32 tx_jbr;
    u32 tx_pok;

    /* Misc */
    u32 rbuf_ovfl;
    u32 rbuf_err;
    u32 mdf_err;
};

void bcmgenet_read_mib_snapshot(struct GenetUnit *unit, struct mib_snapshot *out);
void bcmgenet_reset_mib_counters(struct GenetUnit *unit);

#endif /* _GENET_BCMGENET_MIB_H */
