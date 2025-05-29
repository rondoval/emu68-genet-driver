// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifndef __DEBUG_H
#define __DEBUG_H

#ifdef DEBUG
#define __NOLIBBASE__
#include <stdarg.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define Kprintf PrintPistorm

#ifdef DEBUG_HIGH
#define KprintfH PrintPistorm
#else
#define KprintfH(...)
#endif

static void putch(UBYTE data asm("d0"), APTR dummy asm("a3"))
{
	(void)dummy;
	if(data!=0) {
		*(UBYTE*)0xdeadbeef = data;
	}
}

static inline void PrintPistorm(char *fmt, ...)
{
	struct ExecBase *SysBase = *(struct ExecBase **)4UL;
	va_list args;
	va_start(args, fmt);
	RawDoFmt((CONST_STRPTR)fmt, args, (APTR)putch, NULL);
	va_end(args);
}

#else
#define Kprintf(...)
#define KprintfH(...)
#endif

#endif