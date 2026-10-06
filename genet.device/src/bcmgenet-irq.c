// SPDX-License-Identifier: GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function: see GenetUnit.sysBase */
#include <proto/exec.h>
#endif

#include <iomem.h>
#include <intserver.h>
#include <debug.h>
#include <genet/bcmgenet-irq.h>
#include <genet/bcmgenet-regs.h>
#include <genet/phy.h>
#include <device.h>

/*
 * IRQ1 is currently unsused, as it only contains per-priority queue RX/TX interrupts.
 * We're not using priority queues, so these are not generated.
 */

void bcmgenet_irq0_enable(struct GenetUnit *unit, u32 irq_mask)
{
	mmio_write32(irq_mask,
		   BCMGENET_REG(unit, GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_CLEAR));
}

/* Drop latched status. A masked source keeps latching, so whoever unmasks one
 * that has been off for a while clears it first, or takes an interrupt for
 * history instead of for the next event. */
void bcmgenet_irq0_clear(struct GenetUnit *unit, u32 irq_mask)
{
	mmio_write32(irq_mask,
		   BCMGENET_REG(unit, GENET_INTRL2_0_OFF + INTRL2_CPU_CLEAR));
}

/* Link sources worth an interrupt. UMAC_IRQ_PHY_DET_R only matters with
 * autonegotiation off — a PHY that reset has to have its forced settings
 * pushed again; with autoneg on there is nothing to do about it, and asking
 * for the event would buy an MDIO status read (slow) per occurrence. */
u32 bcmgenet_link_irq_mask(const struct GenetUnit *unit)
{
	u32 mask = UMAC_IRQ_LINK_EVENT;
	if (unit->phydev != NULL && unit->phydev->autoneg != AUTONEG_ENABLE)
		mask |= UMAC_IRQ_PHY_DET_R;
	return mask;
}

void bcmgenet_intr_disable(struct GenetUnit *unit)
{
	/* Mask all interrupts.*/
	mmio_write32(0xFFFFFFFF,
		   BCMGENET_REG(unit, GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_SET));
	mmio_write32(0xFFFFFFFF,
		   BCMGENET_REG(unit, GENET_INTRL2_0_OFF + INTRL2_CPU_CLEAR));
	mmio_write32(0xFFFFFFFF,
		   BCMGENET_REG(unit, GENET_INTRL2_1_OFF + INTRL2_CPU_MASK_SET));
	mmio_write32(0xFFFFFFFF,
		   BCMGENET_REG(unit, GENET_INTRL2_1_OFF + INTRL2_CPU_CLEAR));
}

/*
 * bcmgenet_isr0. Returns 1 if the interrupt was ours, 0 otherwise (the
 * AmigaOS interrupt-server "handled?" convention).
 *
 * Nothing but signals crosses to the bottom half. Exec's Signal() is the
 * atomic, level-latched interrupt-to-task channel. All handled
 * sources are masked here and re-armed once the task has caught up, so a
 * storming source costs one bottom-half pass, not one interrupt per event.
 *
 * MMIO is relaxed throughout with one barrier before the return: Device-nGnRE
 * already orders the accesses, but may ack a write early, and the CPU_CLEAR
 * has to land before the GIC re-samples the line. Reads carry their own
 * completion, so the not-ours path pays for no barrier at all.
 */
EMU68_INTSERVER(bcmgenet_isr0)
ULONG bcmgenet_isr0(struct ExecBase *SysBase asm("a6"), struct GenetUnit *unit asm("a1"),
                    ULONG irq asm("d0"))
{
	(void)irq;

	APTR base = unit->genetBase;

	/* Read irq status */
	u32 status = mmio_read32_relaxed(base + GENET_INTRL2_0_OFF + INTRL2_CPU_STAT) &
				   ~mmio_read32_relaxed(base + GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_STATUS);

	/* Nothing pending for us — report not-handled (Z set), so a shared
	 * line's chain walk carries on to the next server. */
	if (!status)
		return 0;

	/* Clear before handling so any new events after this point re-assert cleanly */
	mmio_write32_relaxed(status, base + GENET_INTRL2_0_OFF + INTRL2_CPU_CLEAR);

	KprintfT("[genet] %s: IRQ0 status: 0x%08lX\n", __func__, (ULONG)status);

	/* Sources fire together under load, so build the combined mask and
	 * spend a single MASK_SET write instead of one per source. */
	mmio_write32_relaxed(status & (UMAC_IRQ_TXDMA_DONE | UMAC_IRQ_RXDMA_DONE |
								   UMAC_IRQ_LINK_EVENT | UMAC_IRQ_PHY_DET_R),
						 base + GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_SET);

	ULONG signals = 0;
	if (status & UMAC_IRQ_RXDMA_DONE)
	{
		signals |= 1UL << unit->rx_signal;
		unit->internalStats.irq0_rx_count++;
#ifdef PROFILE
		u32 now = get_time() | 1; /* 0 means "no stamp" */
		if (unit->gu_RxIrqLast != 0)
			PERF_HIST_ADD(&unit->gu_RxIrqGapHist, now - unit->gu_RxIrqLast);
		unit->gu_RxIrqLast = now;
		unit->gu_RxIrqPending = now;
#endif
	}
	if (status & UMAC_IRQ_TXDMA_DONE)
	{
		signals |= 1UL << unit->tx_signal;
		unit->internalStats.irq0_tx_count++;
	}
	if (status & (UMAC_IRQ_LINK_EVENT | UMAC_IRQ_PHY_DET_R))
		signals |= 1UL << unit->link_signal;

	unit->internalStats.irq0_count++;
	if (!(status & (UMAC_IRQ_TXDMA_DONE | UMAC_IRQ_RXDMA_DONE)))
		unit->internalStats.irq0_other_count++;

	Signal(unit->task, signals);

	emu68_barrier();
	return 1;
}
