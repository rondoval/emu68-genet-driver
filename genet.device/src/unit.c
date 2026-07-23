// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <exec/execbase.h>
#include <exec/types.h>
#include <stdarg.h>

#include <debug.h>
#include <device.h>
#include <bcm_gpio.h>
#include <memory.h>
#include <minlist.h>
#include <runtime_config.h>

static void SetupMDIO(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Setting up MDIO bus\n", __func__);
	gpioSetAlternate(unit->gpioBase, PIN_RGMII_MDIO, GPIO_AF_5);
	gpioSetAlternate(unit->gpioBase, PIN_RGMII_MDC, GPIO_AF_5);
	gpioSetPull(unit->gpioBase, PIN_RGMII_MDIO, GPIO_PULL_UP);
	gpioSetPull(unit->gpioBase, PIN_RGMII_MDC, GPIO_PULL_DOWN);
}

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

u32 UnitOpen(struct GenetUnit *unit, u32 unitNumber, u32 flags, struct Opener *opener)
{
	KprintfT("[genet] %s: Opening unit %lu with flags %lx\n", __func__, unitNumber, flags);
	if (unit->unit.unit_OpenCnt > 0)
	{
		KprintfT("[genet] %s: Unit already running; using control helper to add opener\n", __func__);
		unit->unit.unit_OpenCnt++;
		if (opener)
		{
			UnitSubmitControl(unit, UNIT_CTRL_OPENER_ADD, (union UnitControlPayload){ .opener = opener });
		}
		KprintfT("[genet] %s: Unit opened successfully, current open count: %ld\n", __func__, unit->unit.unit_OpenCnt);
		return S2ERR_NO_ERROR;
	}

	unit->state = STATE_UNCONFIGURED;
	unit->flags = flags;
	unit->unit.unit_OpenCnt = 1;
	unit->unitNumber = unitNumber;
	unit->use_miami_workaround = unit->device->runtimeConfig.use_miami_workaround;
	unit->budget = unit->device->runtimeConfig.budget;

	/* DMA buffers (rings, rx buffer, tx staging) must live in Emu68 (Pi-DRAM) RAM the
	 * GENET DMA engine can reach, so the DMA pool is region-restricted; with no device
	 * tree there is no reachable region and we refuse to open.  CPU-only metadata uses a
	 * separate ordinary Exec pool. */
	dma_mem_init(&unit->dma_ctx);
	unit->dmaPool = dma_pool_create(&unit->dma_ctx);
	unit->metaPool = CreatePool(MEMF_FAST | MEMF_PUBLIC, 16384, 8192);

	u32 result;
	if (unit->dmaPool == NULL || unit->metaPool == NULL)
	{
		Kprintf("[genet] %s: Failed to create memory pools\n", __func__);
		result = S2ERR_NO_RESOURCES;
		goto fail;
	}
	_NewMinList(&unit->multicastRanges);
	unit->multicastCount = 0;

	_NewMinList(&unit->openers);

	result = DevTreeParse(unit);
	if (result != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: Failed to parse device tree: %ld\n", __func__, result);
		goto fail;
	}

	/* On first open, we initialize current MAC to 0 to indicate it was not set yet */
	memset(unit->currentMacAddress, 0, sizeof(unit->currentMacAddress));
	result = UnitTaskStart(unit);
	if (result != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: Failed to start unit task: %ld\n", __func__, result);
		goto fail;
	}

	if (opener != NULL)
	{
		UnitSubmitControl(unit, UNIT_CTRL_OPENER_ADD, (union UnitControlPayload){ .opener = opener });
	}

	return S2ERR_NO_ERROR;

fail:
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

u32 UnitConfigure(struct GenetUnit *unit)
{
	SetupMDIO(unit);
	SetupRGMII(unit);

	KprintfT("[genet] %s: About to probe UMAC\n", __func__);
	u32 result = bcmgenet_eth_probe(unit);
	if (result != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: Failed to probe UMAC: %ld\n", __func__, result);
		return result;
	}

	unit->state = STATE_CONFIGURED;
	return S2ERR_NO_ERROR;
}

u32 UnitOnline(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: About to start UMAC\n", __func__);
	u32 result = bcmgenet_gmac_eth_start(unit);
	if (result != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: Failed to start UMAC: %ld\n", __func__, result);
		bcmgenet_gmac_eth_stop(unit); // This may be needed to free PHY memory
		return result;
	}

	unit->state = STATE_ONLINE;
	return S2ERR_NO_ERROR;
}

void UnitOffline(struct GenetUnit *unit)
{
	KprintfT("[genet] %s: Stopping UMAC\n", __func__);
	unit->state = STATE_OFFLINE;
	bcmgenet_gmac_eth_stop(unit); // This may be needed to free PHY memory
}

u32 UnitClose(struct GenetUnit *unit, struct Opener *opener)
{
	KprintfT("[genet] %s: Closing unit %lu with opener %lx\n", __func__, unit->unitNumber, (ULONG)opener);

	unit->unit.unit_OpenCnt--;
	if (unit->unit.unit_OpenCnt == 0)
	{
		Kprintf("[genet] %s: Last opener closed, cleaning up unit\n", __func__);
		if (unit->state == STATE_ONLINE)
		{
			UnitOffline(unit);
		}
		UnitTaskStop(unit);
		dma_pool_delete(unit->dmaPool);
		unit->dmaPool = NULL;
		DeletePool(unit->metaPool);
		unit->metaPool = NULL;
		unit->state = STATE_UNCONFIGURED;
	}
	else if (opener != NULL)
	{
		UnitSubmitControl(unit, UNIT_CTRL_OPENER_REM, (union UnitControlPayload){ .opener = opener });
	}

	return (u32)unit->unit.unit_OpenCnt;
}
