// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#define __NOLIBBASE__

#ifdef __INTELLISENSE__
#include <clib/timer_protos.h>
#include <clib/exec_protos.h>
#else
#include <proto/timer.h>
#include <proto/exec.h>
#endif

#include <devices/sana2.h>
#include <devices/sana2specialstats.h>
#include <devices/newstyle.h>

#include <device.h>
#include <debug.h>
#include <compat.h>

static const UWORD GENET_SupportedCommands[] = {
    CMD_FLUSH,
    CMD_READ,
    CMD_WRITE,

    S2_DEVICEQUERY,
    S2_GETSTATIONADDRESS,
    S2_CONFIGINTERFACE,
    S2_ADDMULTICASTADDRESS,
    S2_DELMULTICASTADDRESS,
    S2_MULTICAST,
    S2_BROADCAST,
    // S2_TRACKTYPE,
    // S2_UNTRACKTYPE,
    // S2_GETTYPESTATS,
    // S2_GETSPECIALSTATS,
    S2_GETGLOBALSTATS,
    S2_ONEVENT,
    S2_READORPHAN,
    S2_ONLINE,
    S2_OFFLINE,
    S2_ADDMULTICASTADDRESSES,
    S2_DELMULTICASTADDRESSES,

    NSCMD_DEVICEQUERY,
    0};

/* Mask of events known by the driver */
#define EVENT_MASK (S2EVENT_ONLINE | S2EVENT_OFFLINE |       \
                    S2EVENT_TX | S2EVENT_RX | S2EVENT_BUFF | \
                    S2EVENT_ERROR | S2EVENT_HARDWARE | S2EVENT_SOFTWARE)

/* Report events to this unit */
void ReportEvents(struct GenetUnit *unit, ULONG eventSet)
{
    KprintfH("[genet] %s: Reporting events %08lx\n", __func__, eventSet);
    struct ExecBase *SysBase = unit->execBase;

    /* Report event to every listener of every opener accepting the mask */
    ObtainSemaphore(&unit->semaphore);
    for (struct MinNode *node = unit->openers.mlh_Head; node->mln_Succ; node = node->mln_Succ)
    {
        struct Opener *opener = (struct Opener *)node;
        struct Node *ioNode, *nextIoNode;

        for (ioNode = opener->eventPort.mp_MsgList.lh_Head; (nextIoNode = ioNode->ln_Succ) != NULL; ioNode = nextIoNode)
        {
            struct IOSana2Req *io = (struct IOSana2Req *)ioNode;
            /* Check if event mask in WireError fits the events occured */
            if (io->ios2_WireError & eventSet)
            {
                /* We have a match. Leave only matching events in wire error */
                io->ios2_WireError &= eventSet;

                /* Reply it */
                Remove((struct Node *)io);
                ReplyMsg((struct Message *)io);
            }
        }
    }
    ReleaseSemaphore(&unit->semaphore);
    KprintfH("[genet] %s: Reporting done\n", __func__);
}

static int Do_S2_ONEVENT(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    struct ExecBase *SysBase = unit->execBase;
    KprintfH("[genet] %s: S2_ONEVENT %08lx\n", __func__, io->ios2_WireError);

    ULONG preset;
    if (unit->state == STATE_ONLINE)
        preset = S2EVENT_ONLINE;
    else
        preset = S2EVENT_OFFLINE;

    /* If any unsupported events are requested, report an error */
    if (io->ios2_WireError & ~(EVENT_MASK))
    {
        Kprintf("[genet] %s: Unsupported event requested: %08lx\n", __func__, io->ios2_WireError);
        io->ios2_Req.io_Error = S2ERR_NOT_SUPPORTED;
        io->ios2_WireError = S2WERR_BAD_EVENT;
        return COMMAND_PROCESSED;
    }

    /* If expected flags match preset, return back (almost) immediately */
    if (io->ios2_WireError & preset)
    {
        KprintfH("[genet] %s: Event preset %08lx matches requested %08lx, returning immediately\n", __func__, preset, io->ios2_WireError);
        io->ios2_WireError &= preset;
        return COMMAND_PROCESSED;
    }
    else
    {
        KprintfH("[genet] %s: Adding to event listener list, preset %08lx\n", __func__, preset);
        /* Remove QUICK flag and put message on event listener list */
        struct Opener *opener = io->ios2_BufferManagement;
        io->ios2_Req.io_Flags &= ~IOF_QUICK;
        PutMsg(&opener->eventPort, (struct Message *)io);
        return COMMAND_SCHEDULED;
    }
}

