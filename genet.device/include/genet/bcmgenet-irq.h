#ifndef BCMGENET_IRQ_H
#define BCMGENET_IRQ_H

#include <genet/bcmgenet-regs.h>
#include <compat.h>
#include <device.h>

/* Interrupt enable/disable */
void bcmgenet_rx_ring_int_enable(struct GenetUnit *unit, int ring);
void bcmgenet_rx_ring_int_disable(struct GenetUnit *unit, int ring);
void bcmgenet_tx_ring_int_enable(struct GenetUnit *unit, int ring);
void bcmgenet_tx_ring_int_disable(struct GenetUnit *unit, int ring);
void bcmgenet_link_intr_enable(struct GenetUnit *unit);
void bcmgenet_phy_det_intr_enable(struct GenetUnit *unit);
void bcmgenet_intr_disable(struct GenetUnit *unit);

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

/* Interrupt handlers */
void bcmgenet_isr0(struct ExecBase *SysBase asm("a6"), struct GenetUnit *unit asm("a1"), ULONG irq asm("d0"));
void bcmgenet_isr1(struct ExecBase *SysBase asm("a6"), struct GenetUnit *unit asm("a1"), ULONG irq asm("d0"));

#endif