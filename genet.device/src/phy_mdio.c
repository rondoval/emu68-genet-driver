// SPDX-License-Identifier: GPL-2.0+
/*
 * UniMAC MDIO controller transport. Ported from Linux
 * drivers/net/mdio/mdio-bcm-unimac.c.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <errors.h>
#include <iomem.h>
#include <timing.h>
#include <types.h>
#include <device.h>

#include <genet/phy.h>
#include <genet/phy_priv.h>

/*
 * Wait for the MDIO controller to retire the transaction in flight.
 *
 * A C22 transaction takes ~25 us, so the poll opens with a fixed delay of that
 * order and then spins: the common case costs one register read rather than a
 * loop of them, each of which is a PiStorm bus round trip.
 *
 * The deadline is compared with time_deadline_passed() (timing.h), whose signed
 * difference is correct across the 32-bit microsecond counter's wrap.
 *
 * Return: 0 once MDIO_START_BUSY clears, -ETIMEDOUT otherwise.
 */
static inline s32 mdio_wait_idle(struct GenetUnit *unit)
{
	u32 deadline = get_time() + MDIO_TIMEOUT_US;

	delay_us(MDIO_C22_XFER_US);
	while (mmio_read32(unit->genetBase + MDIO_CMD) & MDIO_START_BUSY)
	{
		if (time_deadline_passed(get_time(), deadline))
			return -ETIMEDOUT;
	}

	return 0;
}

static inline void mdio_start(struct GenetUnit *unit)
{
	mmio_set32(unit->genetBase + MDIO_CMD, MDIO_START_BUSY);
}

s32 mdio_write(struct phy_device *phy, u8 reg, u16 value)
{
	struct GenetUnit *unit = phy->unit;

	/* Prepare the write operation */
	u32 val = MDIO_WR | (((u32)phy->addr & MDIO_PMD_MASK) << MDIO_PMD_SHIFT) |
			  (((u32)reg & MDIO_REG_MASK) << MDIO_REG_SHIFT) | ((u32)value & 0xffffU);
	mmio_write32(val, unit->genetBase + MDIO_CMD);

	/* Start MDIO transaction */
	mdio_start(unit);

	return mdio_wait_idle(unit);
}

s32 mdio_read(struct phy_device *phy, u8 reg)
{
	struct GenetUnit *unit = phy->unit;

	/* Prepare the read operation */
	u32 val = MDIO_RD | (((u32)phy->addr & MDIO_PMD_MASK) << MDIO_PMD_SHIFT) |
			  (((u32)reg & MDIO_REG_MASK) << MDIO_REG_SHIFT);
	mmio_write32(val, unit->genetBase + MDIO_CMD);

	/* Start MDIO transaction */
	mdio_start(unit);

	s32 ret = mdio_wait_idle(unit);
	if (ret)
		return ret;

	val = mmio_read32(unit->genetBase + MDIO_CMD);

	/* MDIO_READ_FAIL means the PHY never drove the bus during turn-around, so
	 * the data half of the register is the idle line, not a register value.
	 * Reporting it as data would read every register as 0xffff, which
	 * genphy_update_link()/genphy_parse_link() resolve to "1000BASE-T full
	 * duplex, link up" - a link the MAC is then programmed for and the stack
	 * told about. Only a negative return keeps that off the wire. */
	if (val & MDIO_READ_FAIL)
		return -EIO;

	return (s32)(val & 0xffffU);
}
