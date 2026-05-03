// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * Snapshot the BCM GENET hardware MIB counter block into a software-side
 * structure. Counters are 32-bit free-running, latched in MIB-clock domain.
 * They are zeroed at unit reset (MIB_RESET_*) and never reset between calls
 * here, so the caller is expected to take a baseline at S2_ONLINE and report
 * deltas.
 *
 * The misc RBUF/UMAC counters saturate at 0xFFFFFFFF; following the Linux
 * driver's lead we clear them on saturation so they keep counting.
 */

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#endif

#include <exec/types.h>
#include <exec/execbase.h>

#include <iomem.h>
#include <types.h>
#include <device.h>

#include <genet/bcmgenet-regs.h>
#include <genet/bcmgenet_mib.h>

static u32 read_misc(struct GenetUnit *unit, u32 offset)
{
    u32 val = mmio_read32(BCMGENET_REG(unit, offset));
    if (val == 0xFFFFFFFFul)
        mmio_write32(0, BCMGENET_REG(unit, offset));
    return val;
}

void bcmgenet_read_mib_snapshot(struct GenetUnit *unit, struct mib_snapshot *out)
{
    /* RX */
    out->rx_pkts  = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_RX_PKT));
    out->rx_bytes = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_RX_BYTES));
    out->rx_mca   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_RX_MCA));
    out->rx_bca   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_RX_BCA));
    out->rx_uc    = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_RX_UC));
    out->rx_fcs   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_RX_FCS));
    out->rx_aln   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_RX_ALN));
    out->rx_pf    = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_RX_PF));
    out->rx_ovr   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_RX_OVR));
    out->rx_jbr   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_RX_JBR));
    out->rx_pok   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_RX_POK));
    out->rx_runt  = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_RX_RUNT));

    /* TX */
    out->tx_pkts  = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_PKTS));
    out->tx_bytes = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_BYTES));
    out->tx_mca   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_MCA));
    out->tx_bca   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_BCA));
    out->tx_uc    = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_UC));
    out->tx_fcs   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_FCS));
    out->tx_pf    = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_PF));
    out->tx_ovr   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_OVR));
    out->tx_drf   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_DRF));
    out->tx_edf   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_EDF));
    out->tx_scl   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_SCL));
    out->tx_mcl   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_MCL));
    out->tx_lcl   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_LCL));
    out->tx_ecl   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_ECL));
    out->tx_ncl   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_NCL));
    out->tx_jbr   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_JBR));
    out->tx_pok   = mmio_read32(BCMGENET_REG(unit, UMAC_MIB_TX_POK));

    /* Misc — clear-on-saturation per Linux driver */
    out->rbuf_ovfl = read_misc(unit, UMAC_RBUF_OVFL_CNT);
    out->rbuf_err  = read_misc(unit, UMAC_RBUF_ERR_CNT);
    out->mdf_err   = read_misc(unit, UMAC_MDF_ERR_CNT);
}

void bcmgenet_reset_mib_counters(struct GenetUnit *unit)
{
    /* Pulse the MIB reset bits — same sequence used at unit reset. Clears
     * the entire RX, TX and RUNT counter blocks. */
    mmio_write32(MIB_RESET_RX | MIB_RESET_TX | MIB_RESET_RUNT,
                 BCMGENET_REG(unit, UMAC_MIB_CTRL));
    mmio_write32(0, BCMGENET_REG(unit, UMAC_MIB_CTRL));

    /* Misc counters are not affected by MIB_RESET_*; clear them directly. */
    mmio_write32(0, BCMGENET_REG(unit, UMAC_RBUF_OVFL_CNT));
    mmio_write32(0, BCMGENET_REG(unit, UMAC_RBUF_ERR_CNT));
    mmio_write32(0, BCMGENET_REG(unit, UMAC_MDF_ERR_CNT));
}
