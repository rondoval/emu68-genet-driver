// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifndef _GENET_DEVICE_H
#define _GENET_DEVICE_H

/*
 * This driver speaks the netdev ABI (<devices/netdev.h>) exclusively. There are
 * no openers, no buffer-management callbacks and no per-protocol queues:
 * packets move zero-copy between the stack's DMA memory and the hardware rings.
 */

#if defined(__INTELLISENSE__)
#define asm(x)
#define __attribute__(x)
#endif

#include <exec/devices.h>
#include <exec/interrupts.h>
#include <exec/types.h>

#include <errors.h>
#include <types.h>
#include <bcm_gpio.h>
#include <dma_mem.h>
#include <reset_guard.h>

#include <devices/netdev.h>
#include <perf.h> /* datapath timing (PERF_T0/PERF_ADD, struct perf) */

#include <genet/phy.h>
#include <genet/bcmgenet.h>
#include <genet/bcmgenet-regs.h> /* GENET_MDF_MCAST_MAX sizes the unit's filter list */
#include <runtime_config.h>

/* Datapath perf slots (emu68-common <perf.h>), reported as [genet] by
 * bcmgenet_perf_tick(). Order must match genet_perf_names[] in device.c. */
enum GenetProfSlot
{
	GP_RX_DRAIN,  /* whole bcmgenet_netdev_rx ring walk (excl. recycle) */
	GP_RX_FLUSH,  /* nso_RxInput hand-up (lock wait + stack work) */
	GP_TX_SUBMIT, /* bcmgenet_netdev_tx_submit cache-prime + ring writes + doorbell */
	GP_TX_HARVEST,/* bcmgenet_tx_harvest completion sweep + nso_TxDone */
	GP_SLOT_COUNT
};

/*
 * Internal status codes, from emu68-common <errors.h>. None of these crosses the
 * netdev ABI — the command handlers in netdev_api.c translate to NDERR_*.
 *
 * Two sign conventions live in this driver, each for a reason:
 *   - Functions returning only a status use an unsigned result, 0 for success
 *     and a positive errno otherwise (GENET_OK / ENOMEM / EIO / EINVAL below).
 *   - The PHY layer keeps Linux's negative-errno convention, because its
 *     functions return a register value and an error through the same s32, so
 *     the sign is what tells them apart.
 */
#define GENET_OK 0

#define LIB_MIN_VERSION 39 /* we use memory pools */

/* Resident priority, in the device's own header rather than the runtime config:
 * it is baked into the ROM tag at build time, not read from ENV:genet.prefs. */
#define DEVICE_PRIORITY -90

/* Free-BD watermark: only re-read TDMA_CONS_INDEX when the producer's cached
 * view of the hardware consumer says free slots fell below this. Must exceed
 * the max descriptors per packet (ND_TX_MAX_SEGS). */
#define TX_CONS_REFRESH_BDS 16U

/* netdev geometry. The RX pool size resolves at ATTACH: an explicit
 * RX_POOL_BUFS runtime config is an absolute operator override; 0 (auto,
 * the default) sizes from the stack's declared hold budget. The ring uses
 * 256, the rest covers frames the stack holds in socket receive queues —
 * it must fit the sum of the open connections' receive windows or RX drops
 * to pool-dry. The recycle ring is sized with it at ATTACH (next power of
 * two ≥ pool). */
#define ND_TX_MAX_SEGS 8u	  /* scatter-gather bound advertised in the caps */
#define ND_TXDONE_RING_N 512u
#define ND_TXDONE_RING_MASK (ND_TXDONE_RING_N - 1u)

/* Frames handed to the stack per nso_RxInput call. This and `budget` are the
 * RX lock-cadence knobs: the stack takes one netstack_lock() per batch, so a
 * larger batch means fewer lock acquisitions but a longer single hold. It also
 * bounds the stack's GRO merge window — one batch is one lock hold is one
 * merge run. Two constraints: budget >= ND_RX_BATCH, or the batch cannot fill
 * in one drain pass; and ND_RX_BATCH <= NDIF_RX_CHUNK, or the stack re-locks
 * mid-batch. Measure a change with the [genet] rx_drain/rx_flush perf slots. */
