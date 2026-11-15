// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * sana2.c - the logic tier's program for the SANA-II personality
 * (tests/README.md): sana2_io.c's reads and delivery, with sana2_api.c's
 * openers and commands, sana2_mcast.c's ranges and sana2_tx.c's writes beside
 * it, the TX ring a fake register block. Requests go
 * where the driver sends them: reads through sana2_read() as BeginIO does,
 * the rest to sana2_process_command() as the unit task does, and the opener
 * changes OpenDevice() and CloseDevice() post wait on the unit port for a
 * "task" step.
 *
 *     LogicSana2 <step>...
 *
 * Openers are a, b, c, ... in the order opened; requests are numbered from 0
 * in the order made. Each step, then the state after it as step.N.*:
 *
 *     open[=<option>,...]            OpenDevice(): prom, decline (its packet filter declines), nocopy,
 *                                    copyfail (its CopyToBuff fails), sendfail (its CopyFromBuff fails)
 *     close=<opener>|foreign|null    CloseDevice() with that ios2_BufferManagement
 *     task                           the unit task takes what waits on its port
 *     read=<opener>,<type>[,raw]     CMD_READ through BeginIO; reads=<opener>,<type>,<count> makes several
 *     orphan=<opener>                S2_READORPHAN through BeginIO
 *     cmd=<opener>|null,<command>[,<argument>]   a command for the unit task; the argument is
 *                                    S2_ONEVENT's event mask, S2_DEVICEQUERY's SizeAvailable
 *                                    (or null), S2_CONFIGINTERFACE's or a multicast command's address
 *     frame=<destination>,<type>[,<payload bytes>]   a frame received: station, broadcast or an address
 *     write=<opener>,<destination>,<type>,<length>[,raw]   a write through BeginIO: CMD_WRITE,
 *                                    S2_BROADCAST to broadcast, S2_MULTICAST to a group address
 *     writes=<opener>,<count>        so many 46-byte writes to another station
 *     consumed                       the hardware sent everything on the TX ring (TDMA_CONS_INDEX)
 *     service                        the unit task's TX pass: the backlog and the writers' events
 *     abort=<request>                AbortIO() of a request the unit task keeps, or a write that waits
 *     link=up|down                   the link as the PHY reports it
 *     miami=on|off, netdev=on|off    USE_MIAMI_WORKAROUND; a netdev stack holding the unit
 */

#include "genet.device/src/sana2_io.c"

#include <stdlib.h>
#include <string.h>

#include <genet/ethtool.h>

#include "kv.h"
#include "support.h"

#define OPENERS_MAX 8u
#define REQUESTS_MAX 600u     /* past an opener's read ring (OPENER_READ_RING_N) */
#define FRAME_BYTES 1536u
#define FRAME_ALIGNMENT 2u    /* 2 short of a longword, as S2_RX_FRAME_OFFSET leaves a frame */
#define FIRST_BYTES 4u        /* what a copy record keeps of the copy's start */
#define DEFAULT_PAYLOAD 46u   /* the shortest Ethernet payload */
#define PAYLOAD_PATTERN 0x10u /* payload byte i is PAYLOAD_PATTERN + i */
#define TAGS_MAX 4u
#define ETHER_TYPE_OFFSET 12u
#define ETH_ALEN 6u           /* an Ethernet address's bytes */
#define NUMBER_BASE_ANY 0     /* strtoul: decimal or 0x hex */
#define WRITE_NUMBERS 2u      /* a write's type and length */
#define FILLER_LENGTH 46u     /* the writes= step's payload */
#define GROUP_BIT 0x01u       /* a group address's first byte */

/* What an opener's CopyToBuff saw of one copy: ios2_Data points here. */
struct CopyRecord
{
    ULONG copies;             /* CopyToBuff calls made for the request */
    ULONG copied;             /* the length of the last */
    UBYTE first[FIRST_BYTES]; /* its first bytes */
};

/* A request the program made, and what its opener copied into it. */
struct Request
{
    struct IOSana2Req io;     /* the request */
    struct CopyRecord record; /* its ios2_Data */
};

/* A command a cmd step may send, and how it takes its argument. */
struct Command
{
    const char *name; /* as a step gives it */
    UWORD command;    /* the io_Command */
    BOOL (*argument)(struct IOSana2Req *io, const char *text); /* text NULL without one */
};

static struct Opener *openers[OPENERS_MAX];
static ULONG openerCount;
static struct Request requests[REQUESTS_MAX];
static ULONG requestCount;
static struct Sana2DeviceQuery query;
static UBYTE frameRoom[FRAME_BYTES + FRAME_ALIGNMENT] __attribute__((aligned(4)));
static const UBYTE frameSource[ETH_ALEN] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x99};
static const UBYTE factoryAddress[ETH_ALEN] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01}; /* the station's (unit.c) */
static const UBYTE broadcastAddress[ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const UBYTE otherStation[ETH_ALEN] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x77};
static const struct NetDevStackOps netdevOps; /* a netdev stack holds the unit while ndStackOps is set */
static struct phy_device phy;                 /* the PHY as the link steps leave it */
static UBYTE foreignCookie;                   /* an ios2_BufferManagement no open handed out */

