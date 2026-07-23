// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * The GENET counter surface behind NETDEV_CMD_GET_COUNTERS.
 *
 * Three sources, one flat self-describing list:
 *   - the UniMAC MIB block, every register `ethtool -S` reports. Names match
 *     Linux's ethtool strings so output is greppable against upstream.
 *   - every driver counter the unit keeps.
 *   - ring gauges, computed on the spot.
 *
 * This list stands alone: a caller gets the whole picture from this one
 * command and never has to reach for NETDEV_CMD_GET_STATS to complete it. So it
 * overlaps that command deliberately — rx/tx packets and bytes, the drop and
 * reject tallies, rbuf_ovflow_cnt all appear here as well as in their
 * NetDevStats fields. The two answer different questions (a fixed portable
 * summary vs everything this driver knows) and are each complete on their own
 * terms.
 *
 * The MIB registers are free-running 32-bit and wrap — rx_bytes in ~34 s at
 * 1 Gb/s — so they ship raw under NDCNTF_WRAP32 and the reader differences
 * them. Accumulating into 64 bits would need a periodic sweep, which is
 * exactly the cost this command exists to avoid. The driver's own counters
 * have no such limit and ship at full width.
 */

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <stddef.h>

#include <exec/types.h>

#include <device.h>
#include <iomem.h>
#include <devices/netdev.h>
#include <types.h>

#include <genet/bcmgenet.h>
#include <genet/bcmgenet-mib.h>
#include <genet/bcmgenet-regs.h>

/* Private table flags, alongside the NDCNTF_* published ones. */
#define MIBF_CLEAR_ON_SAT (1u << 15) /* saturating: rearm by writing 0 */
#define MIBF_U64 (1u << 14)          /* software counter is 64-bit, not 32 */

struct genet_hw_counter
{
	const char *ghc_Name;
	u16 ghc_Reg;   /* offset from genetBase */
	u16 ghc_Flags; /* NDCNTF_* | MIBF_* */
};

struct genet_sw_counter
{
	const char *gsc_Name;
	u16 gsc_Offset; /* into struct GenetUnit */
	u16 gsc_Flags;  /* NDCNTF_* | MIBF_U64 */
};

#define HW_OK(name, reg) {name, (reg), NDCNTF_WRAP32}
#define HW_ERR(name, reg) {name, (reg), NDCNTF_WRAP32 | NDCNTF_ERROR}

