// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * netdev_api.c — the netdev ABI personality: command processing (unit task),
 * the direct-call ops table (foreign task context), RX buffer pool and the
 * SPSC rings that decouple the two.
 *
 * Context rules (see netdev.h): ndo_* entries are called from arbitrary
 * tasks, serialized by the stack's core lock — one logical producer; every
 * nso_* callback is made from the unit task only. The two contexts share
 * nothing but single-producer/single-consumer rings, so the datapath takes
 * no lock at all.
 */

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <devices/newstyle.h>
#include <exec/errors.h>

#include <cache_ops.h>
#include <debug.h>
#include <device.h>
#include <iomem.h>
#include <memory.h>

#include <genet/bcmgenet-mib.h>
#include <genet/bcmgenet-regs.h>
#include <genet/bcmgenet.h>
#include <genet/phy.h>

/* ------------------------------------------------------------------ pool --- */
#ifdef DEBUG
/* TRUE iff the pointer is a valid, slot-aligned pool buffer. */
static BOOL netdev_rx_buf_ok(const struct GenetUnit *unit, APTR buffer)
{
    ULONG off = (ULONG)buffer - (ULONG)unit->ndRxSlab;
    return unit->ndRxSlab != NULL &&
           off < unit->ndRxPoolTotal * RX_BUF_LENGTH &&
           (off % RX_BUF_LENGTH) == 0;
}
#endif

APTR netdev_rx_pop(struct GenetUnit *unit)
{
    if (unit->ndRxFreeCount == 0)
        return NULL;
    return unit->ndRxFree[--unit->ndRxFreeCount];
}

void netdev_rx_push(struct GenetUnit *unit, APTR buffer)
{
#ifdef DEBUG
    /* Guard the pool invariant (live buffers == pool size) instead of
     * trusting it: a double-release or a poisoned recycle-ring entry would
     * otherwise overrun ndRxFree[] and re-arm a buffer twice — the DMA
     * writes through anything that follows. Refusing leaks one buffer;
     * corrupting loses the machine. */
    if (!netdev_rx_buf_ok(unit, buffer) ||
        unit->ndRxFreeCount >= unit->ndRxPoolTotal)
    {
        Kprintf("[genet] %s: RX-POOL-GUARD: push rejected buf=%lx free=%lu total=%lu\n",
                __func__, buffer, (ULONG)unit->ndRxFreeCount, (ULONG)unit->ndRxPoolTotal);
        return;
    }
#endif
    unit->ndRxFree[unit->ndRxFreeCount++] = buffer;
}

void netdev_drain_recycle(struct GenetUnit *unit)
{
    u32 cons = unit->ndRecycleCons;
    /* SPSC snapshot: an entry the stack releases after this read just waits
     * for the next drain. Snapshotting also lets the batch know its last op. */
    u32 prod = unit->ndRecycleProd;
    while (cons != prod)
    {
        APTR buffer = unit->ndRecycle[cons & unit->ndRecycleMask];
        cons++;

#ifdef DEBUG
        /* a poisoned ring entry must not reach the cache op (a wild-range
         * civac clean writes stale lines over foreign memory) */
        if (!netdev_rx_buf_ok(unit, buffer))
        {
            Kprintf("[genet] %s: RX-POOL-GUARD: recycle rejected buf=%lx\n",
                    __func__, buffer);
            unit->ndRxHeld--;
            continue;
        }
#endif

        /* MANDATORY clean+invalidate before the buffer is re-armed for RX DMA
         * (same pre-arm contract as nvme_cache_flush(to_device=FALSE) and
         * xhci's IN-transfer flush). RX pbufs are NOT read-only: lwIP writes
         * into them — ip4_reass overlays its ip_reass_helper on the IP header
         * of every queued fragment — leaving DIRTY lines. A dirty line at DMA
         * time corrupts the frame no matter what runs afterwards: natural
         * eviction writes it back over the DMA'd payload, and the post-DMA
         * invalidate itself does too (ARMv8 permits, and Cortex-A cores
         * implement, dc ivac on a dirty line as clean+invalidate). Eliding
         * this corrupts fragmented-UDP RX; the pre-arm clean is mandatory.
         *
         * The batch pays ONE closing barrier: every op but the last carries
         * DMAF_NoSync (cache_ops.h); the ring is re-armed only later via MMIO,
         * well after the final op's dsb. */
        cache_pre_dma(buffer, RX_BUF_LENGTH, (cons != prod) ? DMAF_NoSync : 0);

        netdev_rx_push(unit, buffer);
        unit->ndRxHeld--;
    }

    asm volatile("" ::: "memory");
    unit->ndRecycleCons = cons;
}

