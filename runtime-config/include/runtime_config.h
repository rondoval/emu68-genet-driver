// SPDX-License-Identifier: GPL-2.0+
#ifndef GENET_RUNTIME_CONFIG_H
#define GENET_RUNTIME_CONFIG_H

#include <types.h>

/*
 * ENV:genet.prefs — what the machine's owner decides, and nothing else.
 *
 * A key belongs here only if a user can hit a problem that this key alone
 * solves: a switch that will not negotiate (LINK_MODE / AUTONEG /
 * FLOW_CONTROL), a pool that must be sized before the stack attaches
 * (RX_POOL_BUFS), a scheduler that starves the unit task (UNIT_TASK_PRIORITY).
 *
 * Every key here must be clamped or enumerated on parse. An unvalidated knob
 * is a way for a user to break their own machine quietly.
 */

/* Defaults (compile-time fallbacks) */
/* The unit task is the bottom half of the receive interrupt: it has to run
 * when the interrupt says so. Exec does not preempt among equals, so at the
 * priority of the AmigaDOS handler processes (10) a handler busy with a packet
 * kept it off the CPU for milliseconds - late ACKs, late replies, and a ring
 * that can overrun under a burst. 15 sits above the handlers, below
 * input.device (20), and far above dynamic-scheduler managed bands (Executive
 * reprioritizes pri <= 5). */
#define DEFAULT_UNIT_TASK_PRIORITY 15
/* Clamped: the field is an s8, so an unclamped 200 would land at
 * -56 and park the unit task below every dynamic-scheduler band. Below the
 * floor the driver loses to the handlers it must outrun; above the ceiling it
 * outranks input.device (20) and the keyboard stops answering. */
#define UNIT_TASK_PRIORITY_MIN 6
#define UNIT_TASK_PRIORITY_MAX 19

/* RX buffer pool: the ring uses 256; the rest covers buffers the stack
 * holds in socket receive queues. 0 = auto: sized at netdev ATTACH from
 * the stack's declared hold budget (nda_RxHoldReq). An explicit
 * RX_POOL_BUFS in ENV:genet.prefs is an absolute operator override.
 * The fallback covers an auto attach from a stack that states no budget:
 * four full 256 KB TCP windows (4 × 179 frames) plus the ring and reclaim
 * slack. Bounds keep the DMA slab (× 2 KB per buffer) and recycle ring sane. */
#define DEFAULT_RX_POOL_BUFS 0
#define RX_POOL_BUFS_FALLBACK 1280
#define RX_POOL_BUFS_MIN 512
#define RX_POOL_BUFS_MAX 4096

/*
 * Link configuration, resolved from the LINK_MODE and AUTONEG keys.
 *
 * LINK_MODE narrows what the PHY advertises rather than switching
 * autonegotiation off: 1000BASE-T establishes master/slave clock roles through
 * negotiation, so a link pinned to one speed is expressed by advertising only
 * that speed, not by forcing the BMCR. That works at every speed and stays
 * within the standard.
 *
 * AUTONEG=off is the separate, narrower case: a partner that does not negotiate
 * at all. It forces the BMCR to link_speed/link_duplex, which is legal for
 * 10/100 only - LINK_MODE_NONE below marks the combinations that carry no
 * forced speed and so cannot be honoured.
 */
#define LINK_MODE_NONE 0 /* "auto": advertise everything */

#define DEFAULT_LINK_SPEED LINK_MODE_NONE
#define DEFAULT_LINK_DUPLEX DUPLEX_FULL_CFG
#define DEFAULT_LINK_AUTONEG 1

/* Duplex, mirroring genet/ethtool.h's DUPLEX_* without dragging a
 * hardware-layer header into the config library. */
#define DUPLEX_HALF_CFG 0
#define DUPLEX_FULL_CFG 1

/* Symmetric pause: advertised and negotiated per 802.3 Annex 31B, with the
 * result programmed into the UniMAC's CMD_*_PAUSE_IGNORE bits. Off means the
 * driver neither asks for pause nor honours it - the two halves always agree. */
#define DEFAULT_FLOW_CONTROL 1

struct GenetRuntimeConfig
{
    s8 unit_task_priority;
    u32 rx_pool_bufs;
    u16 link_speed;  /* 10, 100, 1000, or LINK_MODE_NONE for auto */
    u8 link_duplex;  /* DUPLEX_*_CFG; meaningful only with link_speed set */
    u8 link_autoneg; /* 0 forces link_speed/link_duplex outright */
    u8 flow_control;
};

void LoadGenetRuntimeConfig(struct GenetRuntimeConfig *config);
/* Debug-only; compiled out (call included) without DEBUG. */
#ifdef DEBUG
void DumpGenetRuntimeConfig(const struct GenetRuntimeConfig *config);
#else
#define DumpGenetRuntimeConfig(config) ((void)0)
#endif

#endif /* GENET_RUNTIME_CONFIG_H */
