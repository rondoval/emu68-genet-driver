// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * s2check - checks of genet.device's SANA-II personality for the bench rounds
 * (emu68-driver-stack/scripts/bench). Built, never installed.
 *
 *   s2check netdev        A netdev stack holds the unit: the queries answer,
 *                         everything that would take the unit is refused with
 *                         S2ERR_OUTOFSERVICE, a read never queues, and a
 *                         second NETDEV_CMD_ATTACH is refused.
 *   s2check mcast [secs]  A SANA-II stack holds the unit: NETDEV_CMD_ATTACH is
 *                         refused, then the multicast filter through three
 *                         phases of secs seconds each, while a sender on the
 *                         LAN puts datagrams on 239.255.77.3, 239.255.77.17
 *                         and 239.255.78.1 (mcast_send.ps1).
 *
 * One "ok" or "FAIL" line per check, then a summary. Return code 0 when every
 * check passed, 10 when one failed, 20 for bad arguments or when the device
 * or a resource did not open.
 */

#include <proto/exec.h>

#include <exec/errors.h>
#include <exec/io.h>
#include <exec/types.h>
#include <devices/netdev.h>
#include <devices/sana2.h>
#include <devices/timer.h>
#include <dos/dos.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char verstag[] __attribute__((used)) = VERSTAG;

#define GENET_DEVICE "genet.device"
#define GENET_UNIT 0
#define ETHER_ADDRESS_BYTES 6
#define ETHERTYPE_IPV4 0x0800
#define ETHER_GROUP_BIT 0x01
#define EXPECTED_MTU 1500
#define FRAME_BUFFER_BYTES 1536 /* a read's payload: one MTU and some */
#define WRITE_PAYLOAD_BYTES 64
#define READ_COUNT 8
#define MAX_SPECIAL_STATS 64
#define DEFAULT_PHASE_SECONDS 6
#define MICROS_PER_SECOND 1000000UL
#define SETTLE_MICROS 300000UL    /* frames the old filter passed, still being read */
#define READ_WAIT_MICROS 2000000UL /* a refused read comes back at once */

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
static UBYTE write_payload[WRITE_PAYLOAD_BYTES];
static ULONG checks_run;
static ULONG checks_failed;

/**
 * copy_to_buff() - The opener's S2_CopyToBuff: copy a received frame into a
 * read's buffer.
 * @to: the read's buffer (ios2_Data).
 * @from: the frame.
 * @length: the bytes to copy.
 *
 * Return: TRUE when copied; FALSE when @length exceeds FRAME_BUFFER_BYTES.
 */
static BOOL copy_to_buff(APTR to asm("a0"), APTR from asm("a1"), ULONG length asm("d0"))
{
    if (length > FRAME_BUFFER_BYTES)
        return FALSE;
    memcpy(to, from, length);
    return TRUE;
}

/**
 * copy_from_buff() - The opener's S2_CopyFromBuff: copy a write's data into
 * the driver's frame.
 * @to: the driver's frame.
 * @from: the write's buffer (ios2_Data).
 * @length: the bytes to copy.
 *
 * Return: TRUE.
 */
static BOOL copy_from_buff(APTR to asm("a0"), APTR from asm("a1"), ULONG length asm("d0"))
{
    memcpy(to, from, length);
    return TRUE;
}

static struct TagItem buffer_tags[] = {
    {S2_CopyToBuff, (ULONG)copy_to_buff},
    {S2_CopyFromBuff, (ULONG)copy_from_buff},
    {TAG_DONE, 0},
};

static struct TagItem no_buffer_tags[] = {{TAG_DONE, 0}};

/**
 * check() - Print one "ok" or "FAIL" line and count the check.
 * @passed: the check's outcome.
 * @format: the printf() format of the line's text.
 * @...: its arguments.
 */
static void check(BOOL passed, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    printf("%s ", passed ? "ok  " : "FAIL");
    vprintf(format, arguments);
    printf("\n");
    va_end(arguments);
    checks_run++;
    if (!passed)
        checks_failed++;
}