/**
 * copy_ok() - An opener's CopyToBuff and CopyFromBuff: note what was copied.
 * @to: the request's ios2_Data, its CopyRecord.
 * @from: the frame's bytes.
 * @length: how many.
 *
 * Return: TRUE.
 */
static BOOL copy_ok(APTR to asm("a0"), APTR from asm("a1"), ULONG length asm("d0"))
{
    struct CopyRecord *record = to;
    record->copies++;
    record->copied = length;
    memcpy(record->first, from, FIRST_BYTES);
    return TRUE;
}

/**
 * copy_fail() - A CopyToBuff that notes the copy and fails it.
 * @to: the request's ios2_Data, its CopyRecord.
 * @from: the frame's bytes.
 * @length: how many.
 *
 * Return: FALSE.
 */
static BOOL copy_fail(APTR to asm("a0"), APTR from asm("a1"), ULONG length asm("d0"))
{
    copy_ok(to, from, length);
    return FALSE;
}

/**
 * send_ok() - An opener's CopyFromBuff: fill the frame with the payload pattern, and note the copy.
 * @to: the frame's bytes, in the TX staging.
 * @from: the write's ios2_Data, its CopyRecord.
 * @length: how many.
 *
 * Return: TRUE.
 */
static BOOL send_ok(APTR to asm("a0"), APTR from asm("a1"), ULONG length asm("d0"))
{
    struct CopyRecord *record = from;
    UBYTE *bytes = to;
    record->copies++;
    record->copied = length;
    for (ULONG index = 0; index < length; index++)
        bytes[index] = (UBYTE)(PAYLOAD_PATTERN + index);
    return TRUE;
}

/**
 * send_fail() - A CopyFromBuff that notes the copy and fails it, writing nothing.
 * @to: the frame's bytes, in the TX staging.
 * @from: the write's ios2_Data, its CopyRecord.
 * @length: how many.
 *
 * Return: FALSE.
 */
static BOOL send_fail(APTR to asm("a0"), APTR from asm("a1"), ULONG length asm("d0"))
{
    struct CopyRecord *record = from;
    (void)to;
    record->copies++;
    record->copied = length;
    return FALSE;
}

/**
 * packet_filter() - An opener's S2_PacketFilter: its h_Data says whether it accepts.
 * @hook: the hook.
 * @io: the read.
 * @payload: the frame, as the read would get it.
 *
 * Return: TRUE to take the frame.
 */
static ULONG packet_filter(struct Hook *hook asm("a0"), struct IOSana2Req *io asm("a2"), APTR payload asm("a1"))
{
    (void)io;
    (void)payload;
    return hook->h_Data != NULL;
}

static struct Hook acceptingFilter = {.h_Entry = (ULONG(*)(void))(void (*)(void))packet_filter, .h_Data = (APTR)1};
static struct Hook decliningFilter = {.h_Entry = (ULONG(*)(void))(void (*)(void))packet_filter, .h_Data = NULL};

/**
 * has_option() - Whether a comma-separated list holds a word.
 * @list: the list, or NULL.
 * @word: the word.
 *
 * Return: TRUE when it does.
 */
static BOOL has_option(const char *list, const char *word)
{
    size_t length = strlen(word);
    for (const char *at = list; at != NULL && *at != '\0'; at = strchr(at, ','), at = at ? at + 1 : NULL)
        if (strncmp(at, word, length) == 0 && (at[length] == ',' || at[length] == '\0'))
            return TRUE;
    return FALSE;
}

/**
 * cookie_of() - The ios2_BufferManagement a step names: an opener's letter, foreign or null.
 * @text: the name, up to a comma or the end.
 * @cookie: set to the cookie.
 *
 * Return: FALSE for a name that is none of them.
 */
static BOOL cookie_of(const char *text, APTR *cookie)
{
    if (text == NULL)
        return FALSE;
    if (strncmp(text, "null", 4) == 0)
        *cookie = NULL;
    else if (strncmp(text, "foreign", 7) == 0)
        *cookie = &foreignCookie;
    else if (text[0] >= 'a' && (ULONG)(text[0] - 'a') < openerCount)
        *cookie = openers[text[0] - 'a'];
    else
        return FALSE;
    return TRUE;
}

/**
 * new_request() - A request of @cookie's, with its copy record as ios2_Data.
 * @cookie: its ios2_BufferManagement.
 * @command: its io_Command.
 *
 * Return: the request, or NULL when all REQUESTS_MAX are made.
 */
