/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+ */
/*
 * rig.h - GenetTest's shared parts: the opened unit, request helpers, the
 * opener's copy hooks, and the commands (one file per area).
 */
#ifndef GENETTEST_RIG_H
#define GENETTEST_RIG_H

#include <exec/types.h>
#include <devices/sana2.h>
#include <devices/timer.h>

#define GENET_DEVICE "genet.device"
#define GENET_UNIT 0
#define ETHER_ADDRESS_BYTES 6
#define ETHER_GROUP_BIT 0x01
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_UNUSED 0x88b5     /* IEEE 802 local experimental: nobody on the LAN sends it */
#define EXPECTED_MTU 1500
#define FRAME_BUFFER_BYTES 1536     /* one frame and some, for the Miami rounding */
#define MICROS_PER_SECOND 1000000UL
#define MILLIS_PER_SECOND 1000UL
#define MICROS_PER_MILLI 1000UL
#define RIG_EVENT_NOW_MICROS 2000000UL  /* an event whose state holds answers at once */
#define PROBE_TAKE_BACK_MICROS 2000000UL /* an aborted read comes back within this */

/* The rig's own opener and a timer, open for the whole run. */
struct Rig
{
    struct MsgPort *port;       /* the reply port of io and of every request from rig_request() */
    struct IOSana2Req *io;      /* genet.device unit 0, opened with rig_buffer_tags() */
    struct MsgPort *timerPort;
    struct timerequest *timer;  /* timer.device UNIT_MICROHZ, idle between uses */
};

/* What the copy hooks saw: the Miami workaround rounds the lengths up. */
struct CopyLog
{
    ULONG toBuffCalls;
    ULONG lastToBuffLength;
    ULONG fromBuffCalls;
    ULONG lastFromBuffLength;
};

extern struct CopyLog rig_copy_log;

struct TagItem *rig_buffer_tags(void);
BYTE rig_command(struct IOSana2Req *io, UWORD command);
BOOL rig_wait(struct IORequest *io, struct timerequest *timer, ULONG micros);
BOOL rig_await(struct IORequest *io, struct timerequest *timer, ULONG micros);
BYTE rig_station_address(struct IOSana2Req *io, UBYTE *address);
ULONG rig_event_now(struct Rig *rig, ULONG mask);
void rig_note_frame(const struct IOSana2Req *io, const char *what);
BOOL rig_read_marked_pending(struct Rig *rig);
void rig_write_setup(struct IOSana2Req *io, const UBYTE *destination, UBYTE *data, ULONG length);
void rig_sleep(struct timerequest *timer, ULONG micros);
struct IOSana2Req *rig_request(const struct Rig *rig);
void rig_request_delete(struct IOSana2Req *io);
void rig_finish(struct IOSana2Req *io);
BYTE rig_attach_probe(struct IOSana2Req *io);
BOOL rig_same_word(const char *a, const char *b);

/* The commands; each prints its checks (kv.h). @argument is the command's number, if any. */
void command_query(struct Rig *rig, ULONG argument);
void command_holder(struct Rig *rig, ULONG argument);
void command_netdev(struct Rig *rig, ULONG argument);
void command_sana2(struct Rig *rig, ULONG argument);
void command_bareopen(struct Rig *rig, ULONG argument);
void command_close(struct Rig *rig, ULONG argument);
void command_mcast(struct Rig *rig, ULONG argument);
void command_onlinecycle(struct Rig *rig, ULONG argument);
void command_cable(struct Rig *rig, ULONG argument);
void command_abort(struct Rig *rig, ULONG argument);
void command_miami(struct Rig *rig, ULONG argument);

#endif /* GENETTEST_RIG_H */
