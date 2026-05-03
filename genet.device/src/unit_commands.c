// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/timer_protos.h>
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define TIMER_BASE_NAME unitTimerBase
#include <proto/timer.h>
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <devices/sana2.h>
#include <devices/sana2specialstats.h>
#include <devices/newstyle.h>

#include <device.h>
#include <debug.h>
#include <memory.h>
#include <types.h>

#include <genet/bcmgenet_mib.h>
#include <genet/genet_specialstats.h>

static const u16 GENET_SupportedCommands[] = {
    CMD_FLUSH,
    CMD_READ,
    CMD_WRITE,

    S2_DEVICEQUERY,
    S2_GETSTATIONADDRESS,
    S2_CONFIGINTERFACE,
    S2_ADDMULTICASTADDRESS,
    S2_DELMULTICASTADDRESS,
    S2_MULTICAST,
    S2_BROADCAST,
    // S2_TRACKTYPE,
    // S2_UNTRACKTYPE,
    // S2_GETTYPESTATS,
    S2_GETSPECIALSTATS,
    S2_GETGLOBALSTATS,
    S2_GETEXTENDEDGLOBALSTATS,
    S2_SAMPLE_THROUGHPUT,
    S2_ONEVENT,
    S2_READORPHAN,
    S2_ONLINE,
    S2_OFFLINE,
    S2_ADDMULTICASTADDRESSES,
    S2_DELMULTICASTADDRESSES,

    NSCMD_DEVICEQUERY,
    0};

/* Mask of events known by the driver */
#define EVENT_MASK (S2EVENT_ONLINE | S2EVENT_OFFLINE |       \
                    S2EVENT_TX | S2EVENT_RX | S2EVENT_BUFF | \
                    S2EVENT_ERROR | S2EVENT_HARDWARE | S2EVENT_SOFTWARE)

/* Report events to this unit */
void ReportEvents(struct GenetUnit *unit, u32 eventSet)
{
    KprintfH("[genet] %s: Reporting events %08lx\n", __func__, (ULONG)eventSet);

    /* Report event to every listener of every opener accepting the mask */
    for (struct MinNode *node = unit->openers.mlh_Head; node->mln_Succ; node = node->mln_Succ)
    {
        struct Opener *opener = (struct Opener *)node;
        struct MinNode *ioNode, *nextIoNode;
        ObtainSemaphore(&opener->openerSemaphore);
        for (ioNode = opener->eventQueue.mlh_Head; (nextIoNode = ioNode->mln_Succ) != NULL; ioNode = nextIoNode)
        {
            struct IOSana2Req *io = (struct IOSana2Req *)ioNode;
            /* Check if event mask in WireError fits the events occured */
            if (io->ios2_WireError & eventSet)
            {
                /* We have a match. Leave only matching events in wire error */
                io->ios2_WireError &= eventSet;

                /* Reply it */
                Remove((struct Node *)io);
                ReplyMsg((struct Message *)io);
                break; /* Only one event per opener */
            }
        }
        ReleaseSemaphore(&opener->openerSemaphore);
    }
    KprintfH("[genet] %s: Reporting done\n", __func__);
}

void UpdateThroughputStats(struct GenetUnit *unit)
{
    struct throughput_stats *throughput = &unit->throughputStats;
    struct IOSana2Req *io = throughput->req;
    if (io == NULL) return;

    struct Sana2ThroughputStats *ts = (struct Sana2ThroughputStats *)io->ios2_StatData;
    struct Device *unitTimerBase = unit->timerBase;
    struct timeval now;
    now.tv_secs = 0; now.tv_micro = 0;
    if (unitTimerBase) GetSysTime(&now);

    if (now.tv_secs <= ts->s2ts_EndTime.tv_secs) return;

    ts->s2ts_EndTime = now;
    u64 sent = unit->internalStats.tx_bytes - throughput->base_tx_bytes;
    u64 recv = unit->internalStats.rx_bytes  - throughput->base_rx_bytes;
    ts->s2ts_BytesSent.s2q_High     = (ULONG)(sent >> 32);
    ts->s2ts_BytesSent.s2q_Low      = (ULONG)(sent & 0xFFFFFFFFUL);
    ts->s2ts_BytesReceived.s2q_High = (ULONG)(recv >> 32);
    ts->s2ts_BytesReceived.s2q_Low  = (ULONG)(recv & 0xFFFFFFFFUL);
    throughput->sync_updates++;
    ts->s2ts_Updates.s2q_Low = (ULONG)(throughput->sync_updates & 0xFFFFFFFFUL);
    ts->s2ts_Updates.s2q_High = (ULONG)(throughput->sync_updates >> 32);

    Signal(ts->s2ts_NotifyTask, ts->s2ts_NotifyMask);
}

