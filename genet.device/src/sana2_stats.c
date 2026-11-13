// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * sana2_stats.c — SANA-II statistics: the global ones, genet's special ones
 * and throughput sampling. Unit task only.
 *
 * The packet and error counts are the core's (internal_stats), the hardware
 * view the UniMAC MIB's, read through bcmgenet-mib.c: the RBUF overflow
 * register is folded into its software accumulator there, so nothing here
 * reads it raw.
 */

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/timer_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function: see GenetUnit.sysBase */
#include <proto/exec.h>
#define TIMER_BASE_NAME TimerBase /* a local: GenetUnit.timerBase */
#include <proto/timer.h>
#endif

#include <exec/errors.h>
#include <devices/sana2.h>

#include <debug.h>
#include <types.h>
#include <device.h>
#include <sana2_unit.h>

#include <genet/bcmgenet-mib.h>
#include <genet/bcmgenet-regs.h>
#include <genet/genet_specialstats.h>

/* ------------------------------------------------------------------ global --- */

/**
 * rx_bad_frames() - Count the frames the hardware delivered damaged, every
 * kind.
 * @stats: the core's counters.
 *
 * Return: the sum of the RX error counters.
 */
static u64 rx_bad_frames(const struct internal_stats *stats)
{
    return (u64)stats->rx_other_errors + stats->rx_crc_errors + stats->rx_over_errors +
           stats->rx_frame_errors + stats->rx_length_errors + stats->rx_fragmented_errors;
}

/**
 * sana2_get_global_stats() - S2_GETGLOBALSTATS: fill the caller's
 * Sana2DeviceStats.
 * @unit: the unit.
 * @io: the request; ios2_StatData is the Sana2DeviceStats.
 *
 * Unit task. The counts are cut to 32 bits; UnknownTypesReceived is
 * rxOrphan, BadData every damaged frame (rx_bad_frames()).
 *
 * Return: COMMAND_PROCESSED; io_Error S2ERR_BAD_ARGUMENT /
 * S2WERR_NULL_POINTER for no StatData.
 */
u32 sana2_get_global_stats(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct Sana2DeviceStats *stats = io->ios2_StatData;
    const struct internal_stats *counters = &unit->internalStats;

    if (stats == NULL)
    {
        io->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
        io->ios2_WireError = S2WERR_NULL_POINTER;
        return COMMAND_PROCESSED;
    }

    stats->PacketsReceived = (ULONG)counters->rx_packets;
    stats->PacketsSent = (ULONG)counters->tx_packets;
    stats->UnknownTypesReceived = (ULONG)unit->s2->rxOrphan;
    stats->Overruns = counters->rx_overruns;
    stats->BadData = (ULONG)rx_bad_frames(counters);
    stats->LastStart = unit->s2->lastStart;
    return COMMAND_PROCESSED;
}

/**
 * s2_quad_set() - Store a 64-bit count in an S2QUAD.
 * @quad: the S2QUAD.
 * @value: the count.
 */
static void s2_quad_set(S2QUAD *quad, u64 value)
{
    quad->s2q_High = (ULONG)(value >> 32);
    quad->s2q_Low = (ULONG)value;
}

/**
 * sana2_get_extended_stats() - S2_GETEXTENDEDGLOBALSTATS: fill the caller's
 * Sana2ExtDeviceStats.
 * @unit: the unit.
 * @io: the request; ios2_StatData is the Sana2ExtDeviceStats.
 *
 * Unit task. The counts of S2_GETGLOBALSTATS in 64 bits, plus
 * Reconfigurations; s2xds_Actual says how much was filled.
 *
 * Return: COMMAND_PROCESSED; io_Error S2ERR_BAD_ARGUMENT /
 * S2WERR_NULL_POINTER for no StatData, IOERR_BADLENGTH when s2xds_Length is
 * short of the structure.
 */
