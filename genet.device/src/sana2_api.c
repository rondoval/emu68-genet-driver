// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * sana2_api.c — the SANA-II personality: openers, bring-up, events and command
 * processing.
 *
 * The opener lifecycle runs in the opener's task (OpenDevice/CloseDevice);
 * everything else here runs on the unit task, which owns the opener list.
 */

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/timer_protos.h>
#include <clib/utility_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function: see GenetUnit.sysBase */
#include <proto/exec.h>
#define TIMER_BASE_NAME TimerBase /* a local: GenetUnit.timerBase */
#include <proto/timer.h>
#include <proto/utility.h>
#endif

#include <exec/errors.h>
#include <exec/memory.h>
#include <devices/sana2.h>
#include <stddef.h>

#include <debug.h>
#include <memory.h>
#include <minlist.h>
#include <types.h>
#include <device.h>
#include <sana2_unit.h>

#include <genet/bcmgenet.h>
#include <genet/bcmgenet-mib.h>
#include <genet/bcmgenet-regs.h>
#include <genet/phy.h>

/* A unit's SANA-II hardware address is 48 bits. */
#define S2_ETHER_ADDR_BITS 48
#define S2_ETHER_ADDR_BYTES (S2_ETHER_ADDR_BITS / 8)
/* S2_DEVICEQUERY's BPS: the controller's rate, not the link's. */
#define S2_GENET_BPS 1000000000UL

/* Every event S2_ONEVENT may wait for. */
#define S2_EVENT_MASK (S2EVENT_ERROR | S2EVENT_TX | S2EVENT_RX | S2EVENT_ONLINE | S2EVENT_OFFLINE | \
                       S2EVENT_BUFF | S2EVENT_HARDWARE | S2EVENT_SOFTWARE | S2EVENT_CONFIGCHANGED | \
                       S2EVENT_CONNECT | S2EVENT_DISCONNECT)

/* ---------------------------------------------------------------- openers --- */

/**
 * opener_copy_function() - Pick the widest buffer function the opener offers:
 * the 32-bit variant over the 16-bit one over the plain one, as SANA-II R4
 * orders them.
 * @UtilityBase: utility.library, for GetTagData().
 * @tags: the OpenDevice() buffer management tags.
 * @widest: the 32-bit variant's tag (S2_CopyToBuff32, S2_CopyFromBuff32).
 * @wide: the 16-bit variant's tag.
 * @plain: the plain variant's tag.
 *
 * Return: the function, or NULL when the tags offer none.
 */
static APTR opener_copy_function(struct Library *UtilityBase, struct TagItem *tags,
                                 Tag widest, Tag wide, Tag plain)
{
    APTR function = (APTR)GetTagData(plain, 0, tags);
    function = (APTR)GetTagData(wide, (ULONG)function, tags);
    return (APTR)GetTagData(widest, (ULONG)function, tags);
}

/**
 * sana2_opener_create() - Allocate the Opener of an OpenDevice() made with an
 * IOSana2Req-sized request.
 * @SysBase: exec.library.
 * @UtilityBase: utility.library, for the tags.
 * @tags: the opener's buffer management tags.
 * @flags: the OpenDevice() flags; SANA2OPF_PROM makes the opener promiscuous.
 *
 * Runs in the opener's task. Takes S2_PacketFilter and the widest
 * S2_CopyToBuff* and S2_CopyFromBuff* from @tags. The Opener is on no list
 * until sana2_opener_attach().
 *
 * Return: the Opener, or NULL when out of memory.
 */
struct Opener *sana2_opener_create(struct ExecBase *SysBase, struct Library *UtilityBase,
                                   struct TagItem *tags, ULONG flags)
{
    /* AllocMem, not the unit's pool: the open runs in the opener's task while
     * the unit task allocates from that pool, and a pool is not shared. */
    struct Opener *opener = AllocMem(sizeof(struct Opener), MEMF_PUBLIC | MEMF_CLEAR);
    if (opener == NULL)
    {
        Kprintf("[genet] %s: no memory for an opener\n", __func__);
        return NULL;
    }

    opener->readRing.entries = AllocMem(sizeof(struct IOSana2Req *) * OPENER_READ_RING_N, MEMF_PUBLIC);
    if (opener->readRing.entries == NULL)
    {
        Kprintf("[genet] %s: no memory for an opener's read ring\n", __func__);
        FreeMem(opener, sizeof(struct Opener));
        return NULL;
    }
    _NewMinList(&opener->ipv4Queue);
    _NewMinList(&opener->arpQueue);
    _NewMinList(&opener->readQueue);
    _NewMinList(&opener->orphanQueue);
    _NewMinList(&opener->eventQueue);
    opener->promiscuous = (flags & SANA2OPF_PROM) != 0;

