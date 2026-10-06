// SPDX-License-Identifier: GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function: see GenetUnit.sysBase */
#include <proto/exec.h>
#endif

#include <iomem.h>
#include <debug.h>
#include <genet/bcmgenet-irq.h>
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