/**
 * run_command() - Run @command on @io with DoIO(), its flags and errors
 * cleared first.
 * @io: the opened request, its other fields set for @command.
 * @command: the io_Command.
 *
 * Return: the io_Error.
 */
static BYTE run_command(struct IOSana2Req *io, UWORD command)
{
    io->ios2_Req.io_Command = command;
    io->ios2_Req.io_Flags = 0;
    io->ios2_Req.io_Error = 0;
    io->ios2_WireError = 0;
    DoIO((struct IORequest *)io);
    return io->ios2_Req.io_Error;
}

/**
 * wait_with_timeout() - Send @io and wait up to @micros for it.
 * @io: the request, ready to send.
 * @timer: an open timer.device request, idle.
 * @micros: the wait in microseconds.
 *
 * Both requests are idle again on return.
 *
 * Return: TRUE when @io completed in time; FALSE (and @io aborted) on
 * timeout.
 */
static BOOL wait_with_timeout(struct IORequest *io, struct timerequest *timer, ULONG micros)
{
    timer->tr_node.io_Command = TR_ADDREQUEST;
    timer->tr_time.tv_secs = micros / MICROS_PER_SECOND;
    timer->tr_time.tv_micro = micros % MICROS_PER_SECOND;
    SendIO((struct IORequest *)timer);
    SendIO(io);

    ULONG io_signal = 1UL << io->io_Message.mn_ReplyPort->mp_SigBit;
    ULONG timer_signal = 1UL << timer->tr_node.io_Message.mn_ReplyPort->mp_SigBit;
    while (!CheckIO(io) && !CheckIO((struct IORequest *)timer))
        Wait(io_signal | timer_signal);

    BOOL completed = CheckIO(io) != NULL;
    if (!completed)
        AbortIO(io);
    WaitIO(io);
    if (!CheckIO((struct IORequest *)timer))
        AbortIO((struct IORequest *)timer);
    WaitIO((struct IORequest *)timer);
    return completed;
}

/* ------------------------------------------------------------- queries --- */

/**
 * check_device_query() - Check that S2_DEVICEQUERY answers: Ethernet, an MTU
 * of EXPECTED_MTU.
 * @io: the opened request.
 */
static void check_device_query(struct IOSana2Req *io)
{
    struct Sana2DeviceQuery query;
    memset(&query, 0, sizeof(query));
    query.SizeAvailable = sizeof(query);
    io->ios2_StatData = &query;
    BYTE error = run_command(io, S2_DEVICEQUERY);
    check(error == 0 && query.HardwareType == S2WireType_Ethernet && query.MTU == EXPECTED_MTU,
          "S2_DEVICEQUERY answers (error %d, MTU %lu, %lu bit/s)", error, query.MTU, query.BPS);
}

/**
 * check_station_address() - Check that S2_GETSTATIONADDRESS answers with a
 * unicast address in use whose first three bytes are not all zero.
 * @io: the opened request.
 */
static void check_station_address(struct IOSana2Req *io)
{
    BYTE error = run_command(io, S2_GETSTATIONADDRESS);
    const UBYTE *address = io->ios2_SrcAddr;
    BOOL unicast = (address[0] & ETHER_GROUP_BIT) == 0 && (address[0] | address[1] | address[2]) != 0;
    check(error == 0 && unicast, "S2_GETSTATIONADDRESS answers (error %d, %02x:%02x:%02x:%02x:%02x:%02x)",
          error, address[0], address[1], address[2], address[3], address[4], address[5]);
}

/**
 * check_statistics() - Check that S2_GETGLOBALSTATS answers with frames
 * received, and S2_GETSPECIALSTATS with records.
 * @io: the opened request.
 */
