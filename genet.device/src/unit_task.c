// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/timer_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function: see GenetUnit.sysBase */
#include <proto/exec.h>
#include <proto/timer.h>
#endif

#include <dos/dos.h>
#include <devices/newstyle.h>
#include <devices/timer.h>
#include <exec/errors.h>

#include <genet/bcmgenet.h>
#include <genet/bcmgenet-regs.h>
#include <genet/bcmgenet-irq.h>
#include <device.h>
#include <sana2_unit.h>
#include <minlist.h>
#include <debug.h>
#include <iomem.h>
#include <types.h>
#include <runtime_config.h>
#include <driver_task.h>
#include <drv_timer.h>

/* Max IO commands drained per wakeup — a defensive cap so a command flood
 * can't starve the datapath. Commands are rare and reply-gated, so the value
 * only matters under abuse; if hit, the loop re-signals to finish next wakeup. */
#define CMD_DRAIN_MAX 64u

/* ---------------------------------------------------------- commands --- */

/* Both personalities' commands: a stack decides which ABI to speak by
 * NETDEV_CMD_ATTACH in this list, never by the device type. */
static const UWORD GENET_SupportedCommands[] = {
    NETDEV_CMD_ATTACH,
    NETDEV_CMD_DETACH,
    NETDEV_CMD_START,
    NETDEV_CMD_STOP,
    NETDEV_CMD_SET_RXFILTER,
    NETDEV_CMD_SET_COALESCE,
    NETDEV_CMD_SET_RX_PROFILE,
    NETDEV_CMD_GET_STATS,
    NETDEV_CMD_GET_COUNTERS,
    NETDEV_CMD_GET_LINK,
    NETDEV_CMD_SET_MAC,
    CMD_READ,
    CMD_WRITE,
    CMD_FLUSH,
    S2_DEVICEQUERY,
    S2_GETSTATIONADDRESS,
    S2_CONFIGINTERFACE,
    S2_ADDMULTICASTADDRESS,
    S2_DELMULTICASTADDRESS,
    S2_MULTICAST,
    S2_BROADCAST,
    S2_GETSPECIALSTATS,
    S2_GETGLOBALSTATS,
    S2_ONEVENT,
    S2_READORPHAN,
    S2_ONLINE,
    S2_OFFLINE,
    S2_ADDMULTICASTADDRESSES,
    S2_DELMULTICASTADDRESSES,
    S2_GETEXTENDEDGLOBALSTATS,
    S2_SAMPLE_THROUGHPUT,
    NSCMD_DEVICEQUERY,
    0};

/**
 * Do_NSCMD_DEVICEQUERY() - NSCMD_DEVICEQUERY: fill the caller's
 * NSDeviceQueryResult.
 * @io: the request; io_Data is the NSDeviceQueryResult, io_Length its size.
 *
 * Return: 0, with io_Actual set; IOERR_BADADDRESS for no io_Data,
 * IOERR_BADLENGTH when io_Length is short of the structure.
 */
static BYTE Do_NSCMD_DEVICEQUERY(struct IOStdReq *io)
{
    struct NSDeviceQueryResult *dq = io->io_Data;

    if (dq == NULL)
        return IOERR_BADADDRESS;

    dq->nsdqr_SizeAvailable = sizeof(struct NSDeviceQueryResult);
    if (io->io_Length < dq->nsdqr_SizeAvailable)
        return IOERR_BADLENGTH;

    dq->nsdqr_DevQueryFormat = 0; /* the only format NSD defines */
    /* A SANA-II device that also speaks netdev: NSD has no device type for
     * the latter, and the command list says it. */
    dq->nsdqr_DeviceType = NSDEVTYPE_SANA2;
    dq->nsdqr_DeviceSubType = 0;
    dq->nsdqr_SupportedCommands = (UWORD *)GENET_SupportedCommands;
    io->io_Actual = dq->nsdqr_SizeAvailable;
    return 0;
}

/**
 * ProcessCommand() - Route a request from the unit port to its personality,
 * and reply it when it is done.
 * @io: the request; io_Unit is the unit.
 *
 * Unit task. netdev commands and NSCMD_DEVICEQUERY are answered here, with
 * io_Error from their handler. GENET_CMD_OPENER_CONTROL goes to
 * sana2_opener_control() and is not replied here. Everything else goes to
 * sana2_process_command(); a request it keeps (COMMAND_SCHEDULED) is replied
 * when it completes. A request without IOF_QUICK is replied with ReplyMsg().
 */
void ProcessCommand(struct IOStdReq *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->io_Unit;
    struct ExecBase *SysBase = unit->sysBase;
    UWORD cmd = io->io_Command;

    if (NETDEV_IS_CMD(cmd))
    {
        io->io_Error = netdev_process_command(unit, io);
    }
    else if (cmd == NSCMD_DEVICEQUERY)
    {
        io->io_Error = Do_NSCMD_DEVICEQUERY(io);
    }
    else if (cmd == GENET_CMD_OPENER_CONTROL)
    {
        sana2_opener_control(unit, (struct GenetOpenerControl *)io);
        return; /* one-way, and possibly freed with its opener */
    }
    else if (sana2_process_command(unit, (struct IOSana2Req *)io) == COMMAND_SCHEDULED)
    {
        return; /* kept: replied when it completes */
    }

    if (!(io->io_Flags & IOF_QUICK))
        ReplyMsg(&io->io_Message);
}

/* ----------------------------------------------------------- the task --- */

