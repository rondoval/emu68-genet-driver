// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * stubs.c - the driver functions a logic program does not build (support.h):
 * weak, so the real one wins in a program that builds its source. Those whose
 * call is worth seeing are recorded; the others return what does no harm.
 */

#include <exec/types.h>
#include <exec/errors.h>

#include <device.h>
#include <sana2_unit.h>
#include <genet/bcmgenet.h>
#include <genet/bcmgenet-irq.h>
#include <genet/bcmgenet-mib.h>
#include <genet/bcmgenet-priv.h>
#include <genet/bcmgenet-tx.h>
#include <genet/phy.h>
#include <dma_mem.h>

#include <proto/exec.h>
#include <exec/memory.h>

#include "support.h"

#define WEAK __attribute__((weak))

/**
 * sana2_report_events() - Record the events reported.
 * @unit: the unit.
 * @events: the S2EVENT_* bits.
 */
WEAK void sana2_report_events(struct GenetUnit *unit, u32 events)
{
    (void)unit;
    logic_record(LOGIC_EVENTS, NULL, events);
}

/**
 * bcmgenet_mib_reset() - Nothing to reset.
 * @unit: the unit.
 */
WEAK void bcmgenet_mib_reset(struct GenetUnit *unit)
{
    (void)unit;
}

/**
 * bcmgenet_intr_disable() - No interrupts to disable.
 * @unit: the unit.
 */
WEAK void bcmgenet_intr_disable(struct GenetUnit *unit)
{
    (void)unit;
}

/**
 * bcmgenet_irq0_enable() - Record the interrupts enabled.
 * @unit: the unit.
 * @irq_mask: the interrupts.
 */
WEAK void bcmgenet_irq0_enable(struct GenetUnit *unit, u32 irq_mask)
{
    (void)unit;
    logic_record(LOGIC_IRQ0_ENABLE, NULL, irq_mask);
}

/**
 * bcmgenet_set_rx_mode() - Record that the reception filter was programmed.
 * @unit: the unit.
 */
WEAK void bcmgenet_set_rx_mode(struct GenetUnit *unit)
{
    (void)unit;
    logic_record(LOGIC_SET_RX_MODE, NULL, 0);
}

/* The unit's lifecycle (unit.c): every step succeeds and touches no hardware. */
/**
 * UnitConfigure() - The unit is configured.
 * @unit: the unit.
 *
 * Return: GENET_OK.
 */
WEAK u32 UnitConfigure(struct GenetUnit *unit)
{
    (void)unit;
    return GENET_OK;
}

/**
 * UnitOnline() - The unit goes online.
 * @unit: the unit.
 *
 * Return: GENET_OK.
 */
WEAK u32 UnitOnline(struct GenetUnit *unit)
{
    (void)unit;
    return GENET_OK;
}

/**
 * UnitOffline() - The unit goes offline.
 * @unit: the unit.
 */
WEAK void UnitOffline(struct GenetUnit *unit)
{
    (void)unit;
}

/**
 * bcmgenet_link_publish() - Nothing to publish.
 * @unit: the unit.
 * @up: whether the link is up.
 * @force: publish without a change too.
 */
WEAK void bcmgenet_link_publish(struct GenetUnit *unit, BOOL up, BOOL force)
{
    (void)unit;
    (void)up;
    (void)force;
}

/* RX and TX rings and the RX pool: empty, and nothing to move. */
/**
 * bcmgenet_init_rx_queues() - The RX ring is set up.
 * @unit: the unit.
 *
 * Return: GENET_OK.
 */
WEAK u32 bcmgenet_init_rx_queues(struct GenetUnit *unit)
{
    (void)unit;
    return GENET_OK;
}

/**
 * bcmgenet_netdev_rx() - An empty netdev RX pass.
 * @unit: the unit.
 * @limit: the most frames to take.
 *
 * Return: 0, no frames.
 */
WEAK s32 bcmgenet_netdev_rx(struct GenetUnit *unit, u16 limit)
{
    (void)unit;
    (void)limit;
    return 0;
}

/**
 * bcmgenet_sana2_rx() - An empty SANA-II RX pass.
 * @unit: the unit.
 * @limit: the most frames to take.
 *
 * Return: 0, no frames.
 */
WEAK s32 bcmgenet_sana2_rx(struct GenetUnit *unit, u16 limit)
{
    (void)unit;
    (void)limit;
    return 0;
}

