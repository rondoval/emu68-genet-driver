// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifndef GENET_RUNTIME_CONFIG_H
#define GENET_RUNTIME_CONFIG_H

#include <exec/types.h>

#define DEVICE_PRIORITY -90

/* Defaults (compile-time fallbacks) */
#define DEFAULT_UNIT_TASK_PRIORITY 5
#define DEFAULT_UNIT_STACK_BYTES 65536UL /* 64 KB */

#define DEFAULT_USE_DMA 0
#define DEFAULT_USE_MIAMI_WORKAROUND 0

#define DEFAULT_PERIODIC_TASK_MS 200
#define DEFAULT_BUDGET 32

#define DEFAULT_RX_COALESCE_USECS 500
#define DEFAULT_RX_COALESCE_FRAMES 10
#define DEFAULT_TX_COALESCE_FRAMES 10

struct GenetRuntimeConfig
{
    LONG unit_task_priority;
    ULONG unit_stack_bytes;
    UBYTE use_dma;
    UBYTE use_miami_workaround;
    UWORD budget;
    ULONG periodic_task_ms;
    ULONG rx_coalesce_usecs;
    ULONG rx_coalesce_frames;
    ULONG tx_coalesce_frames;
};

extern struct GenetRuntimeConfig genetConfig;

void LoadGenetRuntimeConfig();
void DumpGenetRuntimeConfig();

#endif /* GENET_RUNTIME_CONFIG_H */