BOOL UnitCancelThroughput(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct throughput_stats *throughput = &unit->throughputStats;

    if (throughput->req != io)
    {
        return FALSE;
    }

    throughput->req = NULL;
    io->ios2_Req.io_Error = IOERR_ABORTED;
    io->ios2_WireError = S2WERR_GENERIC_ERROR;
    ReplyMsg((struct Message *)io);
    return TRUE;
}

static u32 Do_S2_GETGLOBALSTATS(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    KprintfH("[genet] %s: S2_GETGLOBALSTATS\n", __func__);
    struct Sana2DeviceStats *stats = (struct Sana2DeviceStats *)io->ios2_StatData;
    stats->PacketsReceived      = (ULONG)unit->internalStats.rx_packets;
    stats->PacketsSent          = (ULONG)unit->internalStats.tx_packets;
    stats->UnknownTypesReceived = (ULONG)unit->internalStats.rx_orphan;
    stats->Overruns             = unit->internalStats.rx_overruns;
    stats->BadData              = unit->internalStats.rx_other_errors
                                + unit->internalStats.rx_crc_errors
                                + unit->internalStats.rx_over_errors
                                + unit->internalStats.rx_frame_errors
                                + unit->internalStats.rx_length_errors
                                + unit->internalStats.rx_fragmented_errors;
    stats->LastStart            = unit->internalStats.last_start;
    io->ios2_Req.io_Error = S2ERR_NO_ERROR;
    return COMMAND_PROCESSED;
}

static u32 Do_S2_GETEXTENDEDGLOBALSTATS(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    KprintfH("[genet] %s: S2_GETEXTENDEDGLOBALSTATS\n", __func__);
    struct Sana2ExtDeviceStats *xs = (struct Sana2ExtDeviceStats *)io->ios2_StatData;
    if (xs == NULL)
    {
        io->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
        return COMMAND_PROCESSED;
    }

    if (xs->s2xds_Length < (ULONG)sizeof(struct Sana2ExtDeviceStats))
    {
        io->ios2_Req.io_Error = IOERR_BADLENGTH;
        return COMMAND_PROCESSED;
    }

    xs->s2xds_Actual = (ULONG)sizeof(struct Sana2ExtDeviceStats);

#define SPLIT64(f, v) do { (f).s2q_High = (ULONG)((v) >> 32); (f).s2q_Low = (ULONG)((v) & 0xFFFFFFFFUL); } while(0)
    SPLIT64(xs->s2xds_PacketsReceived,      unit->internalStats.rx_packets);
    SPLIT64(xs->s2xds_PacketsSent,          unit->internalStats.tx_packets);
    SPLIT64(xs->s2xds_UnknownTypesReceived, unit->internalStats.rx_orphan);
    SPLIT64(xs->s2xds_Overruns, (u64)unit->internalStats.rx_overruns);
    u64 baddata = (u64)unit->internalStats.rx_other_errors
                + unit->internalStats.rx_crc_errors
                + unit->internalStats.rx_over_errors
                + unit->internalStats.rx_frame_errors
                + unit->internalStats.rx_length_errors
                + unit->internalStats.rx_fragmented_errors;
    SPLIT64(xs->s2xds_BadData, baddata);
    SPLIT64(xs->s2xds_Reconfigurations, (u64)unit->reconfigurations);
#undef SPLIT64
    xs->s2xds_LastStart = unit->internalStats.last_start;
    io->ios2_Req.io_Error = S2ERR_NO_ERROR;
    return COMMAND_PROCESSED;
}

