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
#include <genet/phy.h>
#include <compat.h>
#include <device.h>
#include <minlist.h>
#include <debug.h>
#include <runtime_config.h>

struct Device *TimerBase = NULL;

static void UnitTask(struct GenetUnit *unit, struct Task *parent)
{
    // Initialize the built in msg port, we'll receive commands here
    _NewMinList((struct MinList *)&unit->unit.unit_MsgPort.mp_MsgList);
    unit->unit.unit_MsgPort.mp_SigTask = FindTask(NULL);
    unit->unit.unit_MsgPort.mp_SigBit = AllocSignal(-1);
    unit->unit.unit_MsgPort.mp_Flags = PA_SIGNAL;
    unit->unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;

    // Allocate signals for interrupt handlers
    unit->rx_signal = AllocSignal(-1);
    unit->tx_signal = AllocSignal(-1);
    unit->phy_signal = AllocSignal(-1);
    if (unit->rx_signal == -1 || unit->tx_signal == -1 || unit->phy_signal == -1)
    {
        Kprintf("[genet] %s: Failed to allocate RX/TX/PHY signals\n", __func__);
        goto free_signals;
    }

    // Create a timer, we'll use it to poll the PHY
    struct MsgPort *microHZTimerPort = CreateMsgPort();
    unit->openerPort = CreateMsgPort();
    struct timerequest *packetTimerReq = CreateIORequest(microHZTimerPort, sizeof(struct timerequest));
    if (microHZTimerPort == NULL || unit->openerPort == NULL || packetTimerReq == NULL)
    {
        Kprintf("[genet] %s: Failed to create timer msg port or request\n", __func__);
        goto free_ports;
    }

    UBYTE ret = OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)packetTimerReq, LIB_MIN_VERSION);
    if (ret)
    {
        Kprintf("[genet] %s: Failed to open timer device ret=%d\n", __func__, ret);
        goto free_ports;
    }

    /* used to reset stats on S2_ONLINE */
    TimerBase = packetTimerReq->tr_node.io_Device;

    // Set a timer... we need to pull on RX
    packetTimerReq->tr_node.io_Command = TR_ADDREQUEST;
    packetTimerReq->tr_time.tv_secs = 0;
    packetTimerReq->tr_time.tv_micro = genetConfig.poll_delay_us;
    SendIO(&packetTimerReq->tr_node);

    unit->task = FindTask(NULL);
    /* Signal parent that Unit task is up and running now */
    Signal(parent, SIGBREAKF_CTRL_F);

    ULONG sigset;
    ULONG waitMask = (1UL << unit->unit.unit_MsgPort.mp_SigBit) |
                     (1UL << unit->openerPort->mp_SigBit) |
                     (1UL << microHZTimerPort->mp_SigBit) |
                     (1UL << unit->rx_signal) |
                     (1UL << unit->tx_signal) |
                     (1UL << unit->phy_signal) |
                     SIGBREAKF_CTRL_C;

    do
    {
        sigset = Wait(waitMask);

        UWORD budget = genetConfig.budget;

        /* Receive processing */
        if ((sigset & (1UL << unit->rx_signal)) && unit->state == STATE_ONLINE)
        {
            budget -= bcmgenet_gmac_eth_rx(unit, budget);
            if (budget == 0)
            {
                // Still more to process, signal ourselves again
                Signal(unit->task, 1UL << unit->rx_signal);
            }
        }

        /* Transmit processing */
        if ((sigset & (1UL << unit->tx_signal)) && unit->state == STATE_ONLINE)
        {
            // reschedule periodic reclaim
            AbortIO(&packetTimerReq->tr_node);
            WaitIO(&packetTimerReq->tr_node);
            packetTimerReq->tr_node.io_Command = TR_ADDREQUEST;
            packetTimerReq->tr_time.tv_secs = 0;
            packetTimerReq->tr_time.tv_micro = genetConfig.poll_delay_us;
            SendIO(&packetTimerReq->tr_node);

            // TODO budget
            bcmgenet_tx_reclaim(unit);
        }

        // IO queue got a new message
        if (sigset & (1UL << unit->unit.unit_MsgPort.mp_SigBit))
        {
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

        // Opener management messages
        if (unlikely(sigset & (1UL << unit->openerPort->mp_SigBit)))
        {
            struct OpenerControlMsg *omsg;
            while ((omsg = (struct OpenerControlMsg *)GetMsg(unit->openerPort)) && --budget)
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
            if (budget == 0)
            {
                // Still more to process, signal ourselves again
                Signal(unit->task, 1UL << unit->openerPort->mp_SigBit);
            }
        }

        /* process PHY events */
        if (sigset & (1UL << unit->phy_signal))
        {
            ULONG status = unit->irq0_status;
            unit->irq0_status = 0;

            if (status & UMAC_IRQ_PHY_DET_R && unit->phydev->autoneg != AUTONEG_ENABLE)
            {
                //TODO phy_init_hw(unit->phydev);
                genphy_config_aneg(unit->phydev);
            }

            /* Link UP/DOWN event */
            if (status & UMAC_IRQ_LINK_EVENT)
            {
                // phy_mac_interrupt(unit->phydev);
                // TODO PHY state change
                Kprintf("[genet] %s: PHY link state change event\n", __func__);
            }
        }

        // Timer expired, query PHY for link state, reclaim TX
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

            /* Re-arm timer */
            packetTimerReq->tr_node.io_Command = TR_ADDREQUEST;
            packetTimerReq->tr_time.tv_secs = 0;
            packetTimerReq->tr_time.tv_micro = genetConfig.poll_delay_us;
            SendIO(&packetTimerReq->tr_node);
        }

        if (sigset & SIGBREAKF_CTRL_C)
        {
            Kprintf("[genet] %s: Received SIGBREAKF_CTRL_C, stopping genet task\n", __func__);
            AbortIO(&packetTimerReq->tr_node);
            WaitIO(&packetTimerReq->tr_node);
        }
    } while ((sigset & SIGBREAKF_CTRL_C) == 0);

    CloseDevice(&packetTimerReq->tr_node);
free_ports:
    DeleteIORequest(&packetTimerReq->tr_node);
    DeleteMsgPort(microHZTimerPort);
    DeleteMsgPort(unit->openerPort);
free_signals:
    FreeSignal(unit->rx_signal);
    FreeSignal(unit->tx_signal);
    FreeSignal(unit->phy_signal);

    FreeSignal(unit->unit.unit_MsgPort.mp_SigBit);
    Signal(parent, SIGBREAKF_CTRL_F);
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