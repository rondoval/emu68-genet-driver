// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * claim.c - GenetTest NETDEV, SANA2, BAREOPEN and CLOSE: the claim rules, the
 * refusals, and what opening and closing may and may not do.
 */

#include "rig.h"
#include <emu68test/kv.h>

#include <proto/exec.h>

#include <exec/errors.h>
#include <devices/netdev.h>
#include <devices/newstyle.h>

#include <stdio.h>
#include <string.h>

#define NAME_BYTES 96
#define WRITE_PAYLOAD_BYTES 64
#define READ_WAIT_MICROS 2000000UL  /* a refused read comes back at once */
#define READ_HOLD_MICROS 300000UL   /* a read for a type nobody sends waits */
#define OTHER_UNIT 1

static const UBYTE probe_group[ETHER_ADDRESS_BYTES] = {0x01, 0x00, 0x5e, 0x7f, 0x7f, 0x7f};
static const UBYTE broadcast[ETHER_ADDRESS_BYTES] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static UBYTE payload[FRAME_BUFFER_BYTES];

/* A command that would take the unit or its datapath, refused while netdev holds it. */
struct Refusal
{
    UWORD command;
    const char *name;
    BOOL write;   /* a write: its frame set up first */
    BOOL group;   /* addresses: the probe group (ranges from it to itself) */
};

static const struct Refusal refusals[] = {
    {S2_CONFIGINTERFACE, "S2_CONFIGINTERFACE", FALSE, FALSE},
    {S2_ONLINE, "S2_ONLINE", FALSE, FALSE},
    {S2_OFFLINE, "S2_OFFLINE", FALSE, FALSE},
    {S2_ADDMULTICASTADDRESS, "S2_ADDMULTICASTADDRESS", FALSE, TRUE},
    {S2_DELMULTICASTADDRESS, "S2_DELMULTICASTADDRESS", FALSE, TRUE},
    {S2_ADDMULTICASTADDRESSES, "S2_ADDMULTICASTADDRESSES", FALSE, TRUE},
    {S2_DELMULTICASTADDRESSES, "S2_DELMULTICASTADDRESSES", FALSE, TRUE},
    {CMD_WRITE, "CMD_WRITE", TRUE, FALSE},
    {S2_BROADCAST, "S2_BROADCAST", TRUE, FALSE},
    {S2_MULTICAST, "S2_MULTICAST", TRUE, TRUE},
};

/**
 * check_refusal() - Check that one command is refused with S2ERR_OUTOFSERVICE
 * while netdev holds the unit.
 * @io: the opened request.
 * @refusal: the command and how to set it up.
 * @own: the unit's own address, a write's destination.
 */
static void check_refusal(struct IOSana2Req *io, const struct Refusal *refusal, const UBYTE *own)
{
    char name[NAME_BYTES];
    memset(io->ios2_SrcAddr, 0, sizeof(io->ios2_SrcAddr)); /* S2_CONFIGINTERFACE: the factory address */
    if (refusal->write)
        rig_write_setup(io, refusal->group ? probe_group : own, payload, WRITE_PAYLOAD_BYTES);
    else if (refusal->group)
    {
        memcpy(io->ios2_SrcAddr, probe_group, ETHER_ADDRESS_BYTES);
        memcpy(io->ios2_DstAddr, probe_group, ETHER_ADDRESS_BYTES);
    }
    BYTE error = rig_command(io, refusal->command);
    snprintf(name, sizeof(name), "%s refused beside netdev", refusal->name);
    kv_check(error == S2ERR_OUTOFSERVICE, name, "error %d, want %d", error, S2ERR_OUTOFSERVICE);
}

/**
 * check_read_refused() - Check that a read is answered at once with
 * S2ERR_OUTOFSERVICE while netdev holds the unit, not queued.
 * @rig: the rig.
 * @command: CMD_READ or S2_READORPHAN.
 * @name: its name, for the check.
 */
static void check_read_refused(struct Rig *rig, UWORD command, const char *name)
{
    char check[NAME_BYTES];
    struct IOSana2Req *io = rig->io;
    io->ios2_Req.io_Command = command;
    io->ios2_Req.io_Flags = 0;
    io->ios2_Req.io_Error = 0;
    io->ios2_PacketType = ETHERTYPE_IPV4;
    io->ios2_Data = payload;
    BOOL answered = rig_wait((struct IORequest *)io, rig->timer, READ_WAIT_MICROS);
    snprintf(check, sizeof(check), "%s refused at once beside netdev", name);
    kv_check(answered && io->ios2_Req.io_Error == S2ERR_OUTOFSERVICE, check, "%s, error %d",
             answered ? "answered" : "queued", io->ios2_Req.io_Error);
}

/**
 * check_event_offline() - Check that S2_ONEVENT for S2EVENT_OFFLINE answers
 * at once: beside netdev the unit is offline for SANA-II.
 * @rig: the rig.
 */
