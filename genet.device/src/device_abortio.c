// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <devices/sana2.h>

#include <device.h>
#include <debug.h>

LONG abortIO(struct IOSana2Req *io asm("a1"), struct GenetDevice *base asm("a6") __attribute__((unused)))
{
    /* AbortIO is a *wish* call. Someone would like to abort current IORequest */
    KprintfH("[genet] %s: Aborting IO request %lx\n", __func__, io);

    if (io->ios2_Req.io_Unit != NULL)
    {
        Forbid();
        /* If the IO was not quick and is of type message (not handled yet or in process), abord it and remove from queue. 
         * The TX task clears ln_Pred to indicate the request is already on TX ring and can't be cancelled. */
        if ((io->ios2_Req.io_Flags & IOF_QUICK) == 0 && io->ios2_Req.io_Message.mn_Node.ln_Type == NT_MESSAGE && io->ios2_Req.io_Message.mn_Node.ln_Pred != NULL)
        {
            Remove(&io->ios2_Req.io_Message.mn_Node);
            io->ios2_Req.io_Error = IOERR_ABORTED;
            io->ios2_WireError = S2WERR_GENERIC_ERROR;
            ReplyMsg(&io->ios2_Req.io_Message);
        }
        Permit();
    }
    KprintfH("[genet] %s: IO request %lx aborted\n", __func__, io);
    return 0;
}
