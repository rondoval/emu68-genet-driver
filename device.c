// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#define __NOLIBBASE__

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/utility_protos.h>
#else
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
#include <debug.h>
#include "settings.h"

/*
    Put the function at the very beginning of the file in order to avoid
    unexpected results when user executes the device by mistake
*/
static int __attribute__((used)) doNotExecute()
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
    RTF_AUTOINIT | RTF_COLDSTART,
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
APTR initFunction(struct GenetDevice *base asm("d0"), ULONG segList asm("a0"));

static const APTR funcTable[];
static const APTR initTable[4] = {
    (APTR)sizeof(struct GenetDevice),
    (APTR)funcTable,
    NULL,
    (APTR)initFunction};

void openLib(struct IOSana2Req *io asm("a1"), LONG unitNumber asm("d0"),
             ULONG flags asm("d1"), struct GenetDevice *base asm("a6"));
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

APTR initFunction(struct GenetDevice *base asm("d0"), ULONG segList asm("a0"))
{
    struct ExecBase *SysBase = *(struct ExecBase **)4UL;
    Kprintf("[genet] %s: Initializing device\n", __func__);
    base->execBase = SysBase;
    base->segList = segList;
    base->device.dd_Library.lib_Revision = DEVICE_REVISION;
    base->unit = NULL;

    return base;
}

struct Opener *createOpener(struct ExecBase *SysBase, struct TagItem *tags)
{
    struct Opener *opener = NULL;
    opener = AllocMem(sizeof(struct Opener), MEMF_PUBLIC | MEMF_CLEAR);
    if (opener == NULL)
    {
        Kprintf("[genet] %s: Failed to allocate opener\n", __func__);
        return NULL;
    }

    struct Library *UtilityBase = OpenLibrary((CONST_STRPTR) "utility.library", LIB_MIN_VERSION);
    if (UtilityBase == NULL)
    {
        Kprintf("[genet] %s: Failed to open utility.library\n", __func__);
        FreeMem(opener, sizeof(struct Opener));
        return NULL;
    }
#ifdef DEBUG_HIGH
    KprintfH("[genet] %s: S2_CopyToBuff %lx\n", __func__, GetTagData(S2_CopyToBuff, NULL, tags));
    KprintfH("[genet] %s: S2_CopyFromBuff %lx\n", __func__, GetTagData(S2_CopyFromBuff, NULL, tags));
    KprintfH("[genet] %s: S2_PacketFilter %lx\n", __func__, GetTagData(S2_PacketFilter, NULL, tags));
    KprintfH("[genet] %s: S2_CopyToBuff16 %lx\n", __func__, GetTagData(S2_CopyToBuff16, NULL, tags));
    KprintfH("[genet] %s: S2_CopyFromBuff16 %lx\n", __func__, GetTagData(S2_CopyFromBuff16, NULL, tags));
    KprintfH("[genet] %s: S2_CopyToBuff32 %lx\n", __func__, GetTagData(S2_CopyToBuff32, NULL, tags));
    KprintfH("[genet] %s: S2_CopyFromBuff32 %lx\n", __func__, GetTagData(S2_CopyFromBuff32, NULL, tags));
    KprintfH("[genet] %s: S2_DMACopyToBuff32 %lx\n", __func__, GetTagData(S2_DMACopyToBuff32, NULL, tags));
    KprintfH("[genet] %s: S2_DMACopyFromBuff32 %lx\n", __func__, GetTagData(S2_DMACopyFromBuff32, NULL, tags));
    KprintfH("[genet] %s: S2_DMACopyToBuff64 %lx\n", __func__, GetTagData(S2_DMACopyToBuff64, NULL, tags));
    KprintfH("[genet] %s: S2_DMACopyFromBuff64 %lx\n", __func__, GetTagData(S2_DMACopyFromBuff64, NULL, tags));
    KprintfH("[genet] %s: S2_Log %lx\n", __func__, GetTagData(S2_Log, NULL, tags));
#endif
    opener->packetFilter = (struct Hook *)GetTagData(S2_PacketFilter, NULL, tags);
    opener->CopyToBuff = (BOOL (*)(APTR, APTR, ULONG))GetTagData(S2_CopyToBuff, NULL, tags);
    opener->CopyFromBuff = (BOOL (*)(APTR, APTR, ULONG))GetTagData(S2_CopyFromBuff, NULL, tags);
    Kprintf("[genet] %s: CopyToBuff=%lx, CopyFromBuff=%lx, PacketFilter=%lx\n",
            __func__, opener->CopyToBuff, opener->CopyFromBuff, opener->packetFilter);
    CloseLibrary(UtilityBase);

