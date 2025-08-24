// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <gpio/bcm_gpio.h>
#include <compat.h>

void gpioSetPull(tGpioRegs *pGpio, UBYTE ubIndex, tGpioPull ePull)
{
	static const UBYTE ubBitsPerGpio = 2;
	UBYTE ubRegIndex = ubIndex / 16;
	UBYTE ubRegShift = (ubIndex % 16) * ubBitsPerGpio;
	ULONG ulClearMask = ~(0b11 << ubRegShift);
	ULONG ulWriteMask = ePull << ubRegShift;

	writel((readl(&pGpio->GPIO_PUP_PDN_CNTRL_REG[ubRegIndex]) & ulClearMask) | ulWriteMask, &pGpio->GPIO_PUP_PDN_CNTRL_REG[ubRegIndex]);
}

void gpioSetAlternate(tGpioRegs *pGpio, UBYTE ubIndex, tGpioAlternativeFunction eAlternativeFunction)
{
	static const UBYTE ubBitsPerGpio = 3;
	UBYTE ubRegIndex = ubIndex / 10;
	UBYTE ubRegShift = (ubIndex % 10) * ubBitsPerGpio;
	ULONG ulClearMask = ~(0b111 << ubRegShift);
	ULONG ulWriteMask = eAlternativeFunction << ubRegShift;

	writel((readl(&pGpio->GPFSEL[ubRegIndex]) & ulClearMask) | ulWriteMask, &pGpio->GPFSEL[ubRegIndex]);
}

void gpioSetLevel(tGpioRegs *pGpio, UBYTE ubIndex, UBYTE ubState)
{
	UBYTE ubRegIndex = ubIndex / 32;
	UBYTE ubRegShift = ubIndex % 32;
	ULONG ulRegState = (1 << ubRegShift);
	if (ubState)
	{
		writel(ulRegState, &pGpio->GPSET[ubRegIndex]);
	}
	else
	{
		writel(ulRegState, &pGpio->GPCLR[ubRegIndex]);
	}
}
