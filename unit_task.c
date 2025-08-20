// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#define __NOLIBBASE__

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <dos/dos.h>

#include <device.h>
#include <minlist.h>
#include <debug.h>
#include "settings.h"

#define UNIT_STACK_SIZE (65536 / sizeof(ULONG))

// Max 1000000
#define PACKET_WAIT_DELAY_MIN 1000
#define PACKET_WAIT_DELAY_STANDBY 2000
#define PACKET_WAIT_DELAY_MAX 50000
#define PACKET_WAIT_COOLDOWN 40

static inline BOOL ProcessReceive(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->execBase;
    int pkt_len = 0;
    BOOL activity = FALSE;
    do
    {
        UBYTE *buffer = NULL;
        pkt_len = bcmgenet_gmac_eth_recv(unit, &buffer);
        // Distribute received packets to openers
        if (pkt_len > 0)
        {
            ObtainSemaphore(&unit->semaphore);
            activity |= ReceiveFrame(unit, buffer, pkt_len);
            ReleaseSemaphore(&unit->semaphore);
            bcmgenet_gmac_free_pkt(unit, buffer, pkt_len);
        }
    } while (pkt_len > 0);
    return activity;
}

static void UnitTask(struct GenetUnit *unit, struct Task *parent)
{
    struct ExecBase *SysBase = unit->execBase;

    // Initialize the built in msg port, we'll receive commands here
    _NewMinList((struct MinList *)&unit->unit.unit_MsgPort.mp_MsgList);
    unit->unit.unit_MsgPort.mp_SigTask = FindTask(NULL);
    unit->unit.unit_MsgPort.mp_SigBit = AllocSignal(-1);
    unit->activitySigBit = AllocSignal(-1); /* separate wakeup for tx fast-path */
    unit->unit.unit_MsgPort.mp_Flags = PA_SIGNAL;
    unit->unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;

    // Create a timer, we'll use it to poll the PHY
    struct MsgPort *timerPort = CreateMsgPort();
    struct timerequest *timerReq = CreateIORequest(timerPort, sizeof(struct timerequest));
    if (timerPort == NULL || timerReq == NULL)
    {
        Kprintf("[genet] %s: Failed to create timer msg port or request\n", __func__);
        DeleteMsgPort(timerPort);
        DeleteIORequest((struct IORequest *)timerReq);
        Signal(parent, SIGBREAKF_CTRL_C);
        return;
    }

    if (OpenDevice((CONST_STRPTR) "timer.device", UNIT_MICROHZ, (struct IORequest *)timerReq, LIB_MIN_VERSION))
    {
        Kprintf("[genet] %s: Failed to open timer device\n", __func__);
        DeleteMsgPort(timerPort);
        DeleteIORequest((struct IORequest *)timerReq);
        Signal(parent, SIGBREAKF_CTRL_C);
        return;
    }
    unit->timerBase = (struct TimerBase *)timerReq->tr_node.io_Device;

    // Set a timer... we need to pull on RX
    timerReq->tr_node.io_Command = TR_ADDREQUEST;
    timerReq->tr_time.tv_secs = 0;
    timerReq->tr_time.tv_micro = PACKET_WAIT_DELAY_MAX;
    SendIO(&timerReq->tr_node);
    UWORD cooldown = PACKET_WAIT_COOLDOWN;
    ULONG delay = PACKET_WAIT_DELAY_MAX;

    unit->task = FindTask(NULL);
    /* Signal parent that Unit task is up and running now */
    Signal(parent, SIGBREAKF_CTRL_F);

    ULONG sigset;
    BOOL activity = FALSE;
    ULONG waitMask = (1UL << unit->unit.unit_MsgPort.mp_SigBit) |
                     (1UL << timerPort->mp_SigBit) |
                     (1UL << unit->activitySigBit) |
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

        // Moved after ProcessCommand
        // Reasoning is the commands may be new receive requests
        if (unit->state == STATE_ONLINE)
        {
            activity |= ProcessReceive(unit);
        }

        /* Fast-path signalled activity. */
        if (sigset & (1UL << unit->activitySigBit))
        {
            activity = TRUE;
            cooldown = PACKET_WAIT_COOLDOWN;

            if (delay != PACKET_WAIT_DELAY_MIN)
            {
                /* Collapse delay parameters immediately. */
                delay = PACKET_WAIT_DELAY_MIN;

                BOOL timerExpired = (sigset & (1UL << timerPort->mp_SigBit)) != 0; /* old request done? */
                if (!timerExpired && CheckIO(&timerReq->tr_node) == 0)
                {
                    AbortIO(&timerReq->tr_node);
                    WaitIO(&timerReq->tr_node);
                    timerReq->tr_node.io_Command = TR_ADDREQUEST;
                    timerReq->tr_time.tv_secs = 0;
                    timerReq->tr_time.tv_micro = delay;
                    SendIO(&timerReq->tr_node);
                }
            }
        }

        // Timer expired, query PHY for link state
        if (sigset & (1UL << timerPort->mp_SigBit))
        {
            if (activity == FALSE)
            {
                // If we didn't receive anything, increase delay
                cooldown--;
                if (cooldown == 0)
                {
                    cooldown = PACKET_WAIT_COOLDOWN;
                    delay = (delay == PACKET_WAIT_DELAY_MIN) ? PACKET_WAIT_DELAY_STANDBY : PACKET_WAIT_DELAY_MAX;
                }
            }
            else
            {
                // If we received something, reset cooldown and delay
                cooldown = PACKET_WAIT_COOLDOWN;
                delay = PACKET_WAIT_DELAY_MIN;
            }

            if (unit->state == STATE_ONLINE)
            {
                // TODO do we really need to block entire unit
                ObtainSemaphore(&unit->semaphore);
                bcmgenet_timeout(unit);
                ReleaseSemaphore(&unit->semaphore);
            }

            // TODO check link state on PHY

            if (CheckIO(&timerReq->tr_node))
            {
                WaitIO(&timerReq->tr_node);
            }

            activity = FALSE;

            // Schedule next run
            timerReq->tr_node.io_Command = TR_ADDREQUEST;
            timerReq->tr_time.tv_secs = 0;
            timerReq->tr_time.tv_micro = delay;
            SendIO(&timerReq->tr_node);
        }
        if (sigset & SIGBREAKF_CTRL_C)
        {
            Kprintf("[genet] %s: Received SIGBREAKF_CTRL_C, stopping genet task\n", __func__);
            AbortIO(&timerReq->tr_node);
            WaitIO(&timerReq->tr_node);
        }
    } while ((sigset & SIGBREAKF_CTRL_C) == 0);

    FreeSignal(unit->unit.unit_MsgPort.mp_SigBit);
    FreeSignal(unit->activitySigBit);
    CloseDevice(&timerReq->tr_node);
    DeleteIORequest(&timerReq->tr_node);
    DeleteMsgPort(timerPort);
    unit->task = NULL;
}

