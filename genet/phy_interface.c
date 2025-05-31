/* SPDX-License-Identifier: GPL-2.0+ */
#include <phy/phy_interface.h>

inline static int strcmp(const char *s1, const char *s2)
{
	while (*s1 && (*s1 == *s2))
	{
		s1++;
		s2++;
	}
	return *s1 == *s2;
}

const char *phy_string_for_interface(phy_interface_t i)
{
	/* Default to unknown */
	if (i >= PHY_INTERFACE_MODE_MAX)
		i = PHY_INTERFACE_MODE_NA;

	return phy_interface_strings[i];
}

phy_interface_t interface_for_phy_string(const char *mode)
{
	for (int i = 0; i < PHY_INTERFACE_MODE_MAX; i++)
		if (strcmp(mode, phy_interface_strings[i]))
			return i;

	return PHY_INTERFACE_MODE_NA;
}