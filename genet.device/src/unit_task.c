// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/timer_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#include <proto/timer.h>
#endif

#include <dos/dos.h>

#include <genet/bcmgenet.h>
#include <genet/bcmgenet-regs.h>
#include <genet/bcmgenet-irq.h>
#include <genet/phy.h>
#include <device.h>
#include <minlist.h>
#include <debug.h>
#include <iomem.h>
#include <types.h>
#include <runtime_config.h>

/*
 * Unit-control command submission from foreign tasks. Returns the result of the command, or 0 if the command was not processed.
 */
LONG UnitSubmitControl(struct GenetUnit *unit, UWORD command, union UnitControlPayload payload)
{
    if (unit == NULL || unit->controlPort == NULL)
        return 0;

    struct MsgPort *replyPort = CreateMsgPort();
    if (replyPort == NULL)
    {
        Kprintf("[genet] %s: Failed to create reply port for unit control\n", __func__);
        return 0;
    }

    struct UnitControlMsg msg;
    msg.msg.mn_Node.ln_Type = NT_MESSAGE;
    msg.msg.mn_ReplyPort = replyPort;
    msg.command = command;
    msg.result = 0;
    msg.payload = payload;
    PutMsg(unit->controlPort, &msg.msg);
    WaitPort(replyPort);
    GetMsg(replyPort);
    DeleteMsgPort(replyPort);
    return msg.result;
}

/*
 * Unit-control command submission from foreign tasks. Asynchronous version, does not wait for a reply.
 * Memory for the message is allocated from the unit's memory pool and freed by the unit task after processing.
 */
void UnitSubmitControlAsync(struct GenetUnit *unit, UWORD command, union UnitControlPayload payload)
{
    if (unit == NULL || unit->controlPort == NULL)
        return;

    struct UnitControlMsg *msg = pool_alloc(&unit->memoryPool, sizeof(struct UnitControlMsg));
    if (msg == NULL)
    {
        Kprintf("[genet] %s: Failed to allocate message for async unit control\n", __func__);
        return;
    }

    msg->msg.mn_Node.ln_Type = NT_MESSAGE;
    msg->msg.mn_ReplyPort = NULL; // No reply expected for async
    msg->command = command;
    msg->payload = payload;
    PutMsg(unit->controlPort, &msg->msg);
}

