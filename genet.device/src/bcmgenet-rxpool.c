// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * Broadcom GENETv5 — the RX buffer pool: one DMA slab of RX_BUF_LENGTH
 * buffers and the free stack the RX ring slots draw from. Unit task context
 * only, like everything that pops or pushes it.
 */

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function: see GenetUnit.sysBase */
#include <proto/exec.h>
#endif

#include <exec/types.h>

#include <cache_ops.h>
#include <debug.h>
#include <dma_mem.h>
#include <errors.h>
#include <memory.h>
#include <types.h>
#include <device.h>

#include <genet/bcmgenet.h>
#include <genet/bcmgenet-regs.h>

#ifdef DEBUG
/**
 * bcmgenet_rx_buf_ok() - Tell whether a pointer is a valid, slot-aligned pool
 * buffer. DEBUG builds only.
 * @unit: the unit.
 * @buffer: the pointer.
 *
 * Return: TRUE iff @buffer is a buffer of the slab, at a slot boundary.
 */
BOOL bcmgenet_rx_buf_ok(const struct GenetUnit *unit, APTR buffer)
{
	ULONG off = (ULONG)buffer - (ULONG)unit->rxSlab;
	return unit->rxSlab != NULL &&
		   off < unit->rxPoolTotal * RX_BUF_LENGTH &&
		   (off % RX_BUF_LENGTH) == 0;
}
#endif

/**
 * bcmgenet_rx_pool_pop() - Take a buffer off the free stack.
 * @unit: the unit.
 *
 * Unit task.
 *
 * Return: the buffer, or NULL when the pool is empty.
 */
APTR bcmgenet_rx_pool_pop(struct GenetUnit *unit)
{
	if (unit->rxFreeCount == 0)
		return NULL;
	return unit->rxFree[--unit->rxFreeCount];
}

/**
 * bcmgenet_rx_pool_push() - Put a buffer back on the free stack.
 * @unit: the unit.
 * @buffer: one of the pool's buffers.
 *
 * Unit task. A DEBUG build refuses, and logs, a buffer that is not the pool's
 * or one more than the pool holds.
 */
void bcmgenet_rx_pool_push(struct GenetUnit *unit, APTR buffer)
{
#ifdef DEBUG
	/* Guard the pool invariant (live buffers == pool size) instead of
	 * trusting it: a double-release or a poisoned recycle-ring entry would
	 * otherwise overrun rxFree[] and re-arm a buffer twice — the DMA
	 * writes through anything that follows. Refusing leaks one buffer;
	 * corrupting loses the machine. */
	if (!bcmgenet_rx_buf_ok(unit, buffer) ||
		unit->rxFreeCount >= unit->rxPoolTotal)
	{
		Kprintf("[genet] %s: RX-POOL-GUARD: push rejected buf=%lx free=%lu total=%lu\n",
				__func__, buffer, (ULONG)unit->rxFreeCount, (ULONG)unit->rxPoolTotal);
		return;
	}
#endif
	unit->rxFree[unit->rxFreeCount++] = buffer;
}

/**
 * bcmgenet_rx_pool_create() - Allocate the RX pool: a DMA slab of @buffers
 * RX_BUF_LENGTH buffers and their free stack, every buffer on it.
 * @unit: the unit.
 * @buffers: the pool's size in buffers; it must cover the ring.
 *
 * Unit task. The slab is cleaned once here, so every buffer enters the pool
 * as the ring wants it: no dirty line can be evicted over a frame the DMA
 * writes later. ndRxHeld starts at 0.
 *
 * Return: GENET_OK, or ENOMEM with nothing left allocated.
 */
u32 bcmgenet_rx_pool_create(struct GenetUnit *unit, u32 buffers)
{
	struct ExecBase *SysBase = unit->sysBase;
	unit->rxSlab = dma_zalloc(unit->dmaPool, DMA_ALIGN_MIN, buffers * RX_BUF_LENGTH);
	unit->rxFree = pool_alloc(unit->metaPool, buffers * sizeof(APTR));
	if (unit->rxSlab == NULL || unit->rxFree == NULL)
	{
		bcmgenet_rx_pool_destroy(unit);
		return ENOMEM;
	}
	unit->rxPoolTotal = buffers;

	cache_pre_dma(unit->rxSlab, buffers * RX_BUF_LENGTH, 0);

	unit->rxFreeCount = 0;
	for (u32 i = 0; i < buffers; i++)
		bcmgenet_rx_pool_push(unit, (u8 *)unit->rxSlab + i * RX_BUF_LENGTH);

	unit->ndRxHeld = 0;
	return GENET_OK;
}

/**
 * bcmgenet_rx_pool_destroy() - Free the RX pool's slab and free stack, and
 * zero its counts and ndRxHeld.
 * @unit: the unit.
 *
 * Unit task. Safe on a pool never or only partly created.
 */
void bcmgenet_rx_pool_destroy(struct GenetUnit *unit)
{
	struct ExecBase *SysBase = unit->sysBase;
	if (unit->rxSlab != NULL)
	{
		dma_free(unit->dmaPool, unit->rxSlab);
		unit->rxSlab = NULL;
	}
	if (unit->rxFree != NULL)
	{
		pool_free(unit->metaPool, unit->rxFree);
		unit->rxFree = NULL;
	}
	unit->rxFreeCount = 0;
	unit->rxPoolTotal = 0;
	unit->ndRxHeld = 0;
}