/* ------------------------------------------------------------- link state --- */

void netdev_link_update(struct GenetUnit *unit, BOOL up, BOOL force)
{
    struct NetDevLinkState fresh;
    fresh.ndls_Flags = 0;
    fresh.ndls_SpeedMbps = 0;

    if (up && unit->phydev != NULL)
    {
        fresh.ndls_Flags = NDLF_UP;
        if (unit->phydev->duplex)
            fresh.ndls_Flags |= NDLF_FULL_DUPLEX;
        fresh.ndls_SpeedMbps = (UWORD)unit->phydev->speed;
    }

    if (!force &&
        fresh.ndls_Flags == unit->ndLink.ndls_Flags &&
        fresh.ndls_SpeedMbps == unit->ndLink.ndls_SpeedMbps)
        return;

    unit->ndLink = fresh;
    Kprintf("[genet] %s: link %s, %lu Mbps\n", __func__,
            (fresh.ndls_Flags & NDLF_UP) ? "up" : "down",
            (ULONG)fresh.ndls_SpeedMbps);

    if (unit->ndStackOps != NULL)
        unit->ndStackOps->nso_LinkChange(unit->ndStackCtx, &unit->ndLink);
}

/* -------------------------------------------------- direct-call ops table --- */

static LONG genet_ndo_txsubmit(APTR drvctx, const struct NetDevTxDesc *descs, ULONG count)
{
    struct GenetUnit *unit = drvctx;

    /* Marks the call in progress for the STOP quiesce, which must not finish
     * while a submitter that already passed the ndStarted gate is still
     * queueing cookies — those would never be completed. Plain ++/-- is enough:
     * the stack serializes its own submits, so this has one writer. */
    unit->ndTxBusy++;
    asm volatile("" ::: "memory");

    LONG accepted = bcmgenet_netdev_tx_submit(unit, descs, count);

    asm volatile("" ::: "memory");
    unit->ndTxBusy--;
    return accepted;
}

/* Ring the deferred TX doorbell for the batch staged by genet_ndo_txsubmit
 * calls since the last kick. No ndTxBusy guard: it queues no cookies, just
 * publishes the producer index, so it cannot extend the STOP quiesce. */
static VOID genet_ndo_txkick(APTR drvctx)
{
    struct GenetUnit *unit = drvctx;
    bcmgenet_netdev_tx_kick(unit);
}

static VOID genet_ndo_rxrelease(APTR drvctx, APTR cookie)
{
    struct GenetUnit *unit = drvctx;

    /* SPSC: producer side. Single producer by contract (the stack's core
     * lock serializes releases); capacity covers the pool size, so the
     * ring cannot fill. */
    u32 prod = unit->ndRecycleProd;
    unit->ndRecycle[prod & unit->ndRecycleMask] = cookie;
    asm volatile("" ::: "memory");
    unit->ndRecycleProd = prod + 1;
}

static APTR genet_ndo_dmaalloc(APTR drvctx, ULONG size, ULONG align)
{
    struct GenetUnit *unit = drvctx;
    return dma_alloc(unit->dmaPool, align, size);
}

static VOID genet_ndo_dmafree(APTR drvctx, APTR ptr, ULONG size)
{
    struct GenetUnit *unit = drvctx;
    (void)size;
    dma_free(unit->dmaPool, ptr);
}