/**
 * bcmgenet_netdev_tx_submit() - A TX ring that takes nothing.
 * @unit: the unit.
 * @descs: the descriptors.
 * @count: how many.
 *
 * Return: 0, none accepted.
 */
WEAK LONG bcmgenet_netdev_tx_submit(struct GenetUnit *unit, const struct NetDevTxDesc *descs, ULONG count)
{
    (void)unit;
    (void)descs;
    (void)count;
    return 0;
}

/**
 * bcmgenet_netdev_tx_kick() - No doorbell to ring.
 * @unit: the unit.
 */
WEAK void bcmgenet_netdev_tx_kick(struct GenetUnit *unit)
{
    (void)unit;
}

/**
 * bcmgenet_tx_harvest() - Nothing sent to harvest.
 * @unit: the unit.
 *
 * Return: 0, no cookies.
 */
WEAK ULONG bcmgenet_tx_harvest(struct GenetUnit *unit)
{
    (void)unit;
    return 0;
}

/**
 * bcmgenet_tx_done_arm() - No TX-done interrupt to arm.
 * @unit: the unit.
 */
WEAK void bcmgenet_tx_done_arm(struct GenetUnit *unit)
{
    (void)unit;
}

/**
 * bcmgenet_netdev_tx_quiesce() - Nothing in flight to quiesce.
 * @unit: the unit.
 */
WEAK void bcmgenet_netdev_tx_quiesce(struct GenetUnit *unit)
{
    (void)unit;
}

/**
 * bcmgenet_rx_pool_create() - The RX pool is made.
 * @unit: the unit.
 * @buffers: the pool's size.
 *
 * Return: GENET_OK.
 */
WEAK u32 bcmgenet_rx_pool_create(struct GenetUnit *unit, u32 buffers)
{
    (void)unit;
    (void)buffers;
    return GENET_OK;
}

/**
 * bcmgenet_rx_pool_destroy() - No RX pool to free.
 * @unit: the unit.
 */
WEAK void bcmgenet_rx_pool_destroy(struct GenetUnit *unit)
{
    (void)unit;
}

/**
 * bcmgenet_rx_pool_push() - A buffer back to a pool that keeps none.
 * @unit: the unit.
 * @buffer: the buffer.
 */
WEAK void bcmgenet_rx_pool_push(struct GenetUnit *unit, APTR buffer)
{
    (void)unit;
    (void)buffer;
}

/* MIB counters: all zero. */
/**
 * bcmgenet_mib_fill() - No MIB counters.
 * @unit: the unit.
 * @out: the counters' room.
 * @max: how many fit.
 *
 * Return: 0, none filled.
 */
WEAK u16 bcmgenet_mib_fill(struct GenetUnit *unit, struct NetDevCounter *out, u16 max)
{
    (void)unit;
    (void)out;
    (void)max;
    return 0;
}

/**
 * bcmgenet_mib_rbuf_ovfl() - No RBUF overflows.
 * @unit: the unit.
 *
 * Return: 0.
 */
WEAK u32 bcmgenet_mib_rbuf_ovfl(struct GenetUnit *unit)
{
    (void)unit;
    return 0;
}

/**
 * bcmgenet_mib_rbuf_ovfl_rebase() - Nothing to rebase.
 * @unit: the unit.
 */
WEAK void bcmgenet_mib_rbuf_ovfl_rebase(struct GenetUnit *unit)
{
    (void)unit;
}

/**
 * bcmgenet_mib_tx_errors() - No transmit errors.
 * @unit: the unit.
 *
 * Return: 0.
 */
WEAK u32 bcmgenet_mib_tx_errors(struct GenetUnit *unit)
{
    (void)unit;
    return 0;
}

/* SANA-II statistics (sana2_stats.c): answered, with nothing filled in. */
/**
 * sana2_get_global_stats() - S2_GETGLOBALSTATS, answered with nothing filled in.
 * @unit: the unit.
 * @io: the request.
 *
 * Return: COMMAND_PROCESSED.
 */
WEAK u32 sana2_get_global_stats(struct GenetUnit *unit, struct IOSana2Req *io)
{
    (void)unit;
    (void)io;
    return COMMAND_PROCESSED;
}

