// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifndef _GENET_DEVICE_H
#define _GENET_DEVICE_H

#if defined(__INTELLISENSE__)
#define asm(x)
#define __attribute__(x)
#endif

#include <exec/devices.h>
#include <exec/types.h>
#include <exec/semaphores.h>
#include <devices/sana2.h>

#include <genet/phy.h>
#include <genet/bcmgenet.h>
#include <runtime_config.h>

#define LIB_MIN_VERSION 39 /* we use memory pools */

#define ETH_HLEN 14		  /* Total octets in header.				*/
#define VLAN_HLEN 4		  /* The additional bytes required by VLAN	*/
						  /* (in addition to the Ethernet header)	*/
#define ETH_FCS_LEN 4	  /* Octets in the FCS             			*/
#define ETH_DATA_LEN 1500 /* Max. octets in payload					*/

#define COMMAND_PROCESSED 1
#define COMMAND_SCHEDULED 0

/* Generic TODOs
use HW bcast/mcast flags
cleanup mcast handling
packet stats from HW, custom command to expose more stats and tool to read
type statistics
PHY link state updates at runtime
interrupts

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
	struct MinList ipv4Queue;  /* For 0x0800 */
	struct MinList arpQueue;   /* For 0x0806 */

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
	LONG useCount;		 /* How many openers use this range */
	uint64_t lowerBound; /* Inclusive */
	uint64_t upperBound; /* Inclusive */
};

struct bcmgenet_tx_ring
{
	struct enet_cb *tx_control_block; /* tx ring buffer control block*/
	UBYTE clean_ptr;				  /* Tx ring clean pointer */
	UWORD tx_cons_index;			  /* last consumer index of each ring*/
	UWORD free_bds;					  /* # of free bds for each ring */
	UBYTE write_ptr;				  /* Tx ring write pointer SW copy */
	UWORD tx_prod_index;			  /* Tx ring producer index SW copy */

	struct SignalSemaphore tx_ring_sem;
};

struct bcmgenet_rx_ring
{
	struct enet_cb *rx_control_block; /* Rx ring buffer control block */
	UWORD rx_cons_index;			  /* Rx last consumer index */
	ULONG rx_max_coalesced_frames;
	ULONG rx_coalesce_usecs;
};

struct enet_cb
{
	struct IOSana2Req *ioReq;
	APTR descriptor_address;
	APTR internal_buffer; /* Used when data needs to be copied from IP stack */
	APTR data_buffer;
};

struct internal_stats
{
	ULONG rx_packets;
	ULONG rx_bytes;
	ULONG rx_dropped;
	ULONG rx_arp_ip_dropped;
	ULONG rx_overruns;
	// ULONG rx_crc_errors;
	// ULONG rx_over_errors;
	// ULONG rx_frame_errors;
	// ULONG rx_length_errors;

	ULONG tx_packets;
	ULONG tx_bytes;
	ULONG tx_dma;
	ULONG tx_copy;
	ULONG tx_dropped;
};

struct GenetUnit
{
	struct Unit unit;
	APTR memoryPool;

	/* config */
	LONG unitNumber;
	LONG flags;
	UBYTE currentMacAddress[6];

	/* unit/task state */
	UnitState state;
	struct Task *task;
	struct Sana2DeviceStats stats;
	struct internal_stats internalStats;
	struct MinList openers;
	struct MinList multicastRanges;
	ULONG multicastCount;
	BOOL mdfEnabled; /* Multicast filter enabled */

	/* Opener management (message-based modifications) */
	struct MsgPort *openerPort; /* created in unit task */

	/* Device tree */
	CONST_STRPTR compatible;
	const UBYTE *localMacAddress;
	APTR genetBase;
	APTR gpioBase;

	/* PHY */
	phy_interface_t phy_interface;
	int phyaddr;
	struct phy_device *phydev;

	/* MAC layer */
	/* RX */
	struct bcmgenet_rx_ring rx_ring;
	UBYTE *rxbuffer_not_aligned;
	UBYTE *rxbuffer;

	/* TX */
	struct bcmgenet_tx_ring tx_ring;
	UBYTE *txbuffer_not_aligned;
	UBYTE *txbuffer;

	UWORD tx_watchdog_fast_ticks;/* remaining fast polls while data on TX ring */
};

/* Opener management commands */
#define OPENER_CMD_ADD 1
#define OPENER_CMD_REM 2

struct OpenerControlMsg {
	struct Message msg;
	UWORD command; /* OPENER_CMD_* */
	struct Opener *opener;
};

struct GenetDevice
{
	struct Device device;
	ULONG segList;

	// For now, we'll just assume there can be only one unit
	struct GenetUnit *unit;
};

/* Unit interface */
int DevTreeParse(struct GenetUnit *unit);
int UnitTaskStart(struct GenetUnit *unit);
void UnitTaskStop(struct GenetUnit *unit);

int UnitOpen(struct GenetUnit *unit, LONG unitNumber, LONG flags, struct Opener *opener);
int UnitConfigure(struct GenetUnit *unit);
int UnitOnline(struct GenetUnit *unit);
void UnitOffline(struct GenetUnit *unit);
int UnitClose(struct GenetUnit *unit, struct Opener *opener);

BOOL ReceiveFrame(struct GenetUnit *unit, UBYTE *packet, ULONG packetLength);
void ProcessCommand(struct IOSana2Req *io);

/* Inline function for fast packet type queue lookup */
static inline struct MinList* GetPacketTypeQueue(struct Opener *opener, UWORD packetType)
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

int Do_S2_ADDMULTICASTADDRESSES(struct IOSana2Req *io);
int Do_S2_DELMULTICASTADDRESSES(struct IOSana2Req *io);
void ReportEvents(struct GenetUnit *unit, ULONG eventSet);

#endif