static inline ULONG sat_u64_to_ulong(u64 v)
{
    return (v > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (ULONG)v;
}

static ULONG emit_ss(struct Sana2SpecialStatRecord *rec, ULONG max, ULONG idx,
                     ULONG type, const char *name, u64 value)
{
    if (idx >= max) return idx;
    rec[idx].Type   = type;
    rec[idx].Count  = sat_u64_to_ulong(value);
    rec[idx].String = (STRPTR)name;
    return idx + 1;
}

static u32 Do_S2_GETSPECIALSTATS(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    KprintfH("[genet] %s: S2_GETSPECIALSTATS\n", __func__);

    struct Sana2SpecialStatHeader *hdr = (struct Sana2SpecialStatHeader *)io->ios2_StatData;
    if (hdr == NULL) {
        io->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
        return COMMAND_PROCESSED;
    }

    struct mib_snapshot mib;
    bcmgenet_read_mib_snapshot(unit, &mib);
    const struct internal_stats *st = &unit->internalStats;

    struct Sana2SpecialStatRecord *rec = (struct Sana2SpecialStatRecord *)(hdr + 1);
    ULONG max = hdr->RecordCountMax;
    ULONG idx = 0;

    /* Driver software counters */
    idx = emit_ss(rec, max, idx, GENET_SS_RX_OVERRUNS,        "rx_overruns",        st->rx_overruns);
    idx = emit_ss(rec, max, idx, GENET_SS_RX_CRC_ERRORS,      "rx_crc_errors",      st->rx_crc_errors);
    idx = emit_ss(rec, max, idx, GENET_SS_RX_OVER_ERRORS,     "rx_over_errors",     st->rx_over_errors);
    idx = emit_ss(rec, max, idx, GENET_SS_RX_FRAME_ERRORS,    "rx_frame_errors",    st->rx_frame_errors);
    idx = emit_ss(rec, max, idx, GENET_SS_RX_LENGTH_ERRORS,   "rx_length_errors",   st->rx_length_errors);
    idx = emit_ss(rec, max, idx, GENET_SS_RX_FRAGMENTED,      "rx_fragmented",      st->rx_fragmented_errors);
    idx = emit_ss(rec, max, idx, GENET_SS_RX_OTHER_ERRORS,    "rx_other_errors",    st->rx_other_errors);
    idx = emit_ss(rec, max, idx, GENET_SS_RX_DROP_NO_OPENER,  "rx_orphan",          st->rx_orphan);
    idx = emit_ss(rec, max, idx, GENET_SS_RX_DROP_QUEUE_FULL, "rx_buffer_errors",   st->rx_buffer_errors);
    idx = emit_ss(rec, max, idx, GENET_SS_RX_ARP_IP_DROPPED,  "rx_arp_ip_dropped",  st->rx_arp_ip_dropped);

    idx = emit_ss(rec, max, idx, GENET_SS_TX_DROPPED,         "tx_dropped",         st->tx_dropped);
    idx = emit_ss(rec, max, idx, GENET_SS_TX_DMA,             "tx_dma",             st->tx_dma);
    idx = emit_ss(rec, max, idx, GENET_SS_TX_COPY,            "tx_copy",            st->tx_copy);
    idx = emit_ss(rec, max, idx, GENET_SS_IRQ0_COUNT,         "irq0_count",         st->irq0_count);
    idx = emit_ss(rec, max, idx, GENET_SS_IRQ0_TX_COUNT,      "irq0_tx_count",      st->irq0_tx_count);
    idx = emit_ss(rec, max, idx, GENET_SS_IRQ0_RX_COUNT,      "irq0_rx_count",      st->irq0_rx_count);
    idx = emit_ss(rec, max, idx, GENET_SS_IRQ0_OTHER_COUNT,   "irq0_other_count",   st->irq0_other_count);

    /* Hardware MIB RX (counters reset at S2_ONLINE) */
    idx = emit_ss(rec, max, idx, GENET_SS_HW_RX_PKTS,         "hw_rx_pkts",         mib.rx_pkts);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_RX_BYTES,        "hw_rx_bytes",        mib.rx_bytes);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_RX_MULTICAST,    "hw_rx_multicast",    mib.rx_mca);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_RX_BROADCAST,    "hw_rx_broadcast",    mib.rx_bca);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_RX_UNICAST,      "hw_rx_unicast",      mib.rx_uc);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_RX_FCS_ERR,      "hw_rx_fcs_err",      mib.rx_fcs);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_RX_ALIGN_ERR,    "hw_rx_align_err",    mib.rx_aln);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_RX_PAUSE,        "hw_rx_pause",        mib.rx_pf);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_RX_OVERSIZE,     "hw_rx_oversize",     mib.rx_ovr);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_RX_JABBER,       "hw_rx_jabber",       mib.rx_jbr);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_RX_GOOD,         "hw_rx_good",         mib.rx_pok);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_RX_RUNT,         "hw_rx_runt",         mib.rx_runt);

    /* Hardware MIB TX */
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_PKTS,         "hw_tx_pkts",         mib.tx_pkts);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_BYTES,        "hw_tx_bytes",        mib.tx_bytes);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_MULTICAST,    "hw_tx_multicast",    mib.tx_mca);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_BROADCAST,    "hw_tx_broadcast",    mib.tx_bca);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_UNICAST,      "hw_tx_unicast",      mib.tx_uc);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_FCS_ERR,      "hw_tx_fcs_err",      mib.tx_fcs);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_PAUSE,        "hw_tx_pause",        mib.tx_pf);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_SINGLE_COL,   "hw_tx_single_col",   mib.tx_scl);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_MULTI_COL,    "hw_tx_multi_col",    mib.tx_mcl);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_LATE_COL,     "hw_tx_late_col",     mib.tx_lcl);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_EXCESS_COL,   "hw_tx_excess_col",   mib.tx_ecl);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_TOTAL_COL,    "hw_tx_total_col",    mib.tx_ncl);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_DEFER,        "hw_tx_defer",        mib.tx_drf);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_EXCESS_DEFER, "hw_tx_excess_defer", mib.tx_edf);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_JABBER,       "hw_tx_jabber",       mib.tx_jbr);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_OVERSIZE,     "hw_tx_oversize",     mib.tx_ovr);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_TX_GOOD,         "hw_tx_good",         mib.tx_pok);

    /* Hardware MAC misc */
    idx = emit_ss(rec, max, idx, GENET_SS_HW_RBUF_OVFL,       "hw_rbuf_overflow",   mib.rbuf_ovfl);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_RBUF_ERR,        "hw_rbuf_err",        mib.rbuf_err);
    idx = emit_ss(rec, max, idx, GENET_SS_HW_MDF_ERR,         "hw_mdf_err",         mib.mdf_err);

    hdr->RecordCountSupplied = idx;
    io->ios2_Req.io_Error = S2ERR_NO_ERROR;
    return COMMAND_PROCESSED;
}

