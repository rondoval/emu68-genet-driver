// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <device.h>
#include <devices/sana2.h>
#include <debug.h>

void beginIO(struct IOSana2Req *io asm("a1"), struct GenetDevice *base asm("a6") __attribute__((unused)))
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;

    if ((io->ios2_Req.io_Command == CMD_WRITE || io->ios2_Req.io_Command == S2_BROADCAST) && AttemptSemaphore(&unit->tx_ring.tx_ring_sem))
    {
        KprintfH("[genet] %s: Quick CMD_WRITE\n", __func__);
        if (unit->tx_ring.free_bds < 10)
            bcmgenet_tx_reclaim(unit);
        ProcessCommand(io);
        ReleaseSemaphore(&unit->tx_ring.tx_ring_sem);
    }
    else if (io->ios2_Req.io_Command == CMD_READ && AttemptSemaphore(&((struct Opener *)io->ios2_BufferManagement)->openerSemaphore))
    {
        KprintfH("[genet] %s: Quick CMD_READ\n", __func__);
        ProcessCommand(io);
        ReleaseSemaphore(&((struct Opener *)io->ios2_BufferManagement)->openerSemaphore);
    }
    else
    {
        KprintfH("[genet] %s: Queuing %04lx\n", __func__, io->ios2_Req.io_Command);
        io->ios2_Req.io_Error = S2ERR_NO_ERROR;
        io->ios2_Req.io_Flags &= ~IOF_QUICK;
        PutMsg(&unit->unit.unit_MsgPort, (struct Message *)io);
    }
}
