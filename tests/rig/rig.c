// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * rig.c - GenetTest's request helpers and the opener's copy hooks (rig.h).
 */

#include "rig.h"
#include "../common/kv.h"

#include <proto/exec.h>

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/netdev.h>

#include <ctype.h>
#include <string.h>

struct CopyLog rig_copy_log;

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
    rig_copy_log.toBuffCalls++;
    rig_copy_log.lastToBuffLength = length;
    if (length > FRAME_BUFFER_BYTES)
        return FALSE;
    memcpy(to, from, length);
    return TRUE;
}

/**
 * copy_from_buff() - The opener's S2_CopyFromBuff: copy a write's data into
 * the driver's frame.
 * @to: the driver's frame.
 * @from: the write's buffer (ios2_Data), FRAME_BUFFER_BYTES long.
 * @length: the bytes to copy.
 *
 * Return: TRUE when copied; FALSE when @length exceeds FRAME_BUFFER_BYTES.
 */
static BOOL copy_from_buff(APTR to asm("a0"), APTR from asm("a1"), ULONG length asm("d0"))
{
    rig_copy_log.fromBuffCalls++;
    rig_copy_log.lastFromBuffLength = length;
    if (length > FRAME_BUFFER_BYTES)
        return FALSE;
    memcpy(to, from, length);
    return TRUE;
}

static struct TagItem buffer_tags[] = {
    {S2_CopyToBuff, (ULONG)copy_to_buff},
    {S2_CopyFromBuff, (ULONG)copy_from_buff},
    {TAG_DONE, 0},
};

/**
 * rig_buffer_tags() - The rig opener's buffer-management tags.
 *
 * Return: the tag list, for ios2_BufferManagement before OpenDevice().
 */
struct TagItem *rig_buffer_tags(void)
{
    return buffer_tags;
}

/**
 * rig_command() - Run @command on @io with DoIO(), its flags and errors
 * cleared first.
 * @io: an opened request, its other fields set for @command.
 * @command: the io_Command.
 *
 * Return: the io_Error.
 */
BYTE rig_command(struct IOSana2Req *io, UWORD command)
{
    io->ios2_Req.io_Command = command;
    io->ios2_Req.io_Flags = 0;
    io->ios2_Req.io_Error = 0;
    io->ios2_WireError = 0;
    DoIO((struct IORequest *)io);
    return io->ios2_Req.io_Error;
}

/**
 * rig_wait() - Send @io and wait up to @micros for it.
 * @io: the request, ready to send.
 * @timer: an open timer.device request, idle.
 * @micros: the wait in microseconds.
 *
 * Both requests are idle again on return.
 *
 * Return: TRUE when @io completed in time; FALSE (and @io aborted) on
 * timeout.
 */
BOOL rig_wait(struct IORequest *io, struct timerequest *timer, ULONG micros)
{
    SendIO(io);
    return rig_await(io, timer, micros);
}

/**
 * rig_await() - Wait up to @micros for @io, sent already.
 * @io: the request, in the driver.
 * @timer: an open timer.device request, idle.
 * @micros: the wait in microseconds.
 *
 * Both requests are idle again on return.
 *
 * Return: TRUE when @io completed in time; FALSE (and @io aborted) on
 * timeout.
 */
