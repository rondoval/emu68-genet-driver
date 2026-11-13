// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom GENET (Gigabit Ethernet) controller driver — SANA-II transmit.
 *
 * Copyright (c) 2014-2025 Broadcom
 *
 * One Forbid() per write, everything ring-related inside it.
 *
 * Any task may call BeginIO(CMD_WRITE) at any time - several openers, or one
 * opener with several sender tasks - so the ring has no single producer, and
 * the claim, the fill and the publish of a slot have to be one critical
 * section: a slot is claimed by advancing tx_prod_index, which only happens
 * once it is filled, so two writers can never publish out of order. Forbid()
 * does not block interrupts, and the section is a few microseconds: the copy
 * of one frame and a handful of register stores.
 *
 * Staging is ring-bound: slot k belongs to BD k for the life of the claim, so
 * nothing is allocated on the way in and nothing freed on the way out. A slot
 * is reusable exactly when its BD is, and the free-BD check against the
 * cached consumer index already guarantees the hardware has read it. A write
 * is replied at submit when it fits. A full ring queues it on the backlog
 * instead of failing it; the writer that queues the first one wakes the unit
 * task, which replays the backlog and arms the TX-done interrupt while writes
 * still wait - interrupt-mask writes stay on the unit task.
 *
 * Each slot is [pad][TSB][frame] (sana2_unit.h): the frame goes out under one
 * descriptor that starts at the zeroed TSB. The descriptor stores are relaxed
 * (iomem.h): the one barrier before the doorbell orders them together with
 * the NoSync cache clean.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function: see GenetUnit.sysBase */
#include <proto/exec.h>
#endif

#include <devices/sana2.h>
#include <exec/errors.h>

#include <cache_ops.h>
#include <debug.h>
#include <dma_mem.h>
#include <errors.h>
#include <iomem.h>
#include <memory.h>
#include <minlist.h>
#include <types.h>
#include <device.h>
#include <sana2_unit.h>

#include <genet/bcmgenet.h>
#include <genet/bcmgenet-regs.h>
#include <genet/bcmgenet-tx.h>

/* A write needs one BD, and one is always left free so that a full ring is
 * told apart from an empty one. */
#define S2_TX_BDS_NEEDED 1u

#define S2_TX_STAGING_BYTES ((u32)TX_DESCS * RX_BUF_LENGTH)

/**
 * sana2_tx_staging_create() - Allocate the claim's TX staging: one zeroed
 * slot of RX_BUF_LENGTH per BD, from the DMA pool.
 * @unit: the unit.
 *
 * Unit task, at the first claim.
 *
 * Return: GENET_OK, or ENOMEM.
 */
u32 sana2_tx_staging_create(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->sysBase;
    (void)SysBase; /* the LVO cache-op flavour calls exec */
    unit->s2->txStaging = dma_zalloc(unit->dmaPool, DMA_ALIGN_MIN, S2_TX_STAGING_BYTES);
    if (unit->s2->txStaging == NULL)
        return ENOMEM;
    /* the zeroed TSBs reach RAM now: nothing writes them again */
    cache_pre_dma(unit->s2->txStaging, S2_TX_STAGING_BYTES, 0);
    return GENET_OK;
}

/**
 * sana2_tx_staging_destroy() - Free the TX staging, if there is any.
 * @unit: the unit.
 *
 * Unit task, at the release or a failed claim.
 */
void sana2_tx_staging_destroy(struct GenetUnit *unit)
{
    if (unit->s2->txStaging != NULL)
    {
        dma_free(unit->dmaPool, unit->s2->txStaging);
        unit->s2->txStaging = NULL;
    }
}

/**
 * tx_staging() - Give the staging slot of the BD the producer fills next.
 * @unit: the unit.
 * @ring: the TX ring.
 *
 * Return: the slot, laid out [pad][TSB][frame].
 */
static inline u8 *tx_staging(const struct GenetUnit *unit, const struct bcmgenet_tx_ring *ring)
{
    return unit->s2->txStaging + (u32)tx_slot(ring) * RX_BUF_LENGTH;
}

/**
 * tx_backlog_empty() - Tell whether no write waits for ring room.
 * @s2: the unit's SANA-II side.
 *
 * Return: TRUE when txBacklog is empty.
 */
static inline BOOL tx_backlog_empty(const struct GenetS2 *s2)
{
    return s2->txBacklog.mlh_Head->mln_Succ == NULL;
}

