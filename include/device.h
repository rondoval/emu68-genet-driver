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

#define LIB_MIN_VERSION 45

#define ETH_HLEN 14		  /* Total octets in header.				*/
#define VLAN_HLEN 4		  /* The additional bytes required by VLAN	*/
						  /* (in addition to the Ethernet header)	*/
#define ETH_FCS_LEN 4	  /* Octets in the FCS             			*/
#define ETH_DATA_LEN 1500 /* Max. octets in payload					*/

#define ARCH_DMA_MINALIGN 128 // TODO this is likely wrong

/* Generic TODOs
packet stats from HW
type statistics
multicasts
error reporting S2 compliant
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
};

struct GenetUnit
{
	struct Unit unit;
	struct ExecBase *execBase;
	struct TimerBase *timerBase;
	struct Library *utilityBase;

	/* config */
	LONG unitNumber;
	LONG flags;
	UBYTE currentMacAddress[6];

	/* unit/task state */
	UnitState state;
	struct Task *task;
	struct Sana2DeviceStats stats;
	struct MinList openers;
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
	UBYTE *rxbuffer_not_aligned;
	UBYTE *rxbuffer;
	APTR rx_desc_base;
	ULONG rx_cons_index;

	/* TX */
	UBYTE *txbuffer_not_aligned;
	UBYTE *txbuffer;
	APTR tx_desc_base;
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
void UnitTaskStart(struct GenetUnit *unit);
void UnitTaskStop(struct GenetUnit *unit);

int UnitOpen(struct GenetUnit *unit, LONG unitNumber, LONG flags, struct Opener *opener);
int UnitConfigure(struct GenetUnit *unit);
int UnitOnline(struct GenetUnit *unit);
void UnitOffline(struct GenetUnit *unit);
int UnitClose(struct GenetUnit *unit, struct Opener *opener);

int SendFrame(struct GenetUnit *unit, struct IOSana2Req *io);
void ReceiveFrame(struct GenetUnit *unit, UBYTE *packet, ULONG packetLength);
void ProcessCommand(struct IOSana2Req *io);

#endif