#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <compat.h>
#include <debug.h>
#include <genet/bcmgenet-regs.h>
#include <device.h>

/*
 * IRQ1 is currently unsused, as it only contains per-priority queue RX/TX interrupts.
 * We're not using priority queues, so these are not generated.
 */

#ifdef USE_PRIORITY_QUEUES
 void bcmgenet_rx_ring_int_enable(struct GenetUnit *unit, int ring)
{
	writel(BIT(UMAC_IRQ1_RX_INTR_SHIFT + ring),
		   (ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_MASK_CLEAR);
}

void bcmgenet_rx_ring_int_disable(struct GenetUnit *unit, int ring)
{
	writel(BIT(UMAC_IRQ1_RX_INTR_SHIFT + ring),
		   (ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_MASK_SET);
}

void bcmgenet_tx_ring_int_enable(struct GenetUnit *unit, int ring)
{
	writel(BIT(ring),
		   (ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_MASK_CLEAR);
}

void bcmgenet_tx_ring_int_disable(struct GenetUnit *unit, int ring)
{
	writel(BIT(ring),
		   (ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_MASK_SET);
}
#endif

void bcmgenet_irq0_enable(struct GenetUnit *unit, ULONG irq_mask)
{
	writel(irq_mask,
		   (ULONG)unit->genetBase + GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_CLEAR);
}

void bcmgenet_irq0_disable(struct GenetUnit *unit, ULONG irq_mask)
{
	writel(irq_mask,
		   (ULONG)unit->genetBase + GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_SET);
}

void bcmgenet_intr_disable(struct GenetUnit *unit)
{
	/* Mask all interrupts.*/
	writel(0xFFFFFFFF,
		   (ULONG)unit->genetBase + GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_SET);
	writel(0xFFFFFFFF,
		   (ULONG)unit->genetBase + GENET_INTRL2_0_OFF + INTRL2_CPU_CLEAR);
	writel(0xFFFFFFFF,
		   (ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_MASK_SET);
	writel(0xFFFFFFFF,
		   (ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_CLEAR);
}

#ifdef USE_PRIORITY_QUEUES
/* bcmgenet_isr1: handle Rx and Tx queues */
void bcmgenet_isr1(struct ExecBase *SysBase asm("a6"), struct GenetUnit *unit asm("a1"), ULONG irq asm("d0"))
{
	(void)irq;

	/* Read irq status */
	ULONG status = readl((ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_STAT) &
				   ~readl((ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_MASK_STATUS);

	/* clear interrupts */
	writel(status, (ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_CLEAR);

	Kprintf("[%s] IRQ1 status: 0x%08lX\n", __func__, status);

	/* Check Rx priority queue interrupts */
	const ULONG rx_queue = 0;
	if (status & BIT(UMAC_IRQ1_RX_INTR_SHIFT + rx_queue))
	{
		// TODO NAPI-style bcmgenet_rx_ring_int_disable(rx_ring);
		Signal(unit->task, 1UL << unit->rx_signal);
	}

	/* Check Tx priority queue interrupts */
	const ULONG tx_queue = 0;
	if (status & BIT(tx_queue))
	{
		// TODO bcmgenet_tx_ring_int_disable(tx_ring);
		Signal(unit->task, 1UL << unit->tx_signal);
	}
}
#endif

/* bcmgenet_isr0: handle other stuff */
void bcmgenet_isr0(struct ExecBase *SysBase asm("a6"), struct GenetUnit *unit asm("a1"), ULONG irq asm("d0"))
{
	(void)irq;

	/* Read irq status */
	ULONG status = readl((ULONG)unit->genetBase + GENET_INTRL2_0_OFF + INTRL2_CPU_STAT) &
				   ~readl((ULONG)unit->genetBase + GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_STATUS);

	/* Disable interrupts so that we're not flooded until bottom-half catches up */
	bcmgenet_irq0_disable(unit, status & (UMAC_IRQ_RXDMA_DONE | UMAC_IRQ_TXDMA_DONE));

	/* clear interrupts */
	writel(status, (ULONG)unit->genetBase + GENET_INTRL2_0_OFF + INTRL2_CPU_CLEAR);

	// if (bcmgenet_has_mdio_intr(priv) && status & UMAC_IRQ_MDIO_EVENT)
	// 	wake_up(&priv->wq);
	KprintfH("[genet] %s: IRQ0 status: 0x%08lX unit: 0x%08lx\n", __func__, status, (ULONG)unit);

	/* Save irq status for bottom-half processing. */
	unit->irq0_status |= status;
	Signal(unit->task, 1UL << unit->irq0_signal);
}