    opener->packetFilter = (struct Hook *)GetTagData(S2_PacketFilter, 0, tags);
    opener->CopyToBuff = (S2CopyBuffFunc)opener_copy_function(UtilityBase, tags,
                                                              S2_CopyToBuff32, S2_CopyToBuff16, S2_CopyToBuff);
    opener->CopyFromBuff = (S2CopyBuffFunc)opener_copy_function(UtilityBase, tags,
                                                                S2_CopyFromBuff32, S2_CopyFromBuff16, S2_CopyFromBuff);
    KprintfT("[genet] %s: CopyToBuff=%lx CopyFromBuff=%lx PacketFilter=%lx\n", __func__,
             opener->CopyToBuff, opener->CopyFromBuff, opener->packetFilter);
    return opener;
}

/**
 * sana2_opener_free() - Free an Opener and its read ring.
 * @SysBase: exec.library.
 * @opener: the Opener, on no list.
 *
 * Runs in the opener's task when the open fails, on the unit task at
 * GENET_OPENER_REMOVE.
 */
void sana2_opener_free(struct ExecBase *SysBase, struct Opener *opener)
{
    FreeMem(opener->readRing.entries, sizeof(struct IOSana2Req *) * OPENER_READ_RING_N);
    FreeMem(opener, sizeof(struct Opener));
}

/**
 * opener_queue_control() - Queue a change of the opener list for the unit
 * task, on the unit port behind whatever the opener queued before it.
 * @unit: the unit.
 * @request: the change's request, inside @opener.
 * @op: GENET_OPENER_ADD or GENET_OPENER_REMOVE.
 * @opener: the Opener it concerns.
 *
 * Runs in the opener's task. One-way: no reply port, never replied.
 */
static void opener_queue_control(struct GenetUnit *unit, struct GenetOpenerControl *request,
                                  UWORD op, struct Opener *opener)
{
    struct ExecBase *SysBase = unit->sysBase;
    request->io.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    request->io.io_Message.mn_ReplyPort = NULL;
    request->io.io_Message.mn_Length = sizeof(struct GenetOpenerControl);
    request->io.io_Unit = (struct Unit *)unit;
    request->io.io_Command = GENET_CMD_OPENER_CONTROL;
    request->io.io_Flags = 0;
    request->op = op;
    request->opener = opener;
    PutMsg(&unit->unit.unit_MsgPort, &request->io.io_Message);
}

/**
 * sana2_opener_attach() - Hand @opener out, and queue its addition to the
 * unit's opener list.
 * @unit: the unit.
 * @opener: the new Opener.
 *
 * Runs in the opener's task (OpenDevice()). It goes on the issued list, which
 * sana2_opener_detach() checks a closing request against; the unit task links
 * it at GENET_OPENER_ADD.
 */
void sana2_opener_attach(struct GenetUnit *unit, struct Opener *opener)
{
    struct ExecBase *SysBase = unit->sysBase;
    AddTailMinList(&unit->s2->issued, &opener->issuedNode);
    opener_queue_control(unit, &opener->addRequest, GENET_OPENER_ADD, opener);
}

/**
 * sana2_opener_detach() - Take back the Opener a closing request names, and
 * queue its removal from the unit's opener list.
 * @unit: the unit.
 * @cookie: the closing request's ios2_BufferManagement.
 *
 * Runs in the opener's task (CloseDevice()). Only an Opener on the issued
 * list is taken back; anything else in the field - a request of IOSana2Req
 * size that was no SANA-II open, a second close - is left alone. The unit
 * task answers the opener's requests, unlinks it and frees it at
 * GENET_OPENER_REMOVE, so nothing may touch it after this call.
 */
void sana2_opener_detach(struct GenetUnit *unit, APTR cookie)
{
    struct ExecBase *SysBase = unit->sysBase;
    for (struct MinNode *node = unit->s2->issued.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        struct Opener *opener = (struct Opener *)((u8 *)node - offsetof(struct Opener, issuedNode));
        if (opener != cookie)
            continue;
        RemoveMinNode(node);
        opener_queue_control(unit, &opener->removeRequest, GENET_OPENER_REMOVE, opener);
        return;
    }
    if (cookie != NULL)
        Kprintf("[genet] %s: %08lx is no opener of this unit, left alone\n", __func__, (ULONG)cookie);
}