static int Do_CMD_FLUSH(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    struct ExecBase *SysBase = unit->execBase;
    KprintfH("[genet] %s: CMD_FLUSH\n", __func__);

    struct IOSana2Req *req;
    /* Flush and cancel all write requests */
    /*
     * likely nothing to do here,
     * we're pushing packets straight to the ring buffer
     * TODO double check once things settle down
     */
    // while ((req = (struct IOSana2Req *)GetMsg(sdio->s_SenderPort)))
    // {
    //     req->ios2_Req.io_Error = IOERR_ABORTED;
    //     req->ios2_WireError = 0;
    //     ReplyMsg((struct Message *)req);
    // }

    /* For every opener, flush orphan and even queues */
    for (struct MinNode *node = unit->openers.mlh_Head; node->mln_Succ; node = node->mln_Succ)
    {
        struct Opener *opener = (struct Opener *)node;
        while ((req = (struct IOSana2Req *)GetMsg(&opener->orphanPort)))
        {
            req->ios2_Req.io_Error = IOERR_ABORTED;
            req->ios2_WireError = 0;
            ReplyMsg((struct Message *)req);
        }

        while ((req = (struct IOSana2Req *)GetMsg(&opener->eventPort)))
        {
            req->ios2_Req.io_Error = IOERR_ABORTED;
            req->ios2_WireError = 0;
            ReplyMsg((struct Message *)req);
        }

        while ((req = (struct IOSana2Req *)GetMsg(&opener->readPort)))
        {
            req->ios2_Req.io_Error = IOERR_ABORTED;
            req->ios2_WireError = 0;
            ReplyMsg((struct Message *)req);
        }
    }
    KprintfH("[genet] %s: Flush completed\n", __func__);

    return COMMAND_PROCESSED;
}

static int Do_NSCMD_DEVICEQUERY(struct IOStdReq *io)
{
    KprintfH("[genet] %s: NSCMD_DEVICEQUERY\n", __func__);
    struct NSDeviceQueryResult *dq = io->io_Data;

    /* Fill out structure */
    dq->nsdqr_DeviceType = NSDEVTYPE_SANA2;
    dq->nsdqr_DeviceSubType = 0;
    dq->nsdqr_SupportedCommands = (UWORD *)GENET_SupportedCommands;
    io->io_Actual = sizeof(struct NSDeviceQueryResult) + sizeof(APTR);
    dq->nsdqr_SizeAvailable = io->io_Actual;
    io->io_Error = 0;

    return COMMAND_PROCESSED;
}

static inline int Do_CMD_READ(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    struct ExecBase *SysBase = unit->execBase;
    KprintfH("[genet] %s: CMD_READ\n", __func__);

    if (unlikely(unit->state != STATE_ONLINE))
    {
        Kprintf("[genet] %s: Unit is offline, cannot read\n", __func__);
        io->ios2_WireError = S2WERR_UNIT_OFFLINE;
        io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
        return COMMAND_PROCESSED;
    }

    struct Opener *opener = io->ios2_BufferManagement;
    io->ios2_Req.io_Flags &= ~IOF_QUICK;
    PutMsg(&opener->readPort, (struct Message *)io);
    return COMMAND_SCHEDULED;
}

static inline int Do_S2_READORPHAN(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    struct ExecBase *SysBase = unit->execBase;
    KprintfH("[genet] %s: S2_READORPHAN\n", __func__);

    if (unlikely(unit->state != STATE_ONLINE))
    {
        Kprintf("[genet] %s: Unit is offline, cannot read orphan\n", __func__);
        io->ios2_WireError = S2WERR_UNIT_OFFLINE;
        io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
        return COMMAND_PROCESSED;
    }

    struct Opener *opener = io->ios2_BufferManagement;
    io->ios2_Req.io_Flags &= ~IOF_QUICK;
    PutMsg(&opener->orphanPort, (struct Message *)io);
    return COMMAND_SCHEDULED;
}