static void check_statistics(struct IOSana2Req *io)
{
    struct Sana2DeviceStats stats;
    memset(&stats, 0, sizeof(stats));
    io->ios2_StatData = &stats;
    BYTE error = run_command(io, S2_GETGLOBALSTATS);
    check(error == 0 && stats.PacketsReceived != 0,
          "S2_GETGLOBALSTATS answers (error %d, %lu frames received, %lu sent)", error,
          stats.PacketsReceived, stats.PacketsSent);

    static struct
    {
        struct Sana2SpecialStatHeader header;
        struct Sana2SpecialStatRecord records[MAX_SPECIAL_STATS];
    } special;
    memset(&special, 0, sizeof(special));
    special.header.RecordCountMax = MAX_SPECIAL_STATS;
    io->ios2_StatData = &special;
    error = run_command(io, S2_GETSPECIALSTATS);
    check(error == 0 && special.header.RecordCountSupplied != 0,
          "S2_GETSPECIALSTATS answers (error %d, %lu records)", error,
          special.header.RecordCountSupplied);
}

/**
 * check_queries() - Check what genet-stats asks: it must answer beside either
 * stack.
 * @io: the opened request.
 */
static void check_queries(struct IOSana2Req *io)
{
    check_device_query(io);
    check_station_address(io);
    check_statistics(io);
}

/**
 * check_attach_refused() - Check that NETDEV_CMD_ATTACH is refused with
 * NDERR_BUSY while a stack holds the unit.
 * @io: the opened request.
 * @holder: who holds the unit, for the line.
 *
 * NETDEV_CMD_ATTACH as a netdev stack sends it, but with ABI version 0: a
 * held unit answers NDERR_BUSY before it looks at the version, an unheld one
 * NDERR_BADVERSION, so nothing is ever attached.
 */
static void check_attach_refused(struct IOSana2Req *io, const char *holder)
{
    static const struct NetDevStackOps no_operations;
    struct NetDevAttach attach;
    memset(&attach, 0, sizeof(attach));
    attach.nda_StackOps = &no_operations;
    struct IOStdReq *standard = (struct IOStdReq *)io; /* netdev commands' view of the request */
    standard->io_Data = &attach;
    standard->io_Length = sizeof(attach);
    BYTE error = run_command(io, NETDEV_CMD_ATTACH);
    check(error == NDERR_BUSY, "NETDEV_CMD_ATTACH refused while %s holds the unit (error %d, want %d)",
          holder, error, NDERR_BUSY);
}

/* ------------------------------------------------------- netdev holds it --- */

/**
 * check_refused() - Check that @command is refused with S2ERR_OUTOFSERVICE
 * while netdev holds the unit.
 * @io: the opened request, its other fields set for @command.
 * @command: the command.
 * @name: its name, for the line.
 */
static void check_refused(struct IOSana2Req *io, UWORD command, const char *name)
{
    BYTE error = run_command(io, command);
    check(error == S2ERR_OUTOFSERVICE, "%s refused while netdev holds the unit (error %d, want %d)",
          name, error, S2ERR_OUTOFSERVICE);
}

/**
 * check_read_refused() - Check that a CMD_READ is answered at once with
 * S2ERR_OUTOFSERVICE while netdev holds the unit, not queued.
 * @io: the opened request.
 * @timer: an open timer.device request, idle, for the READ_WAIT_MICROS wait.
 */
static void check_read_refused(struct IOSana2Req *io, struct timerequest *timer)
{
    io->ios2_Req.io_Command = CMD_READ;
    io->ios2_Req.io_Flags = 0;
    io->ios2_Req.io_Error = 0;
    io->ios2_PacketType = ETHERTYPE_IPV4;
    io->ios2_Data = read_buffers[0];
    BOOL answered = wait_with_timeout((struct IORequest *)io, timer, READ_WAIT_MICROS);
    check(answered && io->ios2_Req.io_Error == S2ERR_OUTOFSERVICE,
          "CMD_READ refused at once while netdev holds the unit (%s, error %d)",
          answered ? "answered" : "queued", io->ios2_Req.io_Error);
}

/**
 * check_netdev_mode() - The checks of "s2check netdev": a netdev stack holds
 * the unit.
 * @io: the opened request.
 * @timer: an open timer.device request, idle.
 *
 * The queries answer, everything that would take the unit is refused with
 * S2ERR_OUTOFSERVICE, a read never queues, and a second NETDEV_CMD_ATTACH is
 * refused.
 */