static void s2_release(struct GenetUnit *unit);

/**
 * sana2_opener_control() - Carry out a GENET_CMD_OPENER_CONTROL request.
 * @unit: the unit.
 * @request: the request; ADD and REMOVE are never replied, ABORT is.
 *
 * Unit task, which owns the opener list. GENET_OPENER_ADD links the Opener.
 * GENET_OPENER_REMOVE answers all its requests with IOERR_ABORTED, unlinks
 * it, releases the unit on the last claimer's close (s2_release()) and frees
 * it, @request with it. GENET_OPENER_ABORT takes request->target back
 * (sana2_abort_kept()) and replies @request. A change of a promiscuous
 * opener updates the reception filter.
 */
void sana2_opener_control(struct GenetUnit *unit, struct GenetOpenerControl *request)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct Opener *opener = request->opener;

    switch (request->op)
    {
    case GENET_OPENER_ADD:
        AddTailMinList(&unit->s2->openers, &opener->node);
        if (opener->promiscuous)
            sana2_filter_update(unit);
        break;
    case GENET_OPENER_REMOVE:
        /* SANA-II wants a closer's requests done first; one still waiting
         * would wait for ever, and in memory that goes with the opener */
        sana2_flush_opener(unit, opener, TRUE, IOERR_ABORTED, S2WERR_GENERIC_ERROR);
        RemoveMinNode(&opener->node);
        /* the last claimer's close hands the unit back */
        if (opener->claimer && --unit->s2->claimers == 0)
            s2_release(unit);
        else if (opener->promiscuous)
            sana2_filter_update(unit);
        /* the request lives in the opener: nothing may touch it after this */
        sana2_opener_free(SysBase, opener);
        break;
    case GENET_OPENER_ABORT:
        /* AbortIO waits for this reply, so the target stays the caller's
         * pending request until it arrives */
        sana2_abort_kept(unit, request->target);
        ReplyMsg(&request->io.io_Message);
        break;
    default:
        Kprintf("[genet] %s: unknown opener operation %lu\n", __func__, (ULONG)request->op);
        break;
    }
}

/* ------------------------------------------------------- claim, bring-up --- */

/**
 * s2_claim() - Take the unit for SANA-II on @opener's behalf.
 * @unit: the unit.
 * @opener: the Opener; one that is a claimer already changes nothing.
 * @io: the claiming request (S2_ONLINE, S2_CONFIGINTERFACE), for its error.
 *
 * The first claim allocates what the personality needs: the RX pool (the
 * ring's own buffers - frames are copied out, never lent) and the TX staging,
 * and sets the openers' reception filter.
 *
 * Return: TRUE when @opener holds the unit; FALSE with io_Error
 * S2ERR_NO_RESOURCES when the allocation failed.
 */
static BOOL s2_claim(struct GenetUnit *unit, struct Opener *opener, struct IOSana2Req *io)
{
    struct GenetS2 *s2 = unit->s2;
    if (opener->claimer)
        return TRUE;

    if (s2->claimers == 0)
    {
        if (bcmgenet_rx_pool_create(unit, RX_DESCS) != GENET_OK ||
            sana2_tx_staging_create(unit) != GENET_OK)
        {
            sana2_tx_staging_destroy(unit);
            bcmgenet_rx_pool_destroy(unit);
            io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            io->ios2_WireError = S2WERR_GENERIC_ERROR;
            return FALSE;
        }
    }
    opener->claimer = TRUE;
    if (s2->claimers++ == 0)
        sana2_filter_update(unit); /* the openers' filter, not what a netdev session left */
    return TRUE;
}

/**
 * s2_online_answer() - Answer every parked S2_ONLINE.
 * @unit: the unit.
 */
static void s2_online_answer(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct IOSana2Req *io;
    while ((io = (struct IOSana2Req *)RemHeadMinList(&unit->s2->onlineWaiters)) != NULL)
        ReplyMsg((struct Message *)io);
}

/**
 * s2_start() - Bring the hardware up for SANA-II and open the writers' gate.
 * @unit: the unit, claimed.
 *
 * The statistics restart with it: they cover the time since this S2_ONLINE,
 * as LastStart says. A start after an earlier one counts as a
 * reconfiguration. The gate (s2->online) opens under Forbid().
 *
 * Return: GENET_OK, or UnitOnline()'s error.
 */
