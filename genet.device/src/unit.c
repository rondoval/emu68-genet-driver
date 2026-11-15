// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function: see GenetUnit.sysBase */
#include <proto/exec.h>
#endif

#include <exec/execbase.h>
#include <exec/types.h>
#include <stdarg.h>

#include <debug.h>
#include <device.h>
#include <sana2_unit.h>
#include <bcm_gpio.h>
#include <memory.h>
#include <minlist.h>
#include <runtime_config.h>

/**
 * SetupMDIO() - Route the MDIO bus's pins to GENET.
 * @unit: the unit.
 */
static void SetupMDIO(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Setting up MDIO bus\n", __func__);
	gpioSetAlternate(unit->gpioBase, PIN_RGMII_MDIO, GPIO_AF_5);
	gpioSetAlternate(unit->gpioBase, PIN_RGMII_MDC, GPIO_AF_5);
	gpioSetPull(unit->gpioBase, PIN_RGMII_MDIO, GPIO_PULL_UP);
	gpioSetPull(unit->gpioBase, PIN_RGMII_MDC, GPIO_PULL_DOWN);
}

/**
 * SetupRGMII() - Route the RGMII pins to GENET, with their pulls.
 * @unit: the unit.
 */
static void SetupRGMII(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Setting up RGMII bus\n", __func__);
	for (u8 i = 46; i < 58; i++)
	{
		gpioSetAlternate(unit->gpioBase, i, GPIO_AF_INPUT);
	}
	gpioSetPull(unit->gpioBase, 46, GPIO_PULL_UP);
	gpioSetPull(unit->gpioBase, 47, GPIO_PULL_UP);
	for (u8 i = 48; i < 58; i++)
	{
		gpioSetPull(unit->gpioBase, i, GPIO_PULL_DOWN);
	}
}

/**
 * UnitOpen() - Open the unit: count one more open, or set the first one up.
 * @unit: the unit.
 * @unitNumber: the unit's number, the device tree's ethernetN.
 * @flags: the OpenDevice() flags; unused.
 *
 * Return: GENET_OK, or the error of the step that failed (ENOMEM, DevTreeParse()'s, UnitTaskStart()'s), with everything undone.
 */
u32 UnitOpen(struct GenetUnit *unit, u32 unitNumber, u32 flags)
{
	struct ExecBase *SysBase = unit->sysBase;
	KprintfT("[genet] %s: Opening unit %lu with flags %lx\n", __func__, unitNumber, flags);
	(void)flags;
	if (unit->unit.unit_OpenCnt > 0)
	{
		unit->unit.unit_OpenCnt++;
		KprintfT("[genet] %s: Unit opened successfully, current open count: %ld\n", __func__, unit->unit.unit_OpenCnt);
		return GENET_OK;
	}

	unit->state = STATE_UNCONFIGURED;
	unit->unitNumber = unitNumber;
	unit->ndRxPoolBufs = unit->device->runtimeConfig.rx_pool_bufs;

	/* Coalescing starts at the compiled-in seeds, which are in range by
	 * construction; NETDEV_CMD_SET_COALESCE replaces them, validated there,
	 * for the lifetime of the open. */
	unit->coalTxFrames = GENET_COAL_TX_FRAMES;
	unit->coalRxFrames = GENET_COAL_RX_FRAMES;
	unit->coalRxUsecs = GENET_COAL_RX_USECS;
	unit->coalPinned = FALSE;
	unit->rxProfile = NDRP_UNSTATED;

	/* DMA buffers (rings, rx buffer, tx staging) must live in Emu68 (Pi-DRAM) RAM the
	 * GENET DMA engine can reach, so the DMA pool is region-restricted; with no device
	 * tree there is no reachable region and we refuse to open.  CPU-only metadata uses a
	 * separate ordinary Exec pool. */
	dma_mem_init(&unit->dma_ctx, SysBase);
	unit->dmaPool = dma_pool_create(&unit->dma_ctx);
	unit->metaPool = CreatePool(MEMF_FAST | MEMF_PUBLIC, 16384, 8192);

	u32 result;
	if (unit->dmaPool == NULL || unit->metaPool == NULL)
	{
		Kprintf("[genet] %s: Failed to create memory pools\n", __func__);
		result = ENOMEM;
		goto fail;
	}
	/* From the unit's pool: nothing else uses it until the unit task starts.
	 * Freed at the last close (UnitClose), or below on failure. */
	unit->s2 = pool_zalloc(unit->metaPool, sizeof(struct GenetS2));
	if (unit->s2 == NULL)
	{
		Kprintf("[genet] %s: no memory for the SANA-II state\n", __func__);
		result = ENOMEM;
		goto fail;
	}
	_NewMinList(&unit->s2->openers);
	_NewMinList(&unit->s2->issued);
	_NewMinList(&unit->s2->txBacklog);
	_NewMinList(&unit->s2->onlineWaiters);
	_NewMinList(&unit->s2->multicastRanges);

	result = DevTreeParse(unit);
	if (result != GENET_OK)
	{
		Kprintf("[genet] %s: Failed to parse device tree: %ld\n", __func__, result);
		goto fail;
	}

	/* On first open, we initialize current MAC to 0 to indicate it was not set yet */
	memset(unit->currentMacAddress, 0, sizeof(unit->currentMacAddress));
	result = UnitTaskStart(unit);
	if (result != GENET_OK)
	{
		Kprintf("[genet] %s: Failed to start unit task: %ld\n", __func__, result);
		goto fail;
	}

	unit->unit.unit_OpenCnt = 1;
	return GENET_OK;

fail:
	if (unit->s2 != NULL)
	{
		pool_free(unit->metaPool, unit->s2);
		unit->s2 = NULL;
	}
	if (unit->dmaPool)
	{
		dma_pool_delete(unit->dmaPool);
		unit->dmaPool = NULL;
	}
	if (unit->metaPool)
	{
		DeletePool(unit->metaPool);
		unit->metaPool = NULL;
	}
	return result;
}

