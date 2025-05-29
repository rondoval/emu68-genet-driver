// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#define __NOLIBBASE__
#include <proto/exec.h>
#include <dos/dos.h>

#include <device.h>
#include <debug.h>

#define UNIT_STACK_SIZE (65536 / sizeof(ULONG))
#define UNIT_TASK_PRIORITY 0

// Max 1000000
#define PACKET_WAIT_DELAY_MIN 1000
#define PACKET_WAIT_DELAY_STANDBY 2000
#define PACKET_WAIT_DELAY_MAX 50000
#define PACKET_WAIT_COOLDOWN 40

static inline BOOL ProcessReceive(struct GenetUnit *unit)
{
    if (unit->state != STATE_ONLINE)
    {
        return FALSE;
    }

    int pkt_len = 0;
    BOOL activity = FALSE;
    do
    {
        UBYTE *buffer = NULL;
        pkt_len = bcmgenet_gmac_eth_recv(unit, 0, &buffer);
        // Distribute received packets to openers
        if (pkt_len > 0)
        {
            if (pkt_len < ETH_HLEN + ETH_DATA_LEN)
            {
                activity = TRUE;
                ReceiveFrame(unit, buffer, pkt_len);
            }
            // TODO there's some bug in lower layer, sometimes gets 2048 byte frames, all 0's
            // else
            // {
            //     Kprintf("[genet] %s: Received packet of length %ld, but it is too long\n", __func__, pkt_len);
            // }
            bcmgenet_gmac_free_pkt(unit, buffer, pkt_len);
        }
    } while (pkt_len > 0); // && pkt_len < ETH_HLEN + ETH_DATA_LEN);
    return activity;
}

static void UnitTask(struct GenetUnit *unit, struct Task *parent)
{
    struct ExecBase *SysBase = unit->execBase;

    // Initialize the built in msg port, well receive commands here
    NewMinList((struct MinList *)&unit->unit.unit_MsgPort.mp_MsgList);
    unit->unit.unit_MsgPort.mp_SigTask = FindTask(NULL);
    unit->unit.unit_MsgPort.mp_SigBit = AllocSignal(-1);
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
    do
    {
        sigset = Wait((1 << unit->unit.unit_MsgPort.mp_SigBit) | 1 << timerPort->mp_SigBit | SIGBREAKF_CTRL_C);
        BOOL activity = FALSE;

        // IO queue got a new message
        if (sigset & (1 << unit->unit.unit_MsgPort.mp_SigBit))
        {
            activity = TRUE;
            struct IOSana2Req *io;
            ObtainSemaphore(&unit->semaphore);
            // Drain command queue and process it
            while ((io = (struct IOSana2Req *)GetMsg(&unit->unit.unit_MsgPort)))
            {
                ProcessCommand(io);
            }
            ReleaseSemaphore(&unit->semaphore);
        }

        // No matter what, let's receive some packets
        activity |= ProcessReceive(unit);
        if (activity)
        {
            // If we received something, reset cooldown
            cooldown = PACKET_WAIT_COOLDOWN;
            delay = PACKET_WAIT_DELAY_MIN;
        }

        // Timer expired, query PHY for link state
        if (sigset & (1 << timerPort->mp_SigBit))
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
            if (CheckIO(&timerReq->tr_node))
            {
                WaitIO(&timerReq->tr_node);
            }
            // TODO check link state on PHY

            // Schedule next run
            timerReq->tr_node.io_Command = TR_ADDREQUEST;
            timerReq->tr_time.tv_secs = 0;
            timerReq->tr_time.tv_micro = delay;
            SendIO(&timerReq->tr_node);
        }
        if (sigset & SIGBREAKF_CTRL_C)
        {
            AbortIO(&timerReq->tr_node);
            WaitIO(&timerReq->tr_node);
        }
    } while ((sigset & SIGBREAKF_CTRL_C) == 0);

    CloseDevice(&timerReq->tr_node);
    DeleteIORequest(&timerReq->tr_node);
    DeleteMsgPort(timerPort);
    unit->task = NULL;
}

void UnitTaskStart(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->execBase;
    Kprintf("[genet] %s: genet task starting\n", __func__);

    // Get all memory we need for the receiver task
    struct MemList *ml = AllocMem(sizeof(struct MemList) + sizeof(struct MemEntry), MEMF_PUBLIC | MEMF_CLEAR);
    struct Task *task = AllocMem(sizeof(struct Task), MEMF_PUBLIC | MEMF_CLEAR);
    ULONG *stack = AllocMem(UNIT_STACK_SIZE * sizeof(ULONG), MEMF_PUBLIC | MEMF_CLEAR);

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

    NewMinList((struct MinList *)&task->tc_MemEntry);
    AddHead(&task->tc_MemEntry, &ml->ml_Node);

    AddTask(task, UnitTask, NULL);
    Wait(SIGBREAKF_CTRL_F);
    Kprintf("[genet] %s: genet task started\n", __func__);
}

void UnitTaskStop(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->execBase;
    Kprintf("[genet] %s: genet task stopping\n", __func__);

    struct MsgPort *timerPort = CreateMsgPort();
    struct timerequest *timerReq = CreateIORequest(timerPort, sizeof(struct timerequest));

    if (timerPort != NULL || timerReq != NULL)
    {
        OpenDevice((CONST_STRPTR) "timer.device", UNIT_VBLANK, (struct IORequest *)timerReq, LIB_MIN_VERSION);
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