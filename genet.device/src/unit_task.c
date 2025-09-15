// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/timer_protos.h>
#else
#include <proto/exec.h>
#include <proto/timer.h>
#endif

#include <dos/dos.h>

#include <genet/bcmgenet-regs.h>
#include <compat.h>
#include <device.h>
#include <minlist.h>
#include <debug.h>
#include <runtime_config.h>

struct Device *TimerBase = NULL;

static inline BOOL ProcessReceive(struct GenetUnit *unit)
{
    UBYTE *buffer = NULL;
    int pkt_len;
    BOOL activity = FALSE;

    while (TRUE)
    {
        pkt_len = bcmgenet_gmac_eth_recv(unit, &buffer);
        if (pkt_len <= 0)
            break;
        activity |= ReceiveFrame(unit, buffer, pkt_len);
        bcmgenet_gmac_free_pkt(unit);
    }

    if (activity && genetConfig.rx_poll_burst > 0)
    {
        ULONG empty_streak = 0;
        ULONG iter = 0;
        while (iter < genetConfig.rx_poll_burst)
        {
            pkt_len = bcmgenet_gmac_eth_recv(unit, &buffer);
            if (pkt_len <= 0)
            {
                if (++empty_streak >= genetConfig.rx_poll_burst_idle_break)
                    break;
            }
            else
            {
                empty_streak = 0;
                ReceiveFrame(unit, buffer, pkt_len);
                bcmgenet_gmac_free_pkt(unit);
            }
            iter++;
        }
    }
    return activity;
}