static void check_netdev_mode(struct IOSana2Req *io, struct timerequest *timer)
{
    check_queries(io);

    memset(io->ios2_SrcAddr, 0, sizeof(io->ios2_SrcAddr)); /* the factory address */
    check_refused(io, S2_CONFIGINTERFACE, "S2_CONFIGINTERFACE");
    check_refused(io, S2_ONLINE, "S2_ONLINE");
    check_refused(io, S2_OFFLINE, "S2_OFFLINE");

    memcpy(io->ios2_SrcAddr, small_range.lower, ETHER_ADDRESS_BYTES);
    check_refused(io, S2_ADDMULTICASTADDRESS, "S2_ADDMULTICASTADDRESS");

    run_command(io, S2_GETSTATIONADDRESS);
    memcpy(io->ios2_DstAddr, io->ios2_SrcAddr, ETHER_ADDRESS_BYTES); /* to itself, if it ever went */
    io->ios2_PacketType = ETHERTYPE_IPV4;
    io->ios2_Data = write_payload;
    io->ios2_DataLength = WRITE_PAYLOAD_BYTES;
    check_refused(io, CMD_WRITE, "CMD_WRITE");

    check_read_refused(io, timer);
    check_attach_refused(io, "a netdev stack");
}

/* --------------------------------------------------------- multicast --- */

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
 * reader_stop() - Abort and free the reads, then the port.
 * @reader: the reader, as reader_start() left it.
 */