static struct IOSana2Req *new_request(APTR cookie, UWORD command)
{
    if (requestCount == REQUESTS_MAX)
        return NULL;
    struct Request *request = &requests[requestCount++];
    request->io.ios2_Req.io_Message.mn_Length = sizeof(struct IOSana2Req);
    request->io.ios2_Req.io_Command = command;
    request->io.ios2_BufferManagement = cookie;
    request->io.ios2_Data = &request->record;
    return &request->io;
}

/**
 * request_id() - A request's number, or -1 for a message the program did not make.
 * @message: the message.
 *
 * Return: the number.
 */
static LONG request_id(const struct Message *message)
{
    for (ULONG index = 0; index < requestCount; index++)
        if (message == &requests[index].io.ios2_Req.io_Message)
            return (LONG)index;
    return -1;
}

/**
 * print_outcome() - A request's number and what the driver made of it so far.
 * @prefix: the step's prefix.
 * @io: the request.
 * @kept: TRUE when the driver kept it (COMMAND_SCHEDULED).
 */
static void print_outcome(const char *prefix, const struct IOSana2Req *io, BOOL kept)
{
    char key[LOGIC_KEY_TEXT];
    logic_key(key, prefix, ".request");
    kv_snum(key, request_id(&io->ios2_Req.io_Message));
    logic_print_flag(prefix, ".kept", kept);
    logic_print_text(prefix, ".error", logic_error_name(io->ios2_Req.io_Error));
    logic_print_text(prefix, ".wire_error", logic_wire_error_name(io->ios2_WireError));
}

/**
 * step_open() - OpenDevice() of an IOSana2Req: the opener from its tags, attached.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: prom, decline, nocopy, copyfail, sendfail; NULL for none.
 *
 * Return: FALSE when there is no room for one more.
 */
static BOOL step_open(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    struct TagItem tags[TAGS_MAX];
    ULONG count = 0;

    (void)prefix;
    if (openerCount == OPENERS_MAX)
        return FALSE;
    if (!has_option(operands, "nocopy"))
    {
        tags[count].ti_Tag = S2_CopyToBuff;
        tags[count++].ti_Data = (ULONG)(has_option(operands, "copyfail") ? copy_fail : copy_ok);
        tags[count].ti_Tag = S2_CopyFromBuff;
        tags[count++].ti_Data = (ULONG)(has_option(operands, "sendfail") ? send_fail : send_ok);
    }
    tags[count].ti_Tag = S2_PacketFilter;
    tags[count++].ti_Data = (ULONG)(has_option(operands, "decline") ? &decliningFilter : &acceptingFilter);
    tags[count].ti_Tag = TAG_DONE;

    struct Opener *opener = sana2_opener_create(unit->sysBase, unit->device->utilityBase, tags,
                                                has_option(operands, "prom") ? SANA2OPF_PROM : 0);
    if (opener == NULL)
        return FALSE;
    sana2_opener_attach(unit, opener);
    openers[openerCount++] = opener;
    return TRUE;
}

/**
 * step_close() - CloseDevice() of a request whose ios2_BufferManagement is an opener's, foreign or NULL.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the opener's letter, foreign or null.
 *
 * Return: FALSE for another name.
 */
static BOOL step_close(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    APTR cookie;
    (void)prefix;
    if (!cookie_of(operands, &cookie))
        return FALSE;
    sana2_opener_detach(unit, cookie);
    return TRUE;
}

/**
 * step_task() - The unit task takes every message on its port, in order, as ProcessCommand() would.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: none.
 *
 * Return: FALSE for operands.
 */
static BOOL step_task(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct Message *message;

    (void)prefix;
    while ((message = logic_port_take()) != NULL)
    {
        struct IOSana2Req *io = (struct IOSana2Req *)message;
        if (io->ios2_Req.io_Command == GENET_CMD_OPENER_CONTROL)
            sana2_opener_control(unit, (struct GenetOpenerControl *)io);
        else if (sana2_process_command(unit, io) == COMMAND_PROCESSED)
            ReplyMsg(message);
    }
    return operands == NULL;
}

/**
 * make_read() - CMD_READ or S2_READORPHAN of an opener's, through sana2_read() as BeginIO makes it.
 * @unit: the unit.
 * @prefix: the step's prefix, or NULL to print nothing.
 * @operands: <opener>[,<type>[,raw]].
 * @command: CMD_READ or S2_READORPHAN.
 *
 * Return: FALSE for operands that name no opener.
 */
static BOOL make_read(struct GenetUnit *unit, const char *prefix, const char *operands, UWORD command)
{
    APTR cookie;
    if (!cookie_of(operands, &cookie))
        return FALSE;
    struct IOSana2Req *io = new_request(cookie, command);
    if (io == NULL)
        return FALSE;
    const char *type = strchr(operands, ',');
    if (type != NULL)
        io->ios2_PacketType = strtoul(type + 1, NULL, NUMBER_BASE_ANY);
    if (has_option(type, "raw"))
        io->ios2_Req.io_Flags |= SANA2IOF_RAW;
    BOOL kept = sana2_read(unit, io) == COMMAND_SCHEDULED;
    if (prefix != NULL)
        print_outcome(prefix, io, kept);
    return TRUE;
}