static const struct NetDevDrvOps genet_netdev_ops = {
    genet_ndo_txsubmit,
    genet_ndo_txkick,
    genet_ndo_rxrelease,
    genet_ndo_dmaalloc,
    genet_ndo_dmafree,
};

/* ------------------------------------------------------- command handlers --- */

static void netdev_teardown_pool(struct GenetUnit *unit)
{
    if (unit->ndRxSlab != NULL)
    {
        dma_free(unit->dmaPool, unit->ndRxSlab);
        unit->ndRxSlab = NULL;
    }
    if (unit->ndTxTsb != NULL)
    {
        dma_free(unit->dmaPool, unit->ndTxTsb);
        unit->ndTxTsb = NULL;
    }
    if (unit->ndRxFree != NULL)
    {
        pool_free(unit->metaPool, unit->ndRxFree);
        unit->ndRxFree = NULL;
    }
    if (unit->ndRecycle != NULL)
    {
        pool_free(unit->metaPool, unit->ndRecycle);
        unit->ndRecycle = NULL;
    }
    if (unit->ndTxDone != NULL)
    {
        pool_free(unit->metaPool, unit->ndTxDone);
        unit->ndTxDone = NULL;
    }
    if (unit->ndRxBatchDescs != NULL)
    {
        pool_free(unit->metaPool, unit->ndRxBatchDescs);
        unit->ndRxBatchDescs = NULL;
    }
    unit->ndRxFreeCount = 0;
    unit->ndRxPoolTotal = 0;
    unit->ndRxHeld = 0;
    unit->ndRecycleProd = unit->ndRecycleCons = 0;
    unit->ndTxDoneProd = unit->ndTxDoneCons = 0;
}

/*
 * Only the opener that attached may stop, detach or rename the unit: a
 * diagnostic tool holding the same unit open must not be able to tear a live
 * stack's attachment down underneath it. Identity is the request's reply port,
 * which is per-opener and stable (BeginIO clears IOF_QUICK, so every netdev
 * command carries one).
 */
static BOOL netdev_is_owner(const struct GenetUnit *unit, const struct IOStdReq *io)
{
    return unit->ndOwnerPort == NULL || unit->ndOwnerPort == io->io_Message.mn_ReplyPort;
}