static void reader_stop(struct Reader *reader)
{
    for (int i = 0; i < READ_COUNT; i++)
    {
        struct IOSana2Req *read = reader->requests[i];
        if (read == NULL)
            continue;
        if (!CheckIO((struct IORequest *)read))
            AbortIO((struct IORequest *)read);
        WaitIO((struct IORequest *)read);
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
    memcpy(io->ios2_SrcAddr, range->lower, ETHER_ADDRESS_BYTES);
    memcpy(io->ios2_DstAddr, range->upper, ETHER_ADDRESS_BYTES);
    BYTE error = run_command(io, command);
    check(error == 0, "%s %02x..%02x (error %d)",
          command == S2_ADDMULTICASTADDRESSES ? "S2_ADDMULTICASTADDRESSES" : "S2_DELMULTICASTADDRESSES",
          range->lower[ETHER_ADDRESS_BYTES - 1], range->upper[ETHER_ADDRESS_BYTES - 1], error);
    return error == 0;
}

/**
 * phase_report() - Check each target's frames against what @phase expects.
 * @phase: the phase.
 * @reader: the reader, with the phase's counts.
 */
static void phase_report(const struct Phase *phase, const struct Reader *reader)
{
    for (int target = 0; target < TARGET_COUNT; target++)
    {
        BOOL delivered = reader->frames[target] != 0;
        check(delivered == phase->delivered[target], "%s: %s %s (%lu frames)", phase->name,
              target_group[target], phase->delivered[target] ? "delivered" : "filtered",
              reader->frames[target]);
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
 * check_mcast_mode() - The checks of "s2check mcast": a SANA-II stack holds
 * the unit.
 * @io: the opened request.
 * @timer: an open timer.device request, idle.
 * @seconds: each phase's length.
 *
 * NETDEV_CMD_ATTACH is refused, then the multicast filter goes through the
 * phases while a sender on the LAN puts datagrams on the target groups; no
 * frame may come with a wrong cast flag or a read error.
 */
static void check_mcast_mode(struct IOSana2Req *io, struct timerequest *timer, ULONG seconds)
{
    check_queries(io);
    check_attach_refused(io, "SANA-II");

    struct Reader reader;
    BOOL started = reader_start(&reader, io);
    check(started, "%d reads queued", READ_COUNT);

    const struct AddressRange *previous = NULL;
    for (size_t i = 0; started && i < sizeof(phases) / sizeof(phases[0]); i++)
    {
        if (!phase_run(io, &reader, timer, previous, &phases[i], seconds))
            break;
        previous = phases[i].range;
    }
    if (previous != NULL) /* stopped early: leave nothing behind */
        range_command(io, S2_DELMULTICASTADDRESSES, previous);

    check(reader.flagErrors == 0, "group frames flagged MCAST, broadcasts BCAST (%lu wrong)", reader.flagErrors);
    check(reader.readErrors == 0, "reads completed without error (%lu failed)", reader.readErrors);
    reader_stop(&reader);
}

/* ---------------------------------------------------------------- main --- */

/**
 * check_bare_open() - Check that an opener with no buffer tags opens, as
 * genet-stats opens.
 * @port: the reply port for its request.
 */
static void check_bare_open(struct MsgPort *port)
{
    struct IOSana2Req *io = (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req));
    if (io == NULL)
        return;
    io->ios2_BufferManagement = no_buffer_tags;
    BYTE error = OpenDevice((CONST_STRPTR)GENET_DEVICE, GENET_UNIT, (struct IORequest *)io, 0);
    check(error == 0, "opens with no buffer tags, as genet-stats does (error %d)", error);
    if (error == 0)
        CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
}

/**
 * usage() - Print the usage line.
 *
 * Return: RETURN_FAIL.
 */
static int usage(void)
{
    printf("usage: s2check netdev | mcast [seconds]\n");
    return RETURN_FAIL;
}

/**
 * main() - Run the checks of the mode the command line names.
 * @argc: the argument count.
 * @argv: "netdev", or "mcast" and an optional phase length in seconds.
 *
 * Return: RETURN_OK when every check passed, RETURN_ERROR when one failed,
 * RETURN_FAIL for a usage error or when the device or a resource did not
 * open.
 */
int main(int argc, char **argv)
{
    BOOL netdev_mode = argc > 1 && strcmp(argv[1], "netdev") == 0;
    BOOL mcast_mode = argc > 1 && strcmp(argv[1], "mcast") == 0;
    ULONG seconds = argc > 2 ? strtoul(argv[2], NULL, 10) : DEFAULT_PHASE_SECONDS;
    if ((!netdev_mode && !mcast_mode) || seconds == 0)
        return usage();

    int result = RETURN_FAIL;
    struct MsgPort *port = CreateMsgPort();
    struct MsgPort *timer_port = CreateMsgPort();
    struct IOSana2Req *io = port ? (struct IOSana2Req *)CreateIORequest(port, sizeof(*io)) : NULL;
    struct timerequest *timer = timer_port ? (struct timerequest *)CreateIORequest(timer_port, sizeof(*timer)) : NULL;
    if (io == NULL || timer == NULL || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)timer, 0) != 0)
    {
        printf("s2check: no ports, requests or timer.device\n");
        goto cleanup;
    }

    io->ios2_BufferManagement = buffer_tags;
    BYTE error = OpenDevice((CONST_STRPTR)GENET_DEVICE, GENET_UNIT, (struct IORequest *)io, 0);
    if (error != 0)
    {
        printf("s2check: %s unit %d does not open (error %d)\n", GENET_DEVICE, GENET_UNIT, error);
        CloseDevice((struct IORequest *)timer);
        goto cleanup;
    }

    if (netdev_mode)
    {
        check_bare_open(port);
        check_netdev_mode(io, timer);
    }
    else
        check_mcast_mode(io, timer, seconds);

    CloseDevice((struct IORequest *)io);
    CloseDevice((struct IORequest *)timer);
    printf("%lu checks, %lu failed\n", checks_run, checks_failed);
    result = checks_failed == 0 ? RETURN_OK : RETURN_ERROR;

cleanup:
    if (timer != NULL)
        DeleteIORequest((struct IORequest *)timer);
    if (io != NULL)
        DeleteIORequest((struct IORequest *)io);
    if (timer_port != NULL)
        DeleteMsgPort(timer_port);
    if (port != NULL)
        DeleteMsgPort(port);
    return result;
}