u32 sana2_get_extended_stats(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct Sana2ExtDeviceStats *stats = io->ios2_StatData;
    const struct internal_stats *counters = &unit->internalStats;

    if (stats == NULL)
    {
        io->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
        io->ios2_WireError = S2WERR_NULL_POINTER;
        return COMMAND_PROCESSED;
    }
    if (stats->s2xds_Length < sizeof(struct Sana2ExtDeviceStats))
    {
        io->ios2_Req.io_Error = IOERR_BADLENGTH;
        return COMMAND_PROCESSED;
    }

    stats->s2xds_Actual = sizeof(struct Sana2ExtDeviceStats);
    s2_quad_set(&stats->s2xds_PacketsReceived, counters->rx_packets);
    s2_quad_set(&stats->s2xds_PacketsSent, counters->tx_packets);
    s2_quad_set(&stats->s2xds_UnknownTypesReceived, unit->s2->rxOrphan);
    s2_quad_set(&stats->s2xds_Overruns, counters->rx_overruns);
    s2_quad_set(&stats->s2xds_BadData, rx_bad_frames(counters));
    s2_quad_set(&stats->s2xds_Reconfigurations, unit->s2->reconfigurations);
    stats->s2xds_LastStart = unit->s2->lastStart;
    return COMMAND_PROCESSED;
}

/* ----------------------------------------------------------------- special --- */

/* The UniMAC MIB counters genet reports, under the IDs genet_specialstats.h
 * fixes for them. The registers wrap at 32 bits and restart at each bring-up. */
struct genet_ss_mib
{
    ULONG id;
    const char *name;
    u16 reg;
};

static const struct genet_ss_mib genet_ss_mib_table[] = {
    {GENET_SS_HW_RX_PKTS, "hw_rx_pkts", UMAC_MIB_RX_PKT},
    {GENET_SS_HW_RX_BYTES, "hw_rx_bytes", UMAC_MIB_RX_BYTES},
    {GENET_SS_HW_RX_MULTICAST, "hw_rx_multicast", UMAC_MIB_RX_MCA},
    {GENET_SS_HW_RX_BROADCAST, "hw_rx_broadcast", UMAC_MIB_RX_BCA},
    {GENET_SS_HW_RX_UNICAST, "hw_rx_unicast", UMAC_MIB_RX_UC},
    {GENET_SS_HW_RX_FCS_ERR, "hw_rx_fcs_err", UMAC_MIB_RX_FCS},
    {GENET_SS_HW_RX_ALIGN_ERR, "hw_rx_align_err", UMAC_MIB_RX_ALN},
    {GENET_SS_HW_RX_PAUSE, "hw_rx_pause", UMAC_MIB_RX_PF},
    {GENET_SS_HW_RX_OVERSIZE, "hw_rx_oversize", UMAC_MIB_RX_OVR},
    {GENET_SS_HW_RX_JABBER, "hw_rx_jabber", UMAC_MIB_RX_JBR},
    {GENET_SS_HW_RX_GOOD, "hw_rx_good", UMAC_MIB_RX_POK},
    {GENET_SS_HW_RX_RUNT, "hw_rx_runt", UMAC_MIB_RX_RUNT},
    {GENET_SS_HW_TX_PKTS, "hw_tx_pkts", UMAC_MIB_TX_PKTS},
    {GENET_SS_HW_TX_BYTES, "hw_tx_bytes", UMAC_MIB_TX_BYTES},
    {GENET_SS_HW_TX_MULTICAST, "hw_tx_multicast", UMAC_MIB_TX_MCA},
    {GENET_SS_HW_TX_BROADCAST, "hw_tx_broadcast", UMAC_MIB_TX_BCA},
    {GENET_SS_HW_TX_UNICAST, "hw_tx_unicast", UMAC_MIB_TX_UC},
    {GENET_SS_HW_TX_FCS_ERR, "hw_tx_fcs_err", UMAC_MIB_TX_FCS},
    {GENET_SS_HW_TX_PAUSE, "hw_tx_pause", UMAC_MIB_TX_PF},
    {GENET_SS_HW_TX_SINGLE_COL, "hw_tx_single_col", UMAC_MIB_TX_SCL},
    {GENET_SS_HW_TX_MULTI_COL, "hw_tx_multi_col", UMAC_MIB_TX_MCL},
    {GENET_SS_HW_TX_LATE_COL, "hw_tx_late_col", UMAC_MIB_TX_LCL},
    {GENET_SS_HW_TX_EXCESS_COL, "hw_tx_excess_col", UMAC_MIB_TX_ECL},
    {GENET_SS_HW_TX_TOTAL_COL, "hw_tx_total_col", UMAC_MIB_TX_NCL},
    {GENET_SS_HW_TX_DEFER, "hw_tx_defer", UMAC_MIB_TX_DRF},
    {GENET_SS_HW_TX_EXCESS_DEFER, "hw_tx_excess_defer", UMAC_MIB_TX_EDF},
    {GENET_SS_HW_TX_JABBER, "hw_tx_jabber", UMAC_MIB_TX_JBR},
    {GENET_SS_HW_TX_OVERSIZE, "hw_tx_oversize", UMAC_MIB_TX_OVR},
    {GENET_SS_HW_TX_GOOD, "hw_tx_good", UMAC_MIB_TX_POK},
    {GENET_SS_HW_RBUF_ERR, "hw_rbuf_err", UMAC_RBUF_ERR_CNT},
    {GENET_SS_HW_MDF_ERR, "hw_mdf_err", UMAC_MDF_ERR_CNT},
};