/**
 * step_read() - CMD_READ: read=<opener>,<type>[,raw].
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the operands.
 *
 * Return: FALSE for operands that name no opener.
 */
static BOOL step_read(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    return make_read(unit, prefix, operands, CMD_READ);
}

/**
 * step_orphan() - S2_READORPHAN: orphan=<opener>.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the opener.
 *
 * Return: FALSE for operands that name no opener.
 */
static BOOL step_orphan(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    return make_read(unit, prefix, operands, S2_READORPHAN);
}

/**
 * step_reads() - Several reads of one type: reads=<opener>,<type>,<count>, printed as none.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the operands.
 *
 * Return: FALSE for operands that are not that.
 */
static BOOL step_reads(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    char read[LOGIC_KEY_TEXT];
    const char *count = operands != NULL ? strrchr(operands, ',') : NULL;

    (void)prefix;
    if (count == NULL || (size_t)(count - operands) >= sizeof(read))
        return FALSE;
    memcpy(read, operands, (size_t)(count - operands));
    read[count - operands] = '\0';
    for (ULONG made = strtoul(count + 1, NULL, NUMBER_BASE_ANY); made > 0; made--)
        if (!make_read(unit, NULL, read, CMD_READ))
            return FALSE;
    return TRUE;
}

/**
 * argument_none() - A command that takes no argument.
 * @io: the request.
 * @text: the argument.
 *
 * Return: FALSE when one was given.
 */
static BOOL argument_none(struct IOSana2Req *io, const char *text)
{
    (void)io;
    return text == NULL;
}

/**
 * argument_events() - S2_ONEVENT's events, a mask in ios2_WireError.
 * @io: the request.
 * @text: the mask, decimal or 0x hex.
 *
 * Return: FALSE without one.
 */
static BOOL argument_events(struct IOSana2Req *io, const char *text)
{
    ULONG events;
    if (!logic_numbers(text, &events, 1))
        return FALSE;
    io->ios2_WireError = events;
    return TRUE;
}

/**
 * argument_query() - S2_DEVICEQUERY's Sana2DeviceQuery, its SizeAvailable given, or none.
 * @io: the request.
 * @text: the size, null for no StatData, NULL for the whole structure.
 *
 * Return: FALSE for another argument.
 */
static BOOL argument_query(struct IOSana2Req *io, const char *text)
{
    ULONG size = sizeof(query);
    memset(&query, 0, sizeof(query));
    io->ios2_StatData = &query;
    if (text != NULL && strcmp(text, "null") == 0)
        io->ios2_StatData = NULL;
    else if (text != NULL && !logic_numbers(text, &size, 1))
        return FALSE;
    query.SizeAvailable = size;
    return TRUE;
}

/**
 * argument_address() - S2_CONFIGINTERFACE's address, none for the factory one.
 * @io: the request.
 * @text: the address, or NULL.
 *
 * Return: FALSE for one that is not an address.
 */
static BOOL argument_address(struct IOSana2Req *io, const char *text)
{
    return text == NULL || logic_parse_address(text, io->ios2_SrcAddr);
}

/**
 * argument_range() - A multicast command's <address> or <address>-<address>.
 * @io: the request.
 * @text: the range.
 *
 * Return: FALSE for one that is neither.
 */
static BOOL argument_range(struct IOSana2Req *io, const char *text)
{
    return text != NULL && logic_parse_range(text, io->ios2_SrcAddr, io->ios2_DstAddr) != 0;
}

static const struct Command commands[] = {
    {"S2_DEVICEQUERY", S2_DEVICEQUERY, argument_query},
    {"S2_GETSTATIONADDRESS", S2_GETSTATIONADDRESS, argument_none},
    {"S2_CONFIGINTERFACE", S2_CONFIGINTERFACE, argument_address},
    {"S2_ONLINE", S2_ONLINE, argument_none},
    {"S2_OFFLINE", S2_OFFLINE, argument_none},
    {"S2_ONEVENT", S2_ONEVENT, argument_events},
    {"S2_ADDMULTICASTADDRESS", S2_ADDMULTICASTADDRESS, argument_range},
    {"S2_ADDMULTICASTADDRESSES", S2_ADDMULTICASTADDRESSES, argument_range},
    {"CMD_FLUSH", CMD_FLUSH, argument_none},
    {"CMD_READ", CMD_READ, argument_none},
};

/**
 * command_named() - The command a cmd step names.
 * @name: the name, up to a comma or the end.
 *
 * Return: the command, or NULL for a name not in commands[].
 */
