#ifndef BCMGENET_IRQ_H
#define BCMGENET_IRQ_H

#include <genet/bcmgenet-regs.h>
#include <compat.h>
#include <device.h>

/* Interrupt enable/disable */
void bcmgenet_irq0_enable(struct GenetUnit *unit, ULONG irq_mask);
void bcmgenet_irq0_disable(struct GenetUnit *unit, ULONG irq_mask);
void bcmgenet_intr_disable(struct GenetUnit *unit);

/* Interrupt handler */
void bcmgenet_isr0(struct ExecBase *SysBase asm("a6"), struct GenetUnit *unit asm("a1"), ULONG irq asm("d0"));

#ifdef USE_PRIORITY_QUEUES
void bcmgenet_rx_ring_int_enable(struct GenetUnit *unit, int ring);
void bcmgenet_rx_ring_int_disable(struct GenetUnit *unit, int ring);
void bcmgenet_tx_ring_int_enable(struct GenetUnit *unit, int ring);
void bcmgenet_tx_ring_int_disable(struct GenetUnit *unit, int ring);

/* Helpers for clearing interrupt status */
static inline void bcmgenet_rx_ring_int_clear(struct GenetUnit *unit, int ring)
{
    writel(BIT(UMAC_IRQ1_RX_INTR_SHIFT  + ring),
           (ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_CLEAR);
}

static inline void bcmgenet_tx_ring_int_clear(struct GenetUnit *unit, int ring)
{
    writel(BIT(ring),
           (ULONG)unit->genetBase + GENET_INTRL2_1_OFF + INTRL2_CPU_CLEAR);
}

/* IRQ1 is the interrupt for queues other than the default one */
void bcmgenet_isr1(struct ExecBase *SysBase asm("a6"), struct GenetUnit *unit asm("a1"), ULONG irq asm("d0"));
#endif

#endif