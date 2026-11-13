// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifndef _GENET_SANA2_UNIT_H
#define _GENET_SANA2_UNIT_H

/*
 * The SANA-II personality: per-opener state and the unit's SANA-II side.
 *
 * Every OpenDevice() made with an IOSana2Req-sized request gets an Opener,
 * whatever it goes on to speak: a stack may open with SANA-II buffer tags
 * before it knows which ABI the driver has, and a diagnostic tool opens with
 * an IOSana2Req to ask queries. Opening holds nothing; the Opener only says
 * how to reach the opener's buffers.
 *
 * The unit's SANA-II state is allocated with the unit (struct GenetS2) and
 * sits behind one pointer in the cold part of GenetUnit, so the netdev
 * datapath's layout does not see it.
 */

#include <exec/io.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <devices/sana2.h>
#include <devices/timer.h>
#include <utility/hooks.h>

#include <types.h>

struct GenetUnit;
struct Opener;

/* A SANA-II handler's outcome: reply now, or the request was kept (queued for
 * an event, a frame, ring room) and is replied later. */
#define COMMAND_PROCESSED 1u
#define COMMAND_SCHEDULED 0u

/*
 * TX staging: one slot of RX_BUF_LENGTH bytes per BD, bound to it for the
 * life of the claim, laid out [pad][TSB][frame]. The pad leaves the Ethernet
 * header 2 bytes short of a longword, so the body behind it is 4-aligned like
 * a stack's packet buffer and the opener's CopyFromBuff moves longwords on
 * both sides. The TSB is zeroed once (no offload) and goes out with the frame
 * under one descriptor, as the hardware's status-block mode wants.
 */
#define S2_TX_PAD 2u
#define S2_TX_FRAME_OFFSET (S2_TX_PAD + GENET_STATUS64_LEN)
/* Longest ios2_DataLength a slot takes - the frame, its header and the Miami
 * round-up have to fit. An overflow guard, not MTU policy. */
#define S2_TX_MAX_DATALEN (RX_BUF_LENGTH - S2_TX_FRAME_OFFSET - ETH_HLEN - 4u)

/* RX: frames are copied out of the ring's own buffers. With RBUF_ALIGN_2B the
 * frame sits behind the 64-byte RSB and a 2-byte pad, so its IP header is
 * 4-aligned for the opener's CopyToBuff. */
#define S2_RX_PAD 2u
#define S2_RX_FRAME_OFFSET (GENET_STATUS64_LEN + S2_RX_PAD)
/* Frames delivered per unit-task wakeup before it lets TX and the commands
 * have a turn (and signals itself to carry on). */
#define S2_RX_BUDGET 64u

/* Per-opener ring that hands CMD_READ / S2_READORPHAN from BeginIO to the
 * unit task. One CPU, so a compiler barrier orders it. */
#define OPENER_READ_RING_N 512u /* a power of two */
#define OPENER_READ_RING_MASK (OPENER_READ_RING_N - 1u)

/* How long a parked S2_ONLINE waits for the link before it is answered
 * anyway, in housekeeping ticks: autonegotiation takes up to ~3 s. */
#define S2_ONLINE_WAIT_MS 6000u
#define S2_ONLINE_WAIT_TICKS \
    ((S2_ONLINE_WAIT_MS + GENET_PERIODIC_TASK_MS - 1u) / GENET_PERIODIC_TASK_MS)

/*
 * Private command: a change to the unit's opener list. The list belongs to the
 * unit task, and OpenDevice()/CloseDevice() run in the opener's task, so the
 * change travels to the unit task on the unit port, behind whatever the opener
 * queued before it. BeginIO never routes this command; only the open and close
 * paths queue it.
 */
#define GENET_CMD_OPENER_CONTROL 0x7FF0u

enum GenetOpenerOp
{
    GENET_OPENER_ADD = 1,
    GENET_OPENER_REMOVE, /* the unit task frees the opener once it is unlinked */
    GENET_OPENER_ABORT,  /* AbortIO of a request the unit task keeps: replied to
                          * the aborting task, which waits for it */
};

/* ADD and REMOVE are one-way (no reply port), so a close needs no signal and
 * no memory; ABORT is answered. */
struct GenetOpenerControl
{
    struct IOStdReq io; /* io_Command = GENET_CMD_OPENER_CONTROL, io_Unit = the unit */
    UWORD op;           /* enum GenetOpenerOp */
    struct Opener *opener;
    struct IOSana2Req *target; /* GENET_OPENER_ABORT: the request to take back */
};

typedef BOOL (*S2CopyBuffFunc)(APTR to asm("a0"), APTR from asm("a1"), ULONG len asm("d0"));

struct Opener
{
    struct MinNode node;       /* on GenetS2.openers: unit task only */
    struct MinNode issuedNode; /* on GenetS2.issued: openLib/closeLib only */
    BOOL claimer;       /* did S2_CONFIGINTERFACE or S2_ONLINE: holds the unit until it closes */
    BOOL promiscuous;    /* opened with SANA2OPF_PROM: every frame, whatever its address */

