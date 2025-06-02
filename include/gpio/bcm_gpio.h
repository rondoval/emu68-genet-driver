// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BCM_GPIO_H
#define BCM_GPIO_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <exec/types.h>

#define PIN_RGMII_MDIO 28
#define PIN_RGMII_MDC 29

	typedef enum tGpioAlternativeFunction
	{
		GPIO_AF_INPUT = 0b000,
		GPIO_AF_OUTPUT = 0b001,
		GPIO_AF_0 = 0b100,
		GPIO_AF_1 = 0b101,
		GPIO_AF_2 = 0b110,
		GPIO_AF_3 = 0b111,
		GPIO_AF_4 = 0b011,
		GPIO_AF_5 = 0b010,
	} tGpioAlternativeFunction;

	typedef enum tGpioPull
	{
		GPIO_PULL_OFF = 0b00,  // no pull
		GPIO_PULL_UP = 0b01,   // pull up
		GPIO_PULL_DOWN = 0b10, // pull down
	} tGpioPull;

	typedef struct tGpioRegs
	{
		ULONG GPFSEL[6]; // 0x00-0x14 GPIO Function Select
		ULONG RESERVED0;
		ULONG GPSET[2]; // 0x1C-0x20 GPIO Pin Output Set
		ULONG RESERVED1;
		ULONG GPCLR[2]; // 0x28-0x2C GPIO Pin Output Clear
		ULONG RESERVED2;
		ULONG GPLEV[2]; // 0x34-0x38 GPIO Pin Level
		ULONG RESERVED3;
		ULONG GPEDS[2]; // 0x40-0x44 GPIO Pin Event Detect Status
		ULONG RESERVED4;
		ULONG GPREN[2]; // 0x4C-0x50 GPIO Pin Rising Edge Detect Enable
		ULONG RESERVED5;
		ULONG GPFEN[2]; // 0x58-0x5C GPIO Pin Falling Edge Detect Enable
		ULONG RESERVED6;
		ULONG GPHEN[2]; // 0x64-0x68 GPIO Pin High Detect Enable
		ULONG RESERVED7;
		ULONG GPLEN[2]; // 0x70-0x74 GPIO Pin Low Detect Enable
		ULONG RESERVED8;
		ULONG GPAREN[2]; // 0x7C-0x80 GPIO Pin Async Rising Edge Detect Enable
		ULONG RESERVED9;
		ULONG GPAFEN[2]; // 0x88-0x8C GPIO Pin Async Falling Edge Detect Enable
		ULONG RESERVED10[15];
		ULONG GPIO_PUP_PDN_CNTRL_REG[4]; // 0xe4-0xf0 GPIO Pull Up/Down Control
	} tGpioRegs;

	void gpioSetPull(tGpioRegs *pGpio, UBYTE ubIndex, tGpioPull ePull);
	void gpioSetAlternate(tGpioRegs *pGpio, UBYTE ubIndex, tGpioAlternativeFunction eAlternativeFunction);
	void gpioSetLevel(tGpioRegs *pGpio, UBYTE ubIndex, UBYTE ubState);

#ifdef __cplusplus
}
#endif

#endif // BCM_GPIO_H