static BYTE Do_NETDEV_ATTACH(struct GenetUnit *unit, struct IOStdReq *io)
{
    struct NetDevAttach *att = io->io_Data;

    if (att == NULL || io->io_Length < sizeof(struct NetDevAttach))
        return NDERR_BADPARAMS;
    if (att->nda_StackOps == NULL)
        return NDERR_BADPARAMS;
    if (unit->ndStackOps != NULL)
        return NDERR_BUSY;
    if (att->nda_AbiVersion == 0)
        return NDERR_BADVERSION;

    UWORD version = att->nda_AbiVersion;
    if (version > NETDEV_ABI_VERSION)
        version = NETDEV_ABI_VERSION;

    if (unit->state == STATE_UNCONFIGURED)
    {
        CopyMem((APTR)unit->localMacAddress, unit->currentMacAddress,
                sizeof(unit->currentMacAddress));
        u32 result = UnitConfigure(unit);
        if (result != GENET_OK)
        {
            Kprintf("[genet] %s: configure failed: %lu\n", __func__, result);
            return NDERR_NOMEM;
        }
    }

    /* Pool size: an explicit RX_POOL_BUFS prefs value wins; 0 (auto, the
     * default) sizes from the stack's declared hold budget so the pool
     * follows TCP_WND and stream count instead of a guessed constant. */
    u32 pool_bufs = unit->ndRxPoolBufs;
    if (pool_bufs == 0)
    {
        u32 req = att->nda_RxHoldReq;
        pool_bufs = (req != 0) ? RX_DESCS + req + ND_RX_RECLAIM_SLACK
                               : RX_POOL_BUFS_FALLBACK;
        if (pool_bufs < RX_POOL_BUFS_MIN)
            pool_bufs = RX_POOL_BUFS_MIN;
        if (pool_bufs > RX_POOL_BUFS_MAX)
            pool_bufs = RX_POOL_BUFS_MAX;
    }

    /* Flush granularity: our per-nso_RxInput batch = the stack's declared array
     * capacity (nda_RxBatch), so a whole batch lands in one stack lock hold. The
     * marshalling buffer (ndRxBatchDescs) is allocated to this below. Clamp to
     * RX_DESCS — a batch can't exceed one drain pass, bounded by the ring; 0 or
     * oversize means no/bad request, default to a full ring's worth. */
    unit->ndRxBatch = (u16)((att->nda_RxBatch != 0 && att->nda_RxBatch <= RX_DESCS)
                                ? att->nda_RxBatch : RX_DESCS);

    /* RX buffer pool, TX status blocks, and the SPSC rings. The recycle
     * ring is a power of two covering the whole pool (a buffer is in
     * flight at most once, and the free-running SPSC indices tolerate a
     * completely full ring). */
    u32 recycle_n = 1;
    while (recycle_n < pool_bufs)
        recycle_n <<= 1;

    unit->ndRxSlab = dma_zalloc(unit->dmaPool, DMA_ALIGN_MIN,
                                pool_bufs * RX_BUF_LENGTH);
    unit->ndTxTsb = dma_zalloc(unit->dmaPool, DMA_ALIGN_MIN,
                               TX_DESCS * GENET_STATUS64_LEN);
    unit->ndRxFree = pool_alloc(unit->metaPool, pool_bufs * sizeof(APTR));
    unit->ndRecycle = pool_alloc(unit->metaPool, recycle_n * sizeof(APTR));
    unit->ndTxDone = pool_alloc(unit->metaPool, ND_TXDONE_RING_N * sizeof(struct GenetTxDone));
    unit->ndRxBatchDescs = pool_alloc(unit->metaPool, unit->ndRxBatch * sizeof(struct NetDevRxDesc));
    if (unit->ndRxSlab == NULL || unit->ndTxTsb == NULL || unit->ndRxFree == NULL ||
        unit->ndRecycle == NULL || unit->ndTxDone == NULL || unit->ndRxBatchDescs == NULL)
    {
        Kprintf("[genet] %s: pool alloc failed (%lu bufs, %lu KB slab)\n",
                __func__, (ULONG)pool_bufs, (ULONG)(pool_bufs * RX_BUF_LENGTH / 1024));
        netdev_teardown_pool(unit);
        return NDERR_NOMEM;
    }
    unit->ndRecycleMask = recycle_n - 1;
    unit->ndRxPoolTotal = pool_bufs;

    cache_pre_dma(unit->ndRxSlab, pool_bufs * RX_BUF_LENGTH, 0);

    unit->ndRxFreeCount = 0;
    for (u32 i = 0; i < pool_bufs; i++)
        netdev_rx_push(unit, (u8 *)unit->ndRxSlab + i * RX_BUF_LENGTH);

    unit->ndRxHeld = 0;
    unit->ndFilterFlags = 0;
    unit->ndPromisc = FALSE;
    unit->ndMcastCount = 0;
    unit->ndLink.ndls_Flags = 0;
    unit->ndLink.ndls_SpeedMbps = 0;

    /* NetDevStats is monotonic since ATTACH, and every counter lives in this
     * one struct so the guarantee costs one clear and covers counters added
     * later. The FIFO-overflow accumulator then takes its delta baseline from
     * the live register, which a previous session may have left non-zero. */
    memset(&unit->internalStats, 0, sizeof(unit->internalStats));
    bcmgenet_mib_rbuf_ovfl_rebase(unit);

    unit->ndStackCtx = att->nda_StackCtx;
    unit->ndStackOps = att->nda_StackOps;
    unit->ndOwnerPort = io->io_Message.mn_ReplyPort;

    /* OUT fields */
    att->nda_AbiVersion = version;
    att->nda_DrvCtx = unit;
    att->nda_DrvOps = &genet_netdev_ops;

    struct NetDevCaps *caps = &att->nda_Caps;
    caps->ndc_AbiVersion = NETDEV_ABI_VERSION;
    /* Jumbo not implemented this release: report 1500 regardless of the
     * stack's nda_MtuReq. The negotiation field (IN nda_MtuReq -> OUT ndc_Mtu)
     * exists so a future jumbo-capable build raises this with no ABI change. */
    caps->ndc_Mtu = ETH_DATA_LEN;
    CopyMem(unit->currentMacAddress, caps->ndc_Mac, sizeof(caps->ndc_Mac));
    caps->ndc_TxMaxSegs = ND_TX_MAX_SEGS;
    /* no NDCF_RX_CSUM_VALID: with RBUF_L3_PARSE_DIS the RXCHK block never
     * issues per-frame verdicts, only the raw sum */
    caps->ndc_Features = NDCF_COALESCE | NDCF_LINK_EVENTS | NDCF_MCAST_FILTER |
                         NDCF_TX_L4CSUM | NDCF_RX_CSUM_RAW;
    caps->ndc_TxRingSlots = TX_DESCS;
    caps->ndc_RxRingSlots = RX_DESCS;
    caps->ndc_TxAlign = 0;
    caps->ndc_TxInFlightMax = ND_TXDONE_RING_N; /* our completion-FIFO depth */
    caps->ndc_RxPoolBufs = pool_bufs;
    for (u32 i = 0; i < 3; i++)
        caps->ndc_Reserved[i] = 0;

    io->io_Actual = sizeof(struct NetDevAttach);
    Kprintf("[genet] %s: netdev stack attached, ABI v%lu, MTU %lu (req %lu), RX pool %lu bufs\n",
            __func__, (ULONG)version, (ULONG)caps->ndc_Mtu, (ULONG)att->nda_MtuReq,
            (ULONG)pool_bufs);
    return 0;
}