BOOL rig_await(struct IORequest *io, struct timerequest *timer, ULONG micros)
{
    timer->tr_node.io_Command = TR_ADDREQUEST;
    timer->tr_time.tv_secs = micros / MICROS_PER_SECOND;
    timer->tr_time.tv_micro = micros % MICROS_PER_SECOND;
    SendIO((struct IORequest *)timer);

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

/**
 * rig_station_address() - Ask the unit for the address in use.
 * @io: an opened request.
 * @address: ETHER_ADDRESS_BYTES for the answer.
 *
 * Return: the io_Error.
 */
BYTE rig_station_address(struct IOSana2Req *io, UBYTE *address)
{
    BYTE error = rig_command(io, S2_GETSTATIONADDRESS);
    memcpy(address, io->ios2_SrcAddr, ETHER_ADDRESS_BYTES);
    return error;
}

/**
 * rig_write_setup() - Fill @io for a write of ETHERTYPE_UNUSED to @destination.
 * @io: an opened request.
 * @destination: the frame's destination address.
 * @data: the payload, FRAME_BUFFER_BYTES long (the Miami rounding reads past @length).
 * @length: the payload's length.
 *
 * A write to the unit's own address goes nowhere: the switch does not send a
 * frame back out of the port it came in on.
 */
void rig_write_setup(struct IOSana2Req *io, const UBYTE *destination, UBYTE *data, ULONG length)
{
    memcpy(io->ios2_DstAddr, destination, ETHER_ADDRESS_BYTES);
    io->ios2_PacketType = ETHERTYPE_UNUSED;
    io->ios2_Data = data;
    io->ios2_DataLength = length;
}

/**
 * take_reply() - Fetch @io's reply off @port, which only @io uses, without
 * WaitIO(): wait up to @micros for it to arrive.
 * @port: the probe's own reply port.
 * @io: the request, taken back with AbortIO() already.
 * @timer: an open timer.device request, idle; idle again on return.
 * @micros: how long to wait.
 *
 * Return: TRUE when the reply was fetched.
 */
static BOOL take_reply(struct MsgPort *port, struct IORequest *io, struct timerequest *timer, ULONG micros)
{
    timer->tr_node.io_Command = TR_ADDREQUEST;
    timer->tr_time.tv_secs = micros / MICROS_PER_SECOND;
    timer->tr_time.tv_micro = micros % MICROS_PER_SECOND;
    SendIO((struct IORequest *)timer);
    ULONG signals = (1UL << port->mp_SigBit) | (1UL << timer->tr_node.io_Message.mn_ReplyPort->mp_SigBit);
    BOOL taken = FALSE;
    while (!taken && !CheckIO((struct IORequest *)timer))
    {
        Wait(signals);
        struct Message *message;
        while ((message = GetMsg(port)) != NULL)
            taken = taken || message == &io->io_Message;
    }
    if (!CheckIO((struct IORequest *)timer))
        AbortIO((struct IORequest *)timer);
    WaitIO((struct IORequest *)timer);
    return taken;
}

/**
 * rig_read_marked_pending() - Tell whether a read the driver keeps shows as
 * pending to CheckIO(), as WaitIO() needs. A driver that keeps a request off
 * every port without marking it NT_MESSAGE makes it read as done, and WaitIO()
 * then unlinks a node that is on no port, writing through its stale links: $4
 * with a fresh request's.
 * @rig: the rig.
 *
 * The probe is a read for nobody's type on a port of its own, taken back with
 * AbortIO() and its reply fetched off that port, never with WaitIO().
 *
 * Return: TRUE when CheckIO() saw it pending; FALSE when it did not, or when
 * the probe could not be made (reported).
 */
BOOL rig_read_marked_pending(struct Rig *rig)
{
    static UBYTE probe_buffer[FRAME_BUFFER_BYTES];
    struct MsgPort *port = CreateMsgPort();
    struct IOSana2Req *io = port != NULL ? (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req))
                                         : NULL;
    if (io == NULL)
    {
        if (port != NULL)
            DeleteMsgPort(port);
        kv_environment("no memory for the probe read");
        return FALSE;
    }
    io->ios2_Req.io_Device = rig->io->ios2_Req.io_Device;
    io->ios2_Req.io_Unit = rig->io->ios2_Req.io_Unit;
    io->ios2_BufferManagement = rig->io->ios2_BufferManagement;
    io->ios2_Req.io_Command = CMD_READ;
    io->ios2_PacketType = ETHERTYPE_UNUSED;
    io->ios2_Data = probe_buffer;
    SendIO((struct IORequest *)io);
    BOOL pending = CheckIO((struct IORequest *)io) == NULL;
    AbortIO((struct IORequest *)io);
    if (take_reply(port, (struct IORequest *)io, rig->timer, PROBE_TAKE_BACK_MICROS))
    {
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
    }
    else
    {
        kv_note("the probe read never came back: left to the driver, not freed");
    }
    return pending;
}

/**
 * rig_note_frame() - Note the frame a read was answered with: its length,
 * type, source and destination.
 * @io: a read that completed without error.
 * @what: which read it was.
 */
void rig_note_frame(const struct IOSana2Req *io, const char *what)
{
    const UBYTE *from = io->ios2_SrcAddr;
    const UBYTE *to = io->ios2_DstAddr;
    kv_note("%s: %lu bytes of type 0x%04lx from %02x:%02x:%02x:%02x:%02x:%02x to %02x:%02x:%02x:%02x:%02x:%02x",
            what, io->ios2_DataLength, io->ios2_PacketType, from[0], from[1], from[2], from[3], from[4], from[5],
            to[0], to[1], to[2], to[3], to[4], to[5]);
}

