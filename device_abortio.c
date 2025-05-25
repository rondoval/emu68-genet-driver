#define __NOLIBBASE__
#include <exec/types.h>
#include <exec/resident.h>
#include <exec/io.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <dos/dosextens.h>

#include <proto/exec.h>

#include "settings.h"
#include <device.h>
#include <devices/sana2.h>
#include <devices/sana2specialstats.h>

#include <debug.h>

LONG abortIO(struct IOSana2Req *io asm("a1"))
{
    /* AbortIO is a *wish* call. Someone would like to abort current IORequest */
    struct GenetDevice *base = (struct GenetDevice *)io->ios2_Req.io_Device;
    struct ExecBase *SysBase = base->execBase;
    KprintfH("[genet] %s: Aborting IO request %lx\n", __func__, io);

    /* AbortIO is a *wish* call. Someone would like to abort current IORequest */
    if (io->ios2_Req.io_Unit != NULL)
    {
        ObtainSemaphore(&((struct GenetUnit*)io->ios2_Req.io_Unit)->semaphore);
        /* If the IO was not quick and is of type message (not handled yet or in process), abord it and remove from queue */
        if ((io->ios2_Req.io_Flags & IOF_QUICK) == 0 && io->ios2_Req.io_Message.mn_Node.ln_Type == NT_MESSAGE)
        {
            Remove(&io->ios2_Req.io_Message.mn_Node);
            io->ios2_Req.io_Error = IOERR_ABORTED;
            io->ios2_WireError = S2WERR_GENERIC_ERROR;
            ReplyMsg(&io->ios2_Req.io_Message);
        }
        ReleaseSemaphore(&((struct GenetUnit*)io->ios2_Req.io_Unit)->semaphore);
    }
    KprintfH("[genet] %s: IO request %lx aborted\n", __func__, io);
    return 0;
}