#define GENET_SS_MIB_COUNT (sizeof(genet_ss_mib_table) / sizeof(genet_ss_mib_table[0]))

/**
 * ss_emit() - Fill record @index of the caller's array, if it has room.
 * @records: the caller's Sana2SpecialStatRecord array.
 * @max: its RecordCountMax.
 * @index: the record to fill.
 * @id: the record's Type, a GENET_SS_* ID.
 * @name: the record's String.
 * @value: the count, saturated at 0xFFFFFFFF.
 *
 * Return: the index of the next record; @index when the array is full.
 */
static ULONG ss_emit(struct Sana2SpecialStatRecord *records, ULONG max, ULONG index,
                     ULONG id, const char *name, u64 value)
{
    if (index >= max)
        return index;
    records[index].Type = id;
    records[index].Count = (value > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (ULONG)value;
    records[index].String = (STRPTR)name;
    return index + 1;
}

/**
 * sana2_get_special_stats() - S2_GETSPECIALSTATS: genet's special statistics.
 * @unit: the unit.
 * @io: the request; ios2_StatData is the Sana2SpecialStatHeader.
 *
 * Unit task. The records follow the header: the driver's own counters first,
 * then the UniMAC MIB's and the RBUF overflow count, as many as
 * RecordCountMax takes; RecordCountSupplied says how many.
 *
 * Return: COMMAND_PROCESSED; io_Error S2ERR_BAD_ARGUMENT /
 * S2WERR_NULL_POINTER for no StatData.
 */
u32 sana2_get_special_stats(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct Sana2SpecialStatHeader *header = io->ios2_StatData;
    if (header == NULL)
    {
        io->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
        io->ios2_WireError = S2WERR_NULL_POINTER;
        return COMMAND_PROCESSED;
    }

    const struct internal_stats *counters = &unit->internalStats;
    const struct GenetS2 *s2 = unit->s2;
    struct Sana2SpecialStatRecord *records = (struct Sana2SpecialStatRecord *)(header + 1);
    ULONG max = header->RecordCountMax;
    ULONG index = 0;

    /* the driver's own counters */
    index = ss_emit(records, max, index, GENET_SS_RX_OVERRUNS, "rx_overruns", counters->rx_overruns);
    index = ss_emit(records, max, index, GENET_SS_RX_CRC_ERRORS, "rx_crc_errors", counters->rx_crc_errors);
    index = ss_emit(records, max, index, GENET_SS_RX_OVER_ERRORS, "rx_over_errors", counters->rx_over_errors);
    index = ss_emit(records, max, index, GENET_SS_RX_FRAME_ERRORS, "rx_frame_errors", counters->rx_frame_errors);
    index = ss_emit(records, max, index, GENET_SS_RX_LENGTH_ERRORS, "rx_length_errors", counters->rx_length_errors);
    index = ss_emit(records, max, index, GENET_SS_RX_FRAGMENTED, "rx_fragmented", counters->rx_fragmented_errors);
    index = ss_emit(records, max, index, GENET_SS_RX_OTHER_ERRORS, "rx_other_errors", counters->rx_other_errors);
    index = ss_emit(records, max, index, GENET_SS_RX_DROP_NO_OPENER, "rx_orphan", s2->rxOrphan);
    index = ss_emit(records, max, index, GENET_SS_RX_DROP_QUEUE_FULL, "rx_buffer_errors", s2->rxBufferErrors);
    index = ss_emit(records, max, index, GENET_SS_RX_ARP_IP_DROPPED, "rx_arp_ip_dropped", s2->rxIpArpDropped);
    index = ss_emit(records, max, index, GENET_SS_TX_DROPPED, "tx_dropped", counters->tx_dropped);
    index = ss_emit(records, max, index, GENET_SS_TX_QUEUED, "tx_queued", s2->txQueued);
    index = ss_emit(records, max, index, GENET_SS_IRQ0_COUNT, "irq0_count", counters->irq0_count);
    index = ss_emit(records, max, index, GENET_SS_IRQ0_TX_COUNT, "irq0_tx_count", counters->irq0_tx_count);
    index = ss_emit(records, max, index, GENET_SS_IRQ0_RX_COUNT, "irq0_rx_count", counters->irq0_rx_count);
    index = ss_emit(records, max, index, GENET_SS_IRQ0_OTHER_COUNT, "irq0_other_count", counters->irq0_other_count);

    /* the MAC's */
    for (ULONG i = 0; i < GENET_SS_MIB_COUNT; i++)
        index = ss_emit(records, max, index, genet_ss_mib_table[i].id, genet_ss_mib_table[i].name,
                        bcmgenet_mib_read(unit, genet_ss_mib_table[i].reg));
    index = ss_emit(records, max, index, GENET_SS_HW_RBUF_OVFL, "hw_rbuf_overflow", bcmgenet_mib_rbuf_ovfl(unit));

    header->RecordCountSupplied = index;
    return COMMAND_PROCESSED;
}

/* -------------------------------------------------------------- throughput --- */

/**
 * time_now() - Read the system time with GetSysTime().
 * @unit: the unit.
 * @now: receives the time; zero while the unit task has no timer.device.
 */
static void time_now(struct GenetUnit *unit, struct timeval *now)
{
    struct ExecBase *SysBase = unit->sysBase;
    (void)SysBase;
    struct Device *TimerBase = unit->timerBase;
    now->tv_secs = 0;
    now->tv_micro = 0;
    if (TimerBase != NULL)
        GetSysTime(now);
}

/**
 * throughput_fill() - Fill in the bytes each way since the window's bases,
 * and count one more update.
 * @unit: the unit.
 * @stats: the request's Sana2ThroughputStats.
 */
static void throughput_fill(struct GenetUnit *unit, struct Sana2ThroughputStats *stats)
{
    struct GenetS2 *s2 = unit->s2;
    s2->throughputUpdates++;
    s2_quad_set(&stats->s2ts_BytesSent, unit->internalStats.tx_bytes - s2->throughputBaseTx);
    s2_quad_set(&stats->s2ts_BytesReceived, unit->internalStats.rx_bytes - s2->throughputBaseRx);
    s2_quad_set(&stats->s2ts_Updates, s2->throughputUpdates);
}

/**
 * sana2_throughput_restart() - Start a new throughput window from here.
 * @unit: the unit.
 *
 * Unit task. The counters restart with each S2_ONLINE, and the window with
 * them.
 */
void sana2_throughput_restart(struct GenetUnit *unit)
{
    struct GenetS2 *s2 = unit->s2;
    s2->throughputBaseTx = unit->internalStats.tx_bytes;
    s2->throughputBaseRx = unit->internalStats.rx_bytes;
    time_now(unit, &s2->throughputStart);
}

/**
 * sana2_sample_throughput() - S2_SAMPLE_THROUGHPUT: sample the bytes each
 * way.
 * @unit: the unit.
 * @io: the request; ios2_StatData is the Sana2ThroughputStats.
 *
 * Unit task. Without s2ts_NotifyTask, one sample of the window since the last
 * and a new window from here. With it, continuous sampling: the request is
 * kept and updated by sana2_throughput_tick() until AbortIO, one such request
 * at a time.
 *
 * Return: COMMAND_PROCESSED for a single sample or a failure: io_Error
 * S2ERR_BAD_ARGUMENT / S2WERR_NULL_POINTER for no StatData, IOERR_BADLENGTH
 * when s2ts_Length is short of the structure, S2ERR_NO_RESOURCES when
 * another request is sampling already; COMMAND_SCHEDULED when kept for
 * continuous sampling.
 */
u32 sana2_sample_throughput(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct GenetS2 *s2 = unit->s2;
    struct Sana2ThroughputStats *stats = io->ios2_StatData;

    if (stats == NULL)
    {
        io->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
        io->ios2_WireError = S2WERR_NULL_POINTER;
        return COMMAND_PROCESSED;
    }
    if (stats->s2ts_Length < sizeof(struct Sana2ThroughputStats))
    {
        io->ios2_Req.io_Error = IOERR_BADLENGTH;
        return COMMAND_PROCESSED;
    }

    struct timeval now;
    time_now(unit, &now);
    stats->s2ts_Actual = sizeof(struct Sana2ThroughputStats);

    /* No task to notify: one sample of the window since the last, and a new
     * window from here. */
    if (stats->s2ts_NotifyTask == NULL)
    {
        stats->s2ts_StartTime = s2->throughputStart;
        stats->s2ts_EndTime = now;
        throughput_fill(unit, stats);
        sana2_throughput_restart(unit);
        return COMMAND_PROCESSED;
    }

    /* Continuous sampling, one request at a time, until AbortIO. */
    if (s2->throughputRequest != NULL)
    {
        io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        io->ios2_WireError = S2WERR_GENERIC_ERROR;
        return COMMAND_PROCESSED;
    }
    stats->s2ts_StartTime = now;
    stats->s2ts_EndTime = now;
    s2_quad_set(&stats->s2ts_BytesSent, 0);
    s2_quad_set(&stats->s2ts_BytesReceived, 0);
    s2_quad_set(&stats->s2ts_Updates, 0);
    s2->throughputUpdates = 0;
    sana2_throughput_restart(unit);
    s2->throughputRequest = io;
    return COMMAND_SCHEDULED;
}

/**
 * sana2_throughput_tick() - Update the sampling request, at most once a
 * second.
 * @unit: the unit.
 *
 * Unit task, on the housekeeping tick. An update fills in the counts and the
 * end time and signals s2ts_NotifyTask with s2ts_NotifyMask.
 */
void sana2_throughput_tick(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct IOSana2Req *io = unit->s2->throughputRequest;
    if (io == NULL)
        return;

    struct Sana2ThroughputStats *stats = io->ios2_StatData;
    struct timeval now;
    time_now(unit, &now);
    if (now.tv_secs <= stats->s2ts_EndTime.tv_secs)
        return;

    stats->s2ts_EndTime = now;
    throughput_fill(unit, stats);
    Signal(stats->s2ts_NotifyTask, stats->s2ts_NotifyMask);
}