static u32 s2_start(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct GenetS2 *s2 = unit->s2;

    if (s2->wasOnline)
        s2->reconfigurations++;
    memset(&unit->internalStats, 0, sizeof(unit->internalStats));
    bcmgenet_mib_rbuf_ovfl_rebase(unit);
    s2->rxOrphan = 0;
    s2->rxIpArpDropped = 0;
    s2->rxBufferErrors = 0;
    s2->txQueued = 0;
    sana2_throughput_restart(unit);
    struct Device *TimerBase = unit->timerBase;
    if (TimerBase != NULL)
        GetSysTime(&s2->lastStart);

    unit->rxAlign2 = TRUE;
    u32 result = UnitOnline(unit);
    if (result != GENET_OK)
        return result;

    Forbid();
    s2->online = TRUE;
    Permit();
    s2->wasOnline = TRUE;
    return GENET_OK;
}

/**
 * s2_stop() - Take the hardware down, and answer every read and waiting write
 * with IOERR_ABORTED.
 * @unit: the unit.
 *
 * The event waiters stay, for the S2EVENT_OFFLINE this reports. Writers and
 * readers are kept out first: one that saw the gate open is done before this
 * task runs, so nothing is inside the ring when the DMA stops, and no request
 * arrives behind the flush. An offline unit is only flushed.
 */
static void s2_stop(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct GenetS2 *s2 = unit->s2;
    if (s2->online)
    {
        Forbid();
        s2->online = FALSE;
        Permit();
        UnitOffline(unit);
        s2->connected = FALSE;
        sana2_report_events(unit, S2EVENT_OFFLINE);
    }
    sana2_flush(unit, FALSE, IOERR_ABORTED, S2WERR_GENERIC_ERROR);
}

/**
 * s2_release() - Let go of the unit for SANA-II: the last claimer closed.
 * @unit: the unit.
 *
 * Answers the parked S2_ONLINEs, stops the hardware, frees the TX staging and
 * the RX pool, and leaves the unit unconfigured for SANA-II.
 */
static void s2_release(struct GenetUnit *unit)
{
    struct GenetS2 *s2 = unit->s2;
    s2_online_answer(unit);
    s2_stop(unit);
    sana2_tx_staging_destroy(unit);
    bcmgenet_rx_pool_destroy(unit);
    s2->configured = FALSE;
    s2->wasOnline = FALSE;
    unit->rxAlign2 = FALSE;
}

/**
 * s2_refuse_no_opener() - Refuse a request that has to name its opener in
 * ios2_BufferManagement, and does not.
 * @io: the request.
 *
 * Return: COMMAND_PROCESSED, with S2ERR_BAD_ARGUMENT / S2WERR_NULL_POINTER.
 */
static u32 s2_refuse_no_opener(struct IOSana2Req *io)
{
    io->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
    io->ios2_WireError = S2WERR_NULL_POINTER;
    return COMMAND_PROCESSED;
}

/**
 * Do_S2_ONLINE() - S2_ONLINE: claim the unit for the request's opener and
 * bring it online.
 * @unit: the unit.
 * @io: the request.
 *
 * Refused with S2ERR_OUTOFSERVICE while a netdev stack is attached, with
 * S2ERR_BAD_ARGUMENT without an opener, and with S2ERR_BAD_STATE /
 * S2WERR_NOT_CONFIGURED before S2_CONFIGINTERFACE. A unit online already
 * answers at once. Otherwise it reports S2EVENT_ONLINE, and S2EVENT_CONNECT
 * if the link is up; without a link the request is parked on onlineWaiters.
 * The unconfigured unit, a failed claim and a failed start report
 * S2EVENT_SOFTWARE | S2EVENT_ERROR.
 *
 * Return: COMMAND_PROCESSED, with io_Error set on failure (S2ERR_NO_RESOURCES
 * or S2ERR_SOFTWARE when the claim or the start failed); COMMAND_SCHEDULED
 * when parked for the link.
 */
