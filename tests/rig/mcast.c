// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * mcast.c - GenetTest MCAST: the multicast filter beside a SANA-II stack,
 * while the PC's sender (tests/harness/mcast_send.ps1) puts datagrams on
 * 239.255.77.3, 239.255.77.17 and 239.255.78.1.
 */

#include "rig.h"
#include "../common/kv.h"

#include <proto/exec.h>

#include <devices/netdev.h>
#include <dos/dos.h>

#include <stdio.h>
#include <string.h>

#define READ_COUNT 8
#define NAME_BYTES 96
#define SETTLE_MICROS 300000UL /* frames the old filter passed, still being read */
#define DEFAULT_PHASE_SECONDS 6

/* The sender's groups and the Ethernet addresses they map to (RFC 1112). */
enum Target
{
    TARGET_LOW,  /* 239.255.77.3, in both test ranges */
    TARGET_HIGH, /* 239.255.77.17, only in the large range */
    TARGET_NONE, /* 239.255.78.1, in no range */
    TARGET_COUNT
};

static const char *const target_group[TARGET_COUNT] = {"239.255.77.3", "239.255.77.17", "239.255.78.1"};
static const UBYTE target_address[TARGET_COUNT][ETHER_ADDRESS_BYTES] = {
    {0x01, 0x00, 0x5e, 0x7f, 0x4d, 0x03},
    {0x01, 0x00, 0x5e, 0x7f, 0x4d, 0x11},
    {0x01, 0x00, 0x5e, 0x7f, 0x4e, 0x01},
};

struct AddressRange
{
    UBYTE lower[ETHER_ADDRESS_BYTES];
    UBYTE upper[ETHER_ADDRESS_BYTES];
};

/* 4 addresses fit the exact-match filter beside the stack's own groups;
 * 20 are more than its 15 slots, so the hardware passes everything. */
static const struct AddressRange small_range = {
    {0x01, 0x00, 0x5e, 0x7f, 0x4d, 0x01}, {0x01, 0x00, 0x5e, 0x7f, 0x4d, 0x04}};
static const struct AddressRange large_range = {
    {0x01, 0x00, 0x5e, 0x7f, 0x4d, 0x01}, {0x01, 0x00, 0x5e, 0x7f, 0x4d, 0x14}};

struct Phase
{
    const char *name;
    const struct AddressRange *range; /* this phase's range, NULL for none */
    BOOL delivered[TARGET_COUNT];     /* TRUE: frames arrive; FALSE: none may */
};

static const struct Phase phases[] = {
    {"exact filter", &small_range, {TRUE, FALSE, FALSE}},
    {"promiscuous filter", &large_range, {TRUE, TRUE, FALSE}},
    {"ranges removed", NULL, {FALSE, FALSE, FALSE}},
};

struct Reader
{
    struct MsgPort *port;
    struct IOSana2Req *requests[READ_COUNT];
    ULONG frames[TARGET_COUNT];
    ULONG flagErrors; /* a group frame without MCAST, or broadcast without BCAST */
    ULONG readErrors;
    BOOL counting;
};

static UBYTE read_buffers[READ_COUNT][FRAME_BUFFER_BYTES];

/**
 * reader_count() - Count a completed read: a frame for a target, a wrong
 * SANA2IOF_BCAST / SANA2IOF_MCAST flag, or a read error.
 * @reader: the reader.
 * @read: the completed read.
 */
static void reader_count(struct Reader *reader, const struct IOSana2Req *read)
{
    if (read->ios2_Req.io_Error != 0)
    {
        reader->readErrors++;
        return;
    }
    const UBYTE *destination = read->ios2_DstAddr;
    if (destination[0] & ETHER_GROUP_BIT)
    {
        static const UBYTE broadcast[ETHER_ADDRESS_BYTES] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        UBYTE want = memcmp(destination, broadcast, ETHER_ADDRESS_BYTES) == 0 ? SANA2IOF_BCAST : SANA2IOF_MCAST;
        if ((read->ios2_Req.io_Flags & (SANA2IOF_BCAST | SANA2IOF_MCAST)) != want)
            reader->flagErrors++;
    }
    for (int target = 0; target < TARGET_COUNT; target++)
        if (memcmp(destination, target_address[target], ETHER_ADDRESS_BYTES) == 0)
            reader->frames[target]++;
}