    /* Buffer management, from the OpenDevice() tags. CMD_READ: TRUE from the
     * filter hands the frame to this opener, FALSE declines it. The copy
     * functions return TRUE on success. */
    struct Hook *packetFilter;
    S2CopyBuffFunc CopyToBuff;
    S2CopyBuffFunc CopyFromBuff;

    /* Reads waiting for a frame, by type. Unit task only: BeginIO pushes onto
     * readRing, and the unit task moves the entries here before each RX pass. */
    struct MinList ipv4Queue;   /* 0x0800 */
    struct MinList arpQueue;    /* 0x0806 */
    struct MinList readQueue;   /* every other type */
    struct MinList orphanQueue; /* S2_READORPHAN */
    struct MinList eventQueue;  /* S2_ONEVENT waiters, unit task only */
    struct
    {
        volatile ULONG producer; /* written only by BeginIO */
        volatile ULONG consumer; /* written only by the unit task */
        struct IOSana2Req **entries;
    } readRing;

    /* The opener's own list changes, carried inside it (two, so a close never
     * reuses a request that is still queued from the open). */
    struct GenetOpenerControl addRequest;
    struct GenetOpenerControl removeRequest;
};

/* A multicast address range the openers asked for, inclusive, counted per
 * request: an S2_DELMULTICASTADDRESS takes back one S2_ADDMULTICASTADDRESS. */
struct MulticastRange
{
    struct MinNode node;
    u32 useCount;
    u64 lowerBound;
    u64 upperBound;
};

/* The unit's SANA-II side. Unit task only, unless a field says otherwise. */
struct GenetS2
{
    struct MinList openers; /* every Opener of the unit */

    /* Every Opener openLib handed out that closeLib has not taken back: a
     * closing request's ios2_BufferManagement is used only if it is on this
     * list. openLib and closeLib only, which exec calls under Forbid(). */
    struct MinList issued;

    /* The claim: SANA-II holds the unit while claimers != 0, and the first
     * claim takes what the personality needs (RX pool, TX staging). */
    u32 claimers;
    BOOL configured; /* S2_CONFIGINTERFACE has set the address */
    BOOL wasOnline;  /* an S2_ONLINE after this one counts as a reconfiguration */
    BOOL connected;  /* the link as last reported (S2EVENT_CONNECT / DISCONNECT) */

    /* The writers' gate. Read under Forbid by any task: the unit task flips
     * it under Forbid, so a writer that sees it set owns the ring and the
     * staging until its Permit(). */
    volatile BOOL online;

    /* TX: the claim's staging slots (TX_DESCS x RX_BUF_LENGTH, from the DMA
     * pool), and the writes waiting for ring room, under Forbid. */
    u8 *txStaging;
    struct MinList txBacklog;
    u32 txQueued; /* writes that waited in txBacklog */

    /* S2_ONLINE replies parked until the link is up or the wait runs out */
    struct MinList onlineWaiters;
    u32 onlineWaitTicks;

    /* S2_GETGLOBALSTATS / S2_GETEXTENDEDGLOBALSTATS. The packet and error
     * counters are the core's (internal_stats); these are what only SANA-II
     * reports. */
    struct timeval lastStart; /* when the personality last went online */
    u32 reconfigurations;     /* S2_ONLINE after an S2_OFFLINE */
    u64 rxOrphan;             /* frames no opener's read took */
    u32 rxIpArpDropped;       /* IP/ARP frames with no read waiting for them */
    u32 rxBufferErrors;       /* the opener's CopyToBuff failed */

    /* Multicast: the ranges, flattened into the core's reception filter
     * (sana2_filter_update). Too many addresses for the exact-match slots
     * make the hardware promiscuous, and then the delivery checks each frame
     * against them (sana2_receive_frame). */
    struct MinList multicastRanges;

    /* Events raised where the event queues cannot be touched (a writer's
     * task), under Forbid; the unit task reports them. */
    volatile u32 pendingEvents;

    /* S2_SAMPLE_THROUGHPUT: the one request sampling on the housekeeping
     * tick, and the counts its window starts from. */
    struct IOSana2Req *throughputRequest;
    u64 throughputBaseTx;
    u64 throughputBaseRx;
    struct timeval throughputStart;
    u64 throughputUpdates;
};

/**
 * sana2_is_command() - Tell whether @cmd is a SANA-II command, the exec
 * standard ones SANA-II uses included.
 * @cmd: the io_Command.
 *
 * BeginIO serves the reads (CMD_READ, S2_READORPHAN) and the writes (CMD_WRITE,
 * S2_BROADCAST, S2_MULTICAST) in the caller's task before it asks; the rest
 * it queues for the unit task, whose handler answers the ones not supported.
 *
 * Return: TRUE for CMD_READ, CMD_WRITE, CMD_FLUSH and the S2_* ranges.
 */
static inline BOOL sana2_is_command(UWORD cmd)
{
    return cmd == CMD_READ || cmd == CMD_WRITE || cmd == CMD_FLUSH ||
           (cmd >= S2_START && cmd < S2_END) ||
           (cmd >= S2_ADDMULTICASTADDRESSES && cmd <= S2_SAMPLE_THROUGHPUT);
}

