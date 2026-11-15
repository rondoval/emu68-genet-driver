// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * unittask.c - the logic tier's program for unit_task.c's command routing
 * (tests/README.md): what NSCMD_DEVICEQUERY answers, through ProcessCommand()
 * as the unit task runs it.
 *
 *     LogicUnitTask <step>...
 *
 * Each step, then the state after it as step.N.*:
 *
 *     query[=<io_Length>|null]   NSCMD_DEVICEQUERY into a NSDeviceQueryResult of that size, or none
 */

#include "genet.device/src/unit_task.c"

#include <string.h>

#include "kv.h"
#include "support.h"

static struct NSDeviceQueryResult result;

/**
 * print_commands() - The command list the query handed out, as cmd.N=<name>.
 * @prefix: the step's prefix.
 * @commands: the list, ended by 0.
 */
static void print_commands(const char *prefix, const UWORD *commands)
{
    char key[LOGIC_KEY_TEXT];
    ULONG count = 0;

    for (; commands != NULL && *commands != 0; commands++)
    {
        logic_key(key, prefix, ".cmd.%lu", (unsigned long)count++);
        kv_str(key, logic_command_name(*commands));
    }
    logic_print_number(prefix, ".cmds", count);
}

/**
 * step_query() - NSCMD_DEVICEQUERY through ProcessCommand(), quick, so nothing is replied.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: io_Length, null for no io_Data, NULL for the whole structure.
 *
 * Return: FALSE for operands that are none of them.
 */
static BOOL step_query(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    struct IOStdReq io;
    ULONG length = sizeof(result);

    memset(&io, 0, sizeof(io));
    memset(&result, 0, sizeof(result));
    io.io_Message.mn_Length = sizeof(io);
    io.io_Unit = (struct Unit *)unit;
    io.io_Command = NSCMD_DEVICEQUERY;
    io.io_Flags = IOF_QUICK;
    io.io_Data = &result;
    if (operands != NULL && strcmp(operands, "null") == 0)
        io.io_Data = NULL;
    else if (operands != NULL && !logic_numbers(operands, &length, 1))
        return FALSE;
    io.io_Length = length;

    ProcessCommand(&io);
    logic_print_text(prefix, ".error", io.io_Error == 0 ? "0" : logic_error_name(io.io_Error));
    logic_print_number(prefix, ".actual", io.io_Actual);
    logic_print_number(prefix, ".size_available", result.nsdqr_SizeAvailable);
    logic_print_number(prefix, ".format", result.nsdqr_DevQueryFormat);
    logic_print_number(prefix, ".device_type", result.nsdqr_DeviceType);
    logic_print_number(prefix, ".device_subtype", result.nsdqr_DeviceSubType);
    print_commands(prefix, result.nsdqr_SupportedCommands);
    return TRUE;
}

/**
 * print_state() - The calls the step made.
 * @unit: the unit.
 * @prefix: the step's prefix.
 */
static void print_state(const struct GenetUnit *unit, const char *prefix)
{
    char key[LOGIC_KEY_TEXT];
    (void)unit;
    logic_key(key, prefix, ".forbids");
    kv_snum(key, logic_forbids());
    logic_print_calls(prefix);
}

static const struct LogicStep steps[] = {
    {"query", step_query},
};

static const struct LogicProgram program = {"unittask", steps, LOGIC_COUNT_OF(steps), print_state};

/**
 * main() - Make each step.
 * @argc: the argument count.
 * @argv: the steps.
 *
 * Return: logic_main()'s.
 */
int main(int argc, char **argv)
{
    return logic_main(&program, logic_unit_create(), argc, argv);
}