/*
 * The unit task: command processing for both personalities, interrupt
 * bottom half (RX delivery, TX-done), periodic housekeeping. It is the ONLY
 * context that calls the stack's nso_* callbacks.
 *
 * The datapath is state-driven, not event-driven: every wakeup, whatever
 * woke it, harvests TX and polls RX, then re-arms. That is why the ISR needs
 * to hand over nothing but a signal, and why the periodic timer carries no
 * datapath work of its own — a timer tick is simply one more wakeup.
 */
static void UnitTask(struct GenetUnit *unit, struct Task *parent)
{
    struct ExecBase *SysBase = unit->sysBase;
    unit->rx_signal = -1;
    unit->tx_signal = -1;
    unit->link_signal = -1;

    // Initialize the built in msg port, we'll receive commands here
    BYTE msg_sigbit = drv_unit_msgport_init(SysBase, &unit->unit);
    if (msg_sigbit == -1)
    {
        Kprintf("[genet] %s: Failed to allocate unit message signal\n", __func__);
        goto free_signals;
    }

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
    struct drv_timer tick;
    if (!drv_timer_open(&tick, SysBase))
    {
        Kprintf("[genet] %s: Failed to open timer device\n", __func__);
        goto free_signals;
    }
    drv_timer_arm_ms(&tick, GENET_PERIODIC_TASK_MS);
    unit->timerBase = tick.req->tr_node.io_Device;

    unit->task = FindTask(NULL);
    /* Signal parent that Unit task is up and running now */
    Signal(parent, SIGBREAKF_CTRL_F);

    KprintfT("[genet] %s: Entering main unit task loop\n", __func__);

    u32 link_poll_ticks = 0; /* counts up to GENET_LINK_POLL_TICKS */

    ULONG sigset;
    ULONG waitMask = (1UL << unit->unit.unit_MsgPort.mp_SigBit) |
                     drv_timer_sigmask(&tick) |
                     (1UL << unit->rx_signal) |
                     (1UL << unit->tx_signal) |
                     (1UL << unit->link_signal) |
                     SIGBREAKF_CTRL_C;

    do
    {
        sigset = Wait(waitMask);
        // KprintfT("[genet] %s: Woke up, sigset=0x%08lx\n", __func__, sigset);

        /* Datapath: every wakeup, whatever woke it (see netdev_service) -
         * netdev's while a stack is attached and started, SANA-II's while it
         * has the unit online. */
        if (likely(unit->ndStarted))
            netdev_service(unit, sigset);
        else if (unit->state == STATE_ONLINE)
            sana2_service(unit, sigset);

        // IO queue got a new message
        if (sigset & (1UL << unit->unit.unit_MsgPort.mp_SigBit))
        {
            u16 remaining = CMD_DRAIN_MAX;
            struct IOStdReq *io;
            /* Drain the command queue, bounded so a command flood can't starve
             * the datapath. Commands are rare and reply-gated, so this is only a
             * defensive cap; re-signal to finish next wakeup if we hit it. */
            while ((io = (struct IOStdReq *)GetMsg(&unit->unit.unit_MsgPort)) != NULL)
            {
                ProcessCommand(io);
                if (--remaining == 0)
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
        if (sigset & drv_timer_sigmask(&tick))
        {
            drv_timer_consume(&tick);

            if (unit->state == STATE_ONLINE)
            {
                /* PHY link poll — the reliable path to current link state
                 * (see bcmgenet_link_poll). MDIO reads are slow, so it runs at
                 * GENET_LINK_POLL_MS rather than on every housekeeping tick. */
                if (++link_poll_ticks >= GENET_LINK_POLL_TICKS)
                {
                    link_poll_ticks = 0;
                    bcmgenet_link_poll(unit, TRUE); /* poll: keep the latch, so short drops are seen */
                }

                sana2_tick(unit);         /* a parked S2_ONLINE's wait for the link */
                bcmgenet_perf_tick(unit); /* telemetry; compiled out below PROFILE */
            }

            /* Re-arm timer */
            drv_timer_arm_ms(&tick, GENET_PERIODIC_TASK_MS);
        }

        if (unlikely(sigset & SIGBREAKF_CTRL_C))
        {
            KprintfT("[genet] %s: Received SIGBREAKF_CTRL_C, stopping genet task\n", __func__);
            drv_timer_cancel(&tick);
        }
    } while ((sigset & SIGBREAKF_CTRL_C) == 0);

    /* Whatever reached the unit port after the last pass: an opener's list
     * change is still made (a removal frees the opener), a request is
     * answered rather than left waiting for a task that is gone. */
    struct IOStdReq *pending;
    while ((pending = (struct IOStdReq *)GetMsg(&unit->unit.unit_MsgPort)) != NULL)
    {
        if (pending->io_Command == GENET_CMD_OPENER_CONTROL)
        {
            sana2_opener_control(unit, (struct GenetOpenerControl *)pending);
            continue;
        }
        pending->io_Error = IOERR_ABORTED;
        ReplyMsg(&pending->io_Message);
    }

    unit->timerBase = NULL;
    drv_timer_close(&tick);
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

    /* drv_task_exit clears the liveness slot first (drv_task_join polls it),
     * then reports CTRL_F for a task that ran / CTRL_C for one that never got
     * to its loop (unit->task is set only once the loop is entered). */
    drv_task_exit(SysBase, &unit->task, parent, unit->task != NULL);
}

u32 UnitTaskStart(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->sysBase;
    return drv_task_spawn(SysBase, unit, UnitTask, "genet ethernet driver",
                          GENET_UNIT_STACK_BYTES,
                          unit->device->runtimeConfig.unit_task_priority) == 0
               ? GENET_OK
               : ENOMEM;
}

void UnitTaskStop(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->sysBase;
    drv_task_join(SysBase, &unit->task);
}
