/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+ */
/*
 * support.h - what the logic tier's programs share (tests/README.md): exec and
 * utility.library as the driver sees them, a unit to run its code on, and the
 * calls it made, for the program to print.
 *
 * The driver reaches exec through unit->sysBase and utility through the
 * device's utilityBase, so a program hands it fake bases (fakelib.c): each
 * function the code under test may call goes to a stub (fakeexec.c), the
 * ones that only keep memory or lists on to the real exec, and any other call
 * ends the run with fail=<the library and LVO>. Signal(), PutMsg(),
 * ReplyMsg() and the driver functions a program stubs (stubs.c) are recorded.
 */
#ifndef GENET_LOGIC_SUPPORT_H
#define GENET_LOGIC_SUPPORT_H

#include <exec/types.h>

struct GenetUnit;
struct Library;
struct ExecBase;
struct Message;

/* What a recorded call was. */
enum LogicCallKind : UBYTE
{
    LOGIC_SIGNAL = 1,     /* exec Signal(): target the task, value the signal set */
    LOGIC_PUTMSG,         /* exec PutMsg(): target the message, value the port */
    LOGIC_REPLYMSG,       /* exec ReplyMsg(): target the message */
    LOGIC_EVENTS,         /* sana2_report_events(): value the events */
    LOGIC_SET_RX_MODE,    /* bcmgenet_set_rx_mode() */
    LOGIC_IRQ0_ENABLE,    /* bcmgenet_irq0_enable(): value the interrupt mask */
    LOGIC_TX_ABORT,       /* sana2_tx_backlog_abort(_opener)(): target the opener, or NULL for all */
    LOGIC_TX_IRQ_ARM,     /* bcmgenet_tx_irq_arm(): one TX-done interrupt asked for */
};

/* One call the code under test made. */
struct LogicCall
{
    enum LogicCallKind kind; /* what was called */
    APTR target;             /* the task, message or opener it was called on */
    ULONG value;             /* its number: signals, events, mask */
};

#define LOGIC_CALLS_MAX 256u

void logic_record(enum LogicCallKind kind, APTR target, ULONG value);
ULONG logic_calls(const struct LogicCall **calls);
void logic_calls_clear(void);
const char *logic_call_name(enum LogicCallKind kind);

/* A fake library's function: its LVO (negative) and the stub it jumps to. */
struct FakeFunction
{
    LONG lvo;  /* the offset from the base, as the NDK's lvo/<library>_lib.i gives it */
    APTR code; /* the stub, which takes the LVO's registers */
};

struct Library *fake_library_create(const char *name, ULONG slots, ULONG baseSize, const void *image,
                                    const struct FakeFunction *functions, ULONG count);

/* The fake bases, built at the first call; logic_forbids() is the Forbid()
 * and Disable() nesting now, 0 when the code under test left them balanced. */
struct ExecBase *logic_exec(void);
struct Library *logic_utility(void);
LONG logic_forbids(void);
/* The next @count allocations the code under test asks the fake exec for fail. */
void logic_fail_allocations(ULONG count);
/* The oldest message PutMsg() posted and nobody took yet, or NULL. */
struct Message *logic_port_take(void);

/* A unit for the code under test: its device, its SANA-II side, a zeroed
 * register block, a metadata pool and the fake bases. */
#define LOGIC_REGISTER_BYTES 0x10000u /* the GENET register window */
#define LOGIC_RX_SIGNAL 8u            /* the unit task's RX signal bit */
#define LOGIC_TX_SIGNAL 9u            /* and its TX one */

struct GenetUnit *logic_unit_create(void);
ULONG logic_register(const struct GenetUnit *unit, ULONG offset);
void logic_register_set(struct GenetUnit *unit, ULONG offset, ULONG value);

/* Printing: a key under a step's prefix, an address as aa:bb:cc:dd:ee:ff, and the
 * calls as <prefix>.call.N.*. */
#define LOGIC_ADDRESS_TEXT 18u /* aa:bb:cc:dd:ee:ff and its NUL */
#define LOGIC_KEY_TEXT 64u     /* room for a key such as step.12.call.3.value */

void logic_key(char *key, const char *prefix, const char *format, ...) __attribute__((format(printf, 3, 4)));
void logic_format_address(char *text, const UBYTE *address);
void logic_print_address(const char *key, const UBYTE *address);
BOOL logic_parse_address(const char *text, UBYTE *address);
ULONG logic_parse_range(const char *text, UBYTE *lower, UBYTE *upper);
void logic_print_calls(const char *prefix);

/* The step loop (steps.c): each argument is a step, name[=operands], made by
 * the program's function for the name; the state follows as step.N.*. A
 * function returns FALSE for operands it cannot take. */
struct LogicStep
{
    const char *name; /* the text before '=' */
    BOOL (*run)(struct GenetUnit *unit, const char *prefix, const char *operands); /* operands NULL without '=' */
};

struct LogicProgram
{
    const char *command;            /* the command= fact */
    const struct LogicStep *steps;  /* the steps it knows */
    ULONG count;                    /* how many */
    void (*print_state)(const struct GenetUnit *unit, const char *prefix); /* after each step */
};

#define LOGIC_COUNT_OF(table) (sizeof(table) / sizeof((table)[0]))

int logic_main(const struct LogicProgram *program, struct GenetUnit *unit, int argc, char **argv);
BOOL logic_switch(const char *text, BOOL *on);
BOOL logic_numbers(const char *text, ULONG *numbers, ULONG count);
void logic_print_flag(const char *prefix, const char *name, BOOL value);
void logic_print_number(const char *prefix, const char *name, ULONG value);
void logic_print_text(const char *prefix, const char *name, const char *value);

/* io_Command, io_Error (S2ERR_*, IOERR_*, NDERR_*), ios2_WireError and S2EVENT_* masks by name (names.c). */
#define LOGIC_EVENTS_TEXT 200u /* every S2EVENT_* name, joined by '|' */

const char *logic_command_name(UWORD command);
const char *logic_error_name(LONG error);
const char *logic_wire_error_name(ULONG wireError);
void logic_events_text(char *text, ULONG mask);

#endif /* GENET_LOGIC_SUPPORT_H */