/**
 * sana2_get_extended_stats() - S2_GETEXTENDEDGLOBALSTATS, answered with nothing filled in.
 * @unit: the unit.
 * @io: the request.
 *
 * Return: COMMAND_PROCESSED.
 */
WEAK u32 sana2_get_extended_stats(struct GenetUnit *unit, struct IOSana2Req *io)
{
    (void)unit;
    (void)io;
    return COMMAND_PROCESSED;
}

/**
 * sana2_get_special_stats() - S2_GETSPECIALSTATS, answered with nothing filled in.
 * @unit: the unit.
 * @io: the request.
 *
 * Return: COMMAND_PROCESSED.
 */
WEAK u32 sana2_get_special_stats(struct GenetUnit *unit, struct IOSana2Req *io)
{
    (void)unit;
    (void)io;
    return COMMAND_PROCESSED;
}

/**
 * sana2_sample_throughput() - S2_SAMPLE_THROUGHPUT, answered at once.
 * @unit: the unit.
 * @io: the request.
 *
 * Return: COMMAND_PROCESSED.
 */
WEAK u32 sana2_sample_throughput(struct GenetUnit *unit, struct IOSana2Req *io)
{
    (void)unit;
    (void)io;
    return COMMAND_PROCESSED;
}

/**
 * sana2_throughput_restart() - No sampling to restart.
 * @unit: the unit.
 */
WEAK void sana2_throughput_restart(struct GenetUnit *unit)
{
    (void)unit;
}

/**
 * sana2_throughput_tick() - No sampling to update.
 * @unit: the unit.
 */
WEAK void sana2_throughput_tick(struct GenetUnit *unit)
{
    (void)unit;
}

/* SANA-II transmission (sana2_tx.c): no staging, no backlog. An abort is recorded. */
/**
 * sana2_tx_staging_create() - The TX staging is made.
 * @unit: the unit.
 *
 * Return: GENET_OK.
 */
WEAK u32 sana2_tx_staging_create(struct GenetUnit *unit)
{
    (void)unit;
    return GENET_OK;
}

/**
 * sana2_tx_staging_destroy() - No TX staging to free.
 * @unit: the unit.
 */
WEAK void sana2_tx_staging_destroy(struct GenetUnit *unit)
{
    (void)unit;
}

/**
 * sana2_tx_drain() - No backlog to send.
 * @unit: the unit.
 */
WEAK void sana2_tx_drain(struct GenetUnit *unit)
{
    (void)unit;
}

/**
 * sana2_tx_backlog_abort() - Record that every waiting write was answered.
 * @unit: the unit.
 * @error: the io_Error.
 * @wireError: the ios2_WireError.
 */
WEAK void sana2_tx_backlog_abort(struct GenetUnit *unit, BYTE error, ULONG wireError)
{
    (void)unit;
    (void)error;
    (void)wireError;
    logic_record(LOGIC_TX_ABORT, NULL, 0);
}

/**
 * sana2_tx_backlog_abort_opener() - Record that the opener's waiting writes were answered.
 * @unit: the unit.
 * @opener: the Opener.
 * @error: the io_Error.
 * @wireError: the ios2_WireError.
 */
WEAK void sana2_tx_backlog_abort_opener(struct GenetUnit *unit, struct Opener *opener, BYTE error,
                                        ULONG wireError)
{
    (void)unit;
    (void)error;
    (void)wireError;
    logic_record(LOGIC_TX_ABORT, opener, 0);
}

/* The other personality's link report, the MAC's and the PHY's side of the link: nothing to do. */
/**
 * netdev_link_notify() - No netdev stack to tell.
 * @unit: the unit.
 */
WEAK void netdev_link_notify(struct GenetUnit *unit)
{
    (void)unit;
}

/**
 * bcmgenet_mac_config() - The MAC follows the link.
 * @unit: the unit.
 *
 * Return: GENET_OK.
 */
WEAK u32 bcmgenet_mac_config(struct GenetUnit *unit)
{
    (void)unit;
    return GENET_OK;
}

/**
 * bcmgenet_mac_link_down() - The MAC drops the link.
 * @unit: the unit.
 */
WEAK void bcmgenet_mac_link_down(struct GenetUnit *unit)
{
    (void)unit;
}

/**
 * genphy_read_status() - The PHY's status read.
 * @phydev: the PHY.
 * @polling: a poll, not an interrupt.
 *
 * Return: 0.
 */