/* Order follows struct bcmgenet_rx_counters / bcmgenet_tx_counters upstream. */
static const struct genet_hw_counter genet_hw_counters[] = {
	/* UniMAC RSV */
	HW_OK("rx_64_octets", UMAC_MIB_RX_CNT_64),
	HW_OK("rx_65_127_oct", UMAC_MIB_RX_CNT_127),
	HW_OK("rx_128_255_oct", UMAC_MIB_RX_CNT_255),
	HW_OK("rx_256_511_oct", UMAC_MIB_RX_CNT_511),
	HW_OK("rx_512_1023_oct", UMAC_MIB_RX_CNT_1023),
	HW_OK("rx_1024_1518_oct", UMAC_MIB_RX_CNT_1518),
	HW_OK("rx_vlan_1519_1522_oct", UMAC_MIB_RX_CNT_MGV),
	HW_OK("rx_1522_2047_oct", UMAC_MIB_RX_CNT_2047),
	HW_OK("rx_2048_4095_oct", UMAC_MIB_RX_CNT_4095),
	HW_OK("rx_4096_9216_oct", UMAC_MIB_RX_CNT_9216),
	HW_OK("rx_pkts", UMAC_MIB_RX_PKT),
	HW_OK("rx_bytes", UMAC_MIB_RX_BYTES),
	HW_OK("rx_multicast", UMAC_MIB_RX_MCA),
	HW_OK("rx_broadcast", UMAC_MIB_RX_BCA),
	HW_ERR("rx_fcs", UMAC_MIB_RX_FCS),
	HW_OK("rx_control", UMAC_MIB_RX_CF),
	HW_OK("rx_pause", UMAC_MIB_RX_PF),
	HW_ERR("rx_unknown", UMAC_MIB_RX_UO),
	HW_ERR("rx_align", UMAC_MIB_RX_ALN),
	HW_ERR("rx_outrange", UMAC_MIB_RX_FLR),
	HW_ERR("rx_code", UMAC_MIB_RX_CDE),
	HW_ERR("rx_carrier", UMAC_MIB_RX_FCR),
	HW_ERR("rx_oversize", UMAC_MIB_RX_OVR),
	HW_ERR("rx_jabber", UMAC_MIB_RX_JBR),
	HW_ERR("rx_mtu_err", UMAC_MIB_RX_MTUE),
	HW_OK("rx_good_pkts", UMAC_MIB_RX_POK),
	HW_OK("rx_unicast", UMAC_MIB_RX_UC),
	HW_OK("rx_ppp", UMAC_MIB_RX_PPP),
	HW_OK("rx_crc", UMAC_MIB_RX_RCRC),
	/* UniMAC TSV */
	HW_OK("tx_64_octets", UMAC_MIB_TX_CNT_64),
	HW_OK("tx_65_127_oct", UMAC_MIB_TX_CNT_127),
	HW_OK("tx_128_255_oct", UMAC_MIB_TX_CNT_255),
	HW_OK("tx_256_511_oct", UMAC_MIB_TX_CNT_511),
	HW_OK("tx_512_1023_oct", UMAC_MIB_TX_CNT_1023),
	HW_OK("tx_1024_1518_oct", UMAC_MIB_TX_CNT_1518),
	HW_OK("tx_vlan_1519_1522_oct", UMAC_MIB_TX_CNT_MGV),
	HW_OK("tx_1522_2047_oct", UMAC_MIB_TX_CNT_2047),
	HW_OK("tx_2048_4095_oct", UMAC_MIB_TX_CNT_4095),
	HW_OK("tx_4096_9216_oct", UMAC_MIB_TX_CNT_9216),
	HW_OK("tx_pkts", UMAC_MIB_TX_PKTS),
	HW_OK("tx_multicast", UMAC_MIB_TX_MCA),
	HW_OK("tx_broadcast", UMAC_MIB_TX_BCA),
	HW_OK("tx_pause", UMAC_MIB_TX_PF),
	HW_OK("tx_control", UMAC_MIB_TX_CF),
	HW_ERR("tx_fcs_err", UMAC_MIB_TX_FCS),
	HW_ERR("tx_oversize", UMAC_MIB_TX_OVR),
	HW_OK("tx_defer", UMAC_MIB_TX_DRF),
	HW_ERR("tx_excess_defer", UMAC_MIB_TX_EDF),
	HW_OK("tx_single_col", UMAC_MIB_TX_SCL),
	HW_OK("tx_multi_col", UMAC_MIB_TX_MCL),
	HW_ERR("tx_late_col", UMAC_MIB_TX_LCL),
	HW_ERR("tx_excess_col", UMAC_MIB_TX_ECL),
	HW_ERR("tx_frags", UMAC_MIB_TX_FRG),
	HW_OK("tx_total_col", UMAC_MIB_TX_NCL),
	HW_ERR("tx_jabber", UMAC_MIB_TX_JBR),
	HW_OK("tx_bytes", UMAC_MIB_TX_BYTES),
	HW_OK("tx_good_pkts", UMAC_MIB_TX_POK),
	HW_OK("tx_unicast", UMAC_MIB_TX_UC),
	/* UniMAC RUNT */
	HW_OK("rx_runt_pkts", UMAC_MIB_RX_RUNT),
	HW_OK("rx_runt_valid_fcs", UMAC_MIB_RX_RUNT_FCS),
	HW_ERR("rx_runt_inval_fcs_align", UMAC_MIB_RX_RUNT_FCS_ALIGN),
	HW_OK("rx_runt_bytes", UMAC_MIB_RX_RUNT_BYTES),
	/* Misc UniMAC — outside the block, saturating rather than wrapping. Note
	 * mdf_err_cnt is NOT an error despite the register name: it counts frames
	 * the MDF exact-match filter rejected, which is the filter working. On a
	 * normal LAN it climbs steadily with the ambient multicast this station
	 * never joined. */
	/* rbuf_ovflow_cnt is not here: it is accumulated in software (see
	 * bcmgenet_mib_rbuf_ovfl) because NetDevStats reports it and must stay
	 * monotonic. The other two are read raw and flagged WRAP32, since rearming
	 * them looks to a reader exactly like the wrap that flag describes. */
	{"rbuf_err_cnt", UMAC_RBUF_ERR_CNT, NDCNTF_ERROR | NDCNTF_WRAP32 | MIBF_CLEAR_ON_SAT},
	{"mdf_err_cnt", UMAC_MDF_ERR_CNT, NDCNTF_WRAP32 | MIBF_CLEAR_ON_SAT},
};