static BYTE Do_NETDEV_DETACH(struct GenetUnit *unit, struct IOStdReq *io)
{
    if (unit->ndStackOps == NULL)
        return NDERR_NOTATTACHED;
    if (!netdev_is_owner(unit, io))
        return NDERR_BUSY;
    if (unit->ndStarted)
        return NDERR_BADPARAMS; /* STOP first */

    netdev_drain_recycle(unit);
    if (unit->ndRxHeld != 0)
    {
        Kprintf("[genet] %s: stack still holds %lu RX buffers\n", __func__, unit->ndRxHeld);
        return NDERR_BUSY;
    }

    netdev_teardown_pool(unit);
    unit->ndStackOps = NULL;
    unit->ndStackCtx = NULL;
    unit->ndOwnerPort = NULL;
    Kprintf("[genet] %s: netdev stack detached\n", __func__);
    return 0;
}

static BYTE Do_NETDEV_START(struct GenetUnit *unit)
{
    if (unit->ndStackOps == NULL)
        return NDERR_NOTATTACHED;
    if (unit->ndStarted)
        return 0;

    u32 result = UnitOnline(unit);
    if (result != GENET_OK)
        return NDERR_NOMEM;

    unit->ndStarted = TRUE;
    /* Forced: the ABI promises one link callback shortly after START carrying
     * the current state, and a start with no cable changes nothing that
     * netdev_link_update() would otherwise report. */
    netdev_link_update(unit, unit->phydev != NULL && unit->phydev->link, TRUE);
    return 0;
}

static BYTE Do_NETDEV_STOP(struct GenetUnit *unit, struct IOStdReq *io)
{
    if (unit->ndStackOps == NULL)
        return NDERR_NOTATTACHED;
    if (!netdev_is_owner(unit, io))
        return NDERR_BUSY;
    if (!unit->ndStarted)
        return 0;

    /* Cleared before the quiesce: a submitter that passes the gate after this
     * point does nothing, and bcmgenet_netdev_tx_quiesce() waits out any that
     * passed it before. */
    unit->ndStarted = FALSE;
    UnitOffline(unit); /* quiesces DMA, returns ring buffers, completes TX cookies */
    netdev_link_update(unit, FALSE, FALSE);
    return 0;
}

