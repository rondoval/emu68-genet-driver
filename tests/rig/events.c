// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * events.c - GenetTest ONLINECYCLE, CABLE and ABORT: the events SANA-II
 * reports, and the requests the driver keeps until they complete or are
 * taken back.
 *
 * All three need a SANA-II stack holding the unit online. ONLINECYCLE takes
 * the unit offline for everyone and CABLE waits for the cable to be pulled:
 * the agent talks over this interface, so the host starts them with Run and
 * reads their output afterwards.
 */

#include "rig.h"
#include <emu68test/kv.h>

#include <proto/exec.h>
#include <proto/timer.h>

#include <exec/errors.h>

#include <string.h>

#define EVENT_MICROS 3000000UL       /* an event follows its command within this */
#define DEFAULT_CYCLES 3
#define DEFAULT_CABLE_SECONDS 120
#define HOLD_MICROS 500000UL         /* a kept request is still kept after this */

/* The requests ABORT has the driver keep. */
enum Kept
{
    KEPT_READ,
    KEPT_ORPHAN,
    KEPT_EVENT,
    KEPT_SAMPLER,
    KEPT_COUNT
};

static UBYTE read_buffer[FRAME_BUFFER_BYTES];

/**
 * event_send() - Send S2_ONEVENT for @mask on @io.
 * @io: a request from rig_request(), idle.
 * @mask: the S2EVENT_* bits.
 */
static void event_send(struct IOSana2Req *io, ULONG mask)
{
    io->ios2_Req.io_Command = S2_ONEVENT;
    io->ios2_Req.io_Flags = 0;
    io->ios2_Req.io_Error = 0;
    io->ios2_WireError = mask;
    SendIO((struct IORequest *)io);
}

/**
 * event_after() - Send S2_ONEVENT for @mask, run @command on the rig's
 * request, and wait for the event.
 * @rig: the rig.
 * @event: a request from rig_request(), idle.
 * @mask: the event the command must raise.
 * @command: S2_OFFLINE or S2_ONLINE.
 *
 * The event must still be waiting when the command is sent: its state did
 * not hold before.
 *
 * Return: TRUE when the event waited, the command succeeded and the event
 * came within EVENT_MICROS.
 */
static BOOL event_after(struct Rig *rig, struct IOSana2Req *event, ULONG mask, UWORD command)
{
    event_send(event, mask);
    BOOL waited = !CheckIO((struct IORequest *)event);
    BYTE error = rig_command(rig->io, command);
    BOOL came = rig_await((struct IORequest *)event, rig->timer, EVENT_MICROS);
    return waited && error == 0 && came && (event->ios2_WireError & mask) != 0;
}

/**
 * reconfigurations() - The unit's S2_GETEXTENDEDGLOBALSTATS reconfiguration count.
 * @io: the opened request.
 *
 * Return: the count's low longword.
 */
static ULONG reconfigurations(struct IOSana2Req *io)
{
    struct Sana2ExtDeviceStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.s2xds_Length = sizeof(stats);
    io->ios2_StatData = &stats;
    rig_command(io, S2_GETEXTENDEDGLOBALSTATS);
    return stats.s2xds_Reconfigurations.s2q_Low;
}

/**
 * command_onlinecycle() - GenetTest ONLINECYCLE: S2_OFFLINE and S2_ONLINE in
 * turn, each raising its event, the unit online again at the end.
 * @rig: the rig.
 * @argument: how many cycles; 0 for DEFAULT_CYCLES.
 *
 * The rig's S2_ONLINE makes it a claimer too, as any opener's would; its
 * close lets go of that.
 */
void command_onlinecycle(struct Rig *rig, ULONG argument)
{
    ULONG cycles = argument != 0 ? argument : DEFAULT_CYCLES;
    struct IOSana2Req *event = rig_request(rig);
    if (event == NULL)
    {
        kv_environment("no memory for a request");
        return;
    }
    ULONG before = reconfigurations(rig->io);
    ULONG offline = 0;
    ULONG online = 0;
    for (ULONG i = 0; i < cycles; i++)
    {
        offline += event_after(rig, event, S2EVENT_OFFLINE, S2_OFFLINE) ? 1 : 0;
        online += event_after(rig, event, S2EVENT_ONLINE, S2_ONLINE) ? 1 : 0;
    }
    ULONG counted = reconfigurations(rig->io) - before;
    rig_request_delete(event);
    kv_check(offline == cycles, "S2_OFFLINE raises S2EVENT_OFFLINE", "%lu of %lu", offline, cycles);
    kv_check(online == cycles, "S2_ONLINE raises S2EVENT_ONLINE", "%lu of %lu", online, cycles);
    kv_check(counted == cycles, "each S2_ONLINE after an S2_OFFLINE counts as a reconfiguration", "%lu of %lu",
             counted, cycles);
    ULONG events = rig_event_now(rig, S2EVENT_ONLINE);
    kv_check((events & S2EVENT_ONLINE) != 0, "the unit is online at the end", "events 0x%lx", events);
}

