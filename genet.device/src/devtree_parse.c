// SPDX-License-Identifier: GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/devicetree_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#include <proto/devicetree.h>
#endif

#include <exec/types.h>

#include <devtree.h>

#include <debug.h>
#include <device.h>
#include <types.h>

u32 DevTreeParse(struct GenetUnit *unit)
{
	APTR DeviceTreeBase = OpenResource((CONST_STRPTR) "devicetree.resource");
	if (DeviceTreeBase == NULL)
	{
		Kprintf("[genet] %s: Failed to open devicetree.resource\n", __func__);
		return ENOMEM;
	}

	char alias[12] = "ethernet0";
	alias[8] = (char)('0' + unit->unitNumber);
	CONST_STRPTR ethernet_alias = DT_GetAlias((CONST_STRPTR)alias);
	CONST_STRPTR gpio_alias = DT_GetAlias((CONST_STRPTR) "gpio");
	if (ethernet_alias == NULL || gpio_alias == NULL)
	{
		Kprintf("[genet] %s: Failed to get aliases from device tree\n", __func__);
		return ENOMEM;
	}

	APTR key = DT_OpenKey(ethernet_alias);
	if (key == NULL)
	{
		Kprintf("[genet] %s: Failed to open key %s\n", __func__, ethernet_alias);
		return ENOMEM;
	}

	unit->compatible = DT_GetPropValue(DT_FindProperty(key, (CONST_STRPTR) "compatible"));
	const u32 phy_handle = DT_GetPropertyValueULONG(key, "phy-handle", 0, FALSE);
	CONST_STRPTR phyMode = DT_GetPropValue(DT_FindProperty(key, (CONST_STRPTR) "phy-mode"));
	unit->phy_interface = phyMode ? interface_for_phy_string((char *)phyMode) : PHY_INTERFACE_MODE_NA;

	/* The station address is the one property with no fallback: it is copied
	 * into the unit at ATTACH and programmed into UMAC_MAC0/1 at every start,
	 * so a missing or short property has to fail the open rather than be read
	 * past. */
	APTR macProp = DT_FindProperty(key, (CONST_STRPTR) "local-mac-address");
	if (macProp == NULL || DT_GetPropLen(macProp) < sizeof(unit->currentMacAddress))
	{
		Kprintf("[genet] %s: no usable local-mac-address property\n", __func__);
		DT_CloseKey(key);
		return ENOMEM;
	}
	unit->localMacAddress = DT_GetPropValue(macProp);

	unit->genetBase = DT_GetBaseAddressVirtual(ethernet_alias);
	if (unit->genetBase == NULL)
	{
		Kprintf("[genet] %s: Failed to get base address for GENET\n", __func__);
		DT_CloseKey(key);
		return ENOMEM;
	}

	KprintfT("[genet] %s: compatible: %s\n", __func__, unit->compatible);
	Kprintf("[genet] %s: local-mac-address: %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n", __func__, unit->localMacAddress[0], unit->localMacAddress[1], unit->localMacAddress[2], unit->localMacAddress[3], unit->localMacAddress[4], unit->localMacAddress[5]);
	KprintfT("[genet] %s: phy-handle: %08lx\n", __func__, (ULONG)phy_handle);
	KprintfT("[genet] %s: phy-mode: %s\n", __func__, phy_string_for_interface(unit->phy_interface));
	KprintfT("[genet] %s: register base: %08lx\n", __func__, unit->genetBase);

	s32 irq0 = DT_GetInterrupt(key, 0);
	s32 irq1 = DT_GetInterrupt(key, 1);
	if (irq0 < 0 || irq1 < 0)
	{
		Kprintf("[genet] %s: Failed to get interrupt numbers\n", __func__);
		DT_CloseKey(key);
		return ENOMEM;
	}
	/* Only IRQ0 is used; IRQ1 is read to confirm the node names both, which a
	 * GENET node does and a mis-resolved alias would not. */
	unit->irq0_number = (u32)irq0;

	// Now find phy address
	APTR phy_key = DT_FindByPHandle(key, phy_handle);
	if (phy_key)
	{
		KprintfT("[genet] %s: Found phy key: %s\n", __func__, DT_GetKeyName(phy_key));
		u32 phyaddr = DT_GetPropertyValueULONG(phy_key, "reg", 1, FALSE);
		if (phyaddr > 0xffU)
		{
			Kprintf("[genet] %s: Invalid phy address %08lx\n", __func__, (ULONG)phyaddr);
			DT_CloseKey(key);
			return ENOMEM;
		}
		unit->phyaddr = (u8)phyaddr;
		KprintfT("[genet] %s: phy-addr: %lx\n", __func__, unit->phyaddr);
	}
	else
	{
		Kprintf("[genet] %s: Failed to find phy key for handle %08lx\n", __func__, phy_handle);
		DT_CloseKey(key);
		return ENOMEM;
	}

	// We also need GPIO to setup MDIO bus
	unit->gpioBase = DT_GetBaseAddressVirtual(gpio_alias);
	if (unit->gpioBase == NULL)
	{
		Kprintf("[genet] %s: Failed to get base address for GPIO\n", __func__);
		DT_CloseKey(key);
		return ENOMEM;
	}

	// We're done with the device tree
	DT_CloseKey(key);
	return GENET_OK;
}