/**
 * reader_queue() - Send @read as an IPv4 CMD_READ into @buffer.
 * @read: the read request.
 * @buffer: its FRAME_BUFFER_BYTES buffer.
 */
static void reader_queue(struct IOSana2Req *read, UBYTE *buffer)
{
    read->ios2_Req.io_Command = CMD_READ;
    read->ios2_Req.io_Flags = 0;
    read->ios2_Req.io_Error = 0;
    read->ios2_PacketType = ETHERTYPE_IPV4;
    read->ios2_Data = buffer;
    SendIO((struct IORequest *)read);
}

/**
 * reader_start() - Make the reader's port and queue READ_COUNT reads.
 * @reader: the reader, cleared here.
 * @opened: the opened request, whose device, unit and opener the reads share.
 *
 * Return: TRUE when every read is queued; FALSE when a port or request could
 * not be made, with what was made left for reader_stop().
 */
static BOOL reader_start(struct Reader *reader, const struct IOSana2Req *opened)
{
    memset(reader, 0, sizeof(*reader));
    reader->port = CreateMsgPort();
    if (reader->port == NULL)
        return FALSE;
    for (int i = 0; i < READ_COUNT; i++)
    {
        struct IOSana2Req *read = (struct IOSana2Req *)CreateIORequest(reader->port, sizeof(struct IOSana2Req));
        if (read == NULL)
            return FALSE;
        read->ios2_Req.io_Device = opened->ios2_Req.io_Device;
        read->ios2_Req.io_Unit = opened->ios2_Req.io_Unit;
        read->ios2_BufferManagement = opened->ios2_BufferManagement;
        reader->requests[i] = read;
        reader_queue(read, read_buffers[i]);
    }
    return TRUE;
}

/**
 * reader_stop() - Take the reads back from the driver and free them, then the
 * port. Every read is in the driver: reader_listen() sends each again.
 * @reader: the reader, as reader_start() left it.
 */
static void reader_stop(struct Reader *reader)
{
    for (int i = 0; i < READ_COUNT; i++)
    {
        struct IOSana2Req *read = reader->requests[i];
        if (read == NULL)
            continue;
        rig_finish(read);
        DeleteIORequest((struct IORequest *)read);
    }
    if (reader->port != NULL)
        DeleteMsgPort(reader->port);
}

/**
 * reader_listen() - Serve the reads for @micros, counting each completed one
 * while the reader is counting, and send it again.
 * @reader: the reader.
 * @timer: an open timer.device request, idle.
 * @micros: how long, in microseconds.
 *
 * Return: TRUE when the time ran out; FALSE on Ctrl-C.
 */
static BOOL reader_listen(struct Reader *reader, struct timerequest *timer, ULONG micros)
{
    timer->tr_node.io_Command = TR_ADDREQUEST;
    timer->tr_time.tv_secs = micros / MICROS_PER_SECOND;
    timer->tr_time.tv_micro = micros % MICROS_PER_SECOND;
    SendIO((struct IORequest *)timer);

    ULONG read_signal = 1UL << reader->port->mp_SigBit;
    ULONG timer_signal = 1UL << timer->tr_node.io_Message.mn_ReplyPort->mp_SigBit;
    BOOL interrupted = FALSE;
    while (!interrupted && !CheckIO((struct IORequest *)timer))
    {
        ULONG signals = Wait(read_signal | timer_signal | SIGBREAKF_CTRL_C);
        interrupted = (signals & SIGBREAKF_CTRL_C) != 0;
        struct IOSana2Req *read;
        while ((read = (struct IOSana2Req *)GetMsg(reader->port)) != NULL)
        {
            if (reader->counting)
                reader_count(reader, read);
            reader_queue(read, read->ios2_Data);
        }
    }
    if (!CheckIO((struct IORequest *)timer))
        AbortIO((struct IORequest *)timer);
    WaitIO((struct IORequest *)timer);
    return !interrupted;
}

/**
 * range_command() - Send a multicast range command for @range, and check
 * that it succeeds.
 * @io: the opened request.
 * @command: S2_ADDMULTICASTADDRESSES or S2_DELMULTICASTADDRESSES.
 * @range: the address range.
 *
 * Return: TRUE when the command succeeded.
 */
