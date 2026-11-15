/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+ */
/*
 * support.h - what genet's logic programs share on top of emu68-common's
 * <emu68test/logic.h> (tests/README.md): a unit to run the driver's code on,
 * the calls its stand-ins record, Ethernet addresses, and the driver's
 * numbers by name.
 *
 * The driver reaches exec through unit->sysBase and utility through the
 * device's utilityBase, so the unit is handed logic.h's fake bases. The
 * driver functions a program stubs (stubs.c) record their calls.
 */
#ifndef GENET_LOGIC_SUPPORT_H
#define GENET_LOGIC_SUPPORT_H

#include <exec/types.h>

#include <emu68test/logic.h>

struct GenetUnit;

/* The calls the driver's stand-ins record, beside logic.h's exec ones. */
enum GenetCallKind : UBYTE
{
    LOGIC_EVENTS = LOGIC_CALL_PROGRAM, /* sana2_report_events(): value the events */
    LOGIC_SET_RX_MODE,                 /* bcmgenet_set_rx_mode() */
    LOGIC_IRQ0_ENABLE,                 /* bcmgenet_irq0_enable(): value the interrupt mask */
    LOGIC_TX_ABORT,                    /* sana2_tx_backlog_abort(_opener)(): target the opener, or NULL for all */
    LOGIC_TX_IRQ_ARM,                  /* bcmgenet_tx_irq_arm(): one TX-done interrupt asked for */
};

const char *logic_name_call(const struct LogicCall *call, char *value);

/* A unit for the code under test: its device, its SANA-II side, a zeroed
 * register block, a metadata pool and the fake bases. */
#define LOGIC_REGISTER_BYTES 0x10000u /* the GENET register window */
#define LOGIC_RX_SIGNAL 8u            /* the unit task's RX signal bit */
#define LOGIC_TX_SIGNAL 9u            /* and its TX one */

struct GenetUnit *logic_unit_create(void);
ULONG logic_register(const struct GenetUnit *unit, ULONG offset);
void logic_register_set(struct GenetUnit *unit, ULONG offset, ULONG value);

/* An Ethernet address as aa:bb:cc:dd:ee:ff. */
#define LOGIC_ADDRESS_TEXT 18u /* aa:bb:cc:dd:ee:ff and its NUL */

void logic_format_address(char *text, const UBYTE *address);
void logic_print_address(const char *key, const UBYTE *address);
BOOL logic_parse_address(const char *text, UBYTE *address);
ULONG logic_parse_range(const char *text, UBYTE *lower, UBYTE *upper);

/* io_Command, io_Error (S2ERR_*, IOERR_*, NDERR_*), ios2_WireError and S2EVENT_* masks by name (names.c). */
#define LOGIC_EVENTS_TEXT LOGIC_VALUE_TEXT /* every S2EVENT_* name, joined by '|' */

const char *logic_command_name(UWORD command);
const char *logic_error_name(LONG error);
const char *logic_wire_error_name(ULONG wireError);
void logic_events_text(char *text, ULONG mask);

#endif /* GENET_LOGIC_SUPPORT_H */