static void UnitTask(struct GenetUnit *unit, struct Task *parent)
{
    // Initialize the built in msg port, we'll receive commands here
    _NewMinList((struct MinList *)&unit->unit.unit_MsgPort.mp_MsgList);
    unit->unit.unit_MsgPort.mp_SigTask = FindTask(NULL);
    unit->unit.unit_MsgPort.mp_SigBit = AllocSignal(-1);
    unit->unit.unit_MsgPort.mp_Flags = PA_SIGNAL;
    unit->unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;

    // Create a timer, we'll use it to poll the PHY
    struct MsgPort *microHZTimerPort = CreateMsgPort();
    struct MsgPort *vblankTimerPort = CreateMsgPort();
    unit->openerPort = CreateMsgPort();
    struct timerequest *packetTimerReq = CreateIORequest(microHZTimerPort, sizeof(struct timerequest));
    struct timerequest *statsTimerReq = CreateIORequest(vblankTimerPort, sizeof(struct timerequest));
    if (microHZTimerPort == NULL || vblankTimerPort == NULL || unit->openerPort == NULL || packetTimerReq == NULL || statsTimerReq == NULL)
    {
        Kprintf("[genet] %s: Failed to create timer msg port or request\n", __func__);
        DeleteMsgPort(microHZTimerPort);
        DeleteMsgPort(vblankTimerPort);
        DeleteMsgPort(unit->openerPort);
        DeleteIORequest((struct IORequest *)packetTimerReq);
        DeleteIORequest((struct IORequest *)statsTimerReq);
        Signal(parent, SIGBREAKF_CTRL_C);
        return;
    }

    UBYTE ret = OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)packetTimerReq, LIB_MIN_VERSION);
    UBYTE ret2 = OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)statsTimerReq, LIB_MIN_VERSION);
    if (ret || ret2)
    {
        Kprintf("[genet] %s: Failed to open timer device ret=%d, %d\n", __func__, ret, ret2);
        DeleteMsgPort(microHZTimerPort);
        DeleteMsgPort(vblankTimerPort);
        DeleteIORequest((struct IORequest *)packetTimerReq);
        DeleteIORequest((struct IORequest *)statsTimerReq);
        Signal(parent, SIGBREAKF_CTRL_C);
        return;
    }

    /* used to reset stats on S2_ONLINE */
    TimerBase = packetTimerReq->tr_node.io_Device;

    ULONG backoff_idx = genetConfig.poll_delay_len - 1; /* Start conservative until first activity */
    ULONG delay = genetConfig.poll_delay_us[backoff_idx];

    // Set a timer... we need to pull on RX
    packetTimerReq->tr_node.io_Command = TR_ADDREQUEST;
    packetTimerReq->tr_time.tv_secs = 0;
    packetTimerReq->tr_time.tv_micro = delay;
    SendIO(&packetTimerReq->tr_node);

    statsTimerReq->tr_node.io_Command = TR_ADDREQUEST;
    statsTimerReq->tr_time.tv_secs = 15;
    statsTimerReq->tr_time.tv_micro = 0;
    SendIO(&statsTimerReq->tr_node);

    unit->task = FindTask(NULL);
    /* Signal parent that Unit task is up and running now */
    Signal(parent, SIGBREAKF_CTRL_F);

    ULONG sigset;
    BOOL activity = FALSE;
    ULONG waitMask = (1UL << unit->unit.unit_MsgPort.mp_SigBit) |
                     (1UL << unit->openerPort->mp_SigBit) |
                     (1UL << microHZTimerPort->mp_SigBit) |
                     (1UL << vblankTimerPort->mp_SigBit) |
                     SIGBREAKF_CTRL_C;

    do
    {
        sigset = Wait(waitMask);

        // IO queue got a new message
        if (sigset & (1UL << unit->unit.unit_MsgPort.mp_SigBit))
        {
            activity = TRUE;
            struct IOSana2Req *io;
            // Drain command queue and process it
            while ((io = (struct IOSana2Req *)GetMsg(&unit->unit.unit_MsgPort)))
            {
                ProcessCommand(io);
            }
        }

        // Opener management messages
        if (unlikely(sigset & (1UL << unit->openerPort->mp_SigBit)))
        {
            struct OpenerControlMsg *omsg;
            while ((omsg = (struct OpenerControlMsg *)GetMsg(unit->openerPort)))
            {
                switch (omsg->command)
                {
                case OPENER_CMD_ADD:
                    AddTailMinList(&unit->openers, (struct MinNode *)omsg->opener);
                    break;
                case OPENER_CMD_REM:
                    if (omsg->opener)
                        RemoveMinNode((struct MinNode *)omsg->opener);
                    break;
                }
                ReplyMsg(&omsg->msg);
            }
        }

        if (unit->state == STATE_ONLINE)
        {
            activity |= ProcessReceive(unit);
        }

        // Timer expired, query PHY for link state
        if (sigset & (1UL << microHZTimerPort->mp_SigBit))
        {
            if (CheckIO(&packetTimerReq->tr_node))
            {
                WaitIO(&packetTimerReq->tr_node);
            }

            /* Periodic TX reclaim */
            if (unit->state == STATE_ONLINE)
                bcmgenet_tx_reclaim(unit);

            // TODO pool PHY for state

            if (activity || unit->tx_watchdog_fast_ticks)
            {
                backoff_idx = 0;
                if (unit->tx_watchdog_fast_ticks)
                    --unit->tx_watchdog_fast_ticks;
            }
            else
            {
                if (backoff_idx + 1 < genetConfig.poll_delay_len)
                    backoff_idx++;
            }
            activity = FALSE; /* reset activity */
            delay = genetConfig.poll_delay_us[backoff_idx];

            /* TX watchdog soft cap: ensure we never sleep beyond this while descriptors outstanding */
            if (unit->tx_ring.free_bds < TX_DESCS && delay > genetConfig.tx_reclaim_soft_us)
                delay = genetConfig.tx_reclaim_soft_us;

            /* Re-arm timer */
            packetTimerReq->tr_node.io_Command = TR_ADDREQUEST;
            packetTimerReq->tr_time.tv_secs = 0;
            packetTimerReq->tr_time.tv_micro = delay;
            SendIO(&packetTimerReq->tr_node);
        }

        if (sigset & (1UL << vblankTimerPort->mp_SigBit))
        {
            if(CheckIO(&statsTimerReq->tr_node))
            {
                WaitIO(&statsTimerReq->tr_node);
            }
            Kprintf("[genet] %s: Internal stats:\n", __func__);
            Kprintf("[genet] %s: RX packets: %ld\n", __func__, unit->internalStats.rx_packets);
            Kprintf("[genet] %s: RX bytes: %ld\n", __func__, unit->internalStats.rx_bytes);
            Kprintf("[genet] %s: RX dropped: %ld\n", __func__, unit->internalStats.rx_dropped);
            Kprintf("[genet] %s: RX ARP/IP dropped: %ld\n", __func__, unit->internalStats.rx_arp_ip_dropped);
            Kprintf("[genet] %s: RX overruns: %ld\n", __func__, unit->internalStats.rx_overruns);
            Kprintf("[genet] %s: TX packets: %ld\n", __func__, unit->internalStats.tx_packets);
            Kprintf("[genet] %s: TX bytes: %ld\n", __func__, unit->internalStats.tx_bytes);
            Kprintf("[genet] %s: TX DMA: %ld\n", __func__, unit->internalStats.tx_dma);
            Kprintf("[genet] %s: TX copy: %ld\n", __func__, unit->internalStats.tx_copy);
            Kprintf("[genet] %s: TX dropped: %ld\n", __func__, unit->internalStats.tx_dropped);

            statsTimerReq->tr_node.io_Command = TR_ADDREQUEST;
            statsTimerReq->tr_time.tv_secs = 15;
            statsTimerReq->tr_time.tv_micro = 0;
            SendIO(&statsTimerReq->tr_node);
        }
        if (sigset & SIGBREAKF_CTRL_C)
        {
            Kprintf("[genet] %s: Received SIGBREAKF_CTRL_C, stopping genet task\n", __func__);
            AbortIO(&packetTimerReq->tr_node);
            WaitIO(&packetTimerReq->tr_node);
            AbortIO(&statsTimerReq->tr_node);
            WaitIO(&statsTimerReq->tr_node);
        }
    } while ((sigset & SIGBREAKF_CTRL_C) == 0);

    FreeSignal(unit->unit.unit_MsgPort.mp_SigBit);
    CloseDevice(&packetTimerReq->tr_node);
    DeleteIORequest(&packetTimerReq->tr_node);
    DeleteIORequest(&statsTimerReq->tr_node);
    DeleteMsgPort(microHZTimerPort);
    DeleteMsgPort(vblankTimerPort);
    DeleteMsgPort(unit->openerPort);
    unit->task = NULL;
}