static const struct Command *command_named(const char *name)
{
    const char *comma = strchr(name, ',');
    size_t length = comma != NULL ? (size_t)(comma - name) : strlen(name);
    for (ULONG index = 0; index < LOGIC_COUNT_OF(commands); index++)
        if (strlen(commands[index].name) == length && strncmp(commands[index].name, name, length) == 0)
            return &commands[index];
    return NULL;
}

/**
 * print_query() - What S2_DEVICEQUERY filled in.
 * @prefix: the step's prefix.
 */
static void print_query(const char *prefix)
{
    logic_print_number(prefix, ".query.size_supplied", query.SizeSupplied);
    logic_print_number(prefix, ".query.address_bits", query.AddrFieldSize);
    logic_print_number(prefix, ".query.mtu", query.MTU);
    logic_print_number(prefix, ".query.bps", query.BPS);
    logic_print_number(prefix, ".query.hardware_type", query.HardwareType);
    logic_print_number(prefix, ".query.raw_mtu", query.RawMTU);
}

/**
 * step_cmd() - A command for the unit task: cmd=<opener>|null,<command>[,<argument>].
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the operands.
 *
 * Processed at once, as the unit task would on taking it from its port, and
 * replied there unless the driver keeps it.
 *
 * Return: FALSE for operands that are not that.
 */
static BOOL step_cmd(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    struct ExecBase *SysBase = unit->sysBase;
    const char *name = operands != NULL ? strchr(operands, ',') : NULL;
    APTR cookie;

    if (name == NULL || !cookie_of(operands, &cookie))
        return FALSE;
    const struct Command *command = command_named(name + 1);
    const char *argument = strchr(name + 1, ',');
    struct IOSana2Req *io = command != NULL ? new_request(cookie, command->command) : NULL;
    if (io == NULL || !command->argument(io, argument != NULL ? argument + 1 : NULL))
        return FALSE;
    BOOL kept = sana2_process_command(unit, io) == COMMAND_SCHEDULED;
    if (!kept)
        ReplyMsg(&io->ios2_Req.io_Message);
    print_outcome(prefix, io, kept);
    if (io->ios2_Req.io_Command == S2_DEVICEQUERY)
        print_query(prefix);
    return TRUE;
}

/**
 * frame_destination() - Write a frame's destination: station, broadcast or an address.
 * @unit: the unit.
 * @frame: the frame.
 * @text: the destination, up to a comma.
 *
 * Return: FALSE for another destination.
 */
static BOOL frame_destination(const struct GenetUnit *unit, UBYTE *frame, const char *text)
{
    if (strncmp(text, "station,", 8) == 0)
        memcpy(frame, unit->currentMacAddress, ETH_ALEN);
    else if (strncmp(text, "broadcast,", 10) == 0)
        memcpy(frame, broadcastAddress, ETH_ALEN);
    else
        return logic_parse_address(text, frame);
    return TRUE;
}

/**
 * step_frame() - A frame arrives: frame=<destination>,<type>[,<payload bytes>].
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the operands.
 *
 * As an RX pass delivers it: the openers' new reads first, then the frame.
 *
 * Return: FALSE for operands that are not that.
 */