static BYTE Do_NETDEV_SET_RXFILTER(struct GenetUnit *unit, struct IOStdReq *io)
{
    const struct NetDevRxFilter *filter = io->io_Data;

    if (filter == NULL || io->io_Length < sizeof(struct NetDevRxFilter))
        return NDERR_BADPARAMS;
    if (unit->ndStackOps == NULL)
        return NDERR_NOTATTACHED;

    if (filter->ndrx_NumMcast != 0 && filter->ndrx_McastList == NULL)
        return NDERR_BADPARAMS;

    unit->ndFilterFlags = filter->ndrx_Flags;

    /* Exact MDF filtering as long as the list fits the slots left over from
     * broadcast + own MAC. Explicit promiscuity, all-multi and an overrunning
     * list all end at CMD_PROMISC: the UniMAC has no multicast-only accept
     * bit, so promiscuous reception is our all-multi — a correct superset. */
    BOOL exact = (filter->ndrx_Flags & (NDFF_PROMISC | NDFF_ALLMULTI)) == 0 &&
                 filter->ndrx_NumMcast <= GENET_MDF_MCAST_MAX;

    unit->ndMcastCount = exact ? filter->ndrx_NumMcast : 0;
    for (UWORD i = 0; i < unit->ndMcastCount; i++)
        CopyMem((APTR)filter->ndrx_McastList[i], unit->ndMcastList[i], 6);
    unit->ndPromisc = !exact;

    if (!exact)
        Kprintf("[genet] %s: promiscuous fallback (flags 0x%04lx, %lu multicast)\n",
                __func__, (ULONG)filter->ndrx_Flags, (ULONG)filter->ndrx_NumMcast);

    /* Offline the list is only stored: bcmgenet_gmac_eth_start() reprograms the
     * MDF at the next START, so a join between STOP and START is not lost. */
    if (unit->state == STATE_ONLINE)
        bcmgenet_set_rx_mode(unit);
    return 0;
}

static BYTE Do_NETDEV_SET_COALESCE(struct GenetUnit *unit, struct IOStdReq *io)
{
    const struct NetDevCoalesce *coal = io->io_Data;
    const struct GenetRuntimeConfig *config = &unit->device->runtimeConfig;

    if (coal == NULL || io->io_Length < sizeof(struct NetDevCoalesce))
        return NDERR_BADPARAMS;
    if (unit->ndStackOps == NULL)
        return NDERR_NOTATTACHED;

    /* 0 = keep the driver default */
    u32 tx_frames = coal->ndcl_TxMaxFrames ? coal->ndcl_TxMaxFrames : config->tx_coalesce_frames;
    u32 rx_frames = coal->ndcl_RxMaxFrames ? coal->ndcl_RxMaxFrames : config->rx_coalesce_frames;
    u32 rx_usecs = coal->ndcl_RxUsecs ? coal->ndcl_RxUsecs : config->rx_coalesce_usecs;

    if (bcmgenet_coalesce_valid(tx_frames, rx_frames, rx_usecs) != GENET_OK)
        return NDERR_BADPARAMS;

    unit->coalTxFrames = tx_frames;
    unit->coalRxFrames = rx_frames;
    unit->coalRxUsecs = rx_usecs;

    /* Stopped, the values are only stored: ring init programs them at the next
     * START, so a setting made between STOP and START is not lost. */
    if (unit->state == STATE_ONLINE)
        bcmgenet_apply_coalesce(unit);
    return 0;
}