#define SW_U32(name, field, flags) {name, (u16)offsetof(struct GenetUnit, field), (flags)}
#define SW_U64(name, field, flags) \
	{name, (u16)offsetof(struct GenetUnit, field), (flags) | MIBF_U64}

/* Every counter the unit keeps — including the ones NetDevStats also carries,
 * so this list is complete without it. Prefixed drv_ to separate the driver's
 * bookkeeping from the MAC's own view of the wire above. */
static const struct genet_sw_counter genet_sw_counters[] = {
	/* traffic as the driver counted it, next to the MAC's rx_pkts/tx_pkts */
	SW_U64("drv_rx_packets", internalStats.rx_packets, 0),
	SW_U64("drv_rx_bytes", internalStats.rx_bytes, 0),
	SW_U64("drv_tx_packets", internalStats.tx_packets, 0),
	SW_U64("drv_tx_bytes", internalStats.tx_bytes, 0),
	/* the RX error split — NetDevStats can only carry their sum */
	SW_U32("drv_rx_crc_errors", internalStats.rx_crc_errors, NDCNTF_ERROR),
	SW_U32("drv_rx_over_errors", internalStats.rx_over_errors, NDCNTF_ERROR),
	SW_U32("drv_rx_frame_errors", internalStats.rx_frame_errors, NDCNTF_ERROR),
	SW_U32("drv_rx_length_errors", internalStats.rx_length_errors, NDCNTF_ERROR),
	SW_U32("drv_rx_fragmented_errors", internalStats.rx_fragmented_errors, NDCNTF_ERROR),
	SW_U32("drv_rx_other_errors", internalStats.rx_other_errors, NDCNTF_ERROR),
	/* loss points, each naming a distinct bottleneck */
	SW_U32("drv_rx_overruns", internalStats.rx_overruns, NDCNTF_ERROR),
	SW_U32("drv_rx_fifo_errors", internalStats.rx_fifo_errors, NDCNTF_ERROR),
	SW_U64("drv_rx_dropped", internalStats.rx_dropped, NDCNTF_ERROR),
	SW_U32("drv_rx_pool_dry", internalStats.rx_pool_dry, NDCNTF_ERROR),
	SW_U64("drv_tx_dropped", internalStats.tx_dropped, NDCNTF_ERROR),
	SW_U32("drv_tx_rejected", internalStats.tx_rejected, NDCNTF_ERROR),
	SW_U32("drv_tx_bad", internalStats.tx_bad, NDCNTF_ERROR),
	/* interrupt tallies, split by what the status word carried */
	SW_U32("drv_irq0_total", internalStats.irq0_count, 0),
	SW_U32("drv_irq0_rx", internalStats.irq0_rx_count, 0),
	SW_U32("drv_irq0_tx", internalStats.irq0_tx_count, 0),
	SW_U32("drv_irq0_other", internalStats.irq0_other_count, 0),
	/* RX buffer pool, right now */
	SW_U32("drv_rx_pool_free", ndRxFreeCount, NDCNTF_GAUGE),
	SW_U32("drv_rx_held", ndRxHeld, NDCNTF_GAUGE),
};