static void UnitTask(struct GenetUnit *unit, struct Task *parent)
{
	const struct GenetRuntimeConfig *config = &unit->device->runtimeConfig;

    // Initialize the built in msg port, we'll receive commands here
    _NewMinList((struct MinList *)&unit->unit.unit_MsgPort.mp_MsgList);
    unit->unit.unit_MsgPort.mp_SigTask = FindTask(NULL);
    BYTE msg_sigbit = AllocSignal(-1);
    if (msg_sigbit == -1)
    {
        Kprintf("[genet] %s: Failed to allocate unit message signal\n", __func__);
        goto free_signals;
    }
    unit->unit.unit_MsgPort.mp_SigBit = (UBYTE)msg_sigbit;
    unit->unit.unit_MsgPort.mp_Flags = PA_SIGNAL;
    unit->unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;

    // Allocate signals for interrupt handlers
    unit->irq0_signal = AllocSignal(-1);
    if (unit->irq0_signal == -1)
    {
        Kprintf("[genet] %s: Failed to allocate RX/TX/IRQ0 signals\n", __func__);
        goto free_signals;
    }

    // Create a timer, we'll use it to poll the PHY and do housekeeping
    struct MsgPort *microHZTimerPort = CreateMsgPort();
    unit->controlPort = CreateMsgPort();
    struct timerequest *packetTimerReq = CreateIORequest(microHZTimerPort, sizeof(struct timerequest));
    if (microHZTimerPort == NULL || unit->controlPort == NULL || packetTimerReq == NULL)
    {
        Kprintf("[genet] %s: Failed to create timer msg port or request\n", __func__);
        goto free_ports;
    }

    BYTE ret = OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)packetTimerReq, LIB_MIN_VERSION);
    if (ret)
    {
        Kprintf("[genet] %s: Failed to open timer device ret=%ld\n", __func__, ret);
        goto free_ports;
    }

    unit->timerBase = packetTimerReq->tr_node.io_Device;

    // Start the timer
    packetTimerReq->tr_node.io_Command = TR_ADDREQUEST;
    packetTimerReq->tr_time.tv_secs = 0;
    packetTimerReq->tr_time.tv_micro = config->periodic_task_ms * 1000;
    SendIO(&packetTimerReq->tr_node);

    unit->task = FindTask(NULL);
    /* Signal parent that Unit task is up and running now */
    Signal(parent, SIGBREAKF_CTRL_F);

    KprintfH("[genet] %s: Entering main unit task loop\n", __func__);

    ULONG sigset;
    ULONG waitMask = (1UL << unit->unit.unit_MsgPort.mp_SigBit) |
                     (1UL << unit->controlPort->mp_SigBit) |
                     (1UL << microHZTimerPort->mp_SigBit) |
                     (1UL << unit->irq0_signal) |
                     SIGBREAKF_CTRL_C;

    do
    {
        sigset = Wait(waitMask);
        KprintfH("[genet] %s: Woke up, sigset=0x%08lx\n", __func__, sigset);
        u16 budget;

        // IO queue got a new message
        if (sigset & (1UL << unit->unit.unit_MsgPort.mp_SigBit))
        {
            budget = unit->budget;
            struct IOSana2Req *io;
            // Drain command queue and process it
            while ((io = (struct IOSana2Req *)GetMsg(&unit->unit.unit_MsgPort)) && --budget)
            {
                ProcessCommand(io);
            }
            if (budget == 0)
            {
                // Still more to process, signal ourselves again
                Signal(unit->task, 1UL << unit->unit.unit_MsgPort.mp_SigBit);
            }
        }

        // Unit-control messages from foreign tasks
        if (unlikely(sigset & (1UL << unit->controlPort->mp_SigBit)))
        {
            budget = unit->budget;
            struct UnitControlMsg *cmsg;
            while ((cmsg = (struct UnitControlMsg *)GetMsg(unit->controlPort)) && --budget)
            {
                switch (cmsg->command)
                {
                case UNIT_CTRL_OPENER_ADD:
                    AddTailMinList(&unit->openers, (struct MinNode *)cmsg->payload.opener);
                    break;
                case UNIT_CTRL_OPENER_REM:
                    RemoveMinNode((struct MinNode *)cmsg->payload.opener);
                    break;
                case UNIT_CTRL_EVENT_REPORT:
                    ReportEvents(unit, cmsg->payload.eventSet);
                    break;
                case UNIT_CTRL_EVENT_ABORT:
                    UnitCancelEvent(cmsg->payload.io);
                    break;
                case UNIT_CTRL_THROUGHPUT_ABORT:
                    cmsg->result = UnitCancelThroughput(unit, cmsg->payload.io);
                    break;
                }
                if(cmsg->msg.mn_ReplyPort != NULL)
                    ReplyMsg(&cmsg->msg);
                else
                    pool_free(&unit->memoryPool, cmsg);
            }
            if (budget == 0)
            {
                // Still more to process, signal ourselves again
                Signal(unit->task, 1UL << unit->controlPort->mp_SigBit);
            }
        }

        /* process IRQ0 events */
        if (sigset & (1UL << unit->irq0_signal))
        {
            KprintfH("[genet] %s: Interrupt bottom-half processing, status=0x%08lx\n", __func__, unit->irq0_status);
            u32 status = unit->irq0_status;
            unit->irq0_status = 0;

            if (unlikely((status & UMAC_IRQ_PHY_DET_R) && unit->phydev->autoneg != AUTONEG_ENABLE))
            {
                // TODO phy_init_hw(unit->phydev);
                genphy_config_aneg(unit->phydev);
            }

            /* Link UP/DOWN event */
            if (unlikely(status & UMAC_IRQ_LINK_DOWN))
            {
                // phy_mac_interrupt(unit->phydev);
                // TODO PHY state change
                Kprintf("[genet] %s: PHY link down event\n", __func__);
            }
            else if (unlikely(status & UMAC_IRQ_LINK_UP))
            {
                // phy_mac_interrupt(unit->phydev);
                // TODO PHY state change
                Kprintf("[genet] %s: PHY link up event\n", __func__);
            }
            
            /* TX completion processing */
            if (likely((status & UMAC_IRQ_TXDMA_DONE) && unit->state == STATE_ONLINE))
            {
                bcmgenet_tx_reclaim(unit, unit->budget);
                mmio_write32(UMAC_IRQ_TXDMA_DONE,
                        BCMGENET_REG(unit, GENET_INTRL2_0_OFF + INTRL2_CPU_CLEAR));
                bcmgenet_irq0_enable(unit, UMAC_IRQ_TXDMA_DONE);
            }

            /* Receive processing */
            if (likely((status & UMAC_IRQ_RXDMA_DONE) && unit->state == STATE_ONLINE))
            {
                KprintfH("[genet] %s: RX signal received, processing packets\n", __func__);
                budget = unit->budget;
                s32 res = bcmgenet_gmac_eth_rx(unit, budget);
                if (res > 0)
                {
                    budget = (u16)(budget - res);
                }
                KprintfH("[genet] %s: Remaining budget: %ld\n", __func__, budget);
                if (budget == 0)
                {
                    // Still more to process, signal ourselves again
                    unit->irq0_status |= UMAC_IRQ_RXDMA_DONE;
                    Signal(unit->task, 1UL << unit->irq0_signal);
                }
                else
                {
                    /* We caught up, enable interrupts */
                    mmio_write32(UMAC_IRQ_RXDMA_DONE, BCMGENET_REG(unit, GENET_INTRL2_0_OFF + INTRL2_CPU_CLEAR));
                    bcmgenet_irq0_enable(unit, UMAC_IRQ_RXDMA_DONE);
                }
            }
        }

        // Timer expired, query PHY for link state, reclaim TX
        if (sigset & (1UL << microHZTimerPort->mp_SigBit))
        {
            if (CheckIO(&packetTimerReq->tr_node))
            {
                WaitIO(&packetTimerReq->tr_node);
            }

            /* Just in case we got stuck */
            if (unit->state == STATE_ONLINE)
                bcmgenet_irq0_enable(unit, /* UMAC_IRQ_TXDMA_DONE | */ UMAC_IRQ_RXDMA_DONE);

            // TODO pool PHY for state, BCM2711 genet has a bug where PHY interrupts don't work properly

            /* Re-arm timer */
            packetTimerReq->tr_node.io_Command = TR_ADDREQUEST;
            packetTimerReq->tr_time.tv_secs = 0;
            packetTimerReq->tr_time.tv_micro = config->periodic_task_ms * 1000;
            SendIO(&packetTimerReq->tr_node);

            UpdateThroughputStats(unit);
        }

        if (unlikely(sigset & SIGBREAKF_CTRL_C))
        {
            KprintfH("[genet] %s: Received SIGBREAKF_CTRL_C, stopping genet task\n", __func__);
            AbortIO(&packetTimerReq->tr_node);
            WaitIO(&packetTimerReq->tr_node);
        }
    } while ((sigset & SIGBREAKF_CTRL_C) == 0);

    CloseDevice(&packetTimerReq->tr_node);
    unit->timerBase = NULL;