static u32 Do_S2_SAMPLE_THROUGHPUT(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    struct throughput_stats *throughput = &unit->throughputStats;
    KprintfH("[genet] %s: S2_SAMPLE_THROUGHPUT\n", __func__);

    if (io->ios2_StatData == NULL) {
        io->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
        return COMMAND_PROCESSED;
    }

    struct Sana2ThroughputStats *ts = (struct Sana2ThroughputStats *)io->ios2_StatData;
    struct Device *unitTimerBase = unit->timerBase;

    ts->s2ts_Actual = (ULONG)sizeof(struct Sana2ThroughputStats);
    ts->s2ts_BytesSent.s2q_High     = 0; ts->s2ts_BytesSent.s2q_Low     = 0;
    ts->s2ts_BytesReceived.s2q_High = 0; ts->s2ts_BytesReceived.s2q_Low = 0;
    ts->s2ts_Updates.s2q_High       = 0; ts->s2ts_Updates.s2q_Low       = 0;
    struct timeval now;
    now.tv_secs = 0; now.tv_micro = 0;
    if (unitTimerBase) GetSysTime(&now);

    if (ts->s2ts_NotifyTask == NULL) {
        ts->s2ts_StartTime = throughput->window_start;
        ts->s2ts_EndTime   = now;
        u64 sent = unit->internalStats.tx_bytes - throughput->base_tx_bytes;
        u64 recv = unit->internalStats.rx_bytes  - throughput->base_rx_bytes;
        ts->s2ts_BytesSent.s2q_High     = (ULONG)(sent >> 32);
        ts->s2ts_BytesSent.s2q_Low      = (ULONG)(sent & 0xFFFFFFFFUL);
        ts->s2ts_BytesReceived.s2q_High = (ULONG)(recv >> 32);
        ts->s2ts_BytesReceived.s2q_Low  = (ULONG)(recv & 0xFFFFFFFFUL);
        throughput->sync_updates++;
        ts->s2ts_Updates.s2q_Low = (ULONG)(throughput->sync_updates & 0xFFFFFFFFUL);
        ts->s2ts_Updates.s2q_High = (ULONG)(throughput->sync_updates >> 32);
        throughput->base_tx_bytes = unit->internalStats.tx_bytes;
        throughput->base_rx_bytes = unit->internalStats.rx_bytes;
        throughput->window_start  = now;
        io->ios2_Req.io_Error = S2ERR_NO_ERROR;
        return COMMAND_PROCESSED;
    }

    if (throughput->req != NULL) {
        io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        return COMMAND_PROCESSED;
    }

    ts->s2ts_StartTime = now;
    ts->s2ts_EndTime   = now;
    throughput->base_tx_bytes = unit->internalStats.tx_bytes;
    throughput->base_rx_bytes = unit->internalStats.rx_bytes;
    throughput->sync_updates = 0;
    throughput->req = io;
    io->ios2_Req.io_Error = S2ERR_NO_ERROR;
    return COMMAND_SCHEDULED;
}

