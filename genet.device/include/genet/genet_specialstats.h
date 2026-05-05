// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * Vendor-private SANA-II S2_GETSPECIALSTATS record IDs for genet.device.
 *
 * The SANA-II spec encodes the upper 16 bits as the wire type (Ethernet=1)
 * and the lower 16 bits as the statistic identifier. CATS-allocated ethernet
 * IDs occupy the low end of that range. To avoid collision with future
 * official IDs, this driver uses a private base starting at 0x1000.
 *
 *   0x1000-0x10FF -- driver software counters
 *   0x1100-0x11FF -- hardware MIB counters
 */
#ifndef _GENET_SPECIALSTATS_H
#define _GENET_SPECIALSTATS_H

#include <devices/sana2.h>

#define GENET_SS_ID(n) ((((ULONG)S2WireType_Ethernet) & 0xFFFF) << 16 | (ULONG)(n))

/* Driver software counters: 0x1000-0x10FF */
#define GENET_SS_RX_OVERRUNS         GENET_SS_ID(0x1000)
#define GENET_SS_RX_CRC_ERRORS       GENET_SS_ID(0x1001)
#define GENET_SS_RX_OVER_ERRORS      GENET_SS_ID(0x1002)
#define GENET_SS_RX_FRAME_ERRORS     GENET_SS_ID(0x1003)
#define GENET_SS_RX_LENGTH_ERRORS    GENET_SS_ID(0x1004)
#define GENET_SS_RX_FRAGMENTED       GENET_SS_ID(0x1005)
#define GENET_SS_RX_OTHER_ERRORS     GENET_SS_ID(0x1006)
#define GENET_SS_RX_DROP_NO_OPENER   GENET_SS_ID(0x1007)
#define GENET_SS_RX_DROP_QUEUE_FULL  GENET_SS_ID(0x1008)
#define GENET_SS_RX_ARP_IP_DROPPED   GENET_SS_ID(0x1009)

#define GENET_SS_TX_DROPPED          GENET_SS_ID(0x1010)
#define GENET_SS_TX_DMA              GENET_SS_ID(0x1011)
#define GENET_SS_TX_COPY             GENET_SS_ID(0x1012)
/*#define GENET_SS_TX_RING_PEAK        GENET_SS_ID(0x1013)*/
#define GENET_SS_IRQ0_COUNT          GENET_SS_ID(0x1014)
#define GENET_SS_IRQ0_TX_COUNT       GENET_SS_ID(0x1015)
#define GENET_SS_IRQ0_RX_COUNT       GENET_SS_ID(0x1016)
#define GENET_SS_IRQ0_OTHER_COUNT    GENET_SS_ID(0x1017)

/* Hardware MIB RX: 0x1100-0x113F */
#define GENET_SS_HW_RX_PKTS          GENET_SS_ID(0x1100)
#define GENET_SS_HW_RX_BYTES         GENET_SS_ID(0x1101)
#define GENET_SS_HW_RX_MULTICAST     GENET_SS_ID(0x1102)
#define GENET_SS_HW_RX_BROADCAST     GENET_SS_ID(0x1103)
#define GENET_SS_HW_RX_UNICAST       GENET_SS_ID(0x1104)
#define GENET_SS_HW_RX_FCS_ERR       GENET_SS_ID(0x1105)
#define GENET_SS_HW_RX_ALIGN_ERR     GENET_SS_ID(0x1106)
#define GENET_SS_HW_RX_PAUSE         GENET_SS_ID(0x1107)
#define GENET_SS_HW_RX_OVERSIZE      GENET_SS_ID(0x1108)
#define GENET_SS_HW_RX_JABBER        GENET_SS_ID(0x1109)
#define GENET_SS_HW_RX_GOOD          GENET_SS_ID(0x110A)
#define GENET_SS_HW_RX_RUNT          GENET_SS_ID(0x110B)

/* Hardware MIB TX: 0x1140-0x117F */
#define GENET_SS_HW_TX_PKTS          GENET_SS_ID(0x1140)
#define GENET_SS_HW_TX_BYTES         GENET_SS_ID(0x1141)
#define GENET_SS_HW_TX_MULTICAST     GENET_SS_ID(0x1142)
#define GENET_SS_HW_TX_BROADCAST     GENET_SS_ID(0x1143)
#define GENET_SS_HW_TX_UNICAST       GENET_SS_ID(0x1144)
#define GENET_SS_HW_TX_FCS_ERR       GENET_SS_ID(0x1145)
#define GENET_SS_HW_TX_PAUSE         GENET_SS_ID(0x1146)
#define GENET_SS_HW_TX_SINGLE_COL    GENET_SS_ID(0x1147)
#define GENET_SS_HW_TX_MULTI_COL     GENET_SS_ID(0x1148)
#define GENET_SS_HW_TX_LATE_COL      GENET_SS_ID(0x1149)
#define GENET_SS_HW_TX_EXCESS_COL    GENET_SS_ID(0x114A)
#define GENET_SS_HW_TX_TOTAL_COL     GENET_SS_ID(0x114B)
#define GENET_SS_HW_TX_DEFER         GENET_SS_ID(0x114C)
#define GENET_SS_HW_TX_EXCESS_DEFER  GENET_SS_ID(0x114D)
#define GENET_SS_HW_TX_JABBER        GENET_SS_ID(0x114E)
#define GENET_SS_HW_TX_OVERSIZE      GENET_SS_ID(0x114F)
#define GENET_SS_HW_TX_GOOD          GENET_SS_ID(0x1150)

/* Hardware MAC misc: 0x1180-0x118F */
#define GENET_SS_HW_RBUF_OVFL        GENET_SS_ID(0x1180)
#define GENET_SS_HW_RBUF_ERR         GENET_SS_ID(0x1181)
#define GENET_SS_HW_MDF_ERR          GENET_SS_ID(0x1182)

#endif /* _GENET_SPECIALSTATS_H */
