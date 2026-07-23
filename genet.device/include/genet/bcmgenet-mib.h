// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifndef _GENET_BCMGENET_MIB_H
#define _GENET_BCMGENET_MIB_H

#include <types.h>

struct GenetUnit;
struct NetDevCounter;

/*
 * NETDEV_CMD_GET_COUNTERS payload: the whole UniMAC MIB block plus the driver
 * counters and ring gauges that NetDevStats has no field for.
 *
 * Fills up to `max` entries and returns the total available, so a caller can
 * size a buffer from a max=0 probe and call again. Unit task context — the
 * hardware sweep is ~64 MMIO reads, paid at the caller's poll rate.
 */
u16 bcmgenet_mib_fill(struct GenetUnit *unit, struct NetDevCounter *out, u16 max);

/*
 * Zero every counter bcmgenet_mib_fill() reports, so one bring-up is one
 * baseline. Called from the UMAC reset path.
 */
void bcmgenet_mib_reset(struct GenetUnit *unit);

/*
 * Fold the RBUF FIFO-overflow register into its software accumulator and return
 * the running total, which NetDevStats reports as nds_RxFifoOvfl. The register
 * saturates and is rearmed by writing 0, so it cannot be reported raw and stay
 * monotonic; every reader goes through here. Unit task context.
 *
 * _rebase() drops the delta baseline to zero, for the paths that zero the
 * register themselves.
 */
u32 bcmgenet_mib_rbuf_ovfl(struct GenetUnit *unit);
void bcmgenet_mib_rbuf_ovfl_rebase(struct GenetUnit *unit);

/* Sum of the UniMAC transmit-error counters, for NetDevStats' nds_TxErrors. */
u32 bcmgenet_mib_tx_errors(struct GenetUnit *unit);

#endif /* _GENET_BCMGENET_MIB_H */
