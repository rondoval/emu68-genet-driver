// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifndef _SETTINGS_H
#define _SETTINGS_H

/*
 * Central tuneables for the GENET SANA-II driver.
 *
 * Notes:
 *  - Time values are in microseconds unless otherwise stated.
 *  - Changing any of these requires a rebuild (they are compile-time constants).
 */

#define DEVICE_NAME "genet.device"
#define DEVICE_IDSTRING "genet 1.3 (22 Aug 2025)"
#define DEVICE_VERSION 1
#define DEVICE_REVISION 3
#define DEVICE_PRIORITY -90

/* Priority of the per-unit task. 0 = normal priority.
 * Raise (>0) to bias RX/TX responsiveness; lower (<0) to reduce scheduling impact. */
#define UNIT_TASK_PRIORITY 0
/* Stack size for the unit task expressed as number of ULONG slots. */
#define UNIT_STACK_SIZE (65536 / sizeof(ULONG))

/* Enable zero-copy DMA on TX paths when opener provides suitably aligned buffers.
 * 0: always copy into internal, aligned buffers.
 * 1: attempt DMA (requires 64-byte alignment & non-CHIP memory).
 * Leave disabled for now; SANA-II does not provide 64 byte aligned access. 
 * Misaligned use causes instability. */
#define USE_DMA 0

/* Miami DX workaround for CopyFromBuff requiring 32-bit length rounding. 
 * This is in turn not compatible with Roadshow, so you have to choose. */
#define USE_MIAMI_WORKAROUND 1

/* Number of consecutive fast (minimum-delay) polling ticks granted after we detect
 * outstanding TX descriptors. Set 0 to disable. */
#define TX_PENDING_FAST_TICKS 0
/* Soft upper bound on the sleep between TX reclaim checks while *any* descriptors
 * remain outstanding. Prevents long tail latency if the general poll backoff ladder
 * has expanded. */
#define TX_RECLAIM_SOFT_US 500

/* Optional RX burst polling after initial packet activity detected in a tick.
 * If >0 we spin up to RX_POLL_BURST iterations trying to drain more frames before
 * re-arming the timer. */
#define RX_POLL_BURST 0
/* Inside a burst, abort early if we encounter this many consecutive empty polls.
 * Only meaningful when RX_POLL_BURST > 0. */
#define RX_POLL_BURST_IDLE_BREAK 100

/* Poll delay ladder (backoff strategy) used by the unit task.
 * First element = fastest interval after activity. Each idle step advances one slot.
 * Tuning: add larger values for lower idle CPU use, or add an extra small value at
 * the front for lower latency. Must remain a brace-enclosed initializer. */
#define POLL_DELAY_US {1000, 2000, 2000, 4000, 8000}

#endif /* _SETTINGS_H */
