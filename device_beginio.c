// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#define __NOLIBBASE__

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include "settings.h"
#include <device.h>
#include <devices/sana2.h>
#include <debug.h>

void beginIO(struct IOSana2Req *io asm("a1"), struct GenetDevice *base asm("a6"))
{
    struct ExecBase *SysBase = base->execBase;
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;

    /* experimental workaround for application bugs */
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_UNKNOWN;

    // Try to do the request directly by obtaining the lock, otherwise put it in unit's CMD queue
    if (AttemptSemaphore(&unit->semaphore))
    {
        KprintfH("[genet] %s: QUICK processing %04lx\n", __func__, io->ios2_Req.io_Command);
        ProcessCommand(io);
        ReleaseSemaphore(&unit->semaphore);
    }
    else
    {
        KprintfH("[genet] %s: Queuing %04lx\n", __func__, io->ios2_Req.io_Command);
        /* Unit was busy, remove QUICK flag so that Exec will wait for completion properly */
        io->ios2_Req.io_Error = S2ERR_NO_ERROR;
        io->ios2_Req.io_Flags &= ~IOF_QUICK;
        PutMsg(&unit->unit.unit_MsgPort, (struct Message *)io);
    }
}
