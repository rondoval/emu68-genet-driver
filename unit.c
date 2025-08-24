// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#include <clib/utility_protos.h>
#else
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#endif

#include <exec/execbase.h>
#include <exec/types.h>
#include <stdarg.h>

#include <debug.h>
#include <device.h>
#include <gpio/bcm_gpio.h>
#include <compat.h>
#include <minlist.h>
#include <runtime_config.h>

static void SetupMDIO(struct GenetUnit *unit)
{
	Kprintf("[genet] %s: Setting up MDIO bus\n", __func__);
	gpioSetAlternate(unit->gpioBase, PIN_RGMII_MDIO, GPIO_AF_5);
	gpioSetAlternate(unit->gpioBase, PIN_RGMII_MDC, GPIO_AF_5);
	gpioSetPull(unit->gpioBase, PIN_RGMII_MDIO, GPIO_PULL_UP);
	gpioSetPull(unit->gpioBase, PIN_RGMII_MDC, GPIO_PULL_DOWN);
}

static void SetupRGMII(struct GenetUnit *unit)
{
	Kprintf("[genet] %s: Setting up RGMII bus\n", __func__);
	for (int i = 46; i < 58; i++)
	{
		gpioSetAlternate(unit->gpioBase, i, GPIO_AF_INPUT);
	}
	gpioSetPull(unit->gpioBase, 46, GPIO_PULL_UP);
	gpioSetPull(unit->gpioBase, 47, GPIO_PULL_UP);
	for (int i = 48; i < 58; i++)
	{
		gpioSetPull(unit->gpioBase, i, GPIO_PULL_DOWN);
	}
}

int UnitOpen(struct GenetUnit *unit, LONG unitNumber, LONG flags, struct Opener *opener)
{
	Kprintf("[genet] %s: Opening unit %ld with flags %lx\n", __func__, unitNumber, flags);
	if (unit->unit.unit_OpenCnt > 0)
	{
		Kprintf("[genet] %s: Unit was already open\n", __func__);

		ObtainSemaphore(&unit->unitSemaphore);
		unit->unit.unit_OpenCnt++;
		if (opener != NULL)
		{
			Kprintf("[genet] %s: Adding opener %lx to openers list\n", __func__, (ULONG)opener);
			AddTailMinList(&unit->openers, (struct MinNode *)opener);
		}
		ReleaseSemaphore(&unit->unitSemaphore);
		Kprintf("[genet] %s: Unit opened successfully, current open count: %ld\n", __func__, unit->unit.unit_OpenCnt);
		return S2ERR_NO_ERROR;
	}

	unit->state = STATE_UNCONFIGURED;
	unit->flags = flags;
	unit->unit.unit_OpenCnt = 1;
	unit->unitNumber = unitNumber;

	InitSemaphore(&unit->unitSemaphore);

	unit->memoryPool = CreatePool(MEMF_FAST | MEMF_PUBLIC, 16384, 8192);
	if (unit->memoryPool == NULL)
	{
		Kprintf("[genet] %s: Failed to create memory pool\n", __func__);
		return S2ERR_NO_RESOURCES;
	}
	_NewMinList(&unit->multicastRanges);
	unit->multicastCount = 0;

	_NewMinList(&unit->openers);
	if (opener != NULL)
	{
		AddTailMinList(&unit->openers, (struct MinNode *)opener);
	}

	int result = DevTreeParse(unit);
	if (result != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: Failed to parse device tree: %ld\n", __func__, result);
		return result;
	}

	/* On first open, we initialize current MAC to 0 to indicate it was not set yet */
	_memset(unit->currentMacAddress, 0, sizeof(unit->currentMacAddress));
	result = UnitTaskStart(unit);
	if (result != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: Failed to start unit task: %ld\n", __func__, result);
		DeletePool(unit->memoryPool);
		unit->memoryPool = NULL;
		return result;
	}
	return S2ERR_NO_ERROR;
}

int UnitConfigure(struct GenetUnit *unit)
{
	SetupMDIO(unit);
	SetupRGMII(unit);

	Kprintf("[genet] %s: About to probe UMAC\n", __func__);
	int result = bcmgenet_eth_probe(unit);
	if (result != S2ERR_NO_ERROR)
	{
		Kprintf("[genet] %s: Failed to probe UMAC: %ld\n", __func__, result);
		bcmgenet_gmac_eth_stop(unit); // This may be needed to free PHY memory
		return result;
	}

	unit->state = STATE_CONFIGURED;
	return S2ERR_NO_ERROR;
}

int UnitOnline(struct GenetUnit *unit)
{
	Kprintf("[genet] %s: About to start UMAC\n", __func__);
	int result = bcmgenet_gmac_eth_start(unit);
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
	Kprintf("[genet] %s: Stopping UMAC\n", __func__);
	unit->state = STATE_OFFLINE;
	bcmgenet_gmac_eth_stop(unit); // This may be needed to free PHY memory
}

int UnitClose(struct GenetUnit *unit, struct Opener *opener)
{
	Kprintf("[genet] %s: Closing unit %ld with opener %lx\n", __func__, unit->unitNumber, (ULONG)opener);

	unit->unit.unit_OpenCnt--;
	if (unit->unit.unit_OpenCnt == 0)
	{
		Kprintf("[genet] %s: Last opener closed, cleaning up unit\n", __func__);
		if (unit->state == STATE_ONLINE)
		{
			UnitOffline(unit);
		}
		UnitTaskStop(unit);
		DeletePool(unit->memoryPool);
		unit->memoryPool = NULL;
		unit->state = STATE_UNCONFIGURED;
	}
	else if (opener != NULL)
	{
		Kprintf("[genet] %s: Removing opener %lx\n", __func__, (ULONG)opener);
		// We don't free opener memory here, this will be done by device
		ObtainSemaphore(&unit->unitSemaphore);
		RemoveMinNode((struct MinNode *)opener);
		ReleaseSemaphore(&unit->unitSemaphore);
	}

	return unit->unit.unit_OpenCnt;
}