static u32 Do_S2_ONEVENT(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    KprintfH("[genet] %s: S2_ONEVENT %08lx\n", __func__, io->ios2_WireError);

    /* If any unsupported events are requested, report an error */
    if (io->ios2_WireError & ~(EVENT_MASK))
    {
        Kprintf("[genet] %s: Unsupported event requested: %08lx\n", __func__, io->ios2_WireError);
        io->ios2_Req.io_Error = S2ERR_NOT_SUPPORTED;
        io->ios2_WireError = S2WERR_BAD_EVENT;
        return COMMAND_PROCESSED;
    }

    u32 preset = (unit->state == STATE_ONLINE) ? S2EVENT_ONLINE : S2EVENT_OFFLINE;

    /* If expected flags match preset, return back (almost) immediately */
    if (io->ios2_WireError & preset)
    {
        KprintfH("[genet] %s: Event preset %08lx matches requested %08lx, returning immediately\n", __func__, preset, io->ios2_WireError);
        io->ios2_WireError &= preset;
        return COMMAND_PROCESSED;
    }
    else
    {
        KprintfH("[genet] %s: Adding to event listener list, preset %08lx\n", __func__, preset);
        /* Remove QUICK flag and put message on event listener list */
        struct Opener *opener = io->ios2_BufferManagement;
        // io->ios2_Req.io_Flags &= ~IOF_QUICK;
        ObtainSemaphore(&opener->openerSemaphore);
        AddTailMinList(&opener->eventQueue, (struct MinNode *)io);
        ReleaseSemaphore(&opener->openerSemaphore);
        return COMMAND_SCHEDULED;
    }
}