#define ND_RX_BATCH 64u
NETDEV_ABI_ASSERT(MIN_BUDGET >= ND_RX_BATCH);
/* Auto-pool headroom over ring + stack budget: buffers parked in the SPSC
 * recycle ring between a stack release and the unit-task drain. */
#define ND_RX_RECLAIM_SLACK 64u

struct GenetDevice;

/*
 * Unit lifecycle. UNCONFIGURED -> CONFIGURED at the first NETDEV_CMD_ATTACH,
 * which probes the controller and creates the PHY; CONFIGURED <-> ONLINE on
 * START/STOP, which only start and stop hardware. STOP therefore returns to
 * CONFIGURED, and everything a START needs is still there.
 */
typedef enum
{
	STATE_UNCONFIGURED = 0,
	STATE_CONFIGURED,
	STATE_ONLINE
} UnitState;

/*
 * TX ring state — PRODUCER-OWNED IN FULL (ndo_TxSubmit context; the stack's
 * core lock makes that a single logical producer). The descriptors live in
 * the controller's own BD RAM and are read by the DMA engine alone, so BD
 * slots are recycled straight off the hardware consumer index: no consumer
 * state takes part, and the ring needs no lock. Cookies travel to the unit
 * task through the separate ndTxDone FIFO (see struct GenetTxDone).
 */
struct bcmgenet_tx_ring
{
	u16 tx_prod_index; /* BDs handed to hardware (16-bit modular) */
	u16 hw_cons_cache; /* cached TDMA_CONS_INDEX; refreshed on low water */
};

/*
 * TX-done FIFO element. gtd_End is the producer BD index just past the
 * packet, so the entry is complete once the hardware consumer has reached it
 * — see bcmgenet_tx_harvest(). A packet the sanity gate rejects is pushed
 * with gtd_End == tx_prod_index: a zero-BD packet, completed in submission
 * order behind whatever is already queued.
 *
 * gtd_Bytes is the frame length, carried so the byte and packet counters can
 * be tallied where the frame actually reaches the wire (bcmgenet_tx_harvest)
 * rather than where it is queued. It also identifies the sanity-gate rejects:
 * they are the only entries with a length of zero. A frame is at most
 * ND_TX_MAX_SEGS full segments, well inside 16 bits.
 */
struct GenetTxDone
{
	APTR gtd_Cookie;
	u16 gtd_End;
	u16 gtd_Bytes;
};

struct bcmgenet_rx_ring
{
	struct enet_cb *rx_control_block; /* Rx ring buffer control block */
	u16 rx_cons_index;				  /* Rx last consumer index */
	u16 old_discards;
};

/* RX ring slot bookkeeping (the TX ring keeps none — see bcmgenet_tx_ring). */
struct enet_cb
{
	APTR descriptor_address;
	dma_addr_t data_buffer; /* DMA address fed to hardware */
};

/*
 * Every counter the driver keeps.
 *
 * Written from three contexts and read from one: the RX drain and the TX
 * harvest run on the unit task, the submit path bumps tx_rejected/tx_bad in the
 * calling stack's context, and the ISR owns the irq0_ tallies. All of those are
 * single aligned longwords or 64-bit values the unit task alone accumulates, so
 * a read on the unit task needs no guard.
 */
struct internal_stats
{
	u64 rx_packets;
	u64 rx_bytes;
	u64 rx_dropped;	 /* no pool buffer, or stack backpressure */
	u32 rx_pool_dry; /* the pool-dry share of rx_dropped */
	u32 rx_overruns; /* RX HW miss (discard counter) */
	/* RBUF FIFO overflow, accumulated. The hardware counter saturates and is
	 * rearmed by writing 0, so it cannot be reported raw and stay monotonic:
	 * bcmgenet_mib_rbuf_ovfl() folds each read's delta in here. */
	u32 rx_fifo_errors;
	u32 rx_fifo_last; /* last raw register value, for that delta */
	u32 rx_other_errors;
	u32 rx_crc_errors;
	u32 rx_over_errors;
	u32 rx_frame_errors;
	u32 rx_length_errors;
	u32 rx_fragmented_errors;

	u64 tx_packets;
	u64 tx_bytes;
	u64 tx_dropped;	  /* accepted, then completed unsent at STOP */
	u32 tx_rejected;  /* TxSubmit calls that accepted nothing (ring full) */
	u32 tx_bad;		  /* descriptors refused by the sanity gate (garbage SG) */

