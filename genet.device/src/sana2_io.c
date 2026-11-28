// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * sana2_io.c — SANA-II reception: the openers' reads and the delivery of
 * frames to them.
 *
 * A read reaches the unit task through its opener's ring (BeginIO, any task)
 * and waits on a per-type queue there. The RX drain (bcmgenet-rx.c) hands
 * every good frame to sana2_receive_frame(), which copies it into the oldest
 * matching read of each opener and replies it; the frame's buffer stays in
 * its ring slot. Everything but sana2_read runs on the unit task.
 */

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/utility_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function: see GenetUnit.sysBase */
#include <proto/exec.h>
#define UTILITY_BASE_NAME unit->device->utilityBase
#include <proto/utility.h>
#endif

#include <exec/errors.h>
#include <devices/sana2.h>

#include <byteorder.h>
#include <debug.h>
#include <memory.h>
#include <types.h>
#include <device.h>
#include <sana2_unit.h>

#include <genet/bcmgenet.h>
#include <genet/bcmgenet-irq.h>
#include <genet/bcmgenet-regs.h>

/* EtherType values with a queue of their own */
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP 0x0806
/* the group bit of a destination address, in its first byte */
#define ETHER_GROUP_BIT 0x01u

/**
 * read_queue_for() - Give the opener's CMD_READ queue for a packet type.
 * @opener: the Opener.
 * @packetType: the EtherType, or an 802.3 length.
 *
 * Return: ipv4Queue for ETHERTYPE_IPV4, arpQueue for ETHERTYPE_ARP,
 * readQueue for every other type.
 */
static inline struct MinList *read_queue_for(struct Opener *opener, u16 packetType)
{
    switch (packetType)
    {
    case ETHERTYPE_IPV4:
        return &opener->ipv4Queue;
    case ETHERTYPE_ARP:
        return &opener->arpQueue;
    default:
        return &opener->readQueue;
    }
}

/* ------------------------------------------------------------------ reads --- */

/**
 * sana2_read() - CMD_READ / S2_READORPHAN from BeginIO: hand the read to the
 * unit task through its opener's ring.
 * @unit: the unit.
 * @io: the read.
 *
 * Runs in the reader's task. A read it queues loses IOF_QUICK and is marked
 * NT_MESSAGE, pending for CheckIO() and WaitIO(); a full ring sends it to the
 * unit port instead (sana2_queue_read()).
 *
 * Return: COMMAND_SCHEDULED when queued; COMMAND_PROCESSED with
 * S2ERR_BAD_ARGUMENT / S2WERR_NULL_POINTER without an opener, with
 * S2ERR_OUTOFSERVICE / S2WERR_UNIT_OFFLINE while the unit is offline.
 */
u32 sana2_read(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct Opener *opener = io->ios2_BufferManagement;

    io->ios2_Req.io_Error = S2ERR_NO_ERROR;
    if (unlikely(opener == NULL))
    {
        io->ios2_WireError = S2WERR_NULL_POINTER;
        io->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
        return COMMAND_PROCESSED;
    }

    /* Under Forbid, like the gate's flip: a read that sees the unit online is
     * in the ring before the unit task can take it offline and flush. */
    u32 result = COMMAND_SCHEDULED;
    Forbid();
    if (unlikely(!unit->s2->online))
    {
        io->ios2_WireError = S2WERR_UNIT_OFFLINE;
        io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
        result = COMMAND_PROCESSED;
    }
    else
    {
        /* Pending from here: CheckIO() and WaitIO() take NT_REPLYMSG for done,
         * and a read is not on any port until the unit task replies it. */
        io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
        io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        ULONG producer = opener->readRing.producer;
        if (likely(producer - opener->readRing.consumer <= OPENER_READ_RING_MASK))
        {
            opener->readRing.entries[producer & OPENER_READ_RING_MASK] = io;
            asm volatile("" ::: "memory");
            opener->readRing.producer = producer + 1u;
        }
        else
        {
            /* Ring full: the unit port, where sana2_queue_read() empties the
             * ring before it queues this one, so the order holds. */
            PutMsg(&unit->unit.unit_MsgPort, &io->ios2_Req.io_Message);
        }
    }
    Permit();
    return result;
}

