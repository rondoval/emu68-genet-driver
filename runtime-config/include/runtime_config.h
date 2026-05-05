// SPDX-License-Identifier: GPL-2.0+
#ifndef GENET_RUNTIME_CONFIG_H
#define GENET_RUNTIME_CONFIG_H

#include <types.h>

#define DEVICE_PRIORITY -90

/* Defaults (compile-time fallbacks) */
#define DEFAULT_UNIT_TASK_PRIORITY 5
#define DEFAULT_UNIT_STACK_BYTES 65536UL /* 64 KB */

#define DEFAULT_USE_DMA 0
#define DEFAULT_USE_MIAMI_WORKAROUND 0

#define DEFAULT_PERIODIC_TASK_MS 200
#define DEFAULT_BUDGET 64

#define DEFAULT_RX_COALESCE_USECS 500
#define DEFAULT_RX_COALESCE_FRAMES 64
#define DEFAULT_TX_COALESCE_FRAMES 32

struct GenetRuntimeConfig
{
    s8 unit_task_priority;
    u32 unit_stack_bytes;
    u8 use_dma;
    u8 use_miami_workaround;
    u16 budget;
    u32 periodic_task_ms;
    u32 rx_coalesce_usecs;
    u32 rx_coalesce_frames;
    u32 tx_coalesce_frames;
};

void LoadGenetRuntimeConfig(struct GenetRuntimeConfig *config);
void DumpGenetRuntimeConfig(const struct GenetRuntimeConfig *config);

#endif /* GENET_RUNTIME_CONFIG_H */
