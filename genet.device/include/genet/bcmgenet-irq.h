// SPDX-License-Identifier: GPL-2.0+
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

#endif