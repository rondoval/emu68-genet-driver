// SPDX-License-Identifier: GPL-2.0+
#ifndef BCMGENET_IRQ_H
#define BCMGENET_IRQ_H

#include <types.h>
#include <genet/bcmgenet-regs.h>
#include <device.h>

/* Interrupt enable/disable */
void bcmgenet_irq0_enable(struct GenetUnit *unit, u32 irq_mask);
void bcmgenet_intr_disable(struct GenetUnit *unit);

/* The link sources worth an interrupt, which depends on whether the PHY is
 * autonegotiating — see the definition. */
u32 bcmgenet_link_irq_mask(const struct GenetUnit *unit);

/* Interrupt handler */
ULONG bcmgenet_isr0(struct ExecBase *execBase asm("a6"), struct GenetUnit *unit asm("a1"), ULONG irq asm("d0"));

#endif