WEAK s32 genphy_read_status(struct phy_device *phydev, BOOL polling)
{
    (void)phydev;
    (void)polling;
    return 0;
}

/**
 * genphy_config_aneg() - Autonegotiation configured.
 * @phydev: the PHY.
 *
 * Return: 0.
 */
WEAK s32 genphy_config_aneg(struct phy_device *phydev)
{
    (void)phydev;
    return 0;
}

/* The personalities, as the unit task calls them: every command unknown, nothing to service. */
/**
 * netdev_process_command() - A netdev command nobody serves.
 * @unit: the unit.
 * @io: the request.
 *
 * Return: IOERR_NOCMD.
 */
WEAK BYTE netdev_process_command(struct GenetUnit *unit, struct IOStdReq *io)
{
    (void)unit;
    (void)io;
    return IOERR_NOCMD;
}

/**
 * sana2_process_command() - A SANA-II command nobody serves.
 * @unit: the unit.
 * @io: the request.
 *
 * Return: COMMAND_PROCESSED, io_Error IOERR_NOCMD.
 */
WEAK u32 sana2_process_command(struct GenetUnit *unit, struct IOSana2Req *io)
{
    (void)unit;
    io->ios2_Req.io_Error = IOERR_NOCMD;
    return COMMAND_PROCESSED;
}

/**
 * sana2_opener_control() - An opener change nobody carries out.
 * @unit: the unit.
 * @request: the change.
 */
WEAK void sana2_opener_control(struct GenetUnit *unit, struct GenetOpenerControl *request)
{
    (void)unit;
    (void)request;
}

/**
 * netdev_service() - No netdev datapath to service.
 * @unit: the unit.
 * @sigset: the signals that woke the unit task.
 */
WEAK void netdev_service(struct GenetUnit *unit, ULONG sigset)
{
    (void)unit;
    (void)sigset;
}

/**
 * sana2_service() - No SANA-II datapath to service.
 * @unit: the unit.
 * @sigset: the signals that woke the unit task.
 */
WEAK void sana2_service(struct GenetUnit *unit, ULONG sigset)
{
    (void)unit;
    (void)sigset;
}

/**
 * sana2_tick() - No SANA-II housekeeping.
 * @unit: the unit.
 */
WEAK void sana2_tick(struct GenetUnit *unit)
{
    (void)unit;
}

/* The link poll and the PHY: no change. */
/**
 * bcmgenet_link_poll() - The link poll finds no change.
 * @unit: the unit.
 * @polling: a poll, not an interrupt.
 */
WEAK void bcmgenet_link_poll(struct GenetUnit *unit, BOOL polling)
{
    (void)unit;
    (void)polling;
}

/**
 * bcmgenet_phy_refresh_forced() - No forced settings to push again.
 * @unit: the unit.
 */
WEAK void bcmgenet_phy_refresh_forced(struct GenetUnit *unit)
{
    (void)unit;
}

/**
 * bcmgenet_link_irq_mask() - No link interrupts.
 * @unit: the unit.
 *
 * Return: 0.
 */
WEAK u32 bcmgenet_link_irq_mask(const struct GenetUnit *unit)
{
    (void)unit;
    return 0;
}

/**
 * bcmgenet_tx_irq_arm() - Record that one TX-done interrupt was asked for.
 * @unit: the unit.
 */
WEAK void bcmgenet_tx_irq_arm(struct GenetUnit *unit)
{
    (void)unit;
    logic_record(LOGIC_TX_IRQ_ARM, NULL, 0);
}

/* The DMA pool (emu68-common dma_mem.c) needs the device tree's regions: plain memory stands in. */

/**
 * dma_pool_region_alloc() - Memory from the real exec, as a region of the pool would give it.
 * @pool: the pool; unused.
 * @size: its size.
 *
 * Return: the memory, cleared, or NULL.
 */
WEAK APTR dma_pool_region_alloc(struct dma_pool *pool, ULONG size)
{
    (void)pool;
    return AllocVec(size, MEMF_CLEAR);
}

/**
 * dma_pool_region_free() - Give dma_pool_region_alloc() memory back.
 * @pool: the pool; unused.
 * @ptr: the memory.
 * @size: its size; unused.
 */
WEAK void dma_pool_region_free(struct dma_pool *pool, APTR ptr, ULONG size)
{
    (void)pool;
    (void)size;
    FreeVec(ptr);
}