/**
 * wait_event() - Wait up to @seconds for @mask on a request of its own.
 * @rig: the rig.
 * @event: a request from rig_request(), idle.
 * @mask: the event.
 * @seconds: how long to wait.
 *
 * Return: the milliseconds until it came; 0 when it did not.
 */
static ULONG wait_event(struct Rig *rig, struct IOSana2Req *event, ULONG mask, ULONG seconds)
{
    struct timeval start;
    struct timeval end;
    GetSysTime(&start);
    event_send(event, mask);
    BOOL came = rig_await((struct IORequest *)event, rig->timer, seconds * MICROS_PER_SECOND);
    GetSysTime(&end);
    if (!came || (event->ios2_WireError & mask) == 0)
        return 0;
    ULONG millis = (end.tv_secs - start.tv_secs) * MILLIS_PER_SECOND;
    return millis + end.tv_micro / MICROS_PER_MILLI - start.tv_micro / MICROS_PER_MILLI + 1;
}

/**
 * command_cable() - GenetTest CABLE: someone pulls the cable and puts it back;
 * SANA-II hears S2EVENT_DISCONNECT, then S2EVENT_CONNECT.
 * @rig: the rig.
 * @argument: how long to wait for each, in seconds; 0 for DEFAULT_CABLE_SECONDS.
 */
void command_cable(struct Rig *rig, ULONG argument)
{
    ULONG seconds = argument != 0 ? argument : DEFAULT_CABLE_SECONDS;
    struct IOSana2Req *event = rig_request(rig);
    if (event == NULL)
    {
        kv_environment("no memory for a request");
        return;
    }
    ULONG gone = wait_event(rig, event, S2EVENT_DISCONNECT, seconds);
    ULONG back = gone != 0 ? wait_event(rig, event, S2EVENT_CONNECT, seconds) : 0;
    rig_request_delete(event);
    kv_unum("cable.disconnect_ms", gone);
    kv_unum("cable.connect_ms", back);
    kv_check(gone != 0, "pulling the cable raises S2EVENT_DISCONNECT", "after %lu ms", gone);
    kv_check(back != 0, "putting it back raises S2EVENT_CONNECT", "after %lu ms", back);
}

/**
 * read_send() - Send a read on @io.
 * @io: a request from rig_request(), idle.
 * @command: CMD_READ or S2_READORPHAN.
 * @type: the packet type a CMD_READ waits for.
 */
static void read_send(struct IOSana2Req *io, UWORD command, UWORD type)
{
    io->ios2_Req.io_Command = command;
    io->ios2_Req.io_Flags = 0;
    io->ios2_Req.io_Error = 0;
    io->ios2_PacketType = type;
    io->ios2_Data = read_buffer;
    SendIO((struct IORequest *)io);
}

/**
 * sampler_send() - Send S2_SAMPLE_THROUGHPUT for continuous sampling.
 * @io: a request from rig_request(), idle.
 * @stats: the structure the driver keeps updating.
 * @signal: the signal it sends this task on each update.
 */
static void sampler_send(struct IOSana2Req *io, struct Sana2ThroughputStats *stats, BYTE signal)
{
    memset(stats, 0, sizeof(*stats));
    stats->s2ts_Length = sizeof(*stats);
    stats->s2ts_NotifyTask = FindTask(NULL);
    stats->s2ts_NotifyMask = 1UL << signal;
    io->ios2_StatData = stats;
    io->ios2_Req.io_Command = S2_SAMPLE_THROUGHPUT;
    io->ios2_Req.io_Flags = 0;
    io->ios2_Req.io_Error = 0;
    SendIO((struct IORequest *)io);
}

/**
 * check_sampling() - Check the single sample and the one-sampler rule while
 * @kept samples continuously.
 * @rig: the rig.
 * @kept: the continuous sampler, in the driver.
 * @signal: its notification signal.
 */
static void check_sampling(struct Rig *rig, struct IOSana2Req *kept, BYTE signal)
{
    struct Sana2ThroughputStats second;
    memset(&second, 0, sizeof(second));
    second.s2ts_Length = sizeof(second);
    second.s2ts_NotifyTask = FindTask(NULL);
    second.s2ts_NotifyMask = 1UL << signal;
    rig->io->ios2_StatData = &second;
    BYTE error = rig_command(rig->io, S2_SAMPLE_THROUGHPUT);
    kv_check(error == S2ERR_NO_RESOURCES, "a second continuous sampler is refused", "error %d", error);

    struct Sana2ThroughputStats single;
    memset(&single, 0, sizeof(single));
    single.s2ts_Length = sizeof(single);
    rig->io->ios2_StatData = &single;
    error = rig_command(rig->io, S2_SAMPLE_THROUGHPUT);
    kv_check(error == 0 && single.s2ts_Actual == sizeof(single), "a single sample answers at once",
             "error %d, %lu bytes", error, single.s2ts_Actual);
    kv_check(!CheckIO((struct IORequest *)kept), "the continuous sampler is kept", "%s",
             CheckIO((struct IORequest *)kept) ? "answered" : "kept");
}

/**
 * check_aborted() - Take @io back with AbortIO and check how it ended.
 * @io: a request in the driver.
 * @name: the check's name.
 * @may_complete: TRUE when it may have completed on its own first.
 */
static void check_aborted(struct IOSana2Req *io, const char *name, BOOL may_complete)
{
    BOOL kept = !CheckIO((struct IORequest *)io);
    rig_finish(io);
    BYTE error = io->ios2_Req.io_Error;
    if (!kept && error == 0)
        rig_note_frame(io, name);
    BOOL ok = (kept && error == IOERR_ABORTED) || (may_complete && !kept && error == 0);
    kv_check(ok, name, "%s, error %d", kept ? "kept until aborted" : "completed first", error);
}

/**
 * abort_kept() - Have the driver keep one of each kind, then take them back.
 * @rig: the rig.
 * @kept: KEPT_COUNT idle requests from rig_request(); idle again on return.
 * @signal: the continuous sampler's notification signal.
 */
static void abort_kept(struct Rig *rig, struct IOSana2Req **kept, BYTE signal)
{
    struct Sana2ThroughputStats stats;
    read_send(kept[KEPT_READ], CMD_READ, ETHERTYPE_UNUSED);
    read_send(kept[KEPT_ORPHAN], S2_READORPHAN, 0);
    event_send(kept[KEPT_EVENT], S2EVENT_OFFLINE);
    sampler_send(kept[KEPT_SAMPLER], &stats, signal);
    rig_sleep(rig->timer, HOLD_MICROS);
    check_sampling(rig, kept[KEPT_SAMPLER], signal);
    check_aborted(kept[KEPT_READ], "a CMD_READ is kept, and aborted", FALSE);
    check_aborted(kept[KEPT_ORPHAN], "an S2_READORPHAN is aborted or completed", TRUE);
    check_aborted(kept[KEPT_EVENT], "an S2_ONEVENT is kept, and aborted", FALSE);
    check_aborted(kept[KEPT_SAMPLER], "the continuous sampler is aborted", FALSE);
}

/**
 * command_abort() - GenetTest ABORT: every request the driver keeps comes
 * back with IOERR_ABORTED when it is taken back.
 * @rig: the rig.
 * @argument: unused.
 *
 * A read for a type nobody sends, an orphan read (it may get a frame first),
 * an event whose state does not hold, and a continuous throughput sampler.
 * Only when a kept read shows as pending to CheckIO (rig_read_marked_pending()):
 * the checks below take each back with CheckIO() and WaitIO().
 */
void command_abort(struct Rig *rig, ULONG argument)
{
    (void)argument;
    if (!kv_check(rig_read_marked_pending(rig), "a kept read shows as pending to CheckIO",
                  "WaitIO on one that does not corrupts memory: the aborts are not tried"))
        return;
    struct IOSana2Req *kept[KEPT_COUNT];
    BOOL made = TRUE;
    for (int i = 0; i < KEPT_COUNT; i++)
    {
        kept[i] = rig_request(rig);
        made = made && kept[i] != NULL;
    }
    BYTE signal = AllocSignal(-1);
    if (signal >= 0 && made)
        abort_kept(rig, kept, signal);
    else
        kv_environment("no signal or no memory for the requests");
    for (int i = 0; i < KEPT_COUNT; i++)
        rig_request_delete(kept[i]);
    if (signal >= 0)
        FreeSignal(signal);
}