/**
 * drain_read_ring() - Move the reads on @opener's ring onto its queues.
 * @unit: the unit.
 * @opener: the Opener.
 *
 * Unit task, the ring's only consumer. In ring order, S2_READORPHAN onto
 * orphanQueue, CMD_READ onto the queue of its ios2_PacketType.
 */
static void drain_read_ring(struct GenetUnit *unit, struct Opener *opener)
{
    struct ExecBase *SysBase = unit->sysBase;
    ULONG producer = opener->readRing.producer;
    asm volatile("" ::: "memory"); /* the producer index before the entries it publishes */
    ULONG consumer = opener->readRing.consumer;
    while (consumer != producer)
    {
        struct IOSana2Req *io = opener->readRing.entries[consumer & OPENER_READ_RING_MASK];
        consumer++;
        if (io->ios2_Req.io_Command == S2_READORPHAN)
            AddTailMinList(&opener->orphanQueue, (struct MinNode *)io);
        else
            AddTailMinList(read_queue_for(opener, (u16)io->ios2_PacketType), (struct MinNode *)io);
    }
    opener->readRing.consumer = consumer;
}

/**
 * sana2_queue_read() - Queue a read that reached the unit port because its
 * opener's ring was full.
 * @unit: the unit.
 * @io: the read (CMD_READ or S2_READORPHAN).
 *
 * Unit task. The ring is drained first, so the reads keep their order.
 *
 * Return: COMMAND_SCHEDULED when queued; COMMAND_PROCESSED with
 * S2ERR_BAD_ARGUMENT / S2WERR_NULL_POINTER without an opener, with
 * S2ERR_OUTOFSERVICE / S2WERR_UNIT_OFFLINE while the unit is offline.
 */
u32 sana2_queue_read(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct Opener *opener = io->ios2_BufferManagement;

    if (opener == NULL)
    {
        io->ios2_WireError = S2WERR_NULL_POINTER;
        io->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
        return COMMAND_PROCESSED;
    }
    if (!unit->s2->online)
    {
        io->ios2_WireError = S2WERR_UNIT_OFFLINE;
        io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
        return COMMAND_PROCESSED;
    }

    drain_read_ring(unit, opener);
    if (io->ios2_Req.io_Command == S2_READORPHAN)
        AddTailMinList(&opener->orphanQueue, (struct MinNode *)io);
    else
        AddTailMinList(read_queue_for(opener, (u16)io->ios2_PacketType), (struct MinNode *)io);
    return COMMAND_SCHEDULED;
}

/* ---------------------------------------------------------------- delivery --- */

/**
 * copy_packet() - Deliver one frame to one read.
 * @unit: the unit.
 * @io: the read, off its queue.
 * @frame: the frame, from its destination address.
 * @length: the frame's length in bytes.
 * @castFlags: the frame's SANA2IOF_BCAST / SANA2IOF_MCAST, or 0.
 *
 * Fills the read's addresses, packet type and cast flags. A SANA2IOF_RAW read
 * gets the whole frame, any other the payload; use_miami_workaround rounds
 * the copy up to a longword. S2_PacketFilter is asked for CMD_READ only. An
 * empty payload, or no or a failing CopyToBuff, replies the read with
 * S2ERR_NO_RESOURCES / S2WERR_BUFF_ERROR, counts it in rxBufferErrors and
 * reports S2EVENT_BUFF | S2EVENT_RX | S2EVENT_SOFTWARE | S2EVENT_ERROR.
 *
 * Return: TRUE when the read was consumed (replied, with the frame or with an
 * error); FALSE when the opener's S2_PacketFilter declined the frame, in
 * which case the read is not replied and the caller puts it back where it
 * came from: a filtered frame leaves the CMD_READ pending.
 */
