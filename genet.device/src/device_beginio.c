// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <device.h>
#include <devices/sana2.h>
#include <debug.h>
#include <genet/bcmgenet.h>

static inline void Do_CMD_WRITE(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    KprintfH("[genet] %s: CMD_WRITE\n", __func__);

    if (io->ios2_Req.io_Command == S2_BROADCAST)
    {
        KprintfH("[genet] %s: CMD_WRITE with S2_BROADCAST flag, treating as broadcast\n", __func__);
        // Set destination MAC to broadcast address
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
        *(ULONG *)&io->ios2_DstAddr[0] = 0xFFFFFFFF;
        *(UWORD *)&io->ios2_DstAddr[4] = 0xFFFF;
#pragma GCC diagnostic pop
    }

    if (unlikely(unit->state != STATE_ONLINE))
    {
        Kprintf("[genet] %s: Unit is offline, cannot write\n", __func__);
        io->ios2_WireError = S2WERR_UNIT_OFFLINE;
        io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
    }
    else
    {
        bcmgenet_xmit(io, unit);
    }

    if (!(io->ios2_Req.io_Flags & IOF_QUICK))
        ReplyMsg((struct Message *)io);
}

void beginIO(struct IOSana2Req *io asm("a1"), struct GenetDevice *base asm("a6") __attribute__((unused)))
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    UWORD cmd = io->ios2_Req.io_Command;

    if (cmd == CMD_WRITE || cmd == S2_BROADCAST || cmd == S2_MULTICAST)
    {
        KprintfH("[genet] %s: Quick CMD_WRITE\n", __func__);
        Do_CMD_WRITE(io);
        return;
    }

    /* CMD_READ / S2_READORPHAN go through the per-opener SPSC ring.
     * Single producer (this beginIO), single consumer (device task drain).
     */
    if (cmd == CMD_READ || cmd == S2_READORPHAN)
    {
        struct Opener *opener = (struct Opener *)io->ios2_BufferManagement;

        /* Offline check is racy with state changes. */
        if (unlikely(unit->state != STATE_ONLINE))
        {
            io->ios2_WireError = S2WERR_UNIT_OFFLINE;
            io->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            if (!(io->ios2_Req.io_Flags & IOF_QUICK))
                ReplyMsg((struct Message *)io);
            return;
        }

        ULONG p = opener->readRing.producer;
        ULONG c = opener->readRing.consumer;
        if (likely((p - c) <= OPENER_READ_RING_MASK))
        {
            io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
            opener->readRing.entries[p & OPENER_READ_RING_MASK] = io;
            asm volatile("nop");
            opener->readRing.producer = p + 1u;
            KprintfH("[genet] %s: SPSC push cmd=%04lx\n", __func__, (ULONG)cmd);
            return;
        }

        /* Ring full — fall back to MsgPort. The task's Do_CMD_READ /
         * Do_S2_READORPHAN drain the ring before AddTail to preserve FIFO. */
        KprintfH("[genet] %s: SPSC ring full, falling back to MsgPort\n", __func__);
    }

    KprintfH("[genet] %s: Queuing %04lx\n", __func__, (ULONG)cmd);
    io->ios2_Req.io_Error = S2ERR_NO_ERROR;
    io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
    PutMsg(&unit->unit.unit_MsgPort, (struct Message *)io);
}