/* Opener lifecycle (sana2_api.c). create/free run in the opener's task;
 * attach/detach keep the issued list and queue the list change for the unit
 * task. */
struct Opener *sana2_opener_create(struct ExecBase *SysBase, struct Library *UtilityBase,
                                   struct TagItem *tags, ULONG flags);
void sana2_opener_free(struct ExecBase *SysBase, struct Opener *opener);
void sana2_opener_attach(struct GenetUnit *unit, struct Opener *opener);
void sana2_opener_detach(struct GenetUnit *unit, APTR cookie);

/**
 * sana2_holds_unit() - Tell whether SANA-II holds the unit; netdev's ATTACH
 * is refused while it does.
 * @s2: the unit's SANA-II side.
 *
 * Return: TRUE while an opener is a claimer.
 */
static inline BOOL sana2_holds_unit(const struct GenetS2 *s2)
{
    return s2->claimers != 0;
}

/**
 * sana2_refuse_netdev() - Refuse a SANA-II request: a netdev stack is
 * attached, and the unit is out of service for SANA-II.
 * @io: the request.
 *
 * Never S2ERR_BAD_STATE, which stacks read as "already configured / online"
 * and go on to bring up an interface that receives nothing.
 *
 * Return: COMMAND_PROCESSED, with S2ERR_OUTOFSERVICE / S2WERR_GENERIC_ERROR.
 */
static inline u32 sana2_refuse_netdev(struct IOSana2Req *io)
{
    io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
    io->ios2_WireError = S2WERR_GENERIC_ERROR;
    return COMMAND_PROCESSED;
}

/* Unit task. */
void sana2_opener_control(struct GenetUnit *unit, struct GenetOpenerControl *request);
u32 sana2_process_command(struct GenetUnit *unit, struct IOSana2Req *io);
/* The datapath half of a wakeup while SANA-II has the unit online. */
void sana2_service(struct GenetUnit *unit, ULONG sigset);
/* unit->linkState changed (bcmgenet_link_publish). */
void sana2_link_notify(struct GenetUnit *unit);
/* Housekeeping tick while the unit is online. */
void sana2_tick(struct GenetUnit *unit);

/* RX (sana2_io.c). sana2_read runs in the reader's task, the rest on the unit
 * task; sana2_receive_frame is the RX drain's delivery. */
u32 sana2_read(struct GenetUnit *unit, struct IOSana2Req *io);
u32 sana2_queue_read(struct GenetUnit *unit, struct IOSana2Req *io);
void sana2_receive_frame(struct GenetUnit *unit, u8 *frame, u32 length);
void sana2_flush(struct GenetUnit *unit, BOOL events, BYTE error, ULONG wireError);
void sana2_flush_opener(struct GenetUnit *unit, struct Opener *opener, BOOL events, BYTE error,
                        ULONG wireError);
void sana2_abort_kept(struct GenetUnit *unit, struct IOSana2Req *io);
void sana2_rx_service(struct GenetUnit *unit);

/* Events (sana2_api.c): reply each opener's oldest S2_ONEVENT that waits for
 * one of @events. Unit task. */
void sana2_report_events(struct GenetUnit *unit, u32 events);

/* Multicast (sana2_mcast.c), unit task. */
u32 sana2_add_multicast(struct GenetUnit *unit, struct IOSana2Req *io);
u32 sana2_del_multicast(struct GenetUnit *unit, struct IOSana2Req *io);
void sana2_filter_update(struct GenetUnit *unit);
BOOL sana2_multicast_wanted(const struct GenetS2 *s2, const u8 *address);
/* The last close, after the unit task stopped. */
void sana2_multicast_clear(struct GenetUnit *unit);

/* Statistics (sana2_stats.c), unit task. */
u32 sana2_get_global_stats(struct GenetUnit *unit, struct IOSana2Req *io);
u32 sana2_get_extended_stats(struct GenetUnit *unit, struct IOSana2Req *io);
u32 sana2_get_special_stats(struct GenetUnit *unit, struct IOSana2Req *io);
u32 sana2_sample_throughput(struct GenetUnit *unit, struct IOSana2Req *io);
void sana2_throughput_tick(struct GenetUnit *unit);
void sana2_throughput_restart(struct GenetUnit *unit);

/* TX (sana2_tx.c). sana2_write runs in the writer's task; the rest on the
 * unit task, except sana2_tx_backlog_remove (AbortIO, any task). */
u32 sana2_write(struct GenetUnit *unit, struct IOSana2Req *io);
u32 sana2_tx_staging_create(struct GenetUnit *unit);
void sana2_tx_staging_destroy(struct GenetUnit *unit);
void sana2_tx_drain(struct GenetUnit *unit);
void sana2_tx_backlog_abort(struct GenetUnit *unit, BYTE error, ULONG wireError);
void sana2_tx_backlog_abort_opener(struct GenetUnit *unit, struct Opener *opener, BYTE error,
                                   ULONG wireError);
BOOL sana2_tx_backlog_remove(struct GenetUnit *unit, struct IOSana2Req *io);

#endif
