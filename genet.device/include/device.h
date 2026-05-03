// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifndef _GENET_DEVICE_H
#define _GENET_DEVICE_H

#if defined(__INTELLISENSE__)
#define asm(x)
#define __attribute__(x)
#endif

#include <exec/devices.h>
#include <exec/interrupts.h>
#include <exec/types.h>
#include <exec/semaphores.h>
#include <devices/sana2.h>

#include <types.h>
#include <bcm_gpio.h>

#include <genet/phy.h>
#include <genet/bcmgenet.h>
#include <genet/bcmgenet_mib.h>
#include <runtime_config.h>

#define LIB_MIN_VERSION 39 /* we use memory pools */

#define ETH_HLEN 14		  /* Total octets in header.				*/
#define VLAN_HLEN 4		  /* The additional bytes required by VLAN	*/
						  /* (in addition to the Ethernet header)	*/
#define ETH_FCS_LEN 4	  /* Octets in the FCS             			*/
#define ETH_DATA_LEN 1500 /* Max. octets in payload					*/

#define COMMAND_PROCESSED 1u
#define COMMAND_SCHEDULED 0u

/* Generic TODOs
cleanup mcast handling
PHY link state updates at runtime

Long shot:
- SANA-II updates to enable zero-copy DMA on TX and RX
- use checksum offload (changes in SANA-II and stack)

Not tested:
Promiscuous mode
Multicast support
*/

struct GenetDevice;

typedef enum
{
	STATE_UNCONFIGURED = 0,
	STATE_CONFIGURED,
	STATE_ONLINE,
	STATE_OFFLINE
} UnitState;

struct Opener
{
	struct MinNode node;
	struct MinList readQueue;
	struct MinList orphanQueue;
	struct MinList eventQueue;

	/* Optimized queues for common packet types */
	struct MinList ipv4Queue; /* For 0x0800 */
	struct MinList arpQueue;  /* For 0x0806 */

	struct SignalSemaphore openerSemaphore;

	/* for CMD_READ,
	 * BOOL PacketFilter(struct Hook* packetFilter asm("a0"), struct IOSana2Req* asm("a2"), APTR asm("a1"));
	 * fill in ios2_DataLength, ios2_SrcAddr, ios2_DstAddr
	 * pointer is to the buffer
	 * TRUE - send to stack; FALSE - reject
	 */
	struct Hook *packetFilter;
	/* result TRUE - success; FALSE - error */
	BOOL (*CopyToBuff)(APTR to asm("a0"), APTR from asm("a1"), ULONG len asm("d0"));
	BOOL (*CopyFromBuff)(APTR to asm("a0"), APTR from asm("a1"), ULONG len asm("d0"));
	APTR (*DMACopyToBuff)(APTR cookie asm("a0"));
	APTR (*DMACopyFromBuff)(APTR cookie asm("a0"));
};

struct MulticastRange
{
	struct MinNode node;
	u32 useCount;	/* How many openers use this range */
	u64 lowerBound; /* Inclusive */
	u64 upperBound; /* Inclusive */
};

struct bcmgenet_tx_ring
{
	struct enet_cb *tx_control_block; /* tx ring buffer control block*/
	u8 clean_ptr;					  /* Tx ring clean pointer */
	u16 tx_cons_index;				  /* last consumer index of each ring*/
	u8 write_ptr;					  /* Tx ring write pointer SW copy */
	u16 tx_prod_index;				  /* Tx ring producer index SW copy */

	struct SignalSemaphore tx_ring_sem;
};

struct bcmgenet_rx_ring
{
	struct enet_cb *rx_control_block; /* Rx ring buffer control block */
	u16 rx_cons_index;				  /* Rx last consumer index */
	u16 old_discards;
	u32 rx_max_coalesced_frames;
	u32 rx_coalesce_usecs;
};

struct enet_cb
{
	struct IOSana2Req *ioReq;
	APTR descriptor_address;
	dma_addr_t internal_buffer; /* Used when data needs to be copied from IP stack */
	dma_addr_t data_buffer;
};

struct internal_stats
{
	u32 rx_packets;		   // Sana2 PacketsReceived
	u32 rx_bytes;		   // total bytes received
	u32 rx_dropped;		   // Sana2 UnknownTypesReceived
	u32 rx_arp_ip_dropped; // included in rx_dropped
	u32 rx_overruns;	   // Sana2 Overruns
	u32 rx_other_errors;
	u32 rx_crc_errors;
	u32 rx_over_errors;
	u32 rx_frame_errors;
	u32 rx_length_errors;
	u32 rx_fragmented_errors;