static inline int Do_CMD_WRITE(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    KprintfH("[genet] %s: CMD_WRITE\n", __func__);

    if (unlikely(unit->state != STATE_ONLINE))
    {
        Kprintf("[genet] %s: Unit is offline, cannot write\n", __func__);
        io->ios2_WireError = S2WERR_UNIT_OFFLINE;
        io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
        return COMMAND_PROCESSED;
    }

    io->ios2_Req.io_Flags &= ~IOF_QUICK;
    return bcmgenet_tx_poll(unit, io);
}

int Do_S2_DEVICEQUERY(struct IOSana2Req *io)
{
    Kprintf("[genet] %s: S2_DEVICEQUERY\n", __func__);

    struct Sana2DeviceQuery *info = io->ios2_StatData;

    info->SizeSupplied = sizeof(struct Sana2DeviceQuery) - sizeof(info->RawMTU);
    info->DevQueryFormat = 0;
    info->DeviceLevel = 0;
    info->AddrFieldSize = 48;
    info->MTU = ETH_DATA_LEN;
    info->BPS = 1000000000;
    info->HardwareType = S2WireType_Ethernet;
    if (info->SizeAvailable >= sizeof(struct Sana2DeviceQuery))
    {
        info->RawMTU = ETH_DATA_LEN + ETH_HLEN + VLAN_HLEN;
        info->SizeSupplied += sizeof(info->RawMTU);
    }
    return COMMAND_PROCESSED;
}

static int Do_S2_ONLINE(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    struct TimerBase *TimerBase = unit->timerBase;
    Kprintf("[genet] %s: S2_ONLINE\n", __func__);

    /* If unit was not yet online, report event now */
    if (unit->state != STATE_ONLINE)
    {
        Kprintf("[genet] %s: Bringing unit online\n", __func__);
        _memset(&unit->stats, 0, sizeof(unit->stats));
        GetSysTime(&unit->stats.LastStart);
        Kprintf("[genet] %s: statistics zeroed, LastStart: %ld\n", __func__, unit->stats.LastStart.tv_secs);

        int result = UnitOnline(unit);
        if (result != S2ERR_NO_ERROR)
        {
            Kprintf("[genet] %s: Failed to bring unit online: %ld\n", __func__, result);
            io->ios2_Req.io_Error = result;
            io->ios2_WireError = S2WERR_GENERIC_ERROR;
            ReportEvents(unit, S2EVENT_SOFTWARE | S2EVENT_ERROR);
            return COMMAND_PROCESSED;
        }
        Kprintf("[genet] %s: Unit online, about to report events\n", __func__);
        ReportEvents(unit, S2EVENT_ONLINE);
    }

    return COMMAND_PROCESSED;
}

static int Do_S2_CONFIGINTERFACE(struct IOSana2Req *io)
{
    struct ExecBase *SysBase = *((struct ExecBase **)4UL);
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    Kprintf("[genet] %s: S2_CONFIGINTERFACE\n", __func__);

    if (unit->state == STATE_UNCONFIGURED)
    {
        CopyMem(io->ios2_SrcAddr, unit->currentMacAddress, sizeof(unit->currentMacAddress));
        Kprintf("[genet] %s: Setting current MAC address to %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                __func__,
                unit->currentMacAddress[0], unit->currentMacAddress[1],
                unit->currentMacAddress[2], unit->currentMacAddress[3],
                unit->currentMacAddress[4], unit->currentMacAddress[5]);

        int result = UnitConfigure(unit);
        if (result != S2ERR_NO_ERROR)
        {
            Kprintf("[genet] %s: Failed to configure unit: %ld\n", __func__, result);
            io->ios2_Req.io_Error = result;
            io->ios2_WireError = S2WERR_GENERIC_ERROR;
            ReportEvents(unit, S2EVENT_SOFTWARE | S2EVENT_ERROR);
        }
    }

    CopyMem(unit->currentMacAddress, io->ios2_SrcAddr, sizeof(unit->currentMacAddress));
    return COMMAND_PROCESSED;
}

