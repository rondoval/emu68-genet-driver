// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifndef GENET_RUNTIME_CONFIG_H
#define GENET_RUNTIME_CONFIG_H

#include <exec/types.h>

#define DEVICE_PRIORITY -90

/* Defaults (compile-time fallbacks) */
#define DEFAULT_UNIT_TASK_PRIORITY 0
#define DEFAULT_UNIT_STACK_BYTES 65536UL /* 64 KB */

#define DEFAULT_USE_DMA 0
#define DEFAULT_USE_MIAMI_WORKAROUND 0

#define DEFAULT_POLL_DELAY_US 200000

#define DEFAULT_BUDGET 32

struct GenetRuntimeConfig
{
    LONG unit_task_priority;
    ULONG unit_stack_bytes;
    UBYTE use_dma;
    UBYTE use_miami_workaround;
    ULONG poll_delay_us;
    UWORD budget;
};

extern struct GenetRuntimeConfig genetConfig;

void LoadGenetRuntimeConfig();
void DumpGenetRuntimeConfig();

#endif /* GENET_RUNTIME_CONFIG_H */