static BYTE Do_NETDEV_GET_STATS(struct GenetUnit *unit, struct IOStdReq *io)
{
    struct NetDevStats *stats = io->io_Data;
    const struct internal_stats *is = &unit->internalStats;

    if (stats == NULL || io->io_Length < sizeof(struct NetDevStats))
        return NDERR_BADPARAMS;

    netdev_u64_set(&stats->nds_RxPackets, is->rx_packets);
    netdev_u64_set(&stats->nds_RxBytes, is->rx_bytes);
    netdev_u64_set(&stats->nds_RxErrors,
             (u64)is->rx_crc_errors + is->rx_over_errors + is->rx_frame_errors +
                 is->rx_length_errors + is->rx_fragmented_errors + is->rx_other_errors);
    /* Software drops only. Hardware ring overruns are nds_RxOverruns below:
     * the two name different bottlenecks and adding one into the other reports
     * the same loss twice. */
    netdev_u64_set(&stats->nds_RxDropped, unit->internalStats.rx_dropped);

    /* Every counter here is unit-task state, read on the unit task, so no
     * 64-bit read can catch a half-applied carry: bcmgenet_tx_harvest() does
     * the TX tally, not the submitting task. */
    netdev_u64_set(&stats->nds_TxPackets, is->tx_packets);
    netdev_u64_set(&stats->nds_TxBytes, is->tx_bytes);
    netdev_u64_set(&stats->nds_TxErrors, bcmgenet_mib_tx_errors(unit));
    /* Both are frames the stack handed over that never reached the wire —
     * quiesced at STOP, or refused by the TX sanity gate. The ABI wants the
     * total; NETDEV_CMD_GET_COUNTERS keeps the split (drv_tx_dropped,
     * drv_tx_bad) for anyone who needs to know which. */
    netdev_u64_set(&stats->nds_TxDropped, unit->internalStats.tx_dropped + unit->internalStats.tx_bad);

    /* Hardware loss, at two different points: the DMA ring discard counter and
     * the RBUF FIFO. The latter is accumulated in software — the register
     * saturates and gets rearmed, so it cannot be reported raw and stay
     * monotonic. */
    stats->nds_RxOverruns = is->rx_overruns;
    stats->nds_RxFifoOvfl = bcmgenet_mib_rbuf_ovfl(unit);
    for (u32 i = 0; i < 8; i++)
        stats->nds_Reserved[i] = 0;

    io->io_Actual = sizeof(struct NetDevStats);
    return 0;
}

/*
 * The self-describing counter list: everything NetDevStats has no field for.
 * Two-call sizing — ndcs_Max 0 probes for ndcs_Count without a buffer.
 */
static BYTE Do_NETDEV_GET_COUNTERS(struct GenetUnit *unit, struct IOStdReq *io)
{
    struct NetDevCounterSet *set = io->io_Data;

    if (set == NULL || io->io_Length < NETDEV_COUNTERSET_SIZE(0))
        return NDERR_BADPARAMS;

    UWORD max = set->ndcs_Max;
    if (io->io_Length < NETDEV_COUNTERSET_SIZE(max))
        return NDERR_BADPARAMS;

    set->ndcs_Count = bcmgenet_mib_fill(unit, set->ndcs_Counters, max);

    /* Short buffers are not an error: the caller learns the true count and
     * comes back sized. io_Actual covers only what was written. */
    io->io_Actual = NETDEV_COUNTERSET_SIZE(set->ndcs_Count < max ? set->ndcs_Count : max);
    return 0;
}

static BYTE Do_NETDEV_GET_LINK(struct GenetUnit *unit, struct IOStdReq *io)
{
    struct NetDevLinkState *link = io->io_Data;

    if (link == NULL || io->io_Length < sizeof(struct NetDevLinkState))
        return NDERR_BADPARAMS;

    *link = unit->ndLink;
    io->io_Actual = sizeof(struct NetDevLinkState);
    return 0;
}

