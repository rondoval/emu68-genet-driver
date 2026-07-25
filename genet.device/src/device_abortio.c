// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <devices/sana2.h>

#include <device.h>
#include <debug.h>

LONG abortIO(struct IOSana2Req *io asm("a1"), struct GenetDevice *base asm("a6") __attribute__((unused)))
{
    /* AbortIO is a *wish* call. Someone would like to abort current IORequest */
    KprintfT("[genet] %s: Aborting IO request %lx\n", __func__, io);

    if (io->ios2_Req.io_Unit != NULL)
    {
        struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;

        /* S2_SAMPLE_THROUGHPUT is owned by the unit task. */
        if (io->ios2_Req.io_Command == S2_SAMPLE_THROUGHPUT)
        {
            UnitSubmitControlAsync(unit, UNIT_CTRL_THROUGHPUT_ABORT, (union UnitControlPayload){ .io = io });
            return 0;
        }

        if (io->ios2_Req.io_Command == S2_ONEVENT)
        {
            UnitSubmitControlAsync(unit, UNIT_CTRL_EVENT_ABORT, (union UnitControlPayload){ .io = io });
            return 0;
        }

        /* CMD_READ / S2_READORPHAN may live in the per-opener SPSC ring or
         * (after drain) in a per-type MinList. SPSC has no random remove, so
         * we mark in place; the device task replies with IOERR_ABORTED on
         * drain. */
        const UWORD cmd = io->ios2_Req.io_Command;
        Forbid();
        if ((cmd == CMD_READ || cmd == S2_READORPHAN) && io->ios2_BufferManagement != NULL)
        {
            struct Opener *opener = (struct Opener *)io->ios2_BufferManagement;
            ULONG c = opener->readRing.consumer;
            ULONG p = opener->readRing.producer;
            for (ULONG i = c; i != p; i++)
            {
                if (opener->readRing.entries[i & OPENER_READ_RING_MASK] == io)
                {
                    io->ios2_Req.io_Error = IOERR_ABORTED;
                    io->ios2_WireError    = S2WERR_GENERIC_ERROR;
                    Permit();
                    KprintfT("[genet] %s: Marked IO %lx in SPSC ring as aborted\n", __func__, io);
                    return 0;
                }
            }
            /* Not in ring — fall through to MinList check below. */
        }

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
    KprintfT("[genet] %s: IO request %lx aborted\n", __func__, io);
    return 0;
}