static u32 Do_S2_ONLINE(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct GenetS2 *s2 = unit->s2;

    if (unit->ndStackOps != NULL)
        return sana2_refuse_netdev(io);
    if (io->ios2_BufferManagement == NULL)
        return s2_refuse_no_opener(io);
    if (!s2->configured)
    {
        io->ios2_Req.io_Error = S2ERR_BAD_STATE;
        io->ios2_WireError = S2WERR_NOT_CONFIGURED;
        sana2_report_events(unit, S2EVENT_SOFTWARE | S2EVENT_ERROR);
        return COMMAND_PROCESSED;
    }
    if (!s2_claim(unit, io->ios2_BufferManagement, io))
    {
        sana2_report_events(unit, S2EVENT_SOFTWARE | S2EVENT_ERROR);
        return COMMAND_PROCESSED;
    }
    if (s2->online)
        return COMMAND_PROCESSED;

    u32 result = s2_start(unit);
    if (result != GENET_OK)
    {
        Kprintf("[genet] %s: start failed: %lu\n", __func__, (ULONG)result);
        io->ios2_Req.io_Error = (result == ENOMEM) ? S2ERR_NO_RESOURCES : S2ERR_SOFTWARE;
        io->ios2_WireError = S2WERR_GENERIC_ERROR;
        sana2_report_events(unit, S2EVENT_SOFTWARE | S2EVENT_ERROR);
        return COMMAND_PROCESSED;
    }
    sana2_report_events(unit, S2EVENT_ONLINE);
    /* recorded and reported: S2EVENT_CONNECT if the link is up already */
    bcmgenet_link_publish(unit, unit->phydev != NULL && unit->phydev->link, TRUE);
    if (unit->linkState.ndls_Flags & NDLF_UP)
        return COMMAND_PROCESSED;

    /* The start does not wait for autonegotiation; the stack does, here: a
     * SANA-II stack sends as soon as S2_ONLINE returns, and the frames would
     * go nowhere. Answered at link-up, or when the wait runs out (online
     * without a link - a cable plugged in later still works). */
    struct ExecBase *SysBase = unit->sysBase;
    if (s2->onlineWaiters.mlh_Head->mln_Succ == NULL)
        s2->onlineWaitTicks = S2_ONLINE_WAIT_TICKS;
    AddTailMinList(&s2->onlineWaiters, (struct MinNode *)io);
    return COMMAND_SCHEDULED;
}

/**
 * s2_address_given() - Tell whether a request names an address: an all-zero
 * one means none, and the factory one is used.
 * @address: the request's ios2_SrcAddr.
 *
 * Return: TRUE unless all S2_ETHER_ADDR_BYTES are zero.
 */
static BOOL s2_address_given(const UBYTE *address)
{
    for (u32 i = 0; i < S2_ETHER_ADDR_BYTES; i++)
        if (address[i] != 0)
            return TRUE;
    return FALSE;
}

/**
 * Do_S2_CONFIGINTERFACE() - S2_CONFIGINTERFACE: set the unit's address and
 * bring it online.
 * @unit: the unit.
 * @io: the request; ios2_SrcAddr is the address, all zero for the factory one.
 *
 * Refused with S2ERR_OUTOFSERVICE while a netdev stack is attached, with
 * S2ERR_BAD_ARGUMENT without an opener. Once configured, it fails with
 * S2ERR_BAD_STATE / S2WERR_IS_CONFIGURED and gives the address in use in
 * ios2_SrcAddr. An unconfigured unit is configured first (UnitConfigure()).
 * It reports S2EVENT_CONFIGCHANGED, returns the address in ios2_SrcAddr and
 * goes on as Do_S2_ONLINE(), which claims the unit.
 *
 * Return: COMMAND_PROCESSED, with io_Error set on failure (S2ERR_NO_RESOURCES
 * or S2ERR_SOFTWARE when UnitConfigure() failed); COMMAND_SCHEDULED when the
 * S2_ONLINE is parked for the link.
 */
static u32 Do_S2_CONFIGINTERFACE(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct GenetS2 *s2 = unit->s2;

    if (unit->ndStackOps != NULL)
        return sana2_refuse_netdev(io);
    if (io->ios2_BufferManagement == NULL)
        return s2_refuse_no_opener(io);
    if (s2->configured)
    {
        io->ios2_Req.io_Error = S2ERR_BAD_STATE;
        io->ios2_WireError = S2WERR_IS_CONFIGURED;
        memcpy(io->ios2_SrcAddr, unit->currentMacAddress, S2_ETHER_ADDR_BYTES);
        sana2_report_events(unit, S2EVENT_SOFTWARE | S2EVENT_ERROR);
        return COMMAND_PROCESSED;
    }

    memcpy(unit->currentMacAddress,
           s2_address_given(io->ios2_SrcAddr) ? io->ios2_SrcAddr : unit->localMacAddress, S2_ETHER_ADDR_BYTES);
    if (unit->state == STATE_UNCONFIGURED)
    {
        u32 result = UnitConfigure(unit);
        if (result != GENET_OK)
        {
            Kprintf("[genet] %s: configure failed: %lu\n", __func__, (ULONG)result);
            io->ios2_Req.io_Error = (result == ENOMEM) ? S2ERR_NO_RESOURCES : S2ERR_SOFTWARE;
            io->ios2_WireError = S2WERR_GENERIC_ERROR;
            sana2_report_events(unit, S2EVENT_SOFTWARE | S2EVENT_ERROR);
            return COMMAND_PROCESSED;
        }
    }
    s2->configured = TRUE;
    memcpy(io->ios2_SrcAddr, unit->currentMacAddress, S2_ETHER_ADDR_BYTES);
    sana2_report_events(unit, S2EVENT_CONFIGCHANGED);

    /* configuring brings the unit online, and holds it */
    return Do_S2_ONLINE(unit, io);
}

