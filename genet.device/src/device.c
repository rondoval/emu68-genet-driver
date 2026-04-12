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

#include <devices/sana2.h>
#include <devices/sana2specialstats.h>

#include <device.h>
#include <minlist.h>
#include <debug.h>
#include <runtime_config.h>

#ifndef DEVICE_NAME
#define DEVICE_NAME "genet.device"
#endif

#ifndef DEVICE_IDSTRING
#define DEVICE_IDSTRING "genet.device 2.3"
#endif

#ifndef DEVICE_VERSION
#define DEVICE_VERSION 2
#endif

#ifndef DEVICE_REVISION
#define DEVICE_REVISION 3
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
    Const fields containing name of device and ID string. Note! It's not the
    version string as in case of executables (i.e. the $VER:), but rather
    "name version.revision (date)" string.
*/
static const char deviceName[] = DEVICE_NAME;
static const char deviceIdString[] = DEVICE_IDSTRING;
static const APTR initTable[4];

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

void openLib(struct IOSana2Req *io asm("a1"), LONG unitNumber asm("d0"), ULONG flags asm("d1"), struct GenetDevice *base asm("a6"));
ULONG closeLib(struct IOSana2Req *io asm("a1"), struct GenetDevice *base asm("a6"));
ULONG expungeLib(struct GenetDevice *base asm("a6"));
APTR extFunc(struct GenetDevice *base asm("a6"));
void beginIO(struct IOSana2Req *io asm("a1"), struct GenetDevice *base asm("a6"));
LONG abortIO(struct IOSana2Req *io asm("a1"), struct GenetDevice *base asm("a6"));

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

APTR initFunction(struct GenetDevice *base asm("d0"), ULONG segList asm("a0"), struct ExecBase *_SysBase asm("a6"))
{
    (void)_SysBase;
    Kprintf("[genet] %s: Initializing device\n", __func__);
    base->segList = segList;
    base->device.dd_Library.lib_Revision = DEVICE_REVISION;
    base->unit = NULL;
    base->utilityBase = NULL;
    base->gic400Base = NULL;

    return base;
}

#define getBufferFunction(tags, tag32, tag16, tagDefault)     \
    ({                                                        \
        APTR func = (APTR)GetTagData(tagDefault, NULL, tags); \
        func = (APTR)GetTagData(tag16, (ULONG)func, tags);    \
        func = (APTR)GetTagData(tag32, (ULONG)func, tags);    \
        func;                                                 \
    })

static struct Opener *createOpener(struct TagItem *tags, struct Library *UtilityBase, const struct GenetRuntimeConfig *config)
{
    struct Opener *opener = NULL;
    opener = AllocMem(sizeof(struct Opener), MEMF_PUBLIC | MEMF_CLEAR);
    if (opener == NULL)
    {
        Kprintf("[genet] %s: Failed to allocate opener\n", __func__);
        return NULL;
    }

    Kprintf("[genet] %s: S2_CopyToBuff %lx\n", __func__, GetTagData(S2_CopyToBuff, NULL, tags));
    Kprintf("[genet] %s: S2_CopyFromBuff %lx\n", __func__, GetTagData(S2_CopyFromBuff, NULL, tags));
    Kprintf("[genet] %s: S2_PacketFilter %lx\n", __func__, GetTagData(S2_PacketFilter, NULL, tags));
    Kprintf("[genet] %s: S2_CopyToBuff16 %lx\n", __func__, GetTagData(S2_CopyToBuff16, NULL, tags));
    Kprintf("[genet] %s: S2_CopyFromBuff16 %lx\n", __func__, GetTagData(S2_CopyFromBuff16, NULL, tags));
    Kprintf("[genet] %s: S2_CopyToBuff32 %lx\n", __func__, GetTagData(S2_CopyToBuff32, NULL, tags));
    Kprintf("[genet] %s: S2_CopyFromBuff32 %lx\n", __func__, GetTagData(S2_CopyFromBuff32, NULL, tags));
    Kprintf("[genet] %s: S2_DMACopyToBuff32 %lx\n", __func__, GetTagData(S2_DMACopyToBuff32, NULL, tags));
    Kprintf("[genet] %s: S2_DMACopyFromBuff32 %lx\n", __func__, GetTagData(S2_DMACopyFromBuff32, NULL, tags));
    Kprintf("[genet] %s: S2_DMACopyToBuff64 %lx\n", __func__, GetTagData(S2_DMACopyToBuff64, NULL, tags));
    Kprintf("[genet] %s: S2_DMACopyFromBuff64 %lx\n", __func__, GetTagData(S2_DMACopyFromBuff64, NULL, tags));
    Kprintf("[genet] %s: S2_Log %lx\n", __func__, GetTagData(S2_Log, NULL, tags));

    opener->packetFilter = (struct Hook *)GetTagData(S2_PacketFilter, NULL, tags);
    opener->CopyToBuff = (BOOL (*)(APTR, APTR, ULONG))getBufferFunction(tags, S2_CopyToBuff32, S2_CopyToBuff16, S2_CopyToBuff);
    opener->CopyFromBuff = (BOOL (*)(APTR, APTR, ULONG))getBufferFunction(tags, S2_CopyFromBuff32, S2_CopyFromBuff16, S2_CopyFromBuff);

    if (config->use_dma)
    {
        opener->DMACopyToBuff = (APTR (*)(APTR))GetTagData(S2_DMACopyToBuff32, NULL, tags);
        opener->DMACopyFromBuff = (APTR (*)(APTR))GetTagData(S2_DMACopyFromBuff32, NULL, tags);
    }