    NewMinList((struct MinList *)&opener->readPort.mp_MsgList);
    opener->readPort.mp_Flags = PA_IGNORE;
    NewMinList((struct MinList *)&opener->orphanPort.mp_MsgList);
    opener->orphanPort.mp_Flags = PA_IGNORE;
    NewMinList((struct MinList *)&opener->eventPort.mp_MsgList);
    opener->eventPort.mp_Flags = PA_IGNORE;

    return opener;
}

void openLib(struct IOSana2Req *io asm("a1"), LONG unitNumber asm("d0"),
             ULONG flags asm("d1"), struct GenetDevice *base asm("a6"))
{
    struct ExecBase *SysBase = base->execBase;
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
    }

    if (flags & SANA2OPF_MINE && base->unit->unit.unit_OpenCnt > 0)
    {
        Kprintf("[genet] %s: Unit is already open, can't do exclusive access\n", __func__);
        io->ios2_Req.io_Error = IOERR_UNITBUSY;
        return;
    }

    struct Opener *opener = NULL;
    if (io->ios2_Req.io_Message.mn_Length >= sizeof(struct IOSana2Req))
    {
        opener = createOpener(SysBase, io->ios2_BufferManagement);
        if (opener == NULL)
        {
            io->ios2_Req.io_Error = IOERR_OPENFAIL;
            if (base->unit)
            {
                FreeMem(base->unit, sizeof(struct GenetUnit));
                base->unit = NULL;
            }
            return;
        }
        io->ios2_BufferManagement = opener;
    }

    int result = UnitOpen(base->unit, unitNumber, flags, opener);
    io->ios2_Req.io_Unit = (struct Unit *)base->unit;

    if (result == 0)
    {
        Kprintf("[genet] %s: Unit opened successfully\n", __func__);
        base->device.dd_Library.lib_OpenCnt++;
        base->device.dd_Library.lib_Flags &= ~LIBF_DELEXP;
        io->ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    }
    else
    {
        Kprintf("[genet] %s: Failed to open unit, error code %ld\n", __func__, result);
        io->ios2_Req.io_Error = IOERR_OPENFAIL;
    }

    /* In contrast to normal library there is no need to return anything */
    return;
}

ULONG closeLib(struct IOSana2Req *io asm("a1"), struct GenetDevice *base asm("a6"))
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    struct ExecBase *SysBase = base->execBase;
    Kprintf("[genet] %s: Closing device\n", __func__);

    struct Opener *opener = io->ios2_BufferManagement;
    int result = UnitClose(unit, opener);
    if (result == 0) // last user of Unit disappeared
    {
        Kprintf("[genet] %s: Unit closed successfully, freeing resources\n", __func__);
        FreeMem(unit, sizeof(struct GenetUnit));
        base->unit = NULL;
    }
    if (opener)
    {
        Kprintf("[genet] %s: Freeing opener resources\n", __func__);
        FreeMem(opener, sizeof(struct Opener));
    }

    base->device.dd_Library.lib_OpenCnt--;

    if (base->device.dd_Library.lib_OpenCnt == 0)
    {
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
        return 0;
    }
    else
    {
        struct ExecBase *SysBase = base->execBase;
        ULONG segList = base->segList;

        /* Remove yourself from list of devices */
        Remove((struct Node *)base);

        /* Calculate size of device base and deallocate memory */
        ULONG size = base->device.dd_Library.lib_NegSize + base->device.dd_Library.lib_PosSize;
        APTR pointer = (APTR)((ULONG)base - base->device.dd_Library.lib_NegSize);
        FreeMem(pointer, size);

        return segList;
    }
}

APTR extFunc(struct GenetDevice *base asm("a6"))
{
    return base;
}