static inline BOOL copy_packet(struct GenetUnit *unit, struct IOSana2Req *io, u8 *frame,
                               u32 length, UBYTE castFlags)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct Opener *opener = io->ios2_BufferManagement;

    /* The header: DST (6), SRC (6), EtherType (2). The frame is 2 short of a
     * longword (S2_RX_FRAME_OFFSET), so +2, +6 and +10 are ULONG-aligned, and
     * ios2_DstAddr/SrcAddr sit at longword offsets in the request. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
    *(UWORD *)io->ios2_DstAddr = *(const UWORD *)frame;
    *(ULONG *)(io->ios2_DstAddr + 2) = *(const ULONG *)(frame + 2);
    *(ULONG *)io->ios2_SrcAddr = *(const ULONG *)(frame + 6);
    *(UWORD *)(io->ios2_SrcAddr + 4) = *(const UWORD *)(frame + 10);
    io->ios2_PacketType = be16(*(const __be16 *)(frame + 12));
#pragma GCC diagnostic pop

    io->ios2_Req.io_Flags = (UBYTE)((io->ios2_Req.io_Flags & ~(SANA2IOF_BCAST | SANA2IOF_MCAST)) | castFlags);

    /* RAW gets the whole frame, everyone else its payload */
    if (!(io->ios2_Req.io_Flags & SANA2IOF_RAW))
    {
        frame += ETH_HLEN;
        length -= ETH_HLEN;
    }

    if (opener->packetFilter != NULL && io->ios2_Req.io_Command == CMD_READ &&
        !CallHookPkt(opener->packetFilter, io, frame))
        return FALSE;

    u32 copy_length = length;
    if (unit->device->runtimeConfig.use_miami_workaround)
        copy_length = (copy_length + 3u) & ~3u;

    PERF_T0(t_copy);
    if (unlikely(length == 0 || opener->CopyToBuff == NULL ||
                 !opener->CopyToBuff(io->ios2_Data, frame, copy_length)))
    {
        unit->s2->rxBufferErrors++;
        io->ios2_WireError = S2WERR_BUFF_ERROR;
        io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        sana2_report_events(unit, S2EVENT_BUFF | S2EVENT_RX | S2EVENT_SOFTWARE | S2EVENT_ERROR);
    }
    PERF_ADD(&unit->gu_Perf, GP_RX_COPY, t_copy);
    io->ios2_DataLength = length;

    PERF_T0(t_reply);
    ReplyMsg((struct Message *)io);
    PERF_ADD(&unit->gu_Perf, GP_RX_REPLY, t_reply);
    return TRUE;
}

/**
 * deliver_from() - Deliver a frame to the oldest read in @queue.
 * @unit: the unit.
 * @queue: an opener's read queue.
 * @frame: the frame, from its destination address.
 * @length: the frame's length in bytes.
 * @castFlags: the frame's SANA2IOF_BCAST / SANA2IOF_MCAST, or 0.
 *
 * A read the filter declined keeps its place at the head.
 *
 * Return: TRUE when the read took the frame and was consumed; FALSE when
 * @queue is empty or its read declined the frame.
 */
static inline BOOL deliver_from(struct GenetUnit *unit, struct MinList *queue, u8 *frame,
                                u32 length, UBYTE castFlags)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct IOSana2Req *io = (struct IOSana2Req *)RemHeadMinList(queue);
    if (io == NULL)
        return FALSE;
    if (copy_packet(unit, io, frame, length, castFlags))
        return TRUE;
    AddHeadMinList(queue, (struct MinNode *)io);
    return FALSE;
}

/**
 * deliver_other() - Deliver a frame to the first read in @opener's catch-all
 * queue that asks for its type.
 * @unit: the unit.
 * @opener: the Opener.
 * @packetType: the frame's EtherType or 802.3 length.
 * @frame: the frame, from its destination address.
 * @length: the frame's length in bytes.
 * @castFlags: the frame's SANA2IOF_BCAST / SANA2IOF_MCAST, or 0.
 *
 * EthernetII types are above 1500, and every 802.3 length matches every
 * 802.3 read.
 *
 * Return: TRUE when a read took the frame; FALSE when none asks for the type
 * or the first that does declined it.
 */
