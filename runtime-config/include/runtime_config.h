// SPDX-License-Identifier: GPL-2.0+
#ifndef GENET_RUNTIME_CONFIG_H
#define GENET_RUNTIME_CONFIG_H

#include <types.h>

#define DEVICE_PRIORITY -90

/* Defaults (compile-time fallbacks) */
/* The unit task is the bottom half of the receive interrupt: it has to run
 * when the interrupt says so. Exec does not preempt among equals, so at the
 * priority of the AmigaDOS handler processes (10) a handler busy with a packet
 * keeps it off the CPU for milliseconds. 15 sits above the handlers and the pump,
 * below input.device (20), and far above dynamic-scheduler managed bands (Executive
 * reprioritizes pri <= 5). Same value and reasoning as the 4.x driver. */
#define DEFAULT_UNIT_TASK_PRIORITY 15
/* Clamped: the field is an s8, so an unclamped 200 would land at -56 and park
 * the unit task below every dynamic-scheduler band. Below the floor the driver
 * loses to the handlers it must outrun; above the ceiling it outranks
 * input.device (20) and the keyboard stops answering. */
#define UNIT_TASK_PRIORITY_MIN 6
#define UNIT_TASK_PRIORITY_MAX 19
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
/* Debug-only; compiled out (call included) without DEBUG. */
#ifdef DEBUG
void DumpGenetRuntimeConfig(const struct GenetRuntimeConfig *config);
#else
#define DumpGenetRuntimeConfig(config) ((void)0)
#endif

#endif /* GENET_RUNTIME_CONFIG_H */
