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
#include <genet/bcmgenet.h>
#include <genet/bcmgenet-regs.h>
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

void bcmgenet_irq0_disable(struct GenetUnit *unit, u32 irq_mask)
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

/* bcmgenet_isr0: handle other stuff */
void bcmgenet_isr0(struct ExecBase *execBase asm("a6"), struct GenetUnit *unit asm("a1"), ULONG irq asm("d0"))
{
	(void)irq;
	(void)execBase;

	/* Read irq status */
	u32 status = mmio_read32(BCMGENET_REG(unit, GENET_INTRL2_0_OFF + INTRL2_CPU_STAT)) &
				   ~mmio_read32(BCMGENET_REG(unit, GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_STATUS));

	if (status & UMAC_IRQ_TXDMA_DONE)
	{
		bcmgenet_tx_reclaim(unit, unit->budget);
	}

	/* Disable interrupts so that we're not flooded until bottom-half catches up */
	if (status & UMAC_IRQ_RXDMA_DONE)
		bcmgenet_irq0_disable(unit, UMAC_IRQ_RXDMA_DONE);

	/* clear interrupts */
	mmio_write32(status, BCMGENET_REG(unit, GENET_INTRL2_0_OFF + INTRL2_CPU_CLEAR));

	// if (bcmgenet_has_mdio_intr(priv) && status & UMAC_IRQ_MDIO_EVENT)
	// 	wake_up(&priv->wq);
	KprintfH("[genet] %s: IRQ0 status: 0x%08lX unit: 0x%08lx\n", __func__, (ULONG)status, (ULONG)unit);

	status &= ~UMAC_IRQ_TXDMA_DONE;
	if (status)
	{
		unit->internalStats.irq0_count++;
		if ((status & (UMAC_IRQ_TXDMA_DONE | UMAC_IRQ_RXDMA_DONE)) == 0)
			unit->internalStats.irq0_other_count++;
		/* Save irq status for bottom-half processing. */
		unit->irq0_status |= status;
		Signal(unit->task, 1UL << unit->irq0_signal);
	}
}