/**
 * UnitConfigure() - Bring the hardware up: the pins, then the UMAC and the PHY.
 * @unit: the unit.
 *
 * Return: GENET_OK, or bcmgenet_eth_probe()'s error.
 */
u32 UnitConfigure(struct GenetUnit *unit)
{
	SetupMDIO(unit);
	SetupRGMII(unit);

	KprintfT("[genet] %s: About to probe UMAC\n", __func__);
	u32 result = bcmgenet_eth_probe(unit);
	if (result != GENET_OK)
	{
		Kprintf("[genet] %s: Failed to probe UMAC: %ld\n", __func__, result);
		return result;
	}

	unit->state = STATE_CONFIGURED;
	return GENET_OK;
}

/**
 * UnitOnline() - Start the UMAC and the DMA.
 * @unit: the unit.
 *
 * Return: GENET_OK, or bcmgenet_gmac_eth_start()'s error, the start undone.
 */
u32 UnitOnline(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: About to start UMAC\n", __func__);
	u32 result = bcmgenet_gmac_eth_start(unit);
	if (result != GENET_OK)
	{
		Kprintf("[genet] %s: Failed to start UMAC: %ld\n", __func__, result);
		/* Unwinds whichever stages the start reached; the unit stays
		 * configured, so a retry needs no reconfiguration. */
		bcmgenet_gmac_eth_stop(unit);
		return result;
	}

	unit->state = STATE_ONLINE;
	return GENET_OK;
}

/**
 * UnitOffline() - Stop the UMAC and the DMA; the unit stays configured.
 * @unit: the unit.
 */
void UnitOffline(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Stopping UMAC\n", __func__);
	bcmgenet_gmac_eth_stop(unit);
	unit->state = STATE_CONFIGURED;
}

/**
 * UnitClose() - Close the unit: count one open less, and take everything down at the last.
 * @unit: the unit.
 *
 * Return: the opens left.
 */
u32 UnitClose(struct GenetUnit *unit)
{
	struct ExecBase *SysBase = unit->sysBase;
	KprintfT("[genet] %s: Closing unit %lu\n", __func__, unit->unitNumber);

	unit->unit.unit_OpenCnt--;
	if (unit->unit.unit_OpenCnt == 0)
	{
		Kprintf("[genet] %s: Last user closed, cleaning up unit\n", __func__);

		/* Closing while still attached breaks the ABI, which requires DETACH
		 * first and refuses it while the stack holds RX buffers. Everything
		 * below frees the memory those buffers live in, so a release after
		 * this point writes through freed memory - nothing the driver can
		 * defend against. Drop the attachment so at least no callback goes
		 * out to a stack that is already gone, and say so. */
		BOOL forced = unit->ndStackOps != NULL;
		if (forced)
		{
			Kprintf("[genet] %s: closed while attached (%lu RX buffers still out) - forcing detach\n",
					__func__, (ULONG)unit->ndRxHeld);
			netdev_force_detach(unit);
		}

		/* The task goes first: stopping it drains the unit port, where the
		 * closing opener's removal waits - a SANA-II holder's release, which
		 * takes the hardware down on the task that owns it. Whatever is still
		 * running after that stops here, with the rings to itself; the PHY
		 * goes last, since the task's link handling dereferences it. */
		UnitTaskStop(unit);
		if (forced)
		{
			netdev_force_stop(unit);
		}
		if (unit->state == STATE_ONLINE)
		{
			UnitOffline(unit);
		}
		if (unit->state == STATE_CONFIGURED)
		{
			bcmgenet_eth_unconfigure(unit);
		}
		sana2_multicast_clear(unit);
		pool_free(unit->metaPool, unit->s2);
		unit->s2 = NULL;
		dma_pool_delete(unit->dmaPool);
		unit->dmaPool = NULL;
		DeletePool(unit->metaPool);
		unit->metaPool = NULL;
		unit->state = STATE_UNCONFIGURED;
	}

	return (u32)unit->unit.unit_OpenCnt;
}