	u32 tx_packets; // Sana2 PacketsSent
	u32 tx_bytes;	// total bytes transmitted
	u32 tx_dma;		// tx_dma + tx_copy = tx_packets
	u32 tx_copy;
	u32 tx_dropped; // Sana2 Overruns

	TimeVal_Type last_start;
};

struct GenetUnit
{
	struct Unit unit;
	APTR memoryPool;
	struct GenetDevice *device;

	/* config */
	u32 unitNumber;
	u32 flags;
	u8 currentMacAddress[6];
	u8 use_miami_workaround;
	u16 budget;

	/* unit/task state */
	UnitState state;
	struct Task *task;
	struct Device *timerBase;
	struct internal_stats internalStats;
	struct MinList openers;
	struct MinList multicastRanges;
	u32 multicastCount;
	BOOL mdfEnabled; /* Multicast filter enabled */

	/* Opener management (message-based modifications) */
	struct MsgPort *openerPort; /* created in unit task */

	/* Device tree */
	CONST_STRPTR compatible;
	const u8 *localMacAddress;
	APTR genetBase;
	struct tGpioRegs *gpioBase;

	/* Interrupt config and status */
	u32 irq0_number, irq1_number; /* IRQ numbers from Device Tree */
	u32 irq0_status;			  /* status bits of irq0*/
	BYTE irq0_signal;			  /* signals used to wake bottom-half */
	struct Interrupt irq0_isr;

	/* PHY */
	phy_interface_t phy_interface;
	u8 phyaddr;
	struct phy_device *phydev;

	/* MAC layer */
	/* RX */
	struct bcmgenet_rx_ring rx_ring;
	u8 *rxbuffer_not_aligned;
	dma_addr_t rxbuffer;

	/* TX */
	struct bcmgenet_tx_ring tx_ring;
	u8 *txbuffer_not_aligned;
	dma_addr_t txbuffer;
};

/* Opener management commands */
#define OPENER_CMD_ADD 1
#define OPENER_CMD_REM 2

struct OpenerControlMsg
{
	struct Message msg;
	UWORD command; /* OPENER_CMD_* */
	struct Opener *opener;
};

struct GenetDevice
{
	struct Device device;
	ULONG segList;
	struct GenetRuntimeConfig runtimeConfig;
	struct Library *utilityBase;
	struct Library *gic400Base;

	// For now, we'll just assume there can be only one unit
	struct GenetUnit *unit;
};

void beginIO(struct IOSana2Req *io asm("a1"), struct GenetDevice *base asm("a6"));
LONG abortIO(struct IOSana2Req *io asm("a1"), struct GenetDevice *base asm("a6"));

/* Unit interface */
u32 DevTreeParse(struct GenetUnit *unit);
u32 UnitTaskStart(struct GenetUnit *unit);
void UnitTaskStop(struct GenetUnit *unit);

u32 UnitOpen(struct GenetUnit *unit, u32 unitNumber, u32 flags, struct Opener *opener);
u32 UnitConfigure(struct GenetUnit *unit);
u32 UnitOnline(struct GenetUnit *unit);
void UnitOffline(struct GenetUnit *unit);
u32 UnitClose(struct GenetUnit *unit, struct Opener *opener);

BOOL ReceiveFrame(struct GenetUnit *unit, u8 *packet, u32 packetLength, u16 dma_flags);
void ProcessCommand(struct IOSana2Req *io);

/* Inline function for fast packet type queue lookup */
static inline struct MinList *GetPacketTypeQueue(struct Opener *opener, u16 packetType)
{
	switch (packetType)
	{
	case 0x0800: /* IPv4 */
		return &opener->ipv4Queue;
	case 0x0806: /* ARP */
		return &opener->arpQueue;
	default:
		return &opener->readQueue; /* Fallback to legacy port */
	}
}

u32 Do_S2_ADDMULTICASTADDRESSES(struct IOSana2Req *io);
u32 Do_S2_DELMULTICASTADDRESSES(struct IOSana2Req *io);
void ReportEvents(struct GenetUnit *unit, u32 eventSet);

#endif