static u32 Do_CMD_FLUSH(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    KprintfH("[genet] %s: CMD_FLUSH\n", __func__);

    struct IOSana2Req *req;
    /* Flush and cancel all requests */
    while ((req = (struct IOSana2Req *)GetMsg(&unit->unit.unit_MsgPort)))
    {
        req->ios2_Req.io_Error = IOERR_ABORTED;
        req->ios2_WireError = 0;
        ReplyMsg((struct Message *)req);
    }

    /* For every opener, flush all internal queues */
    for (struct MinNode *node = unit->openers.mlh_Head; node->mln_Succ; node = node->mln_Succ)
    {
        struct Opener *opener = (struct Opener *)node;
        ObtainSemaphore(&opener->openerSemaphore);
        while ((req = (struct IOSana2Req *)RemHeadMinList(&opener->orphanQueue)))
        {
            req->ios2_Req.io_Error = IOERR_ABORTED;
            req->ios2_WireError = 0;
            ReplyMsg((struct Message *)req);
        }

        while ((req = (struct IOSana2Req *)RemHeadMinList(&opener->eventQueue)))
        {
            req->ios2_Req.io_Error = IOERR_ABORTED;
            req->ios2_WireError = 0;
            ReplyMsg((struct Message *)req);
        }

        while ((req = (struct IOSana2Req *)RemHeadMinList(&opener->readQueue)))
        {
            req->ios2_Req.io_Error = IOERR_ABORTED;
            req->ios2_WireError = 0;
            ReplyMsg((struct Message *)req);
        }

        while ((req = (struct IOSana2Req *)RemHeadMinList(&opener->ipv4Queue)))
        {
            req->ios2_Req.io_Error = IOERR_ABORTED;
            req->ios2_WireError = 0;
            ReplyMsg((struct Message *)req);
        }

        while ((req = (struct IOSana2Req *)RemHeadMinList(&opener->arpQueue)))
        {
            req->ios2_Req.io_Error = IOERR_ABORTED;
            req->ios2_WireError = 0;
            ReplyMsg((struct Message *)req);
        }
        ReleaseSemaphore(&opener->openerSemaphore);
    }
    KprintfH("[genet] %s: Flush completed\n", __func__);

    return COMMAND_PROCESSED;
}

static u32 Do_NSCMD_DEVICEQUERY(struct IOStdReq *io)
{
    KprintfH("[genet] %s: NSCMD_DEVICEQUERY\n", __func__);
    struct NSDeviceQueryResult *dq = io->io_Data;

    /* Fill out structure */
    dq->nsdqr_SizeAvailable = sizeof(struct NSDeviceQueryResult);
    if (io->io_Length < dq->nsdqr_SizeAvailable)
    {
        io->io_Error = IOERR_BADLENGTH;
        return COMMAND_PROCESSED;
    }
    dq->nsdqr_DeviceType = NSDEVTYPE_SANA2;
    dq->nsdqr_DeviceSubType = 0;
    dq->nsdqr_SupportedCommands = (UWORD *)GENET_SupportedCommands;
    io->io_Actual = dq->nsdqr_SizeAvailable;
    io->io_Error = 0;

    return COMMAND_PROCESSED;
}

static inline u32 Do_CMD_READ(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    KprintfH("[genet] %s: CMD_READ for packet type 0x%lx\n", __func__, io->ios2_PacketType);

    if (unlikely(unit->state != STATE_ONLINE))
    {
        Kprintf("[genet] %s: Unit is offline, cannot read\n", __func__);
        io->ios2_WireError = S2WERR_UNIT_OFFLINE;
        io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
        return COMMAND_PROCESSED;
    }

    struct Opener *opener = io->ios2_BufferManagement;
    u16 packetType = (u16)io->ios2_PacketType;

    /* Get the appropriate queue for this packet type */
    struct MinList *queue = GetPacketTypeQueue(opener, packetType);

    /* Queue the request */
    io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
    ObtainSemaphore(&opener->openerSemaphore);
    AddTailMinList(queue, (struct MinNode *)io);
    ReleaseSemaphore(&opener->openerSemaphore);

    KprintfH("[genet] %s: Queued CMD_READ request for packet type 0x%lx\n", __func__, (ULONG)packetType);
    return COMMAND_SCHEDULED;
}

static inline u32 Do_S2_READORPHAN(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    KprintfH("[genet] %s: S2_READORPHAN\n", __func__);

    if (unlikely(unit->state != STATE_ONLINE))
    {
        Kprintf("[genet] %s: Unit is offline, cannot read orphan\n", __func__);
        io->ios2_WireError = S2WERR_UNIT_OFFLINE;
        io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
        return COMMAND_PROCESSED;
    }

    struct Opener *opener = io->ios2_BufferManagement;
    // io->ios2_Req.io_Flags &= ~IOF_QUICK;
    AddTailMinList(&opener->orphanQueue, (struct MinNode *)io);
    return COMMAND_SCHEDULED;
}

