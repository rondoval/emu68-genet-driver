// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/utility_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
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

#include <device.h>
#include <emu68_features.h>
#include <minlist.h>
#include <debug.h>
#include <runtime_config.h>

#ifndef DEVICE_NAME
#define DEVICE_NAME "genet.device"
#endif

#ifndef DEVICE_IDSTRING
#define DEVICE_IDSTRING "genet.device 4.0"
#endif

#ifndef DEVICE_VERSION
#define DEVICE_VERSION 4
#endif

#ifndef DEVICE_REVISION
#define DEVICE_REVISION 0
#endif
LONG __attribute__((used, no_reorder)) doNotExecute(void);

/*
    Put the function at the very beginning of the file in order to avoid
    unexpected results when user executes the device by mistake
*/
LONG __attribute__((used, no_reorder)) doNotExecute(void)
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
    "rx_drain", "rx_flush", "tx_submit", "tx_harvest",
};

/*
    Resident structure describing the object. RTF_AUTOINIT means the rt_Init field
    points to the initializer table defined below. RTF_COLDSTART defines when the
    object will be initialized (coldstart means, before dos.library, after scheduler
    is started)
*/
static struct Resident const genetDeviceResident __attribute__((used)) = {
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
APTR initFunction(struct GenetDevice *base asm("d0"), ULONG segList asm("a0"), struct ExecBase *_SysBase asm("a6"));

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

static void genet_close_libraries(struct GenetDevice *base)
{
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

static s32 genet_open_libraries(struct GenetDevice *base)
{
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

/* reset_guard prepare: stop RX/TX DMA before the machine resets, so the
 * GENET stops writing into RAM the next OS session will reuse. */
static void genet_reset_prepare(APTR user)
{
    struct GenetDevice *base = user;
    struct GenetUnit *unit = base->unit;

    if (unit && unit->state == STATE_ONLINE)
        bcmgenet_reset_quiesce(unit);
}

APTR initFunction(struct GenetDevice *base asm("d0"), ULONG segList asm("a0"), struct ExecBase *_SysBase asm("a6"))
{
    (void)_SysBase;
    /* the id string carries the build date — the log's deploy beacon */
    Kprintf("[genet] %s: %s\n", __func__, (ULONG)deviceIdString);

    if (!emu68_has_dcache_range_ops())
    {
        Kprintf("[genet] %s: rangeops build, but Emu68 lacks dcache-range-ops rev 1 - refusing to load. Install the standard driver package or update Emu68.\n", __func__);
        ULONG size = (ULONG)base->device.dd_Library.lib_NegSize + base->device.dd_Library.lib_PosSize;
        FreeMem((APTR)((ULONG)base - base->device.dd_Library.lib_NegSize), size);
        return NULL;
    }

    base->segList = segList;
    base->device.dd_Library.lib_Revision = DEVICE_REVISION;
    base->unit = NULL;
    base->utilityBase = NULL;
    base->gic400Base = NULL;

    if (!reset_guard_install(&base->resetGuard, genet_reset_prepare, base,
                             (CONST_STRPTR)"genet.device"))
        Kprintf("[genet] %s: reset guard install failed\n", __func__);

    return base;
}

void openLib(struct IOStdReq *io asm("a1"), LONG unitNumber asm("d0"),
             ULONG flags asm("d1"), struct GenetDevice *base asm("a6"))
{
    BOOL firstOpen = FALSE;
    BOOL createdUnit = FALSE;

    KprintfT("[genet] %s: Opening device with unit number %ld and flags %lx\n", __func__, unitNumber, flags);
    if (unitNumber != 0)
    {
        KprintfT("[genet] %s: Invalid unit number %ld\n", __func__, unitNumber);
        io->io_Error = IOERR_OPENFAIL;
        return;
    }

    if (io->io_Message.mn_Length < sizeof(struct IOStdReq))
    {
        KprintfT("[genet] %s: Invalid request length %ld\n", __func__, io->io_Message.mn_Length);
        io->io_Error = IOERR_OPENFAIL;
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
            return;
        }
        base->unit->device = base;
        base->unit->gu_Perf = (struct perf){
            "genet", genet_perf_names, base->unit->gu_PerfSlots, GP_SLOT_COUNT};
        createdUnit = TRUE;
    }

    firstOpen = (base->device.dd_Library.lib_OpenCnt == 0);
    if (firstOpen && genet_open_libraries(base) != 0)
    {
        io->io_Error = IOERR_OPENFAIL;
        if (createdUnit)
        {
            FreeMem(base->unit, sizeof(struct GenetUnit));
            base->unit = NULL;
        }
        return;
    }

    if (base->unit->unit.unit_OpenCnt == 0)
    {
        /* Configuration is reloaded only when the unit has no other opener. */
        LoadGenetRuntimeConfig(&base->runtimeConfig);
        DumpGenetRuntimeConfig(&base->runtimeConfig);
    }

    u32 result = UnitOpen(base->unit, (u32)unitNumber, (u32)flags);

    if (result == GENET_OK)
    {
        KprintfT("[genet] %s: Unit opened successfully\n", __func__);
        io->io_Unit = (struct Unit *)base->unit;
        base->device.dd_Library.lib_OpenCnt++;
        base->device.dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
        io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    }
    else
    {
        Kprintf("[genet] %s: Failed to open unit, error code %ld\n", __func__, result);
        /* Leave nothing usable behind. */
        io->io_Error = IOERR_OPENFAIL;
        io->io_Device = (struct Device *)-1;
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

ULONG closeLib(struct IOStdReq *io asm("a1"), struct GenetDevice *base asm("a6"))
{
    struct GenetUnit *unit = (struct GenetUnit *)io->io_Unit;
    KprintfT("[genet] %s: Closing device\n", __func__);

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

ULONG expungeLib(struct GenetDevice *base asm("a6"))
{
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

APTR extFunc(struct GenetDevice *base asm("a6"))
{
    return base;
}

/*
    Every packet moves through the direct-call surface exchanged at
    NETDEV_CMD_ATTACH, so no data-path command reaches BeginIO. The netdev
    control ops and NSCMD_DEVICEQUERY are serialized by the unit task;
    everything else is IOERR_NOCMD.
*/
void beginIO(struct IOStdReq *io asm("a1"), struct GenetDevice *base asm("a6") __attribute__((unused)))
{
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

    if (NETDEV_IS_CMD(cmd) || cmd == NSCMD_DEVICEQUERY)
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

/*
    The only queued requests are the control commands waiting at the unit
    port; nothing long-lived is abortable. Best-effort: pull a still-queued
    message off the port, otherwise let it complete.
*/
LONG abortIO(struct IOStdReq *io asm("a1"), struct GenetDevice *base asm("a6") __attribute__((unused)))
{
    KprintfT("[genet] %s: Aborting IO request %lx\n", __func__, io);

    struct GenetUnit *unit = (struct GenetUnit *)io->io_Unit;
    if (unit == NULL)
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
     * critical section. Not queued: the unit task already owns it, or it was
     * never sent — either way it completes on its own. AbortIO is a wish. */
    if (pulled)
    {
        io->io_Error = IOERR_ABORTED;
        ReplyMsg(&io->io_Message);
    }

    return 0;
}