#define GENET_HW_COUNT ((u16)(sizeof(genet_hw_counters) / sizeof(genet_hw_counters[0])))
#define GENET_SW_COUNT ((u16)(sizeof(genet_sw_counters) / sizeof(genet_sw_counters[0])))

/*
 * Append one counter. Everything is counted; only the first `max` are stored,
 * so a max=0 probe returns the total without touching the buffer.
 */
static void mib_put(struct NetDevCounter *out, u16 max, u16 *n,
					const char *name, u64 value, u16 flags)
{
	if (*n < max)
	{
		struct NetDevCounter *c = &out[*n];
		netdev_u64_set(&c->ndcn_Value, value);
		c->ndcn_Name = (CONST_STRPTR)name;
		c->ndcn_Flags = (UWORD)(flags & ~(MIBF_CLEAR_ON_SAT | MIBF_U64));
		c->ndcn_Pad = 0;
	}
	(*n)++;
}

/*
 * Fold the RBUF FIFO-overflow register into its software accumulator and
 * return the running total. Unit task only.
 *
 * The register saturates at ~0 instead of wrapping and is rearmed by writing 0,
 * and bcmgenet_mib_reset() zeroes it at every start — so its raw value can fall,
 * which NetDevStats forbids for nds_RxFifoOvfl. Accumulating on read keeps the
 * reported figure monotonic without a periodic sweep: every reader passes
 * through here, and the delta is taken against the value this function last saw.
 */
u32 bcmgenet_mib_rbuf_ovfl(struct GenetUnit *unit)
{
	struct internal_stats *is = &unit->internalStats;
	u32 raw = mmio_read32(BCMGENET_REG(unit, UMAC_RBUF_OVFL_CNT));

	/* A value below the last one means the register was rearmed underneath us
	 * (a start, or another reader's saturation clear); take it as a fresh
	 * baseline rather than a negative delta. */
	if (raw >= is->rx_fifo_last)
		is->rx_fifo_errors += raw - is->rx_fifo_last;
	else
		is->rx_fifo_errors += raw;

	if (raw == 0xFFFFFFFFul)
	{
		mmio_write32(0, BCMGENET_REG(unit, UMAC_RBUF_OVFL_CNT));
		raw = 0;
	}
	is->rx_fifo_last = raw;

	return is->rx_fifo_errors;
}

/*
 * Take the delta baseline from the live register, so the next accumulate
 * counts only what arrives after this point. Called wherever the accumulator
 * is rebased independently of the register: after the UMAC reset zeroes it,
 * and at ATTACH, where a previous session may have left it non-zero.
 */
void bcmgenet_mib_rbuf_ovfl_rebase(struct GenetUnit *unit)
{
	unit->internalStats.rx_fifo_last = mmio_read32(BCMGENET_REG(unit, UMAC_RBUF_OVFL_CNT));
}

/*
 * The MAC's own view of transmit failures, for NetDevStats' nds_TxErrors.
 *
 * These are free-running 32-bit registers reset once per bring-up, so the sum
 * is meaningful for the session and cannot be accumulated further without a
 * periodic sweep. Late and excessive collisions and jabber are the ones that
 * indicate a real wiring or duplex fault; the rest round it out.
 */
u32 bcmgenet_mib_tx_errors(struct GenetUnit *unit)
{
	static const u16 tx_error_regs[] = {
		UMAC_MIB_TX_FCS, UMAC_MIB_TX_OVR, UMAC_MIB_TX_LCL,
		UMAC_MIB_TX_ECL, UMAC_MIB_TX_FRG, UMAC_MIB_TX_JBR,
	};
	u32 total = 0;

	for (u32 i = 0; i < sizeof(tx_error_regs) / sizeof(tx_error_regs[0]); i++)
		total += mmio_read32(BCMGENET_REG(unit, tx_error_regs[i]));

	return total;
}