static u32 Do_S2_DEVICEQUERY(struct IOSana2Req *io)
{
    KprintfH("[genet] %s: S2_DEVICEQUERY\n", __func__);

    struct Sana2DeviceQuery *info = io->ios2_StatData;

    info->SizeSupplied = sizeof(struct Sana2DeviceQuery) - sizeof(info->RawMTU);
    info->DevQueryFormat = 0;
    info->DeviceLevel = 0;
    info->AddrFieldSize = 48;
    info->MTU = ETH_DATA_LEN;
    info->BPS = 1000000000;
    info->HardwareType = S2WireType_Ethernet;
    if (info->SizeAvailable >= sizeof(struct Sana2DeviceQuery))
    {
        info->RawMTU = ETH_DATA_LEN + ETH_HLEN + VLAN_HLEN;
        info->SizeSupplied += sizeof(info->RawMTU);
    }
    return COMMAND_PROCESSED;
}

static u32 Do_S2_ONLINE(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    KprintfH("[genet] %s: S2_ONLINE\n", __func__);

    if(unit->state == STATE_UNCONFIGURED)
    {
        Kprintf("[genet] %s: Unit is unconfigured, cannot go online\n", __func__);
        io->ios2_Req.io_Error = S2ERR_BAD_STATE;
        io->ios2_WireError = S2WERR_NOT_CONFIGURED;
        ReportEvents(unit, S2EVENT_SOFTWARE | S2EVENT_ERROR);
        return COMMAND_PROCESSED;
    }

    /* If unit was not yet online, report event now */
    if (unit->state != STATE_ONLINE)
    {
        Kprintf("[genet] %s: Bringing unit online\n", __func__);
        /* Count this as a reconfiguration if we've been online before */
        if (unit->state == STATE_OFFLINE)
            unit->reconfigurations++;

        mem_zero(&unit->internalStats, sizeof(unit->internalStats));
        mem_zero(&unit->throughputStats, sizeof(unit->throughputStats));
        bcmgenet_reset_mib_counters(unit);

        struct Device *unitTimerBase = unit->timerBase;
        if (unitTimerBase != NULL)
        {
            GetSysTime(&unit->internalStats.last_start);
            KprintfH("[genet] %s: statistics zeroed, LastStart: %ld\n", __func__, unit->internalStats.last_start.tv_secs);
        }
        else
        {
            Kprintf("[genet] %s: Timer device is not available, continuing without LastStart reset\n", __func__);
        }

        u32 result = UnitOnline(unit);
        if (result != S2ERR_NO_ERROR)
        {
            Kprintf("[genet] %s: Failed to bring unit online: %lu\n", __func__, result);
            io->ios2_Req.io_Error = (BYTE)result;
            io->ios2_WireError = S2WERR_GENERIC_ERROR;
            ReportEvents(unit, S2EVENT_SOFTWARE | S2EVENT_ERROR);
        }
        else
        {
            KprintfH("[genet] %s: Unit online, about to report events\n", __func__);
            ReportEvents(unit, S2EVENT_ONLINE);
        }
    }

    return COMMAND_PROCESSED;
}

static u32 Do_S2_CONFIGINTERFACE(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    KprintfH("[genet] %s: S2_CONFIGINTERFACE\n", __func__);

    if (unit->state == STATE_UNCONFIGURED)
    {
        CopyMem(io->ios2_SrcAddr, unit->currentMacAddress, sizeof(unit->currentMacAddress));
        KprintfH("[genet] %s: Setting current MAC address to %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                __func__,
                unit->currentMacAddress[0], unit->currentMacAddress[1],
                unit->currentMacAddress[2], unit->currentMacAddress[3],
                unit->currentMacAddress[4], unit->currentMacAddress[5]);

        u32 result = UnitConfigure(unit);
        if (result != S2ERR_NO_ERROR)
        {
            Kprintf("[genet] %s: Failed to configure unit: %lu\n", __func__, result);
            io->ios2_Req.io_Error = (BYTE)result;
            io->ios2_WireError = S2WERR_GENERIC_ERROR;
            ReportEvents(unit, S2EVENT_SOFTWARE | S2EVENT_ERROR);
        }
        else
        {
            ReportEvents(unit, S2EVENT_CONFIGCHANGED);
            Do_S2_ONLINE(io);
        }
    }
    else
    {
        KprintfH("[genet] %s: Unit already configured\n", __func__);
        io->ios2_Req.io_Error = S2ERR_BAD_STATE;
        io->ios2_WireError = S2WERR_IS_CONFIGURED;
        ReportEvents(unit, S2EVENT_SOFTWARE | S2EVENT_ERROR);
    }

    CopyMem(unit->currentMacAddress, io->ios2_SrcAddr, sizeof(unit->currentMacAddress));
    return COMMAND_PROCESSED;
}