	u32 irq0_count;		  /* IRQ0 fires (RX/TX/error) */
	u32 irq0_tx_count;	  /* IRQ0 fires that included TXDMA_DONE */
	u32 irq0_rx_count;	  /* IRQ0 fires that included RXDMA_DONE */
	u32 irq0_other_count; /* IRQ0 fires with neither TX nor RX DONE */
};

struct GenetUnit
{
	struct Unit unit;
	struct dma_mem_ctx dma_ctx; /* Emu68 (DMA-reachable) RAM regions; backs dmaPool */
	struct dma_pool *dmaPool;	/* region-restricted DMA pool (Emu68 RAM) */
	APTR metaPool;				/* ordinary Exec pool for CPU-only metadata */
	struct GenetDevice *device;

	/* config */
	u32 unitNumber;
	u8 currentMacAddress[6];
	u16 budget;

	/* unit/task state */
	UnitState state;
	struct Task *task;
	struct Device *timerBase;

	/* stats */
	struct internal_stats internalStats;

	/* Datapath timing (emu68-common <perf.h>): the [genet] perf instance.
	 * Slots live in the allocated unit (ROM-able: no writable statics); the
	 * name table and prefix are rodata. Written under PROFILE; storage is
	 * unconditional so all tiers share one struct layout. bcmgenet_perf_tick
	 * reports it via perf_report() every ~2 s. */
	struct perf_counter gu_PerfSlots[GP_SLOT_COUNT];
	struct perf gu_Perf;
	u32 gu_ProfTicks; /* mib_check tick divider for the ~2 s report */

	/* Device tree */
	CONST_STRPTR compatible;
	const u8 *localMacAddress;
	APTR genetBase;
	struct tGpioRegs *gpioBase;

	/* Interrupt config. The ISR carries no state to the bottom half: Exec
	 * signals are the atomic, level-latched ISR->task channel, and the task
	 * reads the rings themselves rather than replaying status bits. One
	 * signal per source, because the sources want different treatment — see
	 * the unit task's datapath block. */
	/* IRQ0 carries every source this driver uses. IRQ1 is per-priority-queue
	 * RX/TX only, and the driver runs one ring each way, so it never fires —
	 * devtree_parse.c still requires the device tree to name it, as a check
	 * that it found a GENET node rather than something else. */
	u32 irq0_number;
	BYTE rx_signal;				  /* RX DMA done */
	BYTE tx_signal;				  /* TX DMA done, and the submit-path kick */
	BYTE link_signal;			  /* link/PHY-detect */
	BOOL irq0_installed;		  /* irq0_isr is on the GIC's server list */
	struct Interrupt irq0_isr;

	/* PHY */
	phy_interface_t phy_interface;
	u8 phyaddr;
	struct phy_device *phydev;

	/* MAC layer */
	struct bcmgenet_rx_ring rx_ring;
	struct bcmgenet_tx_ring tx_ring;

	/* Interrupt moderation, seeded from the prefs defaults at UnitOpen and
	 * replaced by NETDEV_CMD_SET_COALESCE. Held here rather than programmed
	 * straight into the hardware so a value set while the unit is stopped
	 * survives to the next start, as the RX filter does. */
	u32 coalTxFrames;
	u32 coalRxFrames;
	u32 coalRxUsecs;

	/* --- netdev attachment ------------------------------------------------
	 * One stack at a time. The direct-call surface (TxSubmit/RxRelease/
	 * DmaAlloc/DmaFree) runs in FOREIGN task context, serialized against
	 * each other by the stack's core lock — one logical producer. Nothing
	 * in the datapath takes a lock: every object crossing the two contexts
	 * is a single-producer/single-consumer ring (ndRecycle, ndTxDone), and
	 * the TX BD ring is producer-to-hardware only. All nso_* callbacks are
	 * made from the unit task only. */
	APTR ndStackCtx;
	const struct NetDevStackOps *ndStackOps;
	/* The attaching request's reply port. Identifies the owning opener: only it
	 * may STOP, DETACH or SET_MAC, so a diagnostic tool holding the same unit
	 * open cannot tear the live stack's attachment down. */
	struct MsgPort *ndOwnerPort;
	volatile BOOL ndStarted; /* delivery gate: set/cleared by unit task */
	/* ndo_TxSubmit calls in progress. Single-writer (the stack serializes its
	 * own submits), read by the unit task's STOP quiesce, which must not
	 * complete while a submitter that passed the ndStarted gate is still
	 * queueing cookies. */
	volatile u32 ndTxBusy;
	UWORD ndFilterFlags;	 /* NDFF_* as requested by the stack */
	BOOL ndPromisc;			 /* effective promiscuity (incl. all-multi fallback) */
	UWORD ndMcastCount;		 /* exact multicast MACs held in the MDF slots */
	u8 ndMcastList[GENET_MDF_MCAST_MAX][6];