/**
 * build_eth_header() - Build the 14-byte Ethernet II header into @buf.
 * @buf: where the header goes.
 * @dst_mac: the destination address.
 * @src_mac: the source address.
 * @ethertype: the EtherType.
 */
static inline void build_eth_header(u8 *buf, const u8 *dst_mac,
                                    const u8 *src_mac, u16 ethertype)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
    *(ULONG *)buf = *(const ULONG *)dst_mac;
    *(UWORD *)(buf + 4) = *(const UWORD *)&dst_mac[4];
    *(ULONG *)(buf + 6) = *(const ULONG *)src_mac;
    *(UWORD *)(buf + 10) = *(const UWORD *)&src_mac[4];
    *(UWORD *)(buf + 12) = ethertype;
#pragma GCC diagnostic pop
}

enum TxPut
{
    TX_PUT_OK,
    TX_PUT_FULL,   /* not enough free BDs: nothing touched */
    TX_PUT_FAILED, /* the opener's copy failed: io_Error set, nothing published */
};

/**
 * tx_ring_put() - Put one well-formed write on the ring and ring the doorbell.
 * @unit: the unit, online.
 * @io: the write.
 *
 * Forbid held. Shared by the writer (sana2_write()) and the unit task
 * replaying the backlog (sana2_tx_drain()). A SANA2IOF_RAW write's buffer is
 * the whole frame; any other gets an Ethernet II header from ios2_DstAddr,
 * the address in use and ios2_PacketType. A failed CopyFromBuff counts
 * tx_dropped, sets S2ERR_NO_RESOURCES / S2WERR_BUFF_ERROR, and leaves its
 * events in pendingEvents for the unit task, which it signals.
 *
 * Return: TX_PUT_OK when on the ring; TX_PUT_FULL when not enough BDs are
 * free; TX_PUT_FAILED when the opener's copy failed.
 */
static enum TxPut tx_ring_put(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct ExecBase *SysBase = unit->sysBase;
    (void)SysBase; /* the LVO cache-op flavour calls exec */
    struct Opener *opener = io->ios2_BufferManagement;
    struct bcmgenet_tx_ring *ring = &unit->tx_ring;
    const BOOL raw = (io->ios2_Req.io_Flags & SANA2IOF_RAW) != 0;
    const u32 data_len = io->ios2_DataLength;

    u32 copy_len = data_len;
    if (unit->device->runtimeConfig.use_miami_workaround)
        copy_len = (copy_len + 3U) & ~3U;

    BOOL refreshed = FALSE; /* the unit task harvests nothing: no wake is owed */
    if (unlikely(tx_free_bds_refresh(unit, ring, &refreshed) <= S2_TX_BDS_NEEDED))
        return TX_PUT_FULL;

    u8 *slot = tx_staging(unit, ring);
    u8 *frame = slot + S2_TX_FRAME_OFFSET;
    u32 wire_len;
    if (likely(!raw))
    {
        build_eth_header(frame, io->ios2_DstAddr, unit->currentMacAddress, (u16)io->ios2_PacketType);
        if (unlikely(opener->CopyFromBuff == NULL ||
                     !opener->CopyFromBuff(frame + ETH_HLEN, io->ios2_Data, copy_len)))
            goto copy_failed;
        wire_len = ETH_HLEN + data_len;
    }
    else
    {
        /* RAW: the caller's frame is complete */
        if (unlikely(opener->CopyFromBuff == NULL ||
                     !opener->CopyFromBuff(frame, io->ios2_Data, copy_len)))
            goto copy_failed;
        wire_len = data_len;
    }

    cache_pre_dma(frame, wire_len, DMA_ReadFromRAM | DMAF_NoSync);
    u32 len_stat = (((u32)GENET_STATUS64_LEN + wire_len) << DMA_BUFLENGTH_SHIFT) |
                   (GENET_QTAG_MASK << DMA_TX_QTAG_SHIFT) |
                   DMA_TX_APPEND_CRC | DMA_SOP | DMA_EOP;
    dmadesc_set(tx_desc(unit, ring), (dma_addr_t)(slot + S2_TX_PAD), len_stat);
    tx_advance_prod(ring);
    unit->internalStats.tx_packets++;
    unit->internalStats.tx_bytes += wire_len;

    emu68_barrier(); /* the one dsb: closes the NoSync clean and orders the relaxed BD stores ahead of the doorbell */
    mmio_write32(ring->tx_prod_index, BCMGENET_REG(unit, TDMA_PROD_INDEX));
    return TX_PUT_OK;

copy_failed:
    /* Nothing published: the next write simply reuses the slot. The event
     * queues are the unit task's, so it reports the event. */
    KprintfT("[genet] %s: the opener's CopyFromBuff failed\n", __func__);
    unit->internalStats.tx_dropped++;
    io->ios2_WireError = S2WERR_BUFF_ERROR;
    io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
    unit->s2->pendingEvents |= S2EVENT_BUFF | S2EVENT_TX | S2EVENT_SOFTWARE | S2EVENT_ERROR;
    Signal(unit->task, 1UL << unit->tx_signal);
    return TX_PUT_FAILED;
}