static u32 Do_S2_OFFLINE(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    Kprintf("[genet] %s: S2_OFFLINE\n", __func__);

    /* Flush and cancel all requests */
    Do_CMD_FLUSH(io);

    /* If unit was ONLINE before, report offline event now */
    if (unit->state == STATE_ONLINE)
    {
        UnitOffline(unit);
        ReportEvents(unit, S2EVENT_OFFLINE);
    }

    return COMMAND_PROCESSED;
}

void ProcessCommand(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;

    u32 complete = COMMAND_SCHEDULED;

    /*
        Only NSCMD_DEVICEQUERY can use standard sized request. All other must be of
        size IOSana2Req
    */
    if (io->ios2_Req.io_Message.mn_Length < sizeof(struct IOSana2Req) &&
        io->ios2_Req.io_Command != NSCMD_DEVICEQUERY)
    {
        io->ios2_Req.io_Error = IOERR_BADLENGTH;
        complete = COMMAND_PROCESSED;
    }
    else
    {
        io->ios2_Req.io_Error = S2ERR_NO_ERROR;

        switch (io->ios2_Req.io_Command)
        {
        case CMD_READ:
            complete = Do_CMD_READ(io);
            break;

        case CMD_FLUSH:
            complete = Do_CMD_FLUSH(io);
            break;

        case NSCMD_DEVICEQUERY:
            complete = Do_NSCMD_DEVICEQUERY((struct IOStdReq *)io);
            break;

        case S2_DEVICEQUERY:
            complete = Do_S2_DEVICEQUERY(io);
            break;

        case S2_GETSTATIONADDRESS:
            KprintfH("[genet] %s: S2_GETSTATIONADDRESS\n", __func__);
            CopyMem(unit->localMacAddress, io->ios2_DstAddr, 6);
            CopyMem(unit->currentMacAddress, io->ios2_SrcAddr, 6);
            io->ios2_Req.io_Error = S2ERR_NO_ERROR;
            complete = COMMAND_PROCESSED;
            break;

        case S2_GETGLOBALSTATS:
            complete = Do_S2_GETGLOBALSTATS(io);
            break;

        case S2_GETEXTENDEDGLOBALSTATS:
            complete = Do_S2_GETEXTENDEDGLOBALSTATS(io);
            break;

        case S2_SAMPLE_THROUGHPUT:
            complete = Do_S2_SAMPLE_THROUGHPUT(io);
            break;

        case S2_GETSPECIALSTATS:
            complete = Do_S2_GETSPECIALSTATS(io);
            break;

        case S2_ADDMULTICASTADDRESS: /* Fallthrough */
        case S2_ADDMULTICASTADDRESSES:
            complete = Do_S2_ADDMULTICASTADDRESSES(io);
            break;

        case S2_DELMULTICASTADDRESS: /* Fallthrough */
        case S2_DELMULTICASTADDRESSES:
            complete = Do_S2_DELMULTICASTADDRESSES(io);
            break;

        case S2_CONFIGINTERFACE:
            complete = Do_S2_CONFIGINTERFACE(io);
            break;

        case S2_ONLINE:
            complete = Do_S2_ONLINE(io);
            break;

        case S2_OFFLINE:
            complete = Do_S2_OFFLINE(io);
            break;

        case S2_READORPHAN:
            complete = Do_S2_READORPHAN(io);
            break;

        case S2_ONEVENT:
            complete = Do_S2_ONEVENT(io);
            break;

        default:
            io->ios2_Req.io_Error = IOERR_NOCMD;
            complete = COMMAND_PROCESSED;
            break;
        }
    }

    // If command is complete and not quick, reply it now
    if (complete == COMMAND_PROCESSED && !(io->ios2_Req.io_Flags & IOF_QUICK))
    {
        ReplyMsg((struct Message *)io);
    }
}