static void check_event_offline(struct Rig *rig)
{
    ULONG events = rig_event_now(rig, S2EVENT_OFFLINE);
    kv_check((events & S2EVENT_OFFLINE) != 0, "S2_ONEVENT sees the unit offline beside netdev", "events 0x%lx",
             events);
}

/**
 * command_netdev() - GenetTest NETDEV: a netdev stack holds the unit.
 * @rig: the rig.
 * @argument: unused.
 *
 * Everything that would take the unit or its datapath is refused with
 * S2ERR_OUTOFSERVICE, reads never queue, the unit is offline for SANA-II, and
 * a second NETDEV_CMD_ATTACH is refused.
 */
void command_netdev(struct Rig *rig, ULONG argument)
{
    (void)argument;
    UBYTE own[ETHER_ADDRESS_BYTES];
    rig_station_address(rig->io, own);
    for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++)
        check_refusal(rig->io, &refusals[i], own);
    check_read_refused(rig, CMD_READ, "CMD_READ");
    check_read_refused(rig, S2_READORPHAN, "S2_READORPHAN");
    check_event_offline(rig);
    BYTE error = rig_attach_probe(rig->io);
    kv_check(error == NDERR_BUSY, "NETDEV_CMD_ATTACH refused while netdev holds the unit", "error %d, want %d",
             error, NDERR_BUSY);
}

/**
 * check_write() - Check that one write goes out while SANA-II holds the unit.
 * @io: the opened request.
 * @command: CMD_WRITE, S2_BROADCAST or S2_MULTICAST.
 * @name: its name, for the check.
 * @destination: the frame's destination.
 */
static void check_write(struct IOSana2Req *io, UWORD command, const char *name, const UBYTE *destination)
{
    char check[NAME_BYTES];
    rig_write_setup(io, destination, payload, WRITE_PAYLOAD_BYTES);
    BYTE error = rig_command(io, command);
    snprintf(check, sizeof(check), "%s goes out beside SANA-II", name);
    kv_check(error == 0, check, "error %d, wire error %lu", error, io->ios2_WireError);
}

/**
 * check_read_waits() - Check that a read for a type nobody sends waits.
 * @rig: the rig.
 */
static void check_read_waits(struct Rig *rig)
{
    struct IOSana2Req *io = rig->io;
    io->ios2_Req.io_Command = CMD_READ;
    io->ios2_Req.io_Flags = 0;
    io->ios2_Req.io_Error = 0;
    io->ios2_PacketType = ETHERTYPE_UNUSED;
    io->ios2_Data = payload;
    BOOL answered = rig_wait((struct IORequest *)io, rig->timer, READ_HOLD_MICROS);
    if (answered && io->ios2_Req.io_Error == 0)
        rig_note_frame(io, "the read for nobody's type");
    kv_check(!answered && io->ios2_Req.io_Error == IOERR_ABORTED, "a read waits for its frame beside SANA-II",
             "%s, error %d", answered ? "answered" : "waited, then aborted", io->ios2_Req.io_Error);
}

/**
 * command_sana2() - GenetTest SANA2: a SANA-II stack holds the unit online.
 * @rig: the rig.
 * @argument: unused.
 *
 * NETDEV_CMD_ATTACH is refused, the rig's own writes go out beside the
 * stack's, a kept read shows as pending to CheckIO (rig_read_marked_pending(),
 * first, since WaitIO() on one that does not corrupts memory) and waits for its
 * frame, and the unit is online.
 */
void command_sana2(struct Rig *rig, ULONG argument)
{
    (void)argument;
    UBYTE own[ETHER_ADDRESS_BYTES];
    rig_station_address(rig->io, own);
    BYTE error = rig_attach_probe(rig->io);
    kv_check(error == NDERR_BUSY, "NETDEV_CMD_ATTACH refused while SANA-II holds the unit", "error %d, want %d",
             error, NDERR_BUSY);
    check_write(rig->io, CMD_WRITE, "CMD_WRITE", own);
    check_write(rig->io, S2_BROADCAST, "S2_BROADCAST", broadcast);
    check_write(rig->io, S2_MULTICAST, "S2_MULTICAST", probe_group);
    if (kv_check(rig_read_marked_pending(rig), "a kept read shows as pending to CheckIO",
                 "WaitIO on one that does not corrupts memory"))
        check_read_waits(rig);
    ULONG events = rig_event_now(rig, S2EVENT_ONLINE);
    kv_check((events & S2EVENT_ONLINE) != 0, "S2_ONEVENT sees the unit online beside SANA-II", "events 0x%lx",
             events);
}

/**
 * open_close() - Open genet.device with @io and close it again.
 * @io: the request, its opener fields set.
 * @unit: the unit to open.
 * @flags: the open flags.
 *
 * Return: the OpenDevice() error; closed again when 0.
 */
static BYTE open_close(struct IORequest *io, ULONG unit, ULONG flags)
{
    BYTE error = OpenDevice((CONST_STRPTR)GENET_DEVICE, unit, io, flags);
    if (error == 0)
        CloseDevice(io);
    return error;
}