static int Do_S2_OFFLINE(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    // struct IOSana2Req *req;
    Kprintf("[genet] %s: S2_OFFLINE\n", __func__);

    /* Flush and cancel all write requests */
    /*
     * likely nothing to do here,
     * we're pushing packets straight to the ring buffer
     * TODO double check once things settle down
     */
    // while ((req = (struct IOSana2Req *)GetMsg(sdio->s_SenderPort)))
    // {
    //     req->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
    //     req->ios2_WireError = S2WERR_UNIT_OFFLINE;
    //     ReplyMsg((struct Message *)req);
    // }

    /* If unit was ONLINE before, report offline event now */
    if (unit->state == STATE_ONLINE)
    {
        UnitOffline(unit);
        ReportEvents(unit, S2EVENT_OFFLINE);
    }

    return COMMAND_PROCESSED;
}

void ProcessCommand(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    struct ExecBase *SysBase = unit->execBase;
    ObtainSemaphore(&unit->semaphore);

    ULONG complete = COMMAND_SCHEDULED;

    /*
        Only NSCMD_DEVICEQUERY can use standard sized request. All other must be of
        size IOSana2Req
    */
    if (io->ios2_Req.io_Message.mn_Length < sizeof(struct IOSana2Req) &&
        io->ios2_Req.io_Command != NSCMD_DEVICEQUERY)
    {
        io->ios2_Req.io_Error = IOERR_BADLENGTH;
        complete = COMMAND_PROCESSED;
    }
    else
    {
        io->ios2_Req.io_Error = S2ERR_NO_ERROR;

        switch (io->ios2_Req.io_Command)
        {
        case S2_BROADCAST: /* Fallthrough */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
            *(ULONG *)&io->ios2_DstAddr[0] = 0xFFFFFFFF;
            *(UWORD *)&io->ios2_DstAddr[4] = 0xFFFF;
#pragma GCC diagnostic pop
        case S2_MULTICAST: /* Fallthrough */
        case CMD_WRITE:
            complete = Do_CMD_WRITE(io);
            break;

        case CMD_READ:
            complete = Do_CMD_READ(io);
            break;

        case CMD_FLUSH:
            complete = Do_CMD_FLUSH(io);
            break;

        case NSCMD_DEVICEQUERY:
            complete = Do_NSCMD_DEVICEQUERY((struct IOStdReq *)io);
            break;

        case S2_DEVICEQUERY:
            complete = Do_S2_DEVICEQUERY(io);
            break;

        case S2_GETSTATIONADDRESS:
            Kprintf("[genet] %s: S2_GETSTATIONADDRESS\n", __func__);
            CopyMem(unit->localMacAddress, io->ios2_DstAddr, 6);
            CopyMem(unit->currentMacAddress, io->ios2_SrcAddr, 6);
            io->ios2_Req.io_Error = S2ERR_NO_ERROR;
            complete = COMMAND_PROCESSED;
            break;

        case S2_GETGLOBALSTATS:
            KprintfH("[genet] %s: S2_GETGLOBALSTATS\n", __func__);
            CopyMem(&unit->stats, io->ios2_StatData, sizeof(struct Sana2DeviceStats));
            io->ios2_Req.io_Error = S2ERR_NO_ERROR;
            complete = COMMAND_PROCESSED;
            break;

        case S2_ADDMULTICASTADDRESS: /* Fallthrough */
        case S2_ADDMULTICASTADDRESSES:
            complete = Do_S2_ADDMULTICASTADDRESSES(io);
            break;

        case S2_DELMULTICASTADDRESS: /* Fallthrough */
        case S2_DELMULTICASTADDRESSES:
            complete = Do_S2_DELMULTICASTADDRESSES(io);
            break;

        case S2_CONFIGINTERFACE:
            complete = Do_S2_CONFIGINTERFACE(io);
            break;

        case S2_ONLINE:
            complete = Do_S2_ONLINE(io);
            break;

        case S2_OFFLINE:
            complete = Do_S2_OFFLINE(io);
            break;

        case S2_READORPHAN:
            complete = Do_S2_READORPHAN(io);
            break;

        case S2_ONEVENT:
            complete = Do_S2_ONEVENT(io);
            break;

        default:
            io->ios2_Req.io_Error = IOERR_NOCMD;
            complete = COMMAND_PROCESSED;
            break;
        }
    }

    // If command is complete and not quick, reply it now
    if (complete == COMMAND_PROCESSED && !(io->ios2_Req.io_Flags & IOF_QUICK))
    {
        ReplyMsg((struct Message *)io);
    }
    ReleaseSemaphore(&unit->semaphore);
}