static BOOL step_frame(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    UBYTE *frame = frameRoom + FRAME_ALIGNMENT;
    const char *type = operands != NULL ? strchr(operands, ',') : NULL;
    char *end;

    (void)prefix;
    if (type == NULL || !frame_destination(unit, frame, operands))
        return FALSE;
    ULONG packetType = strtoul(type + 1, &end, NUMBER_BASE_ANY);
    ULONG payload = *end == ',' ? strtoul(end + 1, NULL, NUMBER_BASE_ANY) : DEFAULT_PAYLOAD;
    if (ETH_HLEN + payload > FRAME_BYTES)
        return FALSE;
    memcpy(frame + ETH_ALEN, frameSource, ETH_ALEN);
    frame[ETHER_TYPE_OFFSET] = (UBYTE)(packetType >> 8);
    frame[ETHER_TYPE_OFFSET + 1] = (UBYTE)packetType;
    for (ULONG index = 0; index < payload; index++)
        frame[ETH_HLEN + index] = (UBYTE)(PAYLOAD_PATTERN + index);

    for (struct MinNode *node = unit->s2->openers.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
        drain_read_ring(unit, (struct Opener *)node);
    sana2_receive_frame(unit, frame, ETH_HLEN + payload);
    return TRUE;
}

/**
 * step_abort() - AbortIO() of a kept request or a waiting write: abort=<request>.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the request's number.
 *
 * Return: FALSE for a number not made.
 */
static BOOL step_abort(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    ULONG id;
    (void)prefix;
    if (!logic_numbers(operands, &id, 1) || id >= requestCount)
        return FALSE;
    struct IOSana2Req *io = &requests[id].io;
    UWORD command = io->ios2_Req.io_Command;
    if (command == CMD_WRITE || command == S2_BROADCAST || command == S2_MULTICAST)
        sana2_tx_backlog_remove(unit, io);           /* as abortIO() takes a waiting write back */
    else
        sana2_abort_kept(unit, io);
    return TRUE;
}

/**
 * print_ring() - The TX ring: its producer index, the doorbell, and the frame its last descriptor sends.
 * @unit: the unit.
 * @prefix: the step's prefix.
 */
static void print_ring(const struct GenetUnit *unit, const char *prefix)
{
    char key[LOGIC_KEY_TEXT];
    u16 produced = unit->tx_ring.tx_prod_index;

    logic_print_number(prefix, ".ring.produced", produced);
    logic_print_number(prefix, ".ring.doorbell", logic_register(unit, TDMA_PROD_INDEX));
    if (produced == 0)
        return;
    ULONG descriptor = GENET_TX_OFF + (ULONG)(u8)(produced - 1u) * DMA_DESC_SIZE;
    ULONG status = logic_register(unit, descriptor + DMA_DESC_LENGTH_STATUS);
    const UBYTE *frame = (const UBYTE *)logic_register(unit, descriptor + DMA_DESC_ADDRESS_LO) + GENET_STATUS64_LEN;
    logic_print_number(prefix, ".ring.length", ((status >> DMA_BUFLENGTH_SHIFT) & DMA_BUFLENGTH_MASK) - GENET_STATUS64_LEN);
    logic_key(key, prefix, ".ring.destination");
    logic_print_address(key, frame);
    logic_key(key, prefix, ".ring.source");
    logic_print_address(key, frame + ETH_ALEN);
    logic_key(key, prefix, ".ring.type");
    kv_hex(key, ((ULONG)frame[ETHER_TYPE_OFFSET] << 8) | frame[ETHER_TYPE_OFFSET + 1]);
    logic_key(key, prefix, ".ring.after_header");
    kv_hex(key, ((ULONG)frame[ETH_HLEN] << 24) | ((ULONG)frame[ETH_HLEN + 1] << 16) |
                    ((ULONG)frame[ETH_HLEN + 2] << 8) | frame[ETH_HLEN + 3]);
}

/**
 * make_write() - A write through sana2_write(), as BeginIO makes it, replied unless it waits.
 * @unit: the unit.
 * @cookie: its ios2_BufferManagement.
 * @destination: its ios2_DstAddr; broadcast makes it S2_BROADCAST, a group address S2_MULTICAST.
 * @numbers: its ios2_PacketType and ios2_DataLength.
 * @raw: SANA2IOF_RAW.
 *
 * Return: the write, or NULL when all REQUESTS_MAX are made.
 */
static struct IOSana2Req *make_write(struct GenetUnit *unit, APTR cookie, const UBYTE *destination,
                                     const ULONG *numbers, BOOL raw)
{
    struct ExecBase *SysBase = unit->sysBase;
    BOOL broadcast = memcmp(destination, broadcastAddress, ETH_ALEN) == 0;
    UWORD command = broadcast ? S2_BROADCAST : (destination[0] & GROUP_BIT) ? S2_MULTICAST : CMD_WRITE;
    struct IOSana2Req *io = new_request(cookie, command);
    if (io == NULL)
        return NULL;
    if (!broadcast)
        memcpy(io->ios2_DstAddr, destination, ETH_ALEN);
    io->ios2_PacketType = numbers[0];
    io->ios2_DataLength = numbers[1];
    if (raw)
        io->ios2_Req.io_Flags |= SANA2IOF_RAW;
    if (sana2_write(unit, io) == COMMAND_PROCESSED)
        ReplyMsg(&io->ios2_Req.io_Message);
    return io;
}

/**
 * write_destination() - Read a write's destination: broadcast or an address.
 * @text: the destination, up to a comma.
 * @destination: the address.
 *
 * Return: FALSE for neither.
 */
static BOOL write_destination(const char *text, UBYTE *destination)
{
    if (strncmp(text, "broadcast,", 10) != 0)
        return logic_parse_address(text, destination);
    memcpy(destination, broadcastAddress, ETH_ALEN);
    return TRUE;
}

/**
 * step_write() - A write: write=<opener>,<destination>,<type>,<length>[,raw].
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the operands; the destination is broadcast or an address.
 *
 * Return: FALSE for operands that are not that.
 */
static BOOL step_write(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    UBYTE destination[ETH_ALEN];
    ULONG numbers[WRITE_NUMBERS];
    char rest[LOGIC_KEY_TEXT];
    APTR cookie;
    const char *field = operands != NULL ? strchr(operands, ',') : NULL;

    if (field == NULL || !cookie_of(operands, &cookie) || !write_destination(field + 1, destination))
        return FALSE;
    const char *numbersText = strchr(field + 1, ',');
    BOOL raw = has_option(numbersText, "raw");
    if (numbersText == NULL || strlen(numbersText) >= sizeof(rest))
        return FALSE;
    strcpy(rest, numbersText + 1);
    if (raw)
        rest[strlen(rest) - strlen(",raw")] = '\0';
    if (!logic_numbers(rest, numbers, WRITE_NUMBERS))
        return FALSE;
    struct IOSana2Req *io = make_write(unit, cookie, destination, numbers, raw);
    if (io == NULL)
        return FALSE;
    print_outcome(prefix, io, io->ios2_Req.io_Message.mn_Node.ln_Type == NT_MESSAGE);
    print_ring(unit, prefix);
    return TRUE;
}

/**
 * step_writes() - So many 46-byte writes to another station: writes=<opener>,<count>.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the operands.
 *
 * Return: FALSE for operands that are not that.
 */
static BOOL step_writes(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    static const ULONG numbers[WRITE_NUMBERS] = {0x0800, FILLER_LENGTH};
    APTR cookie;
    ULONG count;
    const char *number = operands != NULL ? strchr(operands, ',') : NULL;

    if (number == NULL || !cookie_of(operands, &cookie) || !logic_numbers(number + 1, &count, 1))
        return FALSE;
    for (; count > 0; count--)
        if (make_write(unit, cookie, otherStation, numbers, FALSE) == NULL)
            return FALSE;
    print_ring(unit, prefix);
    return TRUE;
}

/**
 * step_consumed() - The hardware has sent everything on the TX ring: TDMA_CONS_INDEX catches up.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: none.
 *
 * Return: FALSE for operands.
 */
static BOOL step_consumed(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    (void)prefix;
    logic_register_set(unit, TDMA_CONS_INDEX, unit->tx_ring.tx_prod_index);
    return operands == NULL;
}

/**
 * step_service() - The unit task's pass on its TX signal: the backlog, and the events the writers raised.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: none.
 *
 * Return: FALSE for operands.
 */
static BOOL step_service(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    sana2_service(unit, 1UL << unit->tx_signal);
    print_ring(unit, prefix);
    return operands == NULL;
}

/**
 * step_link() - The PHY's link comes up at 1000 Mb/s full duplex or goes, and the link poll
 * publishes it (bcmgenet_link_publish()).
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: up or down.
 *
 * Return: FALSE for neither.
 */
static BOOL step_link(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    BOOL up;
    (void)prefix;
    if (!logic_switch(operands, &up))
        return FALSE;
    phy.link = up;
    phy.speed = up ? SPEED_1000 : 0;
    phy.duplex = up ? DUPLEX_FULL : DUPLEX_HALF;
    bcmgenet_link_publish(unit, up, FALSE);
    return TRUE;
}

/**
 * step_miami() - USE_MIAMI_WORKAROUND on or off.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: on or off.
 *
 * Return: FALSE for neither.
 */
static BOOL step_miami(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    BOOL on;
    (void)prefix;
    if (!logic_switch(operands, &on))
        return FALSE;
    unit->device->runtimeConfig.use_miami_workaround = (u8)on;
    return TRUE;
}

/**
 * step_netdev() - A netdev stack holds the unit, or lets it go.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: on or off.
 *
 * Return: FALSE for neither.
 */
static BOOL step_netdev(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    BOOL on;
    (void)prefix;
    if (!logic_switch(operands, &on))
        return FALSE;
    unit->ndStackOps = on ? &netdevOps : NULL;
    return TRUE;
}

/**
 * print_read_reply() - What a replied read holds: length, type, cast, destination, and what was copied.
 * @prefix: the reply's prefix.
 * @request: the read.
 */
static void print_read_reply(const char *prefix, const struct Request *request)
{
    const struct IOSana2Req *io = &request->io;
    const UBYTE *first = request->record.first;
    char key[LOGIC_KEY_TEXT];

    logic_print_number(prefix, ".length", io->ios2_DataLength);
    logic_key(key, prefix, ".type");
    kv_hex(key, io->ios2_PacketType);
    logic_print_text(prefix, ".cast", (io->ios2_Req.io_Flags & SANA2IOF_BCAST)   ? "broadcast"
                                      : (io->ios2_Req.io_Flags & SANA2IOF_MCAST) ? "multicast"
                                                                                 : "unicast");
    logic_key(key, prefix, ".destination");
    logic_print_address(key, io->ios2_DstAddr);
    logic_print_number(prefix, ".copies", request->record.copies);
    logic_print_number(prefix, ".copied", request->record.copied);
    logic_key(key, prefix, ".first");
    kv_hex(key, ((ULONG)first[0] << 24) | ((ULONG)first[1] << 16) | ((ULONG)first[2] << 8) | first[3]);
}

/**
 * print_reply() - One request replied during the step, as its owner finds it.
 * @prefix: the step's prefix.
 * @index: the reply's number in the step.
 * @message: the replied message.
 */
static void print_reply(const char *prefix, ULONG index, const struct Message *message)
{
    char reply[LOGIC_KEY_TEXT];
    char events[LOGIC_EVENTS_TEXT];
    LONG id = request_id(message);
    char key[LOGIC_KEY_TEXT];

    logic_key(reply, prefix, ".reply.%lu", (unsigned long)index);
    logic_key(key, reply, ".request");
    kv_snum(key, id);
    if (id < 0)
        return;
    const struct IOSana2Req *io = &requests[id].io;
    logic_print_text(reply, ".error", logic_error_name(io->ios2_Req.io_Error));
    if (io->ios2_Req.io_Command == S2_ONEVENT && io->ios2_Req.io_Error == S2ERR_NO_ERROR)
    {
        logic_events_text(events, io->ios2_WireError); /* the events that happened */
        logic_print_text(reply, ".events", events);
    }
    else
        logic_print_text(reply, ".wire_error", logic_wire_error_name(io->ios2_WireError));
    UWORD command = io->ios2_Req.io_Command;
    if (command == CMD_READ || command == S2_READORPHAN)
        print_read_reply(reply, &requests[id]);
    else if (command == CMD_WRITE || command == S2_BROADCAST || command == S2_MULTICAST)
        logic_print_number(reply, ".copied", requests[id].record.copied);
}

/**
 * print_calls() - The replies and the messages posted during the step.
 * @prefix: the step's prefix.
 */
static void print_calls(const char *prefix)
{
    const struct LogicCall *calls;
    ULONG count = logic_calls(&calls);
    ULONG replies = 0;
    ULONG postings = 0;

    for (ULONG index = 0; index < count; index++)
    {
        if (calls[index].kind == LOGIC_REPLYMSG)
            print_reply(prefix, replies++, calls[index].target);
        else if (calls[index].kind == LOGIC_PUTMSG)
            postings++;
    }
    logic_print_number(prefix, ".replies", replies);
    logic_print_number(prefix, ".posted", postings);
    logic_calls_clear();
}

/**
 * list_length() - How many nodes @list holds.
 * @list: the list.
 *
 * Return: the count.
 */
static ULONG list_length(const struct MinList *list)
{
    ULONG count = 0;
    for (const struct MinNode *node = list->mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
        count++;
    return count;
}

/**
 * print_state() - The unit's SANA-II side after a step.
 * @unit: the unit.
 * @prefix: the step's prefix.
 */
static void print_state(const struct GenetUnit *unit, const char *prefix)
{
    const struct GenetS2 *s2 = unit->s2;
    char key[LOGIC_KEY_TEXT];

    print_calls(prefix);
    logic_print_number(prefix, ".openers", list_length(&s2->openers));
    logic_print_number(prefix, ".issued", list_length(&s2->issued));
    logic_print_number(prefix, ".claimers", s2->claimers);
    logic_print_flag(prefix, ".online", s2->online);
    logic_print_flag(prefix, ".configured", s2->configured);
    logic_print_flag(prefix, ".promiscuous", unit->ndPromisc);
    logic_print_number(prefix, ".rx_packets", (ULONG)unit->internalStats.rx_packets);
    logic_print_number(prefix, ".rx_bytes", (ULONG)unit->internalStats.rx_bytes);
    logic_print_number(prefix, ".rx_orphan", (ULONG)s2->rxOrphan);
    logic_print_number(prefix, ".rx_ip_arp_dropped", s2->rxIpArpDropped);
    logic_print_number(prefix, ".rx_buffer_errors", s2->rxBufferErrors);
    logic_print_number(prefix, ".tx_packets", (ULONG)unit->internalStats.tx_packets);
    logic_print_number(prefix, ".tx_dropped", (ULONG)unit->internalStats.tx_dropped);
    logic_print_number(prefix, ".tx_waiting", list_length(&s2->txBacklog));
    logic_key(key, prefix, ".forbids");
    kv_snum(key, logic_forbids());
}

static const struct LogicStep steps[] = {
    {"open", step_open},     {"close", step_close}, {"task", step_task},   {"read", step_read},
    {"reads", step_reads},   {"orphan", step_orphan}, {"cmd", step_cmd},   {"frame", step_frame},
    {"abort", step_abort},   {"link", step_link},   {"miami", step_miami}, {"netdev", step_netdev},
    {"write", step_write},   {"writes", step_writes}, {"consumed", step_consumed}, {"service", step_service},
};

static const struct LogicProgram program = {"sana2", steps, LOGIC_COUNT_OF(steps), print_state};

/**
 * main() - A configured unit with its factory address and a PHY without a link, then each step.
 * @argc: the argument count.
 * @argv: the steps.
 *
 * Return: logic_main()'s.
 */
int main(int argc, char **argv)
{
    struct GenetUnit *unit = logic_unit_create();

    unit->state = STATE_CONFIGURED;
    unit->localMacAddress = factoryAddress;
    unit->phydev = &phy;
    return logic_main(&program, unit, argc, argv);
}