/**
 * tx_backlog_add() - Park a write that cannot go on the ring yet.
 * @unit: the unit.
 * @io: the write.
 *
 * Forbid held. It becomes an ordinary queued request - not quick, a list node
 * of type NT_MESSAGE - so AbortIO can take it back; txQueued counts it. The
 * first one wakes the unit task, which replays the backlog and arms the
 * TX-done interrupt.
 */
static void tx_backlog_add(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct GenetS2 *s2 = unit->s2;
    BOOL first = tx_backlog_empty(s2);
    io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    AddTailMinList(&s2->txBacklog, (struct MinNode *)io);
    s2->txQueued++;
    if (first)
        Signal(unit->task, 1UL << unit->tx_signal);
}

/**
 * sana2_write() - CMD_WRITE / S2_BROADCAST / S2_MULTICAST from BeginIO: send
 * the frame, or park it until the ring has room.
 * @unit: the unit.
 * @io: the write.
 *
 * Runs in the writer's task, ring work under one Forbid(). S2_BROADCAST sends
 * to ff:ff:ff:ff:ff:ff.
 *
 * Return: COMMAND_PROCESSED when sent or failed: io_Error S2ERR_BAD_ARGUMENT
 * / S2WERR_NULL_POINTER without an opener, S2ERR_NO_RESOURCES /
 * S2WERR_BUFF_ERROR for no data or a failed copy, S2ERR_MTU_EXCEEDED above
 * S2_TX_MAX_DATALEN, S2ERR_OUTOFSERVICE / S2WERR_UNIT_OFFLINE while offline;
 * COMMAND_SCHEDULED when parked on the backlog, replied by the unit task.
 */
u32 sana2_write(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct GenetS2 *s2 = unit->s2;
    const u32 data_len = io->ios2_DataLength;

    PERF_T0(t_submit);
    io->ios2_Req.io_Error = S2ERR_NO_ERROR;
    if (io->ios2_Req.io_Command == S2_BROADCAST)
        memset(io->ios2_DstAddr, 0xff, sizeof(unit->currentMacAddress));

    /* Reject what can never be sent before it could wait in the backlog. */
    if (unlikely(io->ios2_BufferManagement == NULL))
    {
        io->ios2_WireError = S2WERR_NULL_POINTER;
        io->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
        return COMMAND_PROCESSED;
    }
    if (unlikely(data_len == 0))
    {
        unit->internalStats.tx_dropped++;
        io->ios2_WireError = S2WERR_BUFF_ERROR;
        io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        return COMMAND_PROCESSED;
    }
    if (unlikely(data_len > S2_TX_MAX_DATALEN))
    {
        unit->internalStats.tx_dropped++;
        io->ios2_WireError = S2WERR_GENERIC_ERROR;
        io->ios2_Req.io_Error = S2ERR_MTU_EXCEEDED;
        return COMMAND_PROCESSED;
    }

    u32 result = COMMAND_PROCESSED;
    Forbid();
    if (unlikely(!s2->online))
    {
        io->ios2_WireError = S2WERR_UNIT_OFFLINE;
        io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
    }
    /* A full ring means wait, not fail; once anything waits, later writes
     * wait behind it so the wire order stays the submit order. */
    else if (unlikely(!tx_backlog_empty(s2)) || unlikely(tx_ring_put(unit, io) == TX_PUT_FULL))
    {
        tx_backlog_add(unit, io);
        result = COMMAND_SCHEDULED;
    }
    Permit();
    PERF_ADD(&unit->gu_Perf, GP_TX_SUBMIT, t_submit);
    return result;
}