/**
 * Do_S2_OFFLINE() - S2_OFFLINE: take the unit offline; the claim stays.
 * @unit: the unit.
 * @io: the request.
 *
 * Refused with S2ERR_OUTOFSERVICE while a netdev stack is attached. The
 * parked S2_ONLINEs complete first, then s2_stop() reports S2EVENT_OFFLINE
 * and answers the reads and waiting writes.
 *
 * Return: COMMAND_PROCESSED.
 */
static u32 Do_S2_OFFLINE(struct GenetUnit *unit, struct IOSana2Req *io)
{
    if (unit->ndStackOps != NULL)
        return sana2_refuse_netdev(io);
    /* a parked S2_ONLINE did bring the unit online: it completes first */
    s2_online_answer(unit);
    s2_stop(unit);
    return COMMAND_PROCESSED;
}

/**
 * sana2_link_notify() - Report a change of unit->linkState to SANA-II.
 * @unit: the unit.
 *
 * Unit task (bcmgenet_link_publish()), and only while SANA-II holds the unit
 * online. The link is the link-layer connection: its changes reach SANA-II as
 * S2EVENT_CONNECT / DISCONNECT, never as ONLINE / OFFLINE, which are the
 * administrative state. A link that is up answers the parked S2_ONLINEs.
 */
void sana2_link_notify(struct GenetUnit *unit)
{
    struct GenetS2 *s2 = unit->s2;
    if (!sana2_holds_unit(s2) || !s2->online)
        return;
    BOOL up = (unit->linkState.ndls_Flags & NDLF_UP) != 0;
    if (up)
        s2_online_answer(unit);
    /* transitions only: a start without a cable is no disconnection */
    if (up == s2->connected)
        return;
    s2->connected = up;
    sana2_report_events(unit, up ? S2EVENT_CONNECT : S2EVENT_DISCONNECT);
}

/**
 * sana2_tick() - SANA-II's housekeeping tick while the unit is online.
 * @unit: the unit.
 *
 * Unit task. Updates the throughput sampling, and counts down the parked
 * S2_ONLINEs' wait for the link: when S2_ONLINE_WAIT_TICKS have passed, they
 * are answered without one.
 */
void sana2_tick(struct GenetUnit *unit)
{
    struct GenetS2 *s2 = unit->s2;
    sana2_throughput_tick(unit);
    if (likely(s2->onlineWaiters.mlh_Head->mln_Succ == NULL))
        return;
    if (--s2->onlineWaitTicks == 0)
    {
        Kprintf("[genet] %s: no link after %lu ms, online without one\n", __func__,
                (ULONG)S2_ONLINE_WAIT_MS);
        s2_online_answer(unit);
    }
}

/**
 * sana2_service() - The datapath half of a unit-task wakeup while SANA-II has
 * the unit online.
 * @unit: the unit.
 * @sigset: the signals that woke the unit task.
 *
 * The TX signal replays the write backlog (sana2_tx_drain()), the RX signal
 * runs an RX pass (sana2_rx_service()). The events the writers raised in
 * pendingEvents are taken under Forbid() and reported.
 */
void sana2_service(struct GenetUnit *unit, ULONG sigset)
{
    struct GenetS2 *s2 = unit->s2;
    if (sigset & (1UL << unit->tx_signal))
        sana2_tx_drain(unit);
    if (sigset & (1UL << unit->rx_signal))
        sana2_rx_service(unit);
    if (unlikely(s2->pendingEvents != 0))
    {
        struct ExecBase *SysBase = unit->sysBase;
        Forbid();
        u32 events = s2->pendingEvents;
        s2->pendingEvents = 0;
        Permit();
        sana2_report_events(unit, events);
    }
}

/* ----------------------------------------------------------------- events --- */

/**
 * sana2_report_events() - Reply each opener's oldest S2_ONEVENT that waits
 * for one of @events.
 * @unit: the unit.
 * @events: the S2EVENT_* bits that happened.
 *
 * Unit task. The reply's ios2_WireError holds the events that happened, of
 * those it asked for; one waiter per opener is replied.
 */
