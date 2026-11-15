// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * kv.c - the output grammar of the suite's Amiga-side programs (kv.h).
 */

#include "kv.h"

#include <stdarg.h>
#include <stdio.h>

static ULONG checks_made;
static ULONG checks_failed;
static BOOL environment_failed;

/**
 * kv_begin() - Start the output: the format line and the command.
 * @command: what this run does, as its first argument names it.
 */
void kv_begin(const char *command)
{
    printf("format=1\ncommand=%s\n", command);
}

/**
 * kv_str() - Print one fact as text.
 * @key: its key.
 * @value: its value, one line.
 */
void kv_str(const char *key, const char *value)
{
    printf("%s=%s\n", key, value);
}

/**
 * kv_num() - Print one fact as an unsigned decimal.
 * @key: its key.
 * @value: its value.
 */
void kv_num(const char *key, ULONG value)
{
    printf("%s=%lu\n", key, (unsigned long)value);
}

/**
 * kv_snum() - Print one fact as a signed decimal.
 * @key: its key.
 * @value: its value.
 */
void kv_snum(const char *key, LONG value)
{
    printf("%s=%ld\n", key, (long)value);
}

/**
 * kv_hex() - Print one fact as hexadecimal.
 * @key: its key.
 * @value: its value.
 */
void kv_hex(const char *key, ULONG value)
{
    printf("%s=0x%lx\n", key, (unsigned long)value);
}

/**
 * kv_note() - Print a line of prose, for people.
 * @format: its printf() format.
 * @...: the format's arguments.
 */
void kv_note(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    printf("  ");
    vprintf(format, arguments);
    printf("\n");
    va_end(arguments);
}

/**
 * kv_check() - Print one check as check.N.name, check.N.ok and check.N.detail.
 * @passed: its outcome.
 * @name: what it checks, the same text every run.
 * @format: the printf() format of its detail: what was seen.
 * @...: the format's arguments.
 *
 * Return: @passed.
 */
BOOL kv_check(BOOL passed, const char *name, const char *format, ...)
{
    va_list arguments;
    ULONG index = checks_made++;
    printf("check.%lu.name=%s\ncheck.%lu.ok=%s\ncheck.%lu.detail=", (unsigned long)index, name,
           (unsigned long)index, passed ? "yes" : "no", (unsigned long)index);
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    printf("\n");
    if (!passed)
        checks_failed++;
    return passed;
}

/**
 * kv_environment() - Report that the run could not test: a device, port or
 * timer did not open.
 * @format: the printf() format of what failed.
 * @...: the format's arguments.
 */
void kv_environment(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    printf("fail=");
    vprintf(format, arguments);
    printf("\n");
    va_end(arguments);
    environment_failed = TRUE;
}

/**
 * kv_end() - End the output with its verdict.
 *
 * Return: KV_RC_ENVIRONMENT after kv_environment(), KV_RC_CHECK_FAILED after
 * a failed check, KV_RC_PASS otherwise.
 */
int kv_end(void)
{
    BOOL passed = !environment_failed && checks_failed == 0;
    printf("checks=%lu\nchecks_failed=%lu\n%s\n", (unsigned long)checks_made, (unsigned long)checks_failed,
           passed ? "PASS" : "FAIL");
    if (environment_failed)
        return KV_RC_ENVIRONMENT;
    return checks_failed == 0 ? KV_RC_PASS : KV_RC_CHECK_FAILED;
}