/**
 * rig_event_now() - Ask S2_ONEVENT for @mask and give what it answers within
 * RIG_EVENT_NOW_MICROS: an event whose state holds answers at once.
 * @rig: the rig.
 * @mask: the S2EVENT_* bits.
 *
 * Return: the events the answer carries; 0 when it failed or did not come.
 */
ULONG rig_event_now(struct Rig *rig, ULONG mask)
{
    struct IOSana2Req *io = rig->io;
    io->ios2_Req.io_Command = S2_ONEVENT;
    io->ios2_Req.io_Flags = 0;
    io->ios2_Req.io_Error = 0;
    io->ios2_WireError = mask;
    BOOL answered = rig_wait((struct IORequest *)io, rig->timer, RIG_EVENT_NOW_MICROS);
    return answered && io->ios2_Req.io_Error == 0 ? io->ios2_WireError : 0;
}

/**
 * rig_sleep() - Wait @micros.
 * @timer: an open timer.device request, idle.
 * @micros: the wait in microseconds.
 */
void rig_sleep(struct timerequest *timer, ULONG micros)
{
    timer->tr_node.io_Command = TR_ADDREQUEST;
    timer->tr_time.tv_secs = micros / MICROS_PER_SECOND;
    timer->tr_time.tv_micro = micros % MICROS_PER_SECOND;
    DoIO((struct IORequest *)timer);
}

/**
 * rig_request() - Make another request on the rig's opener, for a command
 * kept while others run.
 * @rig: the rig.
 *
 * Return: the request, its device, unit and opener the rig's; NULL when out
 * of memory.
 */
struct IOSana2Req *rig_request(const struct Rig *rig)
{
    struct IOSana2Req *io = (struct IOSana2Req *)CreateIORequest(rig->port, sizeof(struct IOSana2Req));
    if (io == NULL)
        return NULL;
    io->ios2_Req.io_Device = rig->io->ios2_Req.io_Device;
    io->ios2_Req.io_Unit = rig->io->ios2_Req.io_Unit;
    io->ios2_BufferManagement = rig->io->ios2_BufferManagement;
    return io;
}

/**
 * rig_request_delete() - Free a request from rig_request() that is not in
 * the driver (never sent, or finished with rig_finish() or WaitIO()).
 * @io: the request, or NULL.
 */
void rig_request_delete(struct IOSana2Req *io)
{
    if (io != NULL)
        DeleteIORequest((struct IORequest *)io);
}

/**
 * rig_finish() - Take @io back from the driver: abort it unless it is done,
 * and wait for its reply.
 * @io: a request sent with SendIO() and not waited for yet; WaitIO() removes
 * its reply from the port, so this runs once per sending.
 */
void rig_finish(struct IOSana2Req *io)
{
    if (!CheckIO((struct IORequest *)io))
        AbortIO((struct IORequest *)io);
    WaitIO((struct IORequest *)io);
}

/**
 * rig_attach_probe() - Send NETDEV_CMD_ATTACH as a netdev stack sends it, but
 * with ABI version 0.
 * @io: an opened request.
 *
 * A held unit answers NDERR_BUSY before it looks at the version, a free one
 * NDERR_BADVERSION, so nothing is ever attached.
 *
 * Return: the io_Error.
 */
BYTE rig_attach_probe(struct IOSana2Req *io)
{
    static const struct NetDevStackOps no_operations;
    struct NetDevAttach attach;
    memset(&attach, 0, sizeof(attach));
    attach.nda_StackOps = &no_operations;
    struct IOStdReq *standard = (struct IOStdReq *)io; /* netdev commands' view of the request */
    standard->io_Data = &attach;
    standard->io_Length = sizeof(attach);
    return rig_command(io, NETDEV_CMD_ATTACH);
}

/**
 * rig_same_word() - Compare two words, ignoring case.
 * @a: one word.
 * @b: the other.
 *
 * Return: TRUE when they are the same word.
 */
BOOL rig_same_word(const char *a, const char *b)
{
    while (*a != '\0' && tolower((unsigned char)*a) == tolower((unsigned char)*b))
    {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}