int UnitTaskStart(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->execBase;
    Kprintf("[genet] %s: genet task starting\n", __func__);

    // Get all memory we need for the receiver task
    struct MemList *ml = AllocMem(sizeof(struct MemList) + sizeof(struct MemEntry), MEMF_PUBLIC | MEMF_CLEAR);
    struct Task *task = AllocMem(sizeof(struct Task), MEMF_PUBLIC | MEMF_CLEAR);
    ULONG *stack = AllocMem(UNIT_STACK_SIZE * sizeof(ULONG), MEMF_PUBLIC | MEMF_CLEAR);
    if (!ml || !task || !stack)
    {
        Kprintf("[genet] %s: Failed to allocate memory for genet task\n", __func__);
        if (ml)
            FreeMem(ml, sizeof(struct MemList) + sizeof(struct MemEntry));
        if (task)
            FreeMem(task, sizeof(struct Task));
        if (stack)
            FreeMem(stack, UNIT_STACK_SIZE * sizeof(ULONG));
        return S2ERR_NO_RESOURCES;
    }

    // Prepare mem list, put task and its stack there
    ml->ml_NumEntries = 2;
    ml->ml_ME[0].me_Un.meu_Addr = task;
    ml->ml_ME[0].me_Length = sizeof(struct Task);

    ml->ml_ME[1].me_Un.meu_Addr = &stack[0];
    ml->ml_ME[1].me_Length = UNIT_STACK_SIZE * sizeof(ULONG);

    // Set up stack
    task->tc_SPLower = &stack[0];
    task->tc_SPUpper = &stack[UNIT_STACK_SIZE];

    // Push ThisTask and Unit on the stack
    stack = (ULONG *)task->tc_SPUpper;
    *--stack = (ULONG)FindTask(NULL);
    *--stack = (ULONG)unit;
    task->tc_SPReg = stack;

    task->tc_Node.ln_Name = "genet rx/tx";
    task->tc_Node.ln_Type = NT_TASK;
    task->tc_Node.ln_Pri = UNIT_TASK_PRIORITY;

    _NewMinList((struct MinList *)&task->tc_MemEntry);
    AddHead(&task->tc_MemEntry, &ml->ml_Node);

    APTR result = AddTask(task, UnitTask, NULL);
    if (result == NULL)
    {
        Kprintf("[genet] %s: Failed to add genet task\n", __func__);
        FreeMem(ml, sizeof(struct MemList) + sizeof(struct MemEntry));
        FreeMem(task, sizeof(struct Task));
        FreeMem(&stack[0], UNIT_STACK_SIZE * sizeof(ULONG));
        return S2ERR_NO_RESOURCES;
    }

    Wait(SIGBREAKF_CTRL_F);
    Kprintf("[genet] %s: genet task started\n", __func__);
    return S2ERR_NO_ERROR;
}

void UnitTaskStop(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->execBase;
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