void sana2_report_events(struct GenetUnit *unit, u32 events)
{
    struct ExecBase *SysBase = unit->sysBase;
    for (struct MinNode *node = unit->s2->openers.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        struct Opener *opener = (struct Opener *)node;
        for (struct MinNode *waiter = opener->eventQueue.mlh_Head; waiter->mln_Succ != NULL;
             waiter = waiter->mln_Succ)
        {
            struct IOSana2Req *io = (struct IOSana2Req *)waiter;
            if (io->ios2_WireError & events)
            {
                /* the events that happened, of those it asked for */
                io->ios2_WireError &= events;
                RemoveMinNode(waiter);
                ReplyMsg((struct Message *)io);
                break; /* one per opener */
            }
        }
    }
}

/**
 * s2_events_present() - Give the events whose state holds now: a wait for one
 * of them ends at once.
 * @unit: the unit.
 *
 * Return: S2EVENT_OFFLINE | S2EVENT_DISCONNECT while offline; online,
 * S2EVENT_ONLINE with S2EVENT_CONNECT or S2EVENT_DISCONNECT by the link.
 */
static u32 s2_events_present(const struct GenetUnit *unit)
{
    if (!unit->s2->online)
        return S2EVENT_OFFLINE | S2EVENT_DISCONNECT;
    return S2EVENT_ONLINE |
           ((unit->linkState.ndls_Flags & NDLF_UP) ? S2EVENT_CONNECT : S2EVENT_DISCONNECT);
}

/**
 * Do_S2_ONEVENT() - S2_ONEVENT: wait for one of the events in ios2_WireError.
 * @unit: the unit.
 * @io: the request; ios2_WireError is the S2EVENT_* mask.
 *
 * An event outside S2_EVENT_MASK fails with S2ERR_NOT_SUPPORTED /
 * S2WERR_BAD_EVENT, a request without an opener with S2ERR_BAD_ARGUMENT /
 * S2WERR_NULL_POINTER. One whose state holds already (s2_events_present())
 * is answered at once with those events in ios2_WireError; otherwise it waits
 * on its opener's eventQueue for sana2_report_events().
 *
 * Return: COMMAND_PROCESSED when answered; COMMAND_SCHEDULED when queued.
 */
static u32 Do_S2_ONEVENT(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct Opener *opener = io->ios2_BufferManagement;

    if (io->ios2_WireError & ~S2_EVENT_MASK)
    {
        io->ios2_Req.io_Error = S2ERR_NOT_SUPPORTED;
        io->ios2_WireError = S2WERR_BAD_EVENT;
        return COMMAND_PROCESSED;
    }
    if (opener == NULL)
    {
        io->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
        io->ios2_WireError = S2WERR_NULL_POINTER;
        return COMMAND_PROCESSED;
    }

    u32 present = io->ios2_WireError & s2_events_present(unit);
    if (present != 0)
    {
        io->ios2_WireError = present;
        return COMMAND_PROCESSED;
    }
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    AddTailMinList(&opener->eventQueue, (struct MinNode *)io);
    return COMMAND_SCHEDULED;
}

/* ---------------------------------------------------------------- queries --- */

/**
 * Do_S2_DEVICEQUERY() - S2_DEVICEQUERY: fill the caller's Sana2DeviceQuery.
 * @io: the request; ios2_StatData is the Sana2DeviceQuery.
 *
 * Ethernet, S2_ETHER_ADDR_BITS addresses, an MTU of ETH_DATA_LEN and the
 * controller's rate as BPS, not the link's. RawMTU (the header and a VLAN tag
 * included) only when SizeAvailable covers it; SizeSupplied says which.
 *
 * Return: COMMAND_PROCESSED; io_Error S2ERR_BAD_ARGUMENT with
 * S2WERR_NULL_POINTER for no StatData, with S2WERR_BAD_STATDATA when
 * SizeAvailable is short of the structure without RawMTU.
 */
