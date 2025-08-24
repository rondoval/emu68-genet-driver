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

#include <debug.h>
#include <device.h>
#include <compat.h>

APTR DeviceTreeBase;

// Devicetree extras
static APTR DT_FindByPHandle(APTR key, ULONG phandle)
{
	APTR p = DT_FindProperty(key, (CONST_STRPTR) "phandle");

	if (p && *(ULONG *)DT_GetPropValue(p) == phandle)
	{
		return key;
	}
	else
	{
		for (APTR c = DT_GetChild(key, NULL); c; c = DT_GetChild(key, c))
		{
			APTR found = DT_FindByPHandle(c, phandle);
			if (found)
				return found;
		}
	}
	return NULL;
}

static ULONG DT_GetPropertyValueULONG(APTR key, const char *propname, ULONG def_val, BOOL check_parent)
{
	ULONG ret = def_val;

	while (key != NULL)
	{
		APTR p = DT_FindProperty(key, (CONST_STRPTR)propname);

		if (p != NULL || check_parent == FALSE)
		{
			if (p != NULL || DT_GetPropLen(p) >= 4)
			{
				ret = *(ULONG *)DT_GetPropValue(p);
			}

			return ret;
		}
		key = DT_GetParent(key);
	}
	return ret;
}

static ULONG GetAddressTranslationOffset(APTR address)
{
	APTR key = DT_OpenKey((CONST_STRPTR) "/soc"); // TODO scb has bad values in ranges, so we always take from soc...
	if (key)
	{
		const ULONG address_cells_parent = DT_GetPropertyValueULONG(DT_GetParent(key), "#address-cells", 2, FALSE);
		const ULONG address_cells_child = DT_GetPropertyValueULONG(key, "#address-cells", 2, FALSE);
		const ULONG size_cells = DT_GetPropertyValueULONG(DT_GetParent(key), "#size-cells", 2, FALSE);
		const ULONG record_size = address_cells_parent + address_cells_child + size_cells;

		const ULONG *reg = DT_GetPropValue(DT_FindProperty(key, (CONST_STRPTR) "ranges"));
		const ULONG len = DT_GetPropLen(DT_FindProperty(key, (CONST_STRPTR) "ranges"));
		ULONG phys_vc4, phys_cpu;

		for (const ULONG *i = reg; i < reg + len / 4; i += record_size)
		{
			phys_vc4 = i[address_cells_child - 1];
			phys_cpu = i[address_cells_child + address_cells_parent - 1];
			ULONG size = i[record_size - 1];
			Kprintf("[genet] %s: phys_vc4=0x%08lx phys_cpu=0x%08lx size=0x%08lx\n", __func__, phys_vc4, phys_cpu, size);

			if ((ULONG)address >= phys_vc4 && (ULONG)address < phys_vc4 + size)
			{
				ULONG offset = phys_cpu - phys_vc4;
				Kprintf("[genet] %s: Found translation, offset=0x%08lx\n", __func__, offset);
				DT_CloseKey(key);
				return offset;
			}
		}
		Kprintf("[genet] %s: No translation found for address 0x%08lx\n", __func__, address);
		DT_CloseKey(key);
		return 0;
	}
	Kprintf("[genet] %s: Could not open key\n", __func__);
	return 0;
}

static APTR GetBaseAddress(CONST_STRPTR alias)
{
	APTR key = DT_OpenKey(alias);
	if (key == NULL)
	{
		Kprintf("[genet] %s: Failed to open key %s\n", __func__, alias);
		return NULL;
	}

	ULONG address_cells = DT_GetPropertyValueULONG(DT_GetParent(key), "#address-cells", 2, FALSE);

	const ULONG *reg = DT_GetPropValue(DT_FindProperty(key, (CONST_STRPTR) "reg"));
	if (reg != NULL)
	{
		DT_CloseKey(key);
		return (APTR)reg[address_cells - 1];
	}
	Kprintf("[genet] %s: Failed to find reg property in key %s\n", __func__, alias);
	DT_CloseKey(key);
	return NULL;
}

static CONST_STRPTR GetAlias(const char *alias)
{
	APTR key = DT_OpenKey((CONST_STRPTR) "/aliases");
	if (key == NULL)
	{
		Kprintf("[genet] %s: Failed to open key /aliases\n", __func__);
		return NULL;
	}

	APTR prop = DT_FindProperty(key, (CONST_STRPTR)alias);
	if (prop != NULL)
	{
		CONST_STRPTR value = DT_GetPropValue(prop);
		DT_CloseKey(key);
		return value;
	}
	Kprintf("[genet] %s: Failed to find alias %s\n", __func__, alias);
	DT_CloseKey(key);
	return NULL;
}

int DevTreeParse(struct GenetUnit *unit)
{
	DeviceTreeBase = OpenResource((CONST_STRPTR) "devicetree.resource");
	if (!DeviceTreeBase)
	{
		Kprintf("[genet] %s: Failed to open devicetree.resource\n", __func__);
		return S2ERR_NO_RESOURCES;
	}

	char alias[12] = "ethernet0";
	alias[8] = '0' + unit->unitNumber;
	CONST_STRPTR ethernet_alias = GetAlias(alias);
	CONST_STRPTR gpio_alias = GetAlias("gpio");
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

	unit->genetBase = GetBaseAddress(ethernet_alias);
	if (unit->genetBase == NULL)
	{
		Kprintf("[genet] %s: Failed to get base address for GENET\n", __func__);
		DT_CloseKey(key);
		return S2ERR_NO_RESOURCES;
	}

	Kprintf("[genet] %s: Device tree info\n", __func__);
	Kprintf("[genet] %s: compatible: %s\n", __func__, unit->compatible);
	Kprintf("[genet] %s: local-mac-address: %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n", __func__, unit->localMacAddress[0], unit->localMacAddress[1], unit->localMacAddress[2], unit->localMacAddress[3], unit->localMacAddress[4], unit->localMacAddress[5]);
	// Kprintf("[genet] %s: status: %s\n", __func__, status);
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
	unit->gpioBase = GetBaseAddress(gpio_alias);
	if (unit->gpioBase == NULL)
	{
		Kprintf("[genet] %s: Failed to get base address for GPIO\n", __func__);
		DT_CloseKey(key);
		return S2ERR_NO_RESOURCES;
	}

	ULONG genetOffset = GetAddressTranslationOffset(unit->genetBase);
	ULONG gpioOffset = GetAddressTranslationOffset(unit->gpioBase);
	unit->genetBase = (APTR)((ULONG)unit->genetBase + genetOffset);
	unit->gpioBase = (APTR)((ULONG)unit->gpioBase + gpioOffset);
	Kprintf("[genet] %s: Found GENET in CPU space, base address in CPU space: %08lx\n", __func__, unit->genetBase);
	Kprintf("[genet] %s: Found GPIO in CPU space, base address in CPU space: %08lx\n", __func__, unit->gpioBase);

	// We're done with the device tree
	DT_CloseKey(key);
	return 0;
}