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

#define DEFAULT_TX_PENDING_FAST_TICKS 0
#define DEFAULT_TX_RECLAIM_SOFT_US 2000

#define DEFAULT_RX_POLL_BURST 64
#define DEFAULT_RX_POLL_BURST_IDLE_BREAK 16

#define DEFAULT_POLL_LADDER {1000, 1000, 1000, 2000, 2000, 2000, 4000, 8000}
#define DEFAULT_POLL_LADDER_MAX 32

struct GenetRuntimeConfig
{
    LONG unit_task_priority;
    ULONG unit_stack_bytes;
    UBYTE use_dma;
    UBYTE use_miami_workaround;
    UWORD tx_pending_fast_ticks;
    ULONG tx_reclaim_soft_us;
    UWORD rx_poll_burst;
    UWORD rx_poll_burst_idle_break;
    ULONG poll_delay_us[DEFAULT_POLL_LADDER_MAX];
    UWORD poll_delay_len;
};

extern struct GenetRuntimeConfig genetConfig;

void LoadGenetRuntimeConfig();
void DumpGenetRuntimeConfig();

#endif /* GENET_RUNTIME_CONFIG_H */