static u32 Do_S2_DEVICEQUERY(struct IOSana2Req *io)
{
    struct Sana2DeviceQuery *info = io->ios2_StatData;
    const ULONG base_size = sizeof(struct Sana2DeviceQuery) - sizeof(info->RawMTU);

    if (info == NULL)
    {
        io->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
        io->ios2_WireError = S2WERR_NULL_POINTER;
        return COMMAND_PROCESSED;
    }
    if (info->SizeAvailable < base_size)
    {
        io->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
        io->ios2_WireError = S2WERR_BAD_STATDATA;
        return COMMAND_PROCESSED;
    }

    info->SizeSupplied = base_size;
    info->DevQueryFormat = 0;
    info->DeviceLevel = 0;
    info->AddrFieldSize = S2_ETHER_ADDR_BITS;
    info->MTU = ETH_DATA_LEN;
    info->BPS = S2_GENET_BPS;
    info->HardwareType = S2WireType_Ethernet;
    if (info->SizeAvailable >= sizeof(struct Sana2DeviceQuery))
    {
        info->RawMTU = ETH_DATA_LEN + ETH_HLEN + VLAN_HLEN;
        info->SizeSupplied = sizeof(struct Sana2DeviceQuery);
    }
    return COMMAND_PROCESSED;
}

/**
 * Do_S2_GETSTATIONADDRESS() - S2_GETSTATIONADDRESS: give the factory address
 * and the one in use.
 * @unit: the unit.
 * @io: the request; ios2_DstAddr and ios2_SrcAddr receive the addresses.
 *
 * Return: COMMAND_PROCESSED.
 */
static u32 Do_S2_GETSTATIONADDRESS(struct GenetUnit *unit, struct IOSana2Req *io)
{
    /* DstAddr is the factory address, SrcAddr the one in use (zero until a
     * stack configures the unit). */
    memcpy(io->ios2_DstAddr, unit->localMacAddress, sizeof(unit->currentMacAddress));
    memcpy(io->ios2_SrcAddr, unit->currentMacAddress, sizeof(unit->currentMacAddress));
    return COMMAND_PROCESSED;
}

/* ------------------------------------------------------------- dispatcher --- */

/**
 * sana2_process_command() - Carry out a SANA-II command from the unit port.
 * @unit: the unit.
 * @io: the request.
 *
 * Unit task. A request shorter than an IOSana2Req fails with IOERR_BADLENGTH,
 * a command not handled here with IOERR_NOCMD; io_Error starts at
 * S2ERR_NO_ERROR. CMD_READ arrives here only when its opener's ring was full.
 * CMD_FLUSH answers everything pending with IOERR_ABORTED.
 *
 * Return: COMMAND_PROCESSED: the caller replies; COMMAND_SCHEDULED: the
 * request is kept and replied when it completes.
 */
u32 sana2_process_command(struct GenetUnit *unit, struct IOSana2Req *io)
{
    /* Every SANA-II command needs the full request; NSCMD_DEVICEQUERY, the one
     * that takes an IOStdReq, is answered by the core. */
    if (io->ios2_Req.io_Message.mn_Length < sizeof(struct IOSana2Req))
    {
        io->ios2_Req.io_Error = IOERR_BADLENGTH;
        return COMMAND_PROCESSED;
    }

    io->ios2_Req.io_Error = S2ERR_NO_ERROR;
    switch (io->ios2_Req.io_Command)
    {
    case S2_DEVICEQUERY:
        return Do_S2_DEVICEQUERY(io);
    case S2_GETSTATIONADDRESS:
        return Do_S2_GETSTATIONADDRESS(unit, io);
    case S2_GETGLOBALSTATS:
        return sana2_get_global_stats(unit, io);
    case S2_GETEXTENDEDGLOBALSTATS:
        return sana2_get_extended_stats(unit, io);
    case S2_GETSPECIALSTATS:
        return sana2_get_special_stats(unit, io);
    case S2_SAMPLE_THROUGHPUT:
        return sana2_sample_throughput(unit, io);
    case S2_ONEVENT:
        return Do_S2_ONEVENT(unit, io);
    case S2_ADDMULTICASTADDRESS:
    case S2_ADDMULTICASTADDRESSES:
        return sana2_add_multicast(unit, io);
    case S2_DELMULTICASTADDRESS:
    case S2_DELMULTICASTADDRESSES:
        return sana2_del_multicast(unit, io);
    case S2_CONFIGINTERFACE:
        return Do_S2_CONFIGINTERFACE(unit, io);
    case S2_ONLINE:
        return Do_S2_ONLINE(unit, io);
    case S2_OFFLINE:
        return Do_S2_OFFLINE(unit, io);

    case CMD_READ:      /* a read that found its opener's ring full */
    case S2_READORPHAN:
        return sana2_queue_read(unit, io);
    case CMD_FLUSH:
        sana2_flush(unit, TRUE, IOERR_ABORTED, S2WERR_GENERIC_ERROR);
        return COMMAND_PROCESSED;

    default:
        io->ios2_Req.io_Error = IOERR_NOCMD;
        return COMMAND_PROCESSED;
    }
}
