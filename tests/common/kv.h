/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+ */
/*
 * kv.h - the output grammar of the suite's Amiga-side programs (tests/README.md):
 *
 *   format=1
 *   command=<what ran>
 *   key=value ...                facts, groups indexed by number (check.0.name)
 *     prose ...                  for people, indented two spaces
 *   PASS | FAIL                  the last line
 *
 * Every check is check.N.name, check.N.ok=yes|no and check.N.detail. The
 * return codes are AmigaDOS levels: KV_RC_PASS, KV_RC_CHECK_FAILED,
 * KV_RC_ENVIRONMENT, and KV_RC_USAGE for a usage error, which is printed
 * before the format line.
 */
#ifndef GENET_TESTS_KV_H
#define GENET_TESTS_KV_H

#include <exec/types.h>

#define KV_RC_PASS 0
#define KV_RC_CHECK_FAILED 5
#define KV_RC_ENVIRONMENT 10
#define KV_RC_USAGE 20

void kv_begin(const char *command);
void kv_str(const char *key, const char *value);
void kv_num(const char *key, ULONG value);
void kv_snum(const char *key, LONG value);
void kv_hex(const char *key, ULONG value);
void kv_note(const char *format, ...) __attribute__((format(printf, 1, 2)));
BOOL kv_check(BOOL passed, const char *name, const char *format, ...) __attribute__((format(printf, 3, 4)));
void kv_environment(const char *format, ...) __attribute__((format(printf, 1, 2)));
int kv_end(void);

#endif /* GENET_TESTS_KV_H */
