// SPDX-License-Identifier: GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <iomem.h>
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

static inline void bcmgenet_irq0_disable(struct GenetUnit *unit, u32 irq_mask)
{
	mmio_write32(irq_mask,
		   BCMGENET_REG(unit, GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_SET));
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
 */
ULONG bcmgenet_isr0(struct ExecBase *execBase asm("a6"), struct GenetUnit *unit asm("a1"), ULONG irq asm("d0"))
{
	(void)irq;
	(void)execBase;

	/* Read irq status */
	u32 status = mmio_read32(BCMGENET_REG(unit, GENET_INTRL2_0_OFF + INTRL2_CPU_STAT)) &
				   ~mmio_read32(BCMGENET_REG(unit, GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_STATUS));

	/* Nothing pending for us — report not-handled. */
	if (!status)
		return 0;

	/* Clear before handling so any new events after this point re-assert cleanly */
	mmio_write32(status, BCMGENET_REG(unit, GENET_INTRL2_0_OFF + INTRL2_CPU_CLEAR));

	KprintfT("[genet] %s: IRQ0 status: 0x%08lX\n", __func__, (ULONG)status);

	/* Sources fire together under load, so build the combined mask and
	 * spend a single MASK_SET write instead of one per source. */
	bcmgenet_irq0_disable(unit, status & (UMAC_IRQ_TXDMA_DONE | UMAC_IRQ_RXDMA_DONE |
										  UMAC_IRQ_LINK_EVENT | UMAC_IRQ_PHY_DET_R));

	ULONG signals = 0;
	if (status & UMAC_IRQ_RXDMA_DONE)
		signals |= 1UL << unit->rx_signal;
	if (status & UMAC_IRQ_TXDMA_DONE)
		signals |= 1UL << unit->tx_signal;
	if (status & (UMAC_IRQ_LINK_EVENT | UMAC_IRQ_PHY_DET_R))
		signals |= 1UL << unit->link_signal;

	if (status & UMAC_IRQ_TXDMA_DONE)
		unit->internalStats.irq0_tx_count++;
	if (status & UMAC_IRQ_RXDMA_DONE)
		unit->internalStats.irq0_rx_count++;
	unit->internalStats.irq0_count++;
	if (!(status & (UMAC_IRQ_TXDMA_DONE | UMAC_IRQ_RXDMA_DONE)))
		unit->internalStats.irq0_other_count++;

	Signal(unit->task, signals);
	return 1;
}