u16 bcmgenet_mib_fill(struct GenetUnit *unit, struct NetDevCounter *out, u16 max)
{
	u16 n = 0;

	/* Before the table walk: it reports the accumulator, which this refreshes. */
	bcmgenet_mib_rbuf_ovfl(unit);

	for (u16 i = 0; i < GENET_HW_COUNT; i++)
	{
		const struct genet_hw_counter *h = &genet_hw_counters[i];
		u32 val = mmio_read32(BCMGENET_REG(unit, h->ghc_Reg));

		/* Saturating counters stop at ~0 instead of wrapping; write 0 so they
		 * keep counting. Linux does the same in bcmgenet_update_stat_misc(). */
		if ((h->ghc_Flags & MIBF_CLEAR_ON_SAT) && val == 0xFFFFFFFFul)
			mmio_write32(0, BCMGENET_REG(unit, h->ghc_Reg));

		mib_put(out, max, &n, h->ghc_Name, val, h->ghc_Flags);
	}

	/*
	 * Not every counter is written on this task: the submit path owns
	 * tx_rejected and tx_bad, the ISR owns the irq0_ tallies. Neither can be
	 * caught half-written — they are single aligned longwords, which the 68040
	 * reads and writes atomically — so the only question is freshness, and the
	 * volatile read below settles it. The 64-bit entries need no such care:
	 * since the TX tally moved to the harvest, every one of them is
	 * accumulated on this task alone.
	 *
	 * The qualifier belongs on the cast, not on the struct fields. The table
	 * addresses them by offset, so a field-level volatile would be cast away
	 * right here and buy nothing — while making tx_rejected++ and the ring
	 * indices volatile at their real hot-path call sites, which is a cost the
	 * TX path should not pay for a diagnostic command.
	 */
	for (u16 i = 0; i < GENET_SW_COUNT; i++)
	{
		const struct genet_sw_counter *s = &genet_sw_counters[i];
		const void *p = (const u8 *)unit + s->gsc_Offset;
		u64 val = (s->gsc_Flags & MIBF_U64) ? *(const volatile u64 *)p
											: *(const volatile u32 *)p;

		mib_put(out, max, &n, s->gsc_Name, val, s->gsc_Flags);
	}

	/* Ring gauges: what the TX side looks like right now. A wedge reads as
	 * bds_out pinned high with hw_cons frozen. */
	u16 hw_cons = (u16)(mmio_read32(BCMGENET_REG(unit, TDMA_CONS_INDEX)) & DMA_C_INDEX_MASK);
	u16 out_bds = (u16)((u32)(unit->tx_ring.tx_prod_index - hw_cons) & DMA_C_INDEX_MASK);

	mib_put(out, max, &n, "drv_tx_bds_out", out_bds, NDCNTF_GAUGE);
	mib_put(out, max, &n, "drv_tx_bds_free", (u16)(TX_DESCS - out_bds), NDCNTF_GAUGE);
	mib_put(out, max, &n, "drv_tx_hw_cons", hw_cons, NDCNTF_GAUGE);
	mib_put(out, max, &n, "drv_txdone_depth",
			unit->ndTxDoneProd - unit->ndTxDoneCons, NDCNTF_GAUGE);

	return n;
}

void bcmgenet_mib_reset(struct GenetUnit *unit)
{
	/* The control pulse zeroes the RX, TX and RUNT blocks... */
	mmio_write32(MIB_RESET_RX | MIB_RESET_TX | MIB_RESET_RUNT,
				 BCMGENET_REG(unit, UMAC_MIB_CTRL));
	mmio_write32(0, BCMGENET_REG(unit, UMAC_MIB_CTRL));

	/* ...but not the three misc counters, which live outside the block and
	 * have to be written. Without this they would survive a STOP/START and
	 * report against the previous session's baseline. */
	mmio_write32(0, BCMGENET_REG(unit, UMAC_RBUF_OVFL_CNT));
	mmio_write32(0, BCMGENET_REG(unit, UMAC_RBUF_ERR_CNT));
	mmio_write32(0, BCMGENET_REG(unit, UMAC_MDF_ERR_CNT));

	/* The overflow accumulator survives the start; only its delta baseline
	 * follows the register back to zero. */
	bcmgenet_mib_rbuf_ovfl_rebase(unit);
}