free_ports:
    DeleteIORequest(&packetTimerReq->tr_node);
    DeleteMsgPort(microHZTimerPort);
    DeleteMsgPort(unit->controlPort);
free_signals:
    FreeSignal(unit->irq0_signal);
    FreeSignal((BYTE)unit->unit.unit_MsgPort.mp_SigBit);

    Signal(parent, SIGBREAKF_CTRL_F);
    unit->task = NULL;
}

u32 UnitTaskStart(struct GenetUnit *unit)
{
    KprintfH("[genet] %s: genet task starting\n", __func__);
	const struct GenetRuntimeConfig *config = &unit->device->runtimeConfig;

    // Get all memory we need for the receiver task
    struct MemList *ml = AllocMem(sizeof(struct MemList) + sizeof(struct MemEntry), MEMF_PUBLIC | MEMF_CLEAR);
    struct Task *task = AllocMem(sizeof(struct Task), MEMF_PUBLIC | MEMF_CLEAR);
    ULONG *stack = AllocMem(config->unit_stack_bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (!ml || !task || !stack)
    {
        Kprintf("[genet] %s: Failed to allocate memory for genet task\n", __func__);
        if (ml)
            FreeMem(ml, sizeof(struct MemList) + sizeof(struct MemEntry));
        if (task)
            FreeMem(task, sizeof(struct Task));
        if (stack)
            FreeMem(stack, config->unit_stack_bytes);
        return S2ERR_NO_RESOURCES;
    }

    // Prepare mem list, put task and its stack there
    ml->ml_NumEntries = 2;
    ml->ml_ME[0].me_Un.meu_Addr = task;
    ml->ml_ME[0].me_Length = sizeof(struct Task);

    ml->ml_ME[1].me_Un.meu_Addr = &stack[0];
    ml->ml_ME[1].me_Length = config->unit_stack_bytes;

    // Set up stack
    task->tc_SPLower = &stack[0];
    task->tc_SPUpper = &stack[config->unit_stack_bytes / sizeof(ULONG)];

    // Push ThisTask and Unit on the stack
    stack = (ULONG *)task->tc_SPUpper;
    *--stack = (ULONG)FindTask(NULL);
    *--stack = (ULONG)unit;
    task->tc_SPReg = stack;

    task->tc_Node.ln_Name = "genet ethernet driver";
    task->tc_Node.ln_Type = NT_TASK;
    task->tc_Node.ln_Pri = config->unit_task_priority;

    _NewMinList((struct MinList *)&task->tc_MemEntry);
    AddHead(&task->tc_MemEntry, &ml->ml_Node);

    SetSignal(0UL, SIGBREAKF_CTRL_F);

    APTR result = AddTask(task, UnitTask, NULL);
    if (result == NULL)
    {
        Kprintf("[genet] %s: Failed to add genet task\n", __func__);
        FreeMem(ml, sizeof(struct MemList) + sizeof(struct MemEntry));
        FreeMem(task, sizeof(struct Task));
        FreeMem(&stack[0], config->unit_stack_bytes);
        return S2ERR_NO_RESOURCES;
    }

    Wait(SIGBREAKF_CTRL_F);
    KprintfH("[genet] %s: genet task started\n", __func__);
    return S2ERR_NO_ERROR;
}

void UnitTaskStop(struct GenetUnit *unit)
{
    KprintfH("[genet] %s: genet task stopping\n", __func__);

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

    SetSignal(0UL, SIGBREAKF_CTRL_F);

    CloseDevice(&timerReq->tr_node);
    DeleteIORequest(&timerReq->tr_node);
    DeleteMsgPort(timerPort);

    KprintfH("[genet] %s: genet task stopped\n", __func__);
}