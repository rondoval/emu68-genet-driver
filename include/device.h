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

#include <phy/phy.h>
#include <bcmgenet.h>

/*
 * SNPrintf - v47
 * NewMinList - v45
 */
#define LIB_MIN_VERSION 47

#define ETH_HLEN 14		  /* Total octets in header.				*/
#define VLAN_HLEN 4		  /* The additional bytes required by VLAN	*/
						  /* (in addition to the Ethernet header)	*/
#define ETH_FCS_LEN 4	  /* Octets in the FCS             			*/
#define ETH_DATA_LEN 1500 /* Max. octets in payload					*/

#define ARCH_DMA_MINALIGN 128 // TODO this is likely wrong

#define COMMAND_PROCESSED 1
#define COMMAND_SCHEDULED 0

#define ETIMEDOUT -1 // Used by PHY to report errors
#define EAGAIN -2

/* Generic TODOs
packet stats from HW
type statistics
multicasts using HFB
hardware filter block support
better ring buffers handling

Long shot:
- use checksum offload (changes in SANA-II and stack)
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
	struct MsgPort readPort;
	struct MsgPort orphanPort;
	struct MsgPort eventPort;
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
	ULONG packets;
	ULONG bytes;

	struct enet_cb *tx_control_block; /* tx ring buffer control block*/
	UBYTE clean_ptr;				  /* Tx ring clean pointer */
	ULONG tx_cons_index;			  /* last consumer index of each ring*/
	UWORD free_bds;					  /* # of free bds for each ring */
	UBYTE write_ptr;				  /* Tx ring write pointer SW copy */
	ULONG tx_prod_index;				  /* Tx ring producer index SW copy */
};

struct bcmgenet_rx_ring
{
	ULONG bytes;
	ULONG packets;
	ULONG errors;
	ULONG dropped;

	struct enet_cb *rx_control_block; /* Rx ring buffer control block */
	unsigned int rx_cons_index;		  /* Rx last consumer index */
	unsigned int read_ptr;			  /* Rx ring read pointer */
	unsigned int old_discards;
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

struct GenetUnit
{
	struct Unit unit;
	struct ExecBase *execBase;
	struct TimerBase *timerBase;
	struct Library *utilityBase;
	APTR memoryPool;

	/* config */
	LONG unitNumber;
	LONG flags;
	UBYTE currentMacAddress[6];

	/* unit/task state */
	UnitState state;
	struct Task *task;
	struct Sana2DeviceStats stats;
	struct MinList openers;
	struct MinList multicastRanges;
	ULONG multicastCount;
	struct SignalSemaphore semaphore;

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
};

struct GenetDevice
{
	struct Device device;
	struct ExecBase *execBase;
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

void ReceiveFrame(struct GenetUnit *unit, UBYTE *packet, ULONG packetLength);
void ProcessCommand(struct IOSana2Req *io);

int Do_S2_ADDMULTICASTADDRESSES(struct IOSana2Req *io);
int Do_S2_DELMULTICASTADDRESSES(struct IOSana2Req *io);
void ReportEvents(struct GenetUnit *unit, ULONG eventSet);

#endif