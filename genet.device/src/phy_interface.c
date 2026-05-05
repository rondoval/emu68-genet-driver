/* SPDX-License-Identifier: GPL-2.0+ */
#include <genet/phy_interface.h>
#include <strutil.h>

const char *phy_string_for_interface(phy_interface_t i)
{
	/* Default to unknown */
	if (i >= PHY_INTERFACE_MODE_MAX)
		i = PHY_INTERFACE_MODE_NA;

	return phy_interface_strings[i];
}

phy_interface_t interface_for_phy_string(const char *mode)
{
	for (u8 i = 0; i < PHY_INTERFACE_MODE_MAX; i++)
		if (_Stricmp((CONST_STRPTR)mode, (CONST_STRPTR)phy_interface_strings[i]) == 0)
			return i;

	return PHY_INTERFACE_MODE_NA;
}