    Kprintf("[genet] %s: CopyToBuff=%lx, CopyFromBuff=%lx, PacketFilter=%lx\n",
            __func__, opener->CopyToBuff, opener->CopyFromBuff, opener->packetFilter);
    Kprintf("[genet] %s: DMACopyToBuff=%lx, DMACopyFromBuff=%lx\n",
            __func__, opener->DMACopyToBuff, opener->DMACopyFromBuff);

    _NewMinList(&opener->readQueue);
    _NewMinList(&opener->orphanQueue);
    _NewMinList(&opener->eventQueue);
    _NewMinList(&opener->ipv4Queue);
    _NewMinList(&opener->arpQueue);

    InitSemaphore(&opener->openerSemaphore);

    return opener;
}

void openLib(struct IOSana2Req *io asm("a1"), LONG unitNumber asm("d0"),
             ULONG flags asm("d1"), struct GenetDevice *base asm("a6"))
{
    BOOL firstOpen = FALSE;
    BOOL createdUnit = FALSE;

    Kprintf("[genet] %s: Opening device with unit number %ld and flags %lx\n", __func__, unitNumber, flags);
    if (unitNumber != 0)
    {
        Kprintf("[genet] %s: Invalid unit number %ld\n", __func__, unitNumber);
        io->ios2_Req.io_Error = IOERR_OPENFAIL;
        return;
    }

    if (io->ios2_Req.io_Message.mn_Length < sizeof(struct IOStdReq))
    {
        Kprintf("[genet] %s: Invalid request length %ld\n", __func__, io->ios2_Req.io_Message.mn_Length);
        io->ios2_Req.io_Error = IOERR_OPENFAIL;
        return;
    }

    if (base->unit == NULL)
    {
        Kprintf("[genet] %s: Allocating unit structure\n", __func__);
        base->unit = AllocMem(sizeof(struct GenetUnit), MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
        if (base->unit == NULL)
        {
            Kprintf("[genet]%s: Failed to allocate unit\n", __func__);
            io->ios2_Req.io_Error = IOERR_OPENFAIL;
            return;
        }
        base->unit->device = base;
        createdUnit = TRUE;
    }

    if (flags & SANA2OPF_MINE && base->unit->unit.unit_OpenCnt > 0)
    {
        Kprintf("[genet] %s: Unit is already open, can't do exclusive access\n", __func__);
        io->ios2_Req.io_Error = IOERR_UNITBUSY;
        return;
    }

    firstOpen = (base->device.dd_Library.lib_OpenCnt == 0);
    if (firstOpen && genet_open_libraries(base) != 0)
    {
        io->ios2_Req.io_Error = IOERR_OPENFAIL;
        if (createdUnit)
        {
            FreeMem(base->unit, sizeof(struct GenetUnit));
            base->unit = NULL;
        }
        return;
    }

    if (base->unit->unit.unit_OpenCnt == 0)
    {
        /* We're reloading configuration only if the device was previously not in use */
        LoadGenetRuntimeConfig(&base->runtimeConfig);
        DumpGenetRuntimeConfig(&base->runtimeConfig);
    }

    struct Opener *opener = NULL;
    if (io->ios2_Req.io_Message.mn_Length >= sizeof(struct IOSana2Req))
    {
        opener = createOpener(io->ios2_BufferManagement, base->utilityBase, &base->runtimeConfig);
        if (opener == NULL)
        {
            io->ios2_Req.io_Error = IOERR_OPENFAIL;
            if (base->unit)
            {
                FreeMem(base->unit, sizeof(struct GenetUnit));
                base->unit = NULL;
            }
            if (firstOpen)
                genet_close_libraries(base);
            return;
        }
        io->ios2_BufferManagement = opener;
    }

    u32 result = UnitOpen(base->unit, (u32)unitNumber, (u32)flags, opener);
    io->ios2_Req.io_Unit = (struct Unit *)base->unit;

    if (result == S2ERR_NO_ERROR)
    {
        Kprintf("[genet] %s: Unit opened successfully\n", __func__);
        base->device.dd_Library.lib_OpenCnt++;
        base->device.dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
        io->ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    }
    else
    {
        Kprintf("[genet] %s: Failed to open unit, error code %ld\n", __func__, result);
        io->ios2_Req.io_Error = IOERR_OPENFAIL;
        if (firstOpen)
            genet_close_libraries(base);
    }

    /* In contrast to normal library there is no need to return anything */
    return;
}

ULONG closeLib(struct IOSana2Req *io asm("a1"), struct GenetDevice *base asm("a6"))
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    Kprintf("[genet] %s: Closing device\n", __func__);

    struct Opener *opener = NULL;
    if (io->ios2_Req.io_Message.mn_Length >= sizeof(struct IOSana2Req))
    {
        opener = io->ios2_BufferManagement;
    }

    u32 result = UnitClose(unit, opener);
    if (result == 0) // last user of Unit disappeared
    {
        Kprintf("[genet] %s: Unit closed successfully, freeing resources\n", __func__);
        FreeMem(unit, sizeof(struct GenetUnit));
        base->unit = NULL;
    }
    if (opener != NULL)
    {
        Kprintf("[genet] %s: Freeing opener resources\n", __func__);
        FreeMem(opener, sizeof(struct Opener));
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
    Kprintf("[genet] %s: Expunging device\n", __func__);
    if (base->device.dd_Library.lib_OpenCnt > 0)
    {
        Kprintf("[genet] %s: Device is still open, cannot expunge\n", __func__);
        base->device.dd_Library.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }
    else
    {
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