static BOOL deliver_other(struct GenetUnit *unit, struct Opener *opener, u16 packetType,
                          u8 *frame, u32 length, UBYTE castFlags)
{
    struct ExecBase *SysBase = unit->sysBase;
    for (struct MinNode *node = opener->readQueue.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        struct IOSana2Req *io = (struct IOSana2Req *)node;
        /* Its own type, or both 802.3: up to ETH_DATA_LEN the field is a
         * length, not a type, so any 802.3 read takes any 802.3 frame. */
        if (io->ios2_PacketType != packetType &&
            !(packetType <= ETH_DATA_LEN && io->ios2_PacketType <= ETH_DATA_LEN))
            continue;
        RemoveMinNode(node);
        if (copy_packet(unit, io, frame, length, castFlags))
            return TRUE;
        /* declined: it was the first of its type, and stays the first */
        AddHeadMinList(&opener->readQueue, node);
        return FALSE;
    }
    return FALSE;
}

/**
 * cast_flags() - Give the destination's kind, as a read reports it.
 * @destination: the frame's destination address.
 *
 * From the address itself, which leaves no doubt about which of the two a
 * broadcast is.
 *
 * Return: SANA2IOF_BCAST, SANA2IOF_MCAST, or 0 for neither.
 */
static UBYTE cast_flags(const u8 *destination)
{
    static const u8 broadcast[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (!(destination[0] & ETHER_GROUP_BIT))
        return 0;
    return memcmp(destination, broadcast, sizeof(broadcast)) == 0 ? SANA2IOF_BCAST : SANA2IOF_MCAST;
}

/**
 * frame_for_everyone() - Tell whether every opener wants the frame, or only
 * the promiscuous ones.
 * @unit: the unit.
 * @destination: the frame's destination address.
 * @castFlags: the frame's cast_flags().
 *
 * While the hardware filters by exact match, every frame it passes is wanted.
 * While it passes everything - an opener asked to, or the multicast ranges
 * overflow its slots - a frame nobody joined or addressed is for the
 * promiscuous ones.
 *
 * Return: TRUE for every opener: exact-match filtering, a broadcast, a
 * multicast in a requested range or the unit's own address; FALSE for the
 * promiscuous openers only.
 */
static BOOL frame_for_everyone(const struct GenetUnit *unit, const u8 *destination, UBYTE castFlags)
{
    if (likely(!unit->ndPromisc) || castFlags == SANA2IOF_BCAST)
        return TRUE;
    if (castFlags == SANA2IOF_MCAST)
        return sana2_multicast_wanted(unit->s2, destination);
    return memcmp(destination, unit->currentMacAddress, sizeof(unit->currentMacAddress)) == 0;
}

/**
 * sana2_receive_frame() - Deliver a received frame to the openers' reads.
 * @unit: the unit.
 * @frame: the frame in its ring buffer, from its destination address.
 * @length: the frame's length in bytes.
 *
 * Unit task: the RX drain's delivery. The buffer stays in its ring slot.
 * Counts rx_packets and rx_bytes; every opener that waits for the type gets a
 * copy in its oldest matching read. A frame nobody took counts in rxOrphan,
 * an IP or ARP one in rxIpArpDropped too, and goes to each opener's oldest
 * S2_READORPHAN. A frame not for everyone (frame_for_everyone()) reaches the
 * promiscuous openers only.
 */
void sana2_receive_frame(struct GenetUnit *unit, u8 *frame, u32 length)
{
    struct GenetS2 *s2 = unit->s2;
    UBYTE castFlags = cast_flags(frame);
    BOOL everyone = frame_for_everyone(unit, frame, castFlags);

    unit->internalStats.rx_packets++;
    unit->internalStats.rx_bytes += length;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
    u16 packetType = be16(*(const __be16 *)(frame + 12));
#pragma GCC diagnostic pop

    /* every opener that waits for the type gets a copy */
    BOOL taken = FALSE;
    BOOL fast = packetType == ETHERTYPE_IPV4 || packetType == ETHERTYPE_ARP;
    for (struct MinNode *node = s2->openers.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        struct Opener *opener = (struct Opener *)node;
        if (unlikely(!everyone) && !opener->promiscuous)
            continue;
        BOOL delivered = likely(fast)
                             ? deliver_from(unit, read_queue_for(opener, packetType), frame, length, castFlags)
                             : deliver_other(unit, opener, packetType, frame, length, castFlags);
        if (delivered)
            taken = TRUE;
    }
    if (likely(taken))
        return;

    if (fast)
        s2->rxIpArpDropped++;
    s2->rxOrphan++;
    /* nobody wanted it: offered to every opener that reads orphans */
    for (struct MinNode *node = s2->openers.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        struct Opener *opener = (struct Opener *)node;
        if (unlikely(!everyone) && !opener->promiscuous)
            continue;
        (void)deliver_from(unit, &opener->orphanQueue, frame, length, castFlags);
    }
}

/**
 * sana2_rx_service() - One RX pass on an RX wakeup.
 * @unit: the unit.
 *
 * Unit task. The openers' new reads first, then up to S2_RX_BUDGET frames. A
 * pass that used the budget leaves more behind and signals the task to come
 * round again; otherwise the interrupt the ISR masked (UMAC_IRQ_RXDMA_DONE)
 * is re-armed.
 */
void sana2_rx_service(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->sysBase;
    for (struct MinNode *node = unit->s2->openers.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
        drain_read_ring(unit, (struct Opener *)node);

    genet_rx_wake_account(unit);
    if (bcmgenet_sana2_rx(unit, S2_RX_BUDGET) == (s32)S2_RX_BUDGET)
        Signal(unit->task, 1UL << unit->rx_signal);
    else
        bcmgenet_irq0_enable(unit, UMAC_IRQ_RXDMA_DONE);
}

/* ------------------------------------------------------- flush and abort --- */

/**
 * reply_all() - Reply every request on @queue with an error.
 * @SysBase: exec.library.
 * @queue: the queue; left empty.
 * @error: the io_Error.
 * @wireError: the ios2_WireError.
 */
static void reply_all(struct ExecBase *SysBase, struct MinList *queue, BYTE error, ULONG wireError)
{
    struct IOSana2Req *io;
    while ((io = (struct IOSana2Req *)RemHeadMinList(queue)) != NULL)
    {
        io->ios2_Req.io_Error = error;
        io->ios2_WireError = wireError;
        ReplyMsg((struct Message *)io);
    }
}

/**
 * reply_opener() - Reply @opener's requests on @queue with an error; the
 * others keep their place.
 * @SysBase: exec.library.
 * @queue: a list of several openers' requests (onlineWaiters).
 * @opener: the Opener whose requests are replied.
 * @error: the io_Error.
 * @wireError: the ios2_WireError.
 */
static void reply_opener(struct ExecBase *SysBase, struct MinList *queue, struct Opener *opener,
                         BYTE error, ULONG wireError)
{
    struct MinNode *next;
    for (struct MinNode *node = queue->mlh_Head; (next = node->mln_Succ) != NULL; node = next)
    {
        struct IOSana2Req *io = (struct IOSana2Req *)node;
        if (io->ios2_BufferManagement != opener)
            continue;
        RemoveMinNode(node);
        io->ios2_Req.io_Error = error;
        io->ios2_WireError = wireError;
        ReplyMsg((struct Message *)io);
    }
}

/**
 * sana2_flush_opener() - Answer every read of @opener with @error.
 * @unit: the unit.
 * @opener: the Opener.
 * @events: TRUE (its close, CMD_FLUSH): everything else it has pending too.
 * @error: the io_Error.
 * @wireError: the ios2_WireError.
 *
 * Unit task. With @events also its event waits, its throughput sampling, a
 * parked S2_ONLINE and the writes waiting for ring room.
 */
void sana2_flush_opener(struct GenetUnit *unit, struct Opener *opener, BOOL events, BYTE error,
                        ULONG wireError)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct GenetS2 *s2 = unit->s2;
    drain_read_ring(unit, opener);
    reply_all(SysBase, &opener->ipv4Queue, error, wireError);
    reply_all(SysBase, &opener->arpQueue, error, wireError);
    reply_all(SysBase, &opener->readQueue, error, wireError);
    reply_all(SysBase, &opener->orphanQueue, error, wireError);
    if (!events)
        return;
    reply_all(SysBase, &opener->eventQueue, error, wireError);
    reply_opener(SysBase, &s2->onlineWaiters, opener, error, wireError);
    sana2_tx_backlog_abort_opener(unit, opener, error, wireError);
    struct IOSana2Req *sampling = s2->throughputRequest;
    if (sampling != NULL && sampling->ios2_BufferManagement == opener)
    {
        s2->throughputRequest = NULL;
        sampling->ios2_Req.io_Error = error;
        sampling->ios2_WireError = wireError;
        ReplyMsg((struct Message *)sampling);
    }
}

/**
 * sana2_flush() - Answer every read of every opener and every waiting write
 * with @error.
 * @unit: the unit.
 * @events: TRUE: the event waiters and the rest of sana2_flush_opener() too.
 * @error: the io_Error.
 * @wireError: the ios2_WireError.
 *
 * Unit task. CMD_FLUSH passes @events; the unit going offline does not, and
 * keeps the event waiters for the event it reports.
 */
void sana2_flush(struct GenetUnit *unit, BOOL events, BYTE error, ULONG wireError)
{
    sana2_tx_backlog_abort(unit, error, wireError);
    for (struct MinNode *node = unit->s2->openers.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
        sana2_flush_opener(unit, (struct Opener *)node, events, error, wireError);
}

/**
 * list_take() - Take @wanted off @queue if it is on it.
 * @SysBase: exec.library.
 * @queue: the list to search.
 * @wanted: the node.
 *
 * Return: TRUE when found and removed; FALSE when not on @queue.
 */
static BOOL list_take(struct ExecBase *SysBase, struct MinList *queue, struct MinNode *wanted)
{
    for (struct MinNode *node = queue->mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        if (node == wanted)
        {
            RemoveMinNode(node);
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * sana2_abort_kept() - AbortIO of a request the unit task keeps.
 * @unit: the unit.
 * @io: a read, an event wait, the throughput sampling or a parked S2_ONLINE.
 *
 * Unit task (GENET_OPENER_ABORT). A request found is replied with
 * IOERR_ABORTED / S2WERR_GENERIC_ERROR. Not found means it completed on its
 * way here.
 */
void sana2_abort_kept(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct GenetS2 *s2 = unit->s2;
    struct Opener *opener = io->ios2_BufferManagement;
    struct MinNode *node = (struct MinNode *)io;
    BOOL found = list_take(SysBase, &s2->onlineWaiters, node);

    if (!found && s2->throughputRequest == io)
    {
        s2->throughputRequest = NULL;
        found = TRUE;
    }
    if (!found && opener != NULL)
    {
        drain_read_ring(unit, opener);
        found = list_take(SysBase, &opener->ipv4Queue, node) ||
                list_take(SysBase, &opener->arpQueue, node) ||
                list_take(SysBase, &opener->readQueue, node) ||
                list_take(SysBase, &opener->orphanQueue, node) ||
                list_take(SysBase, &opener->eventQueue, node);
    }
    if (found)
    {
        io->ios2_Req.io_Error = IOERR_ABORTED;
        io->ios2_WireError = S2WERR_GENERIC_ERROR;
        ReplyMsg((struct Message *)io);
    }
}