/**
 * check_open() - Open with a request of @size and the given opener fields,
 * and check the outcome.
 * @port: the reply port.
 * @size: the request's size.
 * @tags: ios2_BufferManagement for an IOSana2Req-sized request.
 * @unit: the unit.
 * @flags: the open flags.
 * @want: the OpenDevice() error the check wants.
 * @name: the check's name.
 */
static void check_open(struct MsgPort *port, ULONG size, APTR tags, ULONG unit, ULONG flags, BYTE want,
                       const char *name)
{
    struct IORequest *io = CreateIORequest(port, size);
    if (io == NULL)
    {
        kv_environment("no memory for a request");
        return;
    }
    if (size >= sizeof(struct IOSana2Req))
        ((struct IOSana2Req *)io)->ios2_BufferManagement = tags;
    BYTE error = open_close(io, unit, flags);
    kv_check(error == want && (error == 0 || io->io_Device == NULL), name, "error %d, want %d", error, want);
    DeleteIORequest(io);
}

/**
 * command_bareopen() - GenetTest BAREOPEN: the opens the driver must take and
 * the ones it must refuse.
 * @rig: the rig, whose own open keeps the unit open throughout.
 * @argument: unused.
 */
void command_bareopen(struct Rig *rig, ULONG argument)
{
    (void)argument;
    static struct TagItem no_tags[] = {{TAG_DONE, 0}};
    check_open(rig->port, sizeof(struct IOSana2Req), no_tags, GENET_UNIT, 0, 0,
               "opens with no buffer tags, as genet-stats does");
    check_open(rig->port, sizeof(struct IOSana2Req), NULL, GENET_UNIT, 0, 0, "opens with no tag list at all");
    check_open(rig->port, sizeof(struct IOStdReq), NULL, GENET_UNIT, 0, 0,
               "opens with a plain IOStdReq, as lwip-amiga 1.0 to 1.3 do");
    check_open(rig->port, sizeof(struct IORequest), NULL, GENET_UNIT, 0, IOERR_OPENFAIL,
               "refuses a request shorter than an IOStdReq");
    check_open(rig->port, sizeof(struct IOSana2Req), no_tags, OTHER_UNIT, 0, IOERR_OPENFAIL,
               "refuses a unit it does not have");
    check_open(rig->port, sizeof(struct IOSana2Req), no_tags, GENET_UNIT, SANA2OPF_MINE, IOERR_UNITBUSY,
               "refuses an exclusive open while others have it open");
}

/**
 * close_with_cookie() - Open a second opener, put @cookie in its
 * ios2_BufferManagement, and close it.
 * @port: the reply port.
 * @cookie: what the closing request carries instead of the driver's Opener.
 *
 * The driver must leave whatever it does not know alone; the Opener it
 * handed out stays behind, inert.
 *
 * Return: the OpenDevice() error; the close ran when 0.
 */
static BYTE close_with_cookie(struct MsgPort *port, APTR cookie)
{
    struct IOSana2Req *io = (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req));
    if (io == NULL)
        return IOERR_NOCMD;
    io->ios2_BufferManagement = rig_buffer_tags();
    BYTE error = OpenDevice((CONST_STRPTR)GENET_DEVICE, GENET_UNIT, (struct IORequest *)io, 0);
    if (error == 0)
    {
        io->ios2_BufferManagement = cookie;
        CloseDevice((struct IORequest *)io);
    }
    DeleteIORequest((struct IORequest *)io);
    return error;
}

/**
 * command_close() - GenetTest CLOSE: a close whose ios2_BufferManagement is
 * not the driver's Opener (a stack that broke netdev.h's open rule) is left
 * alone, and the unit goes on serving.
 * @rig: the rig.
 * @argument: unused.
 */
void command_close(struct Rig *rig, ULONG argument)
{
    (void)argument;
    static ULONG foreign[FRAME_BUFFER_BYTES / sizeof(ULONG)]; /* any memory that is no Opener */
    BYTE error = close_with_cookie(rig->port, foreign);
    kv_check(error == 0, "a close carrying a foreign pointer returns", "open error %d", error);
    error = close_with_cookie(rig->port, NULL);
    kv_check(error == 0, "a close carrying NULL returns", "open error %d", error);
    kv_note("two Openers stay behind, inert, until the unit's last close");

    struct Sana2DeviceQuery query;
    memset(&query, 0, sizeof(query));
    query.SizeAvailable = sizeof(query);
    rig->io->ios2_StatData = &query;
    error = rig_command(rig->io, S2_DEVICEQUERY);
    kv_check(error == 0 && query.MTU == EXPECTED_MTU, "the unit still answers after them", "error %d", error);
    check_open(rig->port, sizeof(struct IOSana2Req), rig_buffer_tags(), GENET_UNIT, 0, 0,
               "a new opener opens and closes after them");
}