int UnitTaskStart(struct GenetUnit *unit)
{
    Kprintf("[genet] %s: genet task starting\n", __func__);

    // Get all memory we need for the receiver task
    struct MemList *ml = AllocMem(sizeof(struct MemList) + sizeof(struct MemEntry), MEMF_PUBLIC | MEMF_CLEAR);
    struct Task *task = AllocMem(sizeof(struct Task), MEMF_PUBLIC | MEMF_CLEAR);
    ULONG *stack = AllocMem(genetConfig.unit_stack_bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (!ml || !task || !stack)
    {
        Kprintf("[genet] %s: Failed to allocate memory for genet task\n", __func__);
        if (ml)
            FreeMem(ml, sizeof(struct MemList) + sizeof(struct MemEntry));
        if (task)
            FreeMem(task, sizeof(struct Task));
        if (stack)
            FreeMem(stack, genetConfig.unit_stack_bytes);
        return S2ERR_NO_RESOURCES;
    }

    // Prepare mem list, put task and its stack there
    ml->ml_NumEntries = 2;
    ml->ml_ME[0].me_Un.meu_Addr = task;
    ml->ml_ME[0].me_Length = sizeof(struct Task);

    ml->ml_ME[1].me_Un.meu_Addr = &stack[0];
    ml->ml_ME[1].me_Length = genetConfig.unit_stack_bytes;

    // Set up stack
    task->tc_SPLower = &stack[0];
    task->tc_SPUpper = &stack[genetConfig.unit_stack_bytes / sizeof(ULONG)];

    // Push ThisTask and Unit on the stack
    stack = (ULONG *)task->tc_SPUpper;
    *--stack = (ULONG)FindTask(NULL);
    *--stack = (ULONG)unit;
    task->tc_SPReg = stack;

    task->tc_Node.ln_Name = "genet rx/tx";
    task->tc_Node.ln_Type = NT_TASK;
    task->tc_Node.ln_Pri = genetConfig.unit_task_priority;

    _NewMinList((struct MinList *)&task->tc_MemEntry);
    AddHead(&task->tc_MemEntry, &ml->ml_Node);

    APTR result = AddTask(task, UnitTask, NULL);
    if (result == NULL)
    {
        Kprintf("[genet] %s: Failed to add genet task\n", __func__);
        FreeMem(ml, sizeof(struct MemList) + sizeof(struct MemEntry));
        FreeMem(task, sizeof(struct Task));
    FreeMem(&stack[0], genetConfig.unit_stack_bytes);
        return S2ERR_NO_RESOURCES;
    }

    Wait(SIGBREAKF_CTRL_F);
    Kprintf("[genet] %s: genet task started\n", __func__);
    return S2ERR_NO_ERROR;
}

void UnitTaskStop(struct GenetUnit *unit)
{
    Kprintf("[genet] %s: genet task stopping\n", __func__);

    struct MsgPort *timerPort = CreateMsgPort();
    struct timerequest *timerReq = CreateIORequest(timerPort, sizeof(struct timerequest));

    if (timerPort != NULL && timerReq != NULL)
    {
        BYTE result = OpenDevice((CONST_STRPTR) "timer.device", UNIT_VBLANK, (struct IORequest *)timerReq, LIB_MIN_VERSION);
        if (result != NULL)
        {
            Kprintf("[genet] %s: Failed to open timer device: %ld\n", __func__, result);
            // We'll continue anyway
        }
    }

    Signal(unit->task, SIGBREAKF_CTRL_C);
    do
    {
        timerReq->tr_node.io_Command = TR_ADDREQUEST;
        timerReq->tr_time.tv_secs = 0;
        timerReq->tr_time.tv_micro = 250000;
        DoIO(&timerReq->tr_node);
    } while (unit->task != NULL);

    CloseDevice(&timerReq->tr_node);
    DeleteIORequest(&timerReq->tr_node);
    DeleteMsgPort(timerPort);

    Kprintf("[genet] %s: genet task stopped\n", __func__);
}