/**
 * sana2_tx_drain() - Move backlog writes onto the ring while it has room,
 * replying each.
 * @unit: the unit.
 *
 * Unit task. While writes still wait, ask for one TX-done interrupt; a
 * completion that slipped in before the arm has lost its interrupt, so the
 * hardware is looked at once more and the task comes round again if there is
 * room.
 */
void sana2_tx_drain(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct GenetS2 *s2 = unit->s2;

    Forbid();
    struct IOSana2Req *io;
    while ((io = (struct IOSana2Req *)RemHeadMinList(&s2->txBacklog)) != NULL)
    {
        if (tx_ring_put(unit, io) == TX_PUT_FULL)
        {
            AddHeadMinList(&s2->txBacklog, (struct MinNode *)io);
            break;
        }
        ReplyMsg((struct Message *)io);
    }
    BOOL waiting = !tx_backlog_empty(s2);
    Permit();
    if (!waiting)
        return;

    bcmgenet_tx_irq_arm(unit);

    /* No writer is inside its Forbid while this task runs, so the producer
     * index reads consistent here. */
    const struct bcmgenet_tx_ring *ring = &unit->tx_ring;
    u16 hw_cons = (u16)(mmio_read32(BCMGENET_REG(unit, TDMA_CONS_INDEX)) & DMA_C_INDEX_MASK);
    u16 free_bds = (u16)(TX_DESCS - ((u32)(ring->tx_prod_index - hw_cons) & DMA_P_INDEX_MASK));
    if (free_bds > S2_TX_BDS_NEEDED)
        Signal(unit->task, 1UL << unit->tx_signal);
}

/**
 * sana2_tx_backlog_abort() - Reply every waiting write with @error.
 * @unit: the unit.
 * @error: the io_Error.
 * @wireError: the ios2_WireError.
 *
 * Unit task: offline, release.
 */
void sana2_tx_backlog_abort(struct GenetUnit *unit, BYTE error, ULONG wireError)
{
    struct ExecBase *SysBase = unit->sysBase;
    Forbid();
    struct IOSana2Req *io;
    while ((io = (struct IOSana2Req *)RemHeadMinList(&unit->s2->txBacklog)) != NULL)
    {
        io->ios2_Req.io_Error = error;
        io->ios2_WireError = wireError;
        ReplyMsg((struct Message *)io);
    }
    Permit();
}

/**
 * sana2_tx_backlog_abort_opener() - Reply @opener's waiting writes with
 * @error; the others keep their place.
 * @unit: the unit.
 * @opener: the Opener.
 * @error: the io_Error.
 * @wireError: the ios2_WireError.
 *
 * Unit task: its close, CMD_FLUSH. The writes are taken off under Forbid()
 * and replied after it.
 */
void sana2_tx_backlog_abort_opener(struct GenetUnit *unit, struct Opener *opener, BYTE error,
                                   ULONG wireError)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct MinList taken;
    _NewMinList(&taken);
    Forbid();
    struct MinNode *next;
    for (struct MinNode *node = unit->s2->txBacklog.mlh_Head; (next = node->mln_Succ) != NULL; node = next)
    {
        if (((struct IOSana2Req *)node)->ios2_BufferManagement != opener)
            continue;
        RemoveMinNode(node);
        AddTailMinList(&taken, node);
    }
    Permit();
    struct IOSana2Req *io;
    while ((io = (struct IOSana2Req *)RemHeadMinList(&taken)) != NULL)
    {
        io->ios2_Req.io_Error = error;
        io->ios2_WireError = wireError;
        ReplyMsg((struct Message *)io);
    }
}

/**
 * sana2_tx_backlog_remove() - AbortIO: take a waiting write back.
 * @unit: the unit.
 * @io: the write.
 *
 * Any task. A write taken back is replied with IOERR_ABORTED /
 * S2WERR_GENERIC_ERROR.
 *
 * Return: TRUE when taken back; FALSE when it is not waiting - on the ring
 * already, or never queued - and so completes on its own.
 */
BOOL sana2_tx_backlog_remove(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct ExecBase *SysBase = unit->sysBase;
    BOOL found = FALSE;
    Forbid();
    for (struct MinNode *node = unit->s2->txBacklog.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        if (node == (struct MinNode *)io)
        {
            RemoveMinNode(node);
            found = TRUE;
            break;
        }
    }
    Permit();
    if (found)
    {
        io->ios2_Req.io_Error = IOERR_ABORTED;
        io->ios2_WireError = S2WERR_GENERIC_ERROR;
        ReplyMsg((struct Message *)io);
    }
    return found;
}