static BYTE Do_NETDEV_SET_MAC(struct GenetUnit *unit, struct IOStdReq *io)
{
    if (io->io_Data == NULL || io->io_Length < sizeof(unit->currentMacAddress))
        return NDERR_BADPARAMS;
    if (!netdev_is_owner(unit, io))
        return NDERR_BUSY;
    if (unit->state == STATE_ONLINE)
        return NDERR_BADPARAMS; /* set before START; applied by UMAC bring-up */

    CopyMem(io->io_Data, unit->currentMacAddress, sizeof(unit->currentMacAddress));
    /* The stack read ndc_Mac at ATTACH and there is no way to re-announce it,
     * so a change after that would leave it building frames from the old
     * address. Setting it before ATTACH is the supported order. */
    if (unit->ndStackOps != NULL)
        Kprintf("[genet] %s: MAC changed while attached; the stack keeps the address it read at ATTACH\n",
                __func__);
    return 0;
}

/* ----------------------------------------------------------- NSD + dispatch --- */

static const UWORD GENET_SupportedCommands[] = {
    NETDEV_CMD_ATTACH,
    NETDEV_CMD_DETACH,
    NETDEV_CMD_START,
    NETDEV_CMD_STOP,
    NETDEV_CMD_SET_RXFILTER,
    NETDEV_CMD_SET_COALESCE,
    NETDEV_CMD_GET_STATS,
    NETDEV_CMD_GET_COUNTERS,
    NETDEV_CMD_GET_LINK,
    NETDEV_CMD_SET_MAC,
    NSCMD_DEVICEQUERY,
    0};

static BYTE Do_NSCMD_DEVICEQUERY(struct IOStdReq *io)
{
    struct NSDeviceQueryResult *dq = io->io_Data;

    if (dq == NULL)
        return NDERR_BADPARAMS;

    dq->nsdqr_SizeAvailable = sizeof(struct NSDeviceQueryResult);
    if (io->io_Length < dq->nsdqr_SizeAvailable)
        return IOERR_BADLENGTH;

    dq->nsdqr_DevQueryFormat = 0; /* the only format NSD defines */
    /* No NSD device type describes a netdev NIC, and NSDEVTYPE_SANA2 would be a
     * lie — no SANA-II command works here. The command list is the answer. */
    dq->nsdqr_DeviceType = NSDEVTYPE_UNKNOWN;
    dq->nsdqr_DeviceSubType = 0;
    dq->nsdqr_SupportedCommands = (UWORD *)GENET_SupportedCommands;
    io->io_Actual = dq->nsdqr_SizeAvailable;
    return 0;
}

void ProcessCommand(struct IOStdReq *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->io_Unit;
    BYTE error;

    switch (io->io_Command)
    {
    case NSCMD_DEVICEQUERY:
        error = Do_NSCMD_DEVICEQUERY(io);
        break;
    case NETDEV_CMD_ATTACH:
        error = Do_NETDEV_ATTACH(unit, io);
        break;
    case NETDEV_CMD_DETACH:
        error = Do_NETDEV_DETACH(unit, io);
        break;
    case NETDEV_CMD_START:
        error = Do_NETDEV_START(unit);
        break;
    case NETDEV_CMD_STOP:
        error = Do_NETDEV_STOP(unit, io);
        break;
    case NETDEV_CMD_SET_RXFILTER:
        error = Do_NETDEV_SET_RXFILTER(unit, io);
        break;
    case NETDEV_CMD_SET_COALESCE:
        error = Do_NETDEV_SET_COALESCE(unit, io);
        break;
    case NETDEV_CMD_GET_STATS:
        error = Do_NETDEV_GET_STATS(unit, io);
        break;
    case NETDEV_CMD_GET_COUNTERS:
        error = Do_NETDEV_GET_COUNTERS(unit, io);
        break;
    case NETDEV_CMD_GET_LINK:
        error = Do_NETDEV_GET_LINK(unit, io);
        break;
    case NETDEV_CMD_SET_MAC:
        error = Do_NETDEV_SET_MAC(unit, io);
        break;
    default:
        error = IOERR_NOCMD;
        break;
    }

    io->io_Error = error;
    if (!(io->io_Flags & IOF_QUICK))
        ReplyMsg(&io->io_Message);
}
