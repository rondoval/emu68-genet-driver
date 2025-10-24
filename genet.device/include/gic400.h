// SPDX-License-Identifier: GPL-2.0
#ifndef _GENET_GIC400_H
#define _GENET_GIC400_H

#include <exec/types.h>

struct Interrupt;

#ifdef __cplusplus
extern "C" {
#endif

int gic400_init();
void gic400_shutdown();
int gic400_get_irq_status(ULONG irq, BOOL *pending, BOOL *active, BOOL *enabled);
int gic400_add_int_server(ULONG irq, UBYTE priority, BOOL edge, struct Interrupt *interrupt);
int gic400_rem_int_server(ULONG irq, struct Interrupt *interrupt);

#ifdef __cplusplus
}
#endif

#endif /* _GENET_GIC400_H */