static BOOL range_command(struct IOSana2Req *io, UWORD command, const struct AddressRange *range)
{
    char name[NAME_BYTES];
    memcpy(io->ios2_SrcAddr, range->lower, ETHER_ADDRESS_BYTES);
    memcpy(io->ios2_DstAddr, range->upper, ETHER_ADDRESS_BYTES);
    BYTE error = rig_command(io, command);
    snprintf(name, sizeof(name), "%s %02x..%02x",
             command == S2_ADDMULTICASTADDRESSES ? "S2_ADDMULTICASTADDRESSES" : "S2_DELMULTICASTADDRESSES",
             range->lower[ETHER_ADDRESS_BYTES - 1], range->upper[ETHER_ADDRESS_BYTES - 1]);
    return kv_check(error == 0, name, "error %d", error);
}

/**
 * phase_report() - Check each target's frames against what @phase expects.
 * @phase: the phase.
 * @reader: the reader, with the phase's counts.
 */
static void phase_report(const struct Phase *phase, const struct Reader *reader)
{
    char name[NAME_BYTES];
    for (int target = 0; target < TARGET_COUNT; target++)
    {
        BOOL delivered = reader->frames[target] != 0;
        snprintf(name, sizeof(name), "%s: %s %s", phase->name, target_group[target],
                 phase->delivered[target] ? "delivered" : "filtered");
        kv_check(delivered == phase->delivered[target], name, "%lu frames", reader->frames[target]);
    }
}

/**
 * phase_run() - Leave @previous, take @phase's range, then count what
 * arrives.
 * @io: the opened request.
 * @reader: the reader, its reads queued.
 * @timer: an open timer.device request, idle.
 * @previous: the range the last phase took, or NULL.
 * @phase: the phase.
 * @seconds: how long to count.
 *
 * What the old filter passed is read, uncounted, for SETTLE_MICROS first.
 *
 * Return: TRUE when the phase ran; FALSE on Ctrl-C or a refused range.
 */
static BOOL phase_run(struct IOSana2Req *io, struct Reader *reader, struct timerequest *timer,
                      const struct AddressRange *previous, const struct Phase *phase, ULONG seconds)
{
    if (previous != NULL && !range_command(io, S2_DELMULTICASTADDRESSES, previous))
        return FALSE;
    if (phase->range != NULL && !range_command(io, S2_ADDMULTICASTADDRESSES, phase->range))
        return FALSE;

    reader->counting = FALSE;
    if (!reader_listen(reader, timer, SETTLE_MICROS))
        return FALSE;
    memset(reader->frames, 0, sizeof(reader->frames));
    reader->counting = TRUE;
    if (!reader_listen(reader, timer, seconds * MICROS_PER_SECOND))
        return FALSE;
    phase_report(phase, reader);
    return TRUE;
}

/**
 * command_mcast() - GenetTest MCAST: a SANA-II stack holds the unit; the
 * filter goes through its phases while the PC's sender runs.
 * @rig: the rig.
 * @argument: each phase's length in seconds; 0 for DEFAULT_PHASE_SECONDS.
 *
 * No frame may come with a wrong cast flag or a read error.
 */
void command_mcast(struct Rig *rig, ULONG argument)
{
    ULONG seconds = argument != 0 ? argument : DEFAULT_PHASE_SECONDS;
    BYTE error = rig_attach_probe(rig->io);
    kv_check(error == NDERR_BUSY, "NETDEV_CMD_ATTACH refused while SANA-II holds the unit", "error %d, want %d",
             error, NDERR_BUSY);

    struct Reader reader;
    BOOL started = reader_start(&reader, rig->io);
    kv_check(started, "the reads are queued", "%d reads", READ_COUNT);

    const struct AddressRange *previous = NULL;
    for (size_t i = 0; started && i < sizeof(phases) / sizeof(phases[0]); i++)
    {
        if (!phase_run(rig->io, &reader, rig->timer, previous, &phases[i], seconds))
            break;
        previous = phases[i].range;
    }
    if (previous != NULL) /* stopped early: leave nothing behind */
        range_command(rig->io, S2_DELMULTICASTADDRESSES, previous);

    kv_check(reader.flagErrors == 0, "group frames flagged MCAST, broadcasts BCAST", "%lu wrong", reader.flagErrors);
    kv_check(reader.readErrors == 0, "reads complete without error", "%lu failed", reader.readErrors);
    reader_stop(&reader);
}
