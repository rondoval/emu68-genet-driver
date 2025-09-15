// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/devicetree_protos.h>
#include <clib/utility_protos.h>
#else
#include <proto/exec.h>
#include <proto/devicetree.h>
#include <proto/utility.h>
#endif

#include <exec/types.h>

#include <devtree.h>

#include <debug.h>
#include <device.h>
#include <compat.h>

APTR DeviceTreeBase;

int DevTreeParse(struct GenetUnit *unit)
{
	DT_Init();

	char alias[12] = "ethernet0";
	alias[8] = '0' + unit->unitNumber;
	CONST_STRPTR ethernet_alias = DT_GetAlias((CONST_STRPTR) alias);
	CONST_STRPTR gpio_alias = DT_GetAlias((CONST_STRPTR) "gpio");
	if (ethernet_alias == NULL || gpio_alias == NULL)
	{
		Kprintf("[genet] %s: Failed to get aliases from device tree\n", __func__);
		return S2ERR_NO_RESOURCES;
	}

	APTR key = DT_OpenKey(ethernet_alias);
	if (key == NULL)
	{
		Kprintf("[genet] %s: Failed to open key %s\n", __func__, ethernet_alias);
		return S2ERR_NO_RESOURCES;
	}

	unit->compatible = DT_GetPropValue(DT_FindProperty(key, (CONST_STRPTR) "compatible"));
	unit->localMacAddress = DT_GetPropValue(DT_FindProperty(key, (CONST_STRPTR) "local-mac-address"));
	// CONST_STRPTR status = DT_GetPropValue(DT_FindProperty(key, (CONST_STRPTR) "status"));
	const ULONG phy_handle = DT_GetPropertyValueULONG(key, "phy-handle", 0, FALSE);
	CONST_STRPTR phyMode = DT_GetPropValue(DT_FindProperty(key, (CONST_STRPTR) "phy-mode"));
	unit->phy_interface = phyMode ? interface_for_phy_string((char *)phyMode) : PHY_INTERFACE_MODE_NA;

	unit->genetBase = DT_GetBaseAddressVirtual(ethernet_alias);
	if (unit->genetBase == NULL)
	{
		Kprintf("[genet] %s: Failed to get base address for GENET\n", __func__);
		DT_CloseKey(key);
		return S2ERR_NO_RESOURCES;
	}

	Kprintf("[genet] %s: compatible: %s\n", __func__, unit->compatible);
	Kprintf("[genet] %s: local-mac-address: %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n", __func__, unit->localMacAddress[0], unit->localMacAddress[1], unit->localMacAddress[2], unit->localMacAddress[3], unit->localMacAddress[4], unit->localMacAddress[5]);
	Kprintf("[genet] %s: phy-handle: %08lx\n", __func__, phy_handle);
	Kprintf("[genet] %s: phy-mode: %s\n", __func__, phy_string_for_interface(unit->phy_interface));
	Kprintf("[genet] %s: register base: %08lx\n", __func__, unit->genetBase);

	// Now find phy address
	APTR phy_key = DT_FindByPHandle(key, phy_handle);
	if (phy_key)
	{
		Kprintf("[genet] %s: Found phy key: %s\n", __func__, DT_GetKeyName(phy_key));
		unit->phyaddr = DT_GetPropertyValueULONG(phy_key, "reg", 1, FALSE);
		Kprintf("[genet] %s: phy-addr: %lx\n", __func__, unit->phyaddr);
	}
	else
	{
		Kprintf("[genet] %s: Failed to find phy key for handle %08lx\n", __func__, phy_handle);
		DT_CloseKey(key);
		return S2ERR_NO_RESOURCES;
	}

	// We also need GPIO to setup MDIO bus
	unit->gpioBase = DT_GetBaseAddressVirtual(gpio_alias);
	if (unit->gpioBase == NULL)
	{
		Kprintf("[genet] %s: Failed to get base address for GPIO\n", __func__);
		DT_CloseKey(key);
		return S2ERR_NO_RESOURCES;
	}

	// We're done with the device tree
	DT_CloseKey(key);
	return 0;
}