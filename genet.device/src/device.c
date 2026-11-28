// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/utility_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function: see GenetUnit.sysBase */
#include <proto/exec.h>
#include <proto/utility.h>
#endif

#include <exec/types.h>
#include <exec/resident.h>
#include <exec/io.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <dos/dosextens.h>
#include <devices/newstyle.h>
#include <devices/sana2.h>

#include <device.h>
#include <sana2_unit.h>
#include <emu68_features.h>
#include <minlist.h>
#include <debug.h>
#include <runtime_config.h>

#ifndef DEVICE_NAME
#define DEVICE_NAME "genet.device"
#endif

/* The version comes from CMake (the repository's one version, root CMakeLists.txt); an editor's parser gets placeholders. */
#if !defined(DEVICE_VERSION) || !defined(DEVICE_REVISION) || !defined(DEVICE_IDSTRING)
#ifdef __INTELLISENSE__
#ifndef DEVICE_VERSION
#define DEVICE_VERSION 0
#endif
#ifndef DEVICE_REVISION
#define DEVICE_REVISION 0
#endif
#ifndef DEVICE_IDSTRING
#define DEVICE_IDSTRING "genet.device"
#endif
#else
#error "DEVICE_VERSION, DEVICE_REVISION and DEVICE_IDSTRING are set by CMake"
#endif
#endif
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void);

/**
 * doNotExecute() - The module's entry at offset 0: a device run as a program returns at once.
 *
 * Put the function at the very beginning of the file in order to avoid
 * unexpected results when user executes the device by mistake
 *
 * Return: -1.
 */
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void)
{
    return -1;
}

/*
    Put this marker at the very end of your executable. It's not absolutely
    mandatory but will let rom tag scanner of exec.library work better/faster.
*/
extern const UBYTE endOfCode;

/*
    Const fields containing name of device and ID string.
*/
static const char deviceName[] = DEVICE_NAME;
static const char deviceIdString[] = DEVICE_IDSTRING;
static const APTR initTable[4];

/* [genet] perf slot names — rodata; order matches enum GenetProfSlot. */
static const char *const genet_perf_names[GP_SLOT_COUNT] = {
    "rx_drain", "rx_scan", "rx_flush", "tx_submit", "tx_harvest", "rx_wake",
    "rx_copy", "rx_reply",
};

/* perf_hist bounds — rodata. Frames: fine around the default timeout's worth of
 * line-rate frames (~41) and around the 64-frame threshold, which is also the
 * poll batch. Gap: fine around the default 500 us timeout. */
static const u32 genet_rx_pending_bounds[GENET_RX_PENDING_BOUNDS] = {
    1, 2, 4, 8, 16, 24, 32, 40, 44, 48, 56, 63, 64,
};
static const u32 genet_rx_irq_gap_bounds[GENET_RX_IRQ_GAP_BOUNDS] = {
    100, 200, 300, 400, 480, 520, 560, 640, 800, 1000, 2000, 5000, 20000,
};
/* Held: around one and two poll batches (the ladder's "consumer is behind"
 * limit is two) and a 256 KB reply (180 frames). */
static const u32 genet_rx_held_bounds[GENET_RX_HELD_BOUNDS] = {
    0, 8, 16, 32, 64, 96, 128, 160, 192, 256, 384, 512, 1024,
};

/*
    Resident structure describing the object. RTF_AUTOINIT means the rt_Init field
    points to the initializer table defined below. RTF_COLDSTART defines when the
    object will be initialized (coldstart means, before dos.library, after scheduler
    is started)
*/
static struct Resident const genetDeviceResident __attribute__((used, section(".text.modhdr"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&genetDeviceResident,
    (APTR)&endOfCode,
    RTF_AUTOINIT | RTF_AFTERDOS,
    DEVICE_VERSION,
    NT_DEVICE,
    DEVICE_PRIORITY,
    (APTR)&deviceName,
    (APTR)&deviceIdString,
    (APTR)&initTable};

/*
    Initializer table. First field is the size of structure describing the object,
    can be sizeof(struct Library), sizeof(struct Device) or any size necessary to
    store user defined object extending the Device structure.
*/
APTR initFunction(struct GenetDevice *base asm("d0"), ULONG segList asm("a0"), struct ExecBase *SysBase asm("a6"));

static const APTR funcTable[];
static const APTR initTable[4] = {
    (APTR)sizeof(struct GenetDevice),
    (APTR)funcTable,
    NULL,
    (APTR)initFunction};

void openLib(struct IOStdReq *io asm("a1"), LONG unitNumber asm("d0"), ULONG flags asm("d1"), struct GenetDevice *base asm("a6"));
ULONG closeLib(struct IOStdReq *io asm("a1"), struct GenetDevice *base asm("a6"));
ULONG expungeLib(struct GenetDevice *base asm("a6"));
APTR extFunc(struct GenetDevice *base asm("a6"));

static const APTR funcTable[] = {
    (APTR)openLib,
    (APTR)closeLib,
    (APTR)expungeLib,
    (APTR)extFunc,
    (APTR)beginIO,
    (APTR)abortIO,
    (APTR)-1};

/**
 * genet_close_libraries() - Close the libraries the opens use.
 * @base: the device.
 */
static void genet_close_libraries(struct GenetDevice *base)
{
    struct ExecBase *SysBase = base->sysBase;
    if (base->gic400Base != NULL)
    {
        CloseLibrary(base->gic400Base);
        base->gic400Base = NULL;
    }

    if (base->utilityBase != NULL)
    {
        CloseLibrary(base->utilityBase);
        base->utilityBase = NULL;
    }
}

/**
 * genet_open_libraries() - Open utility.library and gic400.library, unless both are open already.
 * @base: the device.
 *
 * Return: 0, or -1 when one would not open.
 */
static s32 genet_open_libraries(struct GenetDevice *base)
{
    struct ExecBase *SysBase = base->sysBase;
    if (base->utilityBase != NULL && base->gic400Base != NULL)
        return 0;

    genet_close_libraries(base);

    base->utilityBase = OpenLibrary((CONST_STRPTR) "utility.library", LIB_MIN_VERSION);
    if (base->utilityBase == NULL)
    {
        Kprintf("[genet] %s: Failed to open utility.library\n", __func__);
        return -1;
    }

    base->gic400Base = OpenLibrary((CONST_STRPTR) "gic400.library", 0);
    if (base->gic400Base == NULL)
    {
        Kprintf("[genet] %s: Failed to open gic400.library\n", __func__);
        genet_close_libraries(base);
        return -1;
    }

    return 0;
}

/**
 * genet_reset_prepare() - The reset guard's prepare hook: stop the DMA of an online unit before the machine resets.
 * @user: the device.
 *
 * reset_guard prepare: stop RX/TX DMA before the machine resets, so the
 * GENET stops writing into RAM the next OS session will reuse.
 */
static void genet_reset_prepare(APTR user)
{
    struct GenetDevice *base = user;
    struct GenetUnit *unit = base->unit;

    if (unit && unit->state == STATE_ONLINE)
        bcmgenet_reset_quiesce(unit);
}

/**
 * initFunction() - The device's init: refuse an Emu68 without the range cache operations, then set the base up.
 * @base: the device base exec made.
 * @segList: the device's segment list.
 * @SysBase: exec.library.
 *
 * Return: @base, or NULL when the device refused to load (the base freed).
 */
APTR initFunction(struct GenetDevice *base asm("d0"), ULONG segList asm("a0"), struct ExecBase *SysBase asm("a6"))
{
    /* the id string carries the build date — the log's deploy beacon */
    Kprintf("[genet] %s: %s\n", __func__, (ULONG)deviceIdString);

    if (!emu68_has_dcache_range_ops(SysBase))
    {
        Kprintf("[genet] %s: rangeops build, but Emu68 lacks dcache-range-ops rev 1 - refusing to load. Install the standard driver package or update Emu68.\n", __func__);
        ULONG size = (ULONG)base->device.dd_Library.lib_NegSize + base->device.dd_Library.lib_PosSize;
        FreeMem((APTR)((ULONG)base - base->device.dd_Library.lib_NegSize), size);
        return NULL;
    }

    base->segList = segList;
    base->sysBase = SysBase;
    base->device.dd_Library.lib_Revision = DEVICE_REVISION;
    base->unit = NULL;
    base->utilityBase = NULL;
    base->gic400Base = NULL;

    if (!reset_guard_install(&base->resetGuard, SysBase, genet_reset_prepare, base,
                             (CONST_STRPTR)"genet.device"))
        Kprintf("[genet] %s: reset guard install failed\n", __func__);

    return base;
}

/**
 * openLib() - OpenDevice(): open the unit, and give an IOSana2Req-sized request an opener.
 * @io: the opening request.
 * @unitNumber: the unit; only 0.
 * @flags: the open flags (SANA2OPF_MINE, SANA2OPF_PROM).
 * @base: the device.
 */
void openLib(struct IOStdReq *io asm("a1"), LONG unitNumber asm("d0"),
             ULONG flags asm("d1"), struct GenetDevice *base asm("a6"))
{
    struct ExecBase *SysBase = base->sysBase;
    BOOL firstOpen = FALSE;
    BOOL createdUnit = FALSE;

    KprintfT("[genet] %s: Opening device with unit number %ld and flags %lx\n", __func__, unitNumber, flags);
    if (unitNumber != 0)
    {
        KprintfT("[genet] %s: Invalid unit number %ld\n", __func__, unitNumber);
        io->io_Error = IOERR_OPENFAIL;
        io->io_Device = NULL; /* a CloseDevice() after the failed open must not reach closeLib */
        io->io_Unit = NULL;
        return;
    }

    if (io->io_Message.mn_Length < sizeof(struct IOStdReq))
    {
        KprintfT("[genet] %s: Invalid request length %ld\n", __func__, io->io_Message.mn_Length);
        io->io_Error = IOERR_OPENFAIL;
        io->io_Device = NULL; /* a CloseDevice() after the failed open must not reach closeLib */
        io->io_Unit = NULL;
        return;
    }

    if (base->unit == NULL)
    {
        KprintfT("[genet] %s: Allocating unit structure\n", __func__);
        base->unit = AllocMem(sizeof(struct GenetUnit), MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
        if (base->unit == NULL)
        {
            Kprintf("[genet]%s: Failed to allocate unit\n", __func__);
            io->io_Error = IOERR_OPENFAIL;
            io->io_Device = NULL; /* a CloseDevice() after the failed open must not reach closeLib */
            io->io_Unit = NULL;
            return;
        }
        base->unit->sysBase = base->sysBase;
        base->unit->device = base;
        base->unit->gu_Perf = (struct perf){
            "genet", genet_perf_names, base->unit->gu_PerfSlots, GP_SLOT_COUNT};
        base->unit->gu_RxPendingHist = (struct perf_hist){
            "genet", "rx_pending", genet_rx_pending_bounds,
            base->unit->gu_RxPendingBuckets, GENET_RX_PENDING_BOUNDS};
        base->unit->gu_RxIrqGapHist = (struct perf_hist){
            "genet", "rx_irq_gap_us", genet_rx_irq_gap_bounds,
            base->unit->gu_RxIrqGapBuckets, GENET_RX_IRQ_GAP_BOUNDS};
        base->unit->gu_RxHeldHist = (struct perf_hist){
            "genet", "rx_held_at_quiet", genet_rx_held_bounds,
            base->unit->gu_RxHeldBuckets, GENET_RX_HELD_BOUNDS};
        createdUnit = TRUE;
    }

    firstOpen = (base->device.dd_Library.lib_OpenCnt == 0);
    if (firstOpen && genet_open_libraries(base) != 0)
    {
        io->io_Error = IOERR_OPENFAIL;
        io->io_Device = NULL; /* a CloseDevice() after the failed open must not reach closeLib */
        io->io_Unit = NULL;
        if (createdUnit)
        {
            FreeMem(base->unit, sizeof(struct GenetUnit));
            base->unit = NULL;
        }
        return;
    }

    if ((flags & SANA2OPF_MINE) && base->unit->unit.unit_OpenCnt > 0)
    {
        KprintfT("[genet] %s: unit in use, no exclusive open\n", __func__);
        io->io_Error = IOERR_UNITBUSY;
        io->io_Device = NULL; /* a CloseDevice() after the failed open must not reach closeLib */
        io->io_Unit = NULL;
        if (firstOpen)
            genet_close_libraries(base);
        return;
    }

    if (base->unit->unit.unit_OpenCnt == 0)
    {
        /* Configuration is reloaded only when the unit has no other opener. */
        LoadGenetRuntimeConfig(&base->runtimeConfig, SysBase);
        DumpGenetRuntimeConfig(&base->runtimeConfig);
    }

    /* An IOSana2Req-sized open gets an Opener for its buffer-management tags,
     * which takes their place in ios2_BufferManagement - the SANA-II
     * convention for the driver's per-opener cookie. */
    struct IOSana2Req *s2io = (struct IOSana2Req *)io;
    struct Opener *opener = NULL;
    if (io->io_Message.mn_Length >= sizeof(struct IOSana2Req))
    {
        opener = sana2_opener_create(SysBase, base->utilityBase, s2io->ios2_BufferManagement, flags);
        if (opener == NULL)
        {
            io->io_Error = IOERR_OPENFAIL;
            io->io_Device = NULL; /* a CloseDevice() after the failed open must not reach closeLib */
            io->io_Unit = NULL;
            if (createdUnit)
            {
                FreeMem(base->unit, sizeof(struct GenetUnit));
                base->unit = NULL;
            }
            if (firstOpen)
                genet_close_libraries(base);
            return;
        }
    }

    u32 result = UnitOpen(base->unit, (u32)unitNumber, (u32)flags);

    if (result == GENET_OK)
    {
        KprintfT("[genet] %s: Unit opened successfully\n", __func__);
        if (opener != NULL)
        {
            sana2_opener_attach(base->unit, opener);
            s2io->ios2_BufferManagement = opener;
        }
        io->io_Unit = (struct Unit *)base->unit;
        base->device.dd_Library.lib_OpenCnt++;
        base->device.dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
        io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    }
    else
    {
        Kprintf("[genet] %s: Failed to open unit, error code %ld\n", __func__, result);
        /* Leave nothing usable behind. */
        if (opener != NULL)
            sana2_opener_free(SysBase, opener);
        io->io_Error = IOERR_OPENFAIL;
        io->io_Device = NULL; /* a CloseDevice() after the failed open must not reach closeLib */
        io->io_Unit = NULL;
        if (createdUnit)
        {
            FreeMem(base->unit, sizeof(struct GenetUnit));
            base->unit = NULL;
        }
        if (firstOpen)
            genet_close_libraries(base);
    }

    /* In contrast to normal library there is no need to return anything */
    return;
}

/**
 * closeLib() - CloseDevice(): take the opener back, close the unit, and expunge at the last close if one is pending.
 * @io: the closing request.
 * @base: the device.
 *
 * Return: expungeLib()'s segment list when the close expunged, else 0.
 */
ULONG closeLib(struct IOStdReq *io asm("a1"), struct GenetDevice *base asm("a6"))
{
    struct ExecBase *SysBase = base->sysBase;
    struct GenetUnit *unit = (struct GenetUnit *)io->io_Unit;
    KprintfT("[genet] %s: Closing device\n", __func__);

    /* The unit task unlinks and frees the opener - on the last close too,
     * which drains the unit port before the task ends - so a claimer's close
     * hands the unit back on the task that owns it. Only an Opener this
     * driver handed out is taken back (sana2_opener_detach()). */
    if (io->io_Message.mn_Length >= sizeof(struct IOSana2Req))
        sana2_opener_detach(unit, ((struct IOSana2Req *)io)->ios2_BufferManagement);

    u32 result = UnitClose(unit);
    if (result == 0) // last user of Unit disappeared
    {
        KprintfT("[genet] %s: Unit closed successfully, freeing resources\n", __func__);
        FreeMem(unit, sizeof(struct GenetUnit));
        base->unit = NULL;
    }

    base->device.dd_Library.lib_OpenCnt--;

    if (base->device.dd_Library.lib_OpenCnt == 0)
    {
        genet_close_libraries(base);
        if (base->device.dd_Library.lib_Flags & LIBF_DELEXP)
        {
            return expungeLib(base);
        }
    }

    return 0;
}

/**
 * expungeLib() - Expunge the device, or mark it to go at the last close.
 * @base: the device.
 *
 * Return: the segment list to unload, or 0 while it is open or its reset guard cannot be removed.
 */
ULONG expungeLib(struct GenetDevice *base asm("a6"))
{
    struct ExecBase *SysBase = base->sysBase;
    KprintfT("[genet] %s: Expunging device\n", __func__);
    if (base->device.dd_Library.lib_OpenCnt > 0)
    {
        KprintfT("[genet] %s: Device is still open, cannot expunge\n", __func__);
        base->device.dd_Library.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }
    else
    {
        /* The ColdReboot vector may have been re-patched on top of our
         * stub — then the code must stay resident. */
        if (!reset_guard_remove(&base->resetGuard))
        {
            KprintfT("[genet] %s: reset guard not removable, staying resident\n", __func__);
            return NULL;
        }

        ULONG segList = base->segList;

        /* Remove yourself from list of devices */
        Forbid();
        Remove((struct Node *)base);
        Permit();

        /* Calculate size of device base and deallocate memory */
        ULONG size = (ULONG)(base->device.dd_Library.lib_NegSize + base->device.dd_Library.lib_PosSize);
        APTR pointer = (APTR)((ULONG)base - base->device.dd_Library.lib_NegSize);
        FreeMem(pointer, size);

        return segList;
    }
}

/**
 * extFunc() - The reserved vector.
 * @base: the device.
 *
 * Return: @base.
 */
APTR extFunc(struct GenetDevice *base asm("a6"))
{
    return base;
}

/**
 * beginIO() - BeginIO(): serve the SANA-II reads and writes in the caller's task, queue the other commands for the unit task.
 * @io: the request.
 * @base: the device.
 *
 * netdev packets move through the direct-call surface exchanged at
 * NETDEV_CMD_ATTACH, so none reaches BeginIO. The netdev control ops,
 * NSCMD_DEVICEQUERY and the SANA-II commands are serialized by the unit
 * task; everything else is IOERR_NOCMD.
 */
void beginIO(struct IOStdReq *io asm("a1"), struct GenetDevice *base asm("a6"))
{
    struct ExecBase *SysBase = base->sysBase;
    struct GenetUnit *unit = (struct GenetUnit *)io->io_Unit;
    UWORD cmd = io->io_Command;

    if (unit == NULL)
    {
        Kprintf("[genet] %s: request has no unit\n", __func__);
        io->io_Error = IOERR_OPENFAIL;
        if (!(io->io_Flags & IOF_QUICK))
            ReplyMsg(&io->io_Message);
        return;
    }

    /* SANA-II reads go onto their opener's ring, in the reader's task. */
    if (cmd == CMD_READ || cmd == S2_READORPHAN)
    {
        if (io->io_Message.mn_Length < sizeof(struct IOSana2Req))
            io->io_Error = IOERR_BADLENGTH;
        else if (sana2_read(unit, (struct IOSana2Req *)io) == COMMAND_SCHEDULED)
            return; /* replied with a frame, or by a flush */
        if (!(io->io_Flags & IOF_QUICK))
            ReplyMsg(&io->io_Message);
        return;
    }

    /* SANA-II writes go straight to the ring, in the writer's task. */
    if (cmd == CMD_WRITE || cmd == S2_BROADCAST || cmd == S2_MULTICAST)
    {
        if (io->io_Message.mn_Length < sizeof(struct IOSana2Req))
            io->io_Error = IOERR_BADLENGTH;
        else if (sana2_write(unit, (struct IOSana2Req *)io) == COMMAND_SCHEDULED)
            return; /* waits for ring room: replied by the unit task */
        if (!(io->io_Flags & IOF_QUICK))
            ReplyMsg(&io->io_Message);
        return;
    }

    if (NETDEV_IS_CMD(cmd) || cmd == NSCMD_DEVICEQUERY || sana2_is_command(cmd))
    {
        KprintfT("[genet] %s: Queuing %04lx\n", __func__, (ULONG)cmd);
        io->io_Error = 0;
        io->io_Flags &= (UBYTE)~IOF_QUICK;
        PutMsg(&unit->unit.unit_MsgPort, &io->io_Message);
        return;
    }

    KprintfT("[genet] %s: Unsupported command %04lx\n", __func__, (ULONG)cmd);
    io->io_Error = IOERR_NOCMD;
    if (!(io->io_Flags & IOF_QUICK))
        ReplyMsg(&io->io_Message);
}

/**
 * abortIO() - AbortIO(): take a request back where it waits, or let it complete.
 * @io: the request.
 * @base: the device.
 *
 * Queued requests are the commands waiting at the unit port and the SANA-II
 * writes waiting for ring room. Best-effort: take a still-queued one back,
 * otherwise let it complete.
 *
 * Return: 0.
 */
LONG abortIO(struct IOStdReq *io asm("a1"), struct GenetDevice *base asm("a6"))
{
    struct ExecBase *SysBase = base->sysBase;
    KprintfT("[genet] %s: Aborting IO request %lx\n", __func__, io);

    struct GenetUnit *unit = (struct GenetUnit *)io->io_Unit;
    if (unit == NULL)
        return 0;

    /* A SANA-II write waiting for ring room. */
    UWORD cmd = io->io_Command;
    if ((cmd == CMD_WRITE || cmd == S2_BROADCAST || cmd == S2_MULTICAST) &&
        io->io_Message.mn_Length >= sizeof(struct IOSana2Req) &&
        sana2_tx_backlog_remove(unit, (struct IOSana2Req *)io))
        return 0;

    BOOL pulled = FALSE;
    Disable();
    for (struct Node *node = unit->unit.unit_MsgPort.mp_MsgList.lh_Head;
         node->ln_Succ != NULL; node = node->ln_Succ)
    {
        if (node == &io->io_Message.mn_Node)
        {
            Remove(node);
            pulled = TRUE;
            break;
        }
    }
    Enable();

    /* Once removed the request is ours alone, so the reply happens outside the
     * critical section. */
    if (pulled)
    {
        io->io_Error = IOERR_ABORTED;
        ReplyMsg(&io->io_Message);
        return 0;
    }

    /* A read, an event wait, the throughput sampling or a parked S2_ONLINE is
     * kept by the unit task, on lists only it touches: the abort travels
     * there, and AbortIO waits for it. So the request is aborted or completed
     * when this returns, and no abort can arrive late for a request the
     * caller has meanwhile freed or sent again. Anything else completes on
     * its own; AbortIO is a wish, and so is this one without a reply port. */
    if ((cmd == CMD_READ || cmd == S2_READORPHAN || cmd == S2_ONEVENT || cmd == S2_SAMPLE_THROUGHPUT ||
         cmd == S2_ONLINE || cmd == S2_CONFIGINTERFACE) &&
        io->io_Message.mn_Length >= sizeof(struct IOSana2Req))
    {
        struct MsgPort *reply = CreateMsgPort();
        if (reply != NULL)
        {
            struct GenetOpenerControl abort;
            memset(&abort, 0, sizeof(abort));
            abort.io.io_Message.mn_Node.ln_Type = NT_MESSAGE;
            abort.io.io_Message.mn_ReplyPort = reply;
            abort.io.io_Message.mn_Length = sizeof(struct GenetOpenerControl);
            abort.io.io_Unit = (struct Unit *)unit;
            abort.io.io_Command = GENET_CMD_OPENER_CONTROL;
            abort.op = GENET_OPENER_ABORT;
            abort.target = (struct IOSana2Req *)io;
            PutMsg(&unit->unit.unit_MsgPort, &abort.io.io_Message);
            WaitPort(reply);
            GetMsg(reply);
            DeleteMsgPort(reply);
        }
    }
    return 0;
}
