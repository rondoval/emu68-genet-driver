#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <compat.h>
#include <debug.h>
#include <genet/bcmgenet-regs.h>
#include <device.h>

void bcmgenet_rx_ring_int_enable(struct GenetUnit *unit, int ring)
{
	writel(BIT(UMAC_IRQ1_RX_INTR_SHIFT + ring),
		   (ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_MASK_CLEAR);

	// bcmgenet_intrl2_1_writel(ring->priv,
	// 			 1 << (UMAC_IRQ1_RX_INTR_SHIFT + ring->index),
	// 			 INTRL2_CPU_MASK_CLEAR);
}

void bcmgenet_rx_ring_int_disable(struct GenetUnit *unit, int ring)
{
	writel(BIT(UMAC_IRQ1_RX_INTR_SHIFT + ring),
		   (ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_MASK_SET);

	// bcmgenet_intrl2_1_writel(ring->priv,
	// 			 1 << (UMAC_IRQ1_RX_INTR_SHIFT + ring->index),
	// 			 INTRL2_CPU_MASK_SET);
}

void bcmgenet_tx_ring_int_enable(struct GenetUnit *unit, int ring)
{
	writel(BIT(ring),
		   (ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_MASK_CLEAR);

	// bcmgenet_intrl2_1_writel(ring->priv, 1 << ring->index,
	// 			 INTRL2_CPU_MASK_CLEAR);
}

void bcmgenet_tx_ring_int_disable(struct GenetUnit *unit, int ring)
{
	writel(BIT(ring),
		   (ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_MASK_SET);

	// bcmgenet_intrl2_1_writel(ring->priv, 1 << ring->index,
	// 			 INTRL2_CPU_MASK_SET);
}

void bcmgenet_link_intr_enable(struct GenetUnit *unit)
{
	writel(UMAC_IRQ_LINK_EVENT,
		   (ULONG)unit->genetBase + GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_CLEAR);
}

void bcmgenet_phy_det_intr_enable(struct GenetUnit *unit)
{
	writel(UMAC_IRQ_PHY_DET_R,
		   (ULONG)unit->genetBase + GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_CLEAR);
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

	// bcmgenet_intrl2_0_writel(priv, 0xFFFFFFFF, INTRL2_CPU_MASK_SET);
	// bcmgenet_intrl2_0_writel(priv, 0xFFFFFFFF, INTRL2_CPU_CLEAR);
	// bcmgenet_intrl2_1_writel(priv, 0xFFFFFFFF, INTRL2_CPU_MASK_SET);
	// bcmgenet_intrl2_1_writel(priv, 0xFFFFFFFF, INTRL2_CPU_CLEAR);
}

/* bcmgenet_isr1: handle Rx and Tx queues */
void bcmgenet_isr1(struct ExecBase *SysBase asm("a6"), struct GenetUnit *unit asm("a1"), ULONG irq asm("d0"))
{
	(void)irq;

	/* Read irq status */
	ULONG status = readl((ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_STAT) &
				   ~readl((ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_MASK_STATUS);
	// status = bcmgenet_intrl2_1_readl(priv, INTRL2_CPU_STAT) &
	// 	~bcmgenet_intrl2_1_readl(priv, INTRL2_CPU_MASK_STATUS);

	/* clear interrupts */
	writel(status, (ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_CLEAR);
	// bcmgenet_intrl2_1_writel(priv, status, INTRL2_CPU_CLEAR);

	/* Check Rx priority queue interrupts */
	const ULONG rx_queue = 0;
	if (status & BIT(UMAC_IRQ1_RX_INTR_SHIFT + rx_queue))
	{
		// TODO NAPI-style bcmgenet_rx_ring_int_disable(rx_ring);
		Signal(unit->task, unit->rx_signal);
	}

	/* Check Tx priority queue interrupts */
	const ULONG tx_queue = 0;
	if (status & BIT(tx_queue))
	{
		// TODO bcmgenet_tx_ring_int_disable(tx_ring);
		Signal(unit->task, unit->tx_signal);
	}
}

/* bcmgenet_isr0: handle other stuff */
void bcmgenet_isr0(struct ExecBase *SysBase asm("a6"), struct GenetUnit *unit asm("a1"), ULONG irq asm("d0"))
{
	(void)irq;

	/* Read irq status */
	ULONG status = readl((ULONG)unit->genetBase + GENET_INTRL2_0_OFF + INTRL2_CPU_STAT) &
				   ~readl((ULONG)unit->genetBase + GENET_INTRL2_0_OFF + INTRL2_CPU_MASK_STATUS);
	// status = bcmgenet_intrl2_0_readl(priv, INTRL2_CPU_STAT) &
	// 		 ~bcmgenet_intrl2_0_readl(priv, INTRL2_CPU_MASK_STATUS);

	/* clear interrupts */
	writel(status, (ULONG)unit->genetBase + GENET_INTRL2_0_OFF + INTRL2_CPU_CLEAR);
	// bcmgenet_intrl2_0_writel(priv, status, INTRL2_CPU_CLEAR);

	// if (bcmgenet_has_mdio_intr(priv) && status & UMAC_IRQ_MDIO_EVENT)
	// 	wake_up(&priv->wq);

	/* all other interested interrupts handled in bottom half */
	status &= (UMAC_IRQ_LINK_EVENT | UMAC_IRQ_PHY_DET_R);
	if (status)
	{
		/* Save irq status for bottom-half processing. */
		unit->irq0_status |= status;
		Signal(unit->task, unit->phy_signal);
	}
}
