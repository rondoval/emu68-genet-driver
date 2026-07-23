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
#include <device.h>
#include <minlist.h>
#include <debug.h>
#include <iomem.h>
#include <types.h>
#include <runtime_config.h>
#include <driver_task.h>

/*
 * The unit task: netdev command processing, interrupt bottom half (RX
 * delivery, TX-done), periodic housekeeping. It is the ONLY context that
 * calls the stack's nso_* callbacks.
 *
 * The datapath is state-driven, not event-driven: every wakeup, whatever
 * woke it, harvests TX and polls RX, then re-arms. That is why the ISR needs
 * to hand over nothing but a signal, and why the periodic timer carries no
 * datapath work of its own — a timer tick is simply one more wakeup.
 */
static void UnitTask(struct GenetUnit *unit, struct Task *parent)
{
    const struct GenetRuntimeConfig *config = &unit->device->runtimeConfig;

    unit->rx_signal = -1;
    unit->tx_signal = -1;
    unit->link_signal = -1;

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
    unit->rx_signal = AllocSignal(-1);
    unit->tx_signal = AllocSignal(-1);
    unit->link_signal = AllocSignal(-1);
    if (unit->rx_signal == -1 || unit->tx_signal == -1 || unit->link_signal == -1)
    {
        Kprintf("[genet] %s: Failed to allocate interrupt signals\n", __func__);
        goto free_signals;
    }

    // Create a timer, we'll use it to poll the PHY and do housekeeping
    struct MsgPort *microHZTimerPort = CreateMsgPort();
    struct timerequest *packetTimerReq = CreateIORequest(microHZTimerPort, sizeof(struct timerequest));
    if (microHZTimerPort == NULL || packetTimerReq == NULL)
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

    KprintfT("[genet] %s: Entering main unit task loop\n", __func__);

    /* Housekeeping ticks between PHY link polls, rounded up (>= 1). */
    u32 link_poll_period_ticks = 1;
    if (config->periodic_task_ms != 0 && config->link_poll_ms > config->periodic_task_ms)
        link_poll_period_ticks =
            (config->link_poll_ms + config->periodic_task_ms - 1) / config->periodic_task_ms;
    u32 link_poll_ticks = 0;

    ULONG sigset;
    ULONG waitMask = (1UL << unit->unit.unit_MsgPort.mp_SigBit) |
                     (1UL << microHZTimerPort->mp_SigBit) |
                     (1UL << unit->rx_signal) |
                     (1UL << unit->tx_signal) |
                     (1UL << unit->link_signal) |
                     SIGBREAKF_CTRL_C;

    do
    {
        sigset = Wait(waitMask);
        // KprintfT("[genet] %s: Woke up, sigset=0x%08lx\n", __func__, sigset);

        /*
         * Datapath. Both halves read the rings rather than replaying ISR
         * status bits, but they are woken differently on purpose:
         *
         *   TX  — harvested on EVERY wakeup. It is lock-free and costs one
         *         index compare when idle, and completing early only frees
         *         the stack's memory sooner.
         *   RX  — polled only when its own interrupt says so. Handing frames
         *         up takes the stack's core lock, so the batch size IS the
         *         lock cadence (see ND_RX_BATCH) and it is also the GRO merge
         *         window. The hardware coalescer (frames + usecs) is the
         *         policy that decides when a batch is worth that; draining on
         *         unrelated wakeups just shreds it into single-frame handups
         *         that contend with the sending task.
         */
        if (likely(unit->state == STATE_ONLINE))
        {
            bcmgenet_tx_harvest(unit);

            /* Re-arm what the ISR masked, as one MASK_CLEAR write. TX is
             * always caught up by here; RX stays masked while we are still
             * behind, and the ISR already ACKed, so a writeback that latched
             * STAT during the drain re-fires on unmask — nothing is lost. */
            u32 rearm = UMAC_IRQ_TXDMA_DONE;

            if (sigset & (1UL << unit->rx_signal))
            {
                u16 budget = unit->budget;
                if (likely(bcmgenet_netdev_rx(unit, budget) != (s32)budget))
                    rearm |= UMAC_IRQ_RXDMA_DONE;
                else
                    Signal(unit->task, 1UL << unit->rx_signal); /* still behind */
            }

            bcmgenet_irq0_enable(unit, rearm);
        }

        // IO queue got a new message
        if (sigset & (1UL << unit->unit.unit_MsgPort.mp_SigBit))
        {
            u16 budget = unit->budget;
            struct IOStdReq *io;
            /* Drain the command queue, bounded. */
            while ((io = (struct IOStdReq *)GetMsg(&unit->unit.unit_MsgPort)) != NULL)
            {
                ProcessCommand(io);
                if (--budget == 0)
                {
                    Signal(unit->task, 1UL << unit->unit.unit_MsgPort.mp_SigBit);
                    break;
                }
            }
        }

        /* Link or PHY-detect event. The hardware does not tell us reliably
         * which way the link went (and can latch both), so ask the PHY
         * instead of trusting the bits. Resets the poll phase, since the
         * state was just read.
         *
         * Gated on STATE_ONLINE like the other two branches: a STOP is
         * processed above, in the same pass, and it removes the interrupt
         * server. Re-arming UMAC_IRQ_LINK_EVENT after that would leave a
         * level-triggered line asserted with nothing to ACK it. */
        if (unlikely((sigset & (1UL << unit->link_signal)) &&
                     unit->state == STATE_ONLINE))
        {
            bcmgenet_link_poll(unit, FALSE); /* interrupt: current state, not the latch */
            bcmgenet_phy_refresh_forced(unit); /* acts on the state just read */
            link_poll_ticks = 0;
            bcmgenet_irq0_enable(unit, bcmgenet_link_irq_mask(unit));
        }

        // Timer expired: housekeeping
        if (sigset & (1UL << microHZTimerPort->mp_SigBit))
        {
            if (CheckIO(&packetTimerReq->tr_node))
            {
                WaitIO(&packetTimerReq->tr_node);
            }

            if (unit->state == STATE_ONLINE)
            {
                /* PHY link poll — the reliable path to current link state
                 * (see bcmgenet_link_poll). MDIO reads are slow, so it runs at
                 * link_poll_ms rather than on every housekeeping tick. */
                if (++link_poll_ticks >= link_poll_period_ticks)
                {
                    link_poll_ticks = 0;
                    bcmgenet_link_poll(unit, TRUE); /* poll: keep the latch, so short drops are seen */
                }

                bcmgenet_perf_tick(unit); /* telemetry; compiled out below PROFILE */
            }

            /* Re-arm timer */
            packetTimerReq->tr_node.io_Command = TR_ADDREQUEST;
            packetTimerReq->tr_time.tv_secs = 0;
            packetTimerReq->tr_time.tv_micro = config->periodic_task_ms * 1000;
            SendIO(&packetTimerReq->tr_node);
        }

        if (unlikely(sigset & SIGBREAKF_CTRL_C))
        {
            KprintfT("[genet] %s: Received SIGBREAKF_CTRL_C, stopping genet task\n", __func__);
            AbortIO(&packetTimerReq->tr_node);
            WaitIO(&packetTimerReq->tr_node);
        }
    } while ((sigset & SIGBREAKF_CTRL_C) == 0);

    CloseDevice(&packetTimerReq->tr_node);
    unit->timerBase = NULL;
free_ports:
    DeleteIORequest(&packetTimerReq->tr_node);
    DeleteMsgPort(microHZTimerPort);
free_signals:
    /* Reachable before every signal exists — freeing an unallocated -1 (or
     * worse, the cleared struct's bit 0, which belongs to Exec) must not
     * happen. */
    if (unit->link_signal != -1)
        FreeSignal(unit->link_signal);
    if (unit->tx_signal != -1)
        FreeSignal(unit->tx_signal);
    if (unit->rx_signal != -1)
        FreeSignal(unit->rx_signal);
    if (msg_sigbit != -1)
        FreeSignal(msg_sigbit);

    /* CTRL_F reports a running task, CTRL_C a task that never got there. Both
     * paths land here, so the distinction is which signal is sent: unit->task
     * is set only once the loop is entered, and cleared on the way out. */
    BOOL wasRunning = (unit->task != NULL);
    unit->task = NULL;
    Signal(parent, wasRunning ? SIGBREAKF_CTRL_F : SIGBREAKF_CTRL_C);
}

u32 UnitTaskStart(struct GenetUnit *unit)
{
    const struct GenetRuntimeConfig *config = &unit->device->runtimeConfig;
    return drv_task_spawn(unit, UnitTask, "genet ethernet driver",
                          config->unit_stack_bytes, config->unit_task_priority) == 0
               ? GENET_OK
               : ENOMEM;
}

void UnitTaskStop(struct GenetUnit *unit)
{
    drv_task_join(&unit->task);
}
