#define __NOLIBBASE__
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/execbase.h>
#include <exec/types.h>
#include <stdarg.h>

#include <debug.h>
#include <device.h>
#include <gpio/bcm_gpio.h>
#include <compat.h>

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
	struct ExecBase *SysBase = *((struct ExecBase **)4UL);
	Kprintf("[genet] %s: Opening unit %ld with flags %lx\n", __func__, unitNumber, flags);
	if (unit->unit.unit_OpenCnt > 0)
	{
		Kprintf("[genet] %s: Unit is already open, adding opener %lx\n", __func__, (ULONG)opener);
		unit->unit.unit_OpenCnt++;
		ObtainSemaphore(&unit->semaphore);
		AddTail((APTR)&unit->openers, (APTR)opener);
		ReleaseSemaphore(&unit->semaphore);
		return 0;
	}

	unit->execBase = SysBase;
	unit->utilityBase = OpenLibrary((CONST_STRPTR) "utility.library", LIB_MIN_VERSION);
	if (unit->utilityBase == NULL)
	{
		Kprintf("[genet] %s: Failed to open utility.library\n", __func__);
		return -ENODEV;
	}

	unit->state = STATE_UNCONFIGURED;
	unit->unit.unit_OpenCnt = 1;
	unit->unitNumber = unitNumber;

	NewMinList(&unit->openers);
	AddTail((APTR)&unit->openers, (APTR)opener);
	InitSemaphore(&unit->semaphore);

	int result = DevTreeParse(unit);
	if (result < 0)
	{
		Kprintf("[genet] %s: Failed to parse device tree: %d\n", __func__, result);
		return result;
	}

	CopyMem(unit->localMacAddress, unit->currentMacAddress, 6);
	UnitTaskStart(unit);
	return 0;
}

int UnitConfigure(struct GenetUnit *unit)
{
	SetupMDIO(unit);
	SetupRGMII(unit);
	// TODO process PROM flag

	Kprintf("[genet] %s: About to initalize UMAC\n", __func__);
	int result = bcmgenet_eth_probe(unit);
	if (result < 0)
	{
		Kprintf("[genet] %s: Failed to probe UMAC: %ld\n", __func__, result);
		bcmgenet_gmac_eth_stop(unit); // This may be needed to free PHY memory
		return result;
	}

	unit->state = STATE_CONFIGURED;
	return 0;
}

int UnitOnline(struct GenetUnit *unit)
{
	Kprintf("[genet] %s: About to start UMAC\n", __func__);
	int result = bcmgenet_gmac_eth_start(unit);
	if (result < 0)
	{
		Kprintf("[genet] %s: Failed to start UMAC: %ld\n", __func__, result);
		bcmgenet_gmac_eth_stop(unit); // This may be needed to free PHY memory
		return result;
	}

	unit->state = STATE_ONLINE;
	return 0;
}

void UnitOffline(struct GenetUnit *unit)
{
	Kprintf("[genet] %s: Stopping UMAC\n", __func__);
	bcmgenet_gmac_eth_stop(unit); // This may be needed to free PHY memory
	unit->state = STATE_OFFLINE;
}

int UnitClose(struct GenetUnit *unit, struct Opener *opener)
{
	struct ExecBase *SysBase = unit->execBase;
	Kprintf("[genet] %s: Closing unit %ld with opener %lx\n", __func__, unit->unitNumber, (ULONG)opener);
	if (opener)
	{
		Kprintf("[genet] %s: Removing opener %lx\n", __func__, (ULONG)opener);
		// We don't free opener memory here, this will be done by device
		ObtainSemaphore(&unit->semaphore);
		Remove((struct Node *)opener);
		ReleaseSemaphore(&unit->semaphore);
	}

	unit->unit.unit_OpenCnt--;
	if (unit->unit.unit_OpenCnt == 0)
	{
		Kprintf("[genet] %s: Last opener closed, cleaning up unit\n", __func__);
		if (unit->state == STATE_ONLINE)
		{
			UnitOffline(unit);
		}
		UnitTaskStop(unit);
		CloseLibrary(unit->utilityBase);
		unit->state = STATE_UNCONFIGURED;
	}
	return unit->unit.unit_OpenCnt;
}
