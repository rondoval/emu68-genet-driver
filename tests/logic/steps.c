// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * steps.c - the step loop of the logic tier's programs (support.h): each
 * argument is a step, name[=operands], made by the program's function for the
 * name, and followed by the state as step.N.*.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kv.h"
#include "support.h"

#define NUMBER_BASE_ANY 0 /* strtoul: decimal, 0x hex or 0 octal */

/**
 * run_step() - Make one step with the program's function for its name.
 * @program: the program's steps and printer.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @step: the step as given.
 *
 * Return: FALSE for a name the program does not know, or operands its function refused.
 */
static BOOL run_step(const struct LogicProgram *program, struct GenetUnit *unit, const char *prefix,
                     const char *step)
{
    const char *equals = strchr(step, '=');
    size_t length = equals != NULL ? (size_t)(equals - step) : strlen(step);

    for (ULONG index = 0; index < program->count; index++)
    {
        const struct LogicStep *kind = &program->steps[index];
        if (strlen(kind->name) == length && strncmp(kind->name, step, length) == 0)
            return kind->run(unit, prefix, equals != NULL ? equals + 1 : NULL);
    }
    return FALSE;
}

/**
 * logic_main() - Make each step given and print the state after it.
 * @program: the program's steps and printer.
 * @unit: the unit.
 * @argc: the argument count.
 * @argv: the steps.
 *
 * Return: KV_RC_PASS (the test judges what was printed); KV_RC_ENVIRONMENT
 * and fail= for a step the program does not know.
 */
int logic_main(const struct LogicProgram *program, struct GenetUnit *unit, int argc, char **argv)
{
    char prefix[LOGIC_KEY_TEXT];

    kv_begin(program->command);
    for (int index = 1; index < argc; index++)
    {
        sprintf(prefix, "step.%d", index - 1);
        if (!run_step(program, unit, prefix, argv[index]))
        {
            kv_environment("step %d: %s is not a step", index - 1, argv[index]);
            return kv_end();
        }
        program->print_state(unit, prefix);
    }
    kv_num("steps", (ULONG)(argc - 1));
    return kv_end();
}

/**
 * logic_switch() - Read a switch: on or up, off or down.
 * @text: the text, or NULL.
 * @on: set to the switch.
 *
 * Return: FALSE for neither.
 */
BOOL logic_switch(const char *text, BOOL *on)
{
    if (text == NULL)
        return FALSE;
    *on = strcmp(text, "on") == 0 || strcmp(text, "up") == 0;
    return *on || strcmp(text, "off") == 0 || strcmp(text, "down") == 0;
}

/**
 * logic_numbers() - Read @count numbers separated by commas, decimal or 0x hex.
 * @text: the text, or NULL.
 * @numbers: where they go.
 * @count: how many.
 *
 * Return: TRUE when exactly @count were there.
 */
BOOL logic_numbers(const char *text, ULONG *numbers, ULONG count)
{
    if (text == NULL)
        return FALSE;
    for (ULONG index = 0; index < count; index++)
    {
        char *end;
        numbers[index] = strtoul(text, &end, NUMBER_BASE_ANY);
        if (end == text || *end != (index + 1 < count ? ',' : '\0'))
            return FALSE;
        text = end + 1;
    }
    return TRUE;
}

/**
 * logic_print_flag() - Print a yes/no fact under a step's prefix.
 * @prefix: the step's prefix.
 * @name: the fact's name, from its dot.
 * @value: the flag.
 */
void logic_print_flag(const char *prefix, const char *name, BOOL value)
{
    char key[LOGIC_KEY_TEXT];
    logic_key(key, prefix, "%s", name);
    kv_str(key, value ? "yes" : "no");
}

/**
 * logic_print_number() - Print a number under a step's prefix.
 * @prefix: the step's prefix.
 * @name: the fact's name, from its dot.
 * @value: the number.
 */
void logic_print_number(const char *prefix, const char *name, ULONG value)
{
    char key[LOGIC_KEY_TEXT];
    logic_key(key, prefix, "%s", name);
    kv_num(key, value);
}

/**
 * logic_print_text() - Print a text under a step's prefix.
 * @prefix: the step's prefix.
 * @name: the fact's name, from its dot.
 * @value: the text, one line.
 */
void logic_print_text(const char *prefix, const char *name, const char *value)
{
    char key[LOGIC_KEY_TEXT];
    logic_key(key, prefix, "%s", name);
    kv_str(key, value);
}