	/* RX buffer pool: ndRxPoolBufs × RX_BUF_LENGTH out of one DMA slab.
	 * Free stack + held count are unit-task-only. */
	u32 ndRxPoolBufs; /* latched prefs value at UnitOpen; 0 = auto,
	                   * resolved at ATTACH from nda_RxHoldReq */
	u32 ndRxPoolTotal; /* resolved pool size (ndRxFree capacity) */
	APTR ndRxSlab;
	APTR *ndRxFree;
	u32 ndRxFreeCount;
	u32 ndRxHeld; /* buffers currently owned by the stack */

	/* TX status blocks (TSB): one 64-byte block per TX ring slot, indexed by
	 * the packet's SOP slot; submitted as the packet's first descriptor. */
	APTR ndTxTsb;

	/* RX recycle ring: producer = ndo_RxRelease (stack context), consumer =
	 * unit task. Sized at ATTACH to cover the whole pool (each buffer can
	 * be in flight at most once), so it never fills. */
	APTR *ndRecycle;
	u32 ndRecycleMask; /* ring capacity − 1 (power of two) */
	volatile u32 ndRecycleProd;
	volatile u32 ndRecycleCons;

	/* TX-done cookie FIFO: producer = ndo_TxSubmit (stack context),
	 * consumer = unit task (bcmgenet_tx_harvest). Sized well past the BD
	 * ring's packet capacity so a late harvest never throttles the
	 * producer — and so an unharvested entry can never age past the
	 * 16-bit half-window its completion test relies on. */
	struct GenetTxDone *ndTxDone;
	volatile u32 ndTxDoneProd;
	volatile u32 ndTxDoneCons;

	struct NetDevLinkState ndLink;
};

struct GenetDevice
{
	struct Device device;
	ULONG segList;
	struct GenetRuntimeConfig runtimeConfig;
	struct Library *utilityBase;
	struct Library *gic400Base;
	struct reset_guard resetGuard; /* pre-reset DMA quiesce hooks */

	// For now, we'll just assume there can be only one unit
	struct GenetUnit *unit;
};

void beginIO(struct IOStdReq *io asm("a1"), struct GenetDevice *base asm("a6"));
LONG abortIO(struct IOStdReq *io asm("a1"), struct GenetDevice *base asm("a6"));

/* Unit interface */
u32 DevTreeParse(struct GenetUnit *unit);
u32 UnitTaskStart(struct GenetUnit *unit);
void UnitTaskStop(struct GenetUnit *unit);

u32 UnitOpen(struct GenetUnit *unit, u32 unitNumber, u32 flags);
u32 UnitConfigure(struct GenetUnit *unit);
u32 UnitOnline(struct GenetUnit *unit);
void UnitOffline(struct GenetUnit *unit);
u32 UnitClose(struct GenetUnit *unit);

/* netdev command processing (unit task context) */
void ProcessCommand(struct IOStdReq *io);

/* netdev plumbing (netdev_api.c) */
APTR netdev_rx_pop(struct GenetUnit *unit);				  /* unit task */
void netdev_rx_push(struct GenetUnit *unit, APTR buffer); /* unit task */
void netdev_drain_recycle(struct GenetUnit *unit); /* unit task */
/* Publish link state to the stack, unit task only. Notifies on a change, or
 * unconditionally with @force — which START needs, since the ABI requires one
 * callback after it whether or not anything changed. */
void netdev_link_update(struct GenetUnit *unit, BOOL up, BOOL force);

#endif
