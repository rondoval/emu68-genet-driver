// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom GENET (Gigabit Ethernet) controller driver
 *
 * Copyright (c) 2014-2025 Broadcom
 */

/* Register definitions derived from Linux source */
#ifndef _BCMGENET_REGS_H
#define _BCMGENET_REGS_H

#include <bits.h>
#include <types.h>

#define SYS_REV_CTRL 0x00

#define SYS_PORT_CTRL 0x04
#define PORT_MODE_EXT_GPHY 3

#define GENET_SYS_OFF 0x0000
#define SYS_RBUF_FLUSH_CTRL (GENET_SYS_OFF + 0x08)
#define SYS_TBUF_FLUSH_CTRL (GENET_SYS_OFF + 0x0c)

#define GENET_EXT_OFF 0x0080
#define EXT_RGMII_OOB_CTRL (GENET_EXT_OFF + 0x0c)
#define RGMII_LINK BIT(4)
#define OOB_DISABLE BIT(5)
#define RGMII_MODE_EN BIT(6)
#define ID_MODE_DIS BIT(16)

#define GENET_RBUF_OFF 0x0300
#define RBUF_TBUF_SIZE_CTRL (GENET_RBUF_OFF + 0xb4)
#define RBUF_CTRL (GENET_RBUF_OFF + 0x00)
#define RBUF_64B_EN BIT(0)
#define RBUF_ALIGN_2B BIT(1)
#define RBUF_CHK_CTRL (GENET_RBUF_OFF + 0x14)
#define RBUF_RXCHK_EN BIT(0)
#define RBUF_SKIP_FCS BIT(4)
#define RBUF_L3_PARSE_DIS BIT(5)

#define GENET_TBUF_OFF 0x0600
#define TBUF_CTRL (GENET_TBUF_OFF + 0x00)
#define TBUF_64B_EN BIT(0)

/* Energy-detect / power management, one register per buffer block. Both sit
 * outside the UniMAC, so CMD_SW_RESET leaves them at whatever the firmware or a
 * previous OS wrote. Broadcom's driver keeps the RBUF pair clear unconditionally
 * — RBUF EEE/PM breaks the GENET receive path — and only sets the TBUF pair when
 * EEE is in use, which this driver never enables. */
#define RBUF_ENERGY_CTRL (GENET_RBUF_OFF + 0x9c)
#define TBUF_ENERGY_CTRL (GENET_TBUF_OFF + 0x14)
#define ENERGY_EEE_EN BIT(0)
#define ENERGY_PM_EN BIT(1)

/* 64-byte status block, prepended to every frame when RBUF_64B_EN /
 * TBUF_64B_EN are set (GENET v5). TX: only tx_csum_info is consumed by the
 * hardware; RX: length_status mirrors the descriptor, rx_csum carries the
 * RXCHK result. Layout and bits follow the Linux bcmgenet driver.
 * The words are LITTLE-ENDIAN in DMA memory — unlike the descriptors,
 * which live behind the byte-swapping mmio_* accessors, every access
 * here needs an explicit le32(). */
struct genet_status_64
{
	u32 length_status;	/* length and peripheral status */
	u32 ext_status;		/* extended status */
	u32 rx_csum;		/* STATUS_RX_CSUM_* */
	u32 unused1[9];
	u32 tx_csum_info;	/* STATUS_TX_CSUM_* */
	u32 unused2[3];
};

#define GENET_STATUS64_LEN 64

#define STATUS_RX_CSUM_MASK 0xFFFFu		/* 1's-complement sum, network-order halfword */
#define STATUS_RX_CSUM_OK 0x10000u		/* L4 verified — L3 parser only, unused w/ RBUF_L3_PARSE_DIS */
#define STATUS_RX_CSUM_FR 0x20000u		/* fragmented — L3 parser only, unused w/ RBUF_L3_PARSE_DIS */

#define STATUS_TX_CSUM_START_MASK 0x7FFFu
#define STATUS_TX_CSUM_START_SHIFT 16
#define STATUS_TX_CSUM_PROTO_UDP 0x8000u
#define STATUS_TX_CSUM_OFFSET_MASK 0x7FFFu
#define STATUS_TX_CSUM_LV 0x80000000u

#define GENET_UMAC_OFF 0x0800
#define UMAC_MIB_CTRL (GENET_UMAC_OFF + 0x580)
#define UMAC_MAX_FRAME_LEN (GENET_UMAC_OFF + 0x014)
#define UMAC_MAC0 (GENET_UMAC_OFF + 0x00c)
#define UMAC_MAC1 (GENET_UMAC_OFF + 0x010)
#define UMAC_CMD (GENET_UMAC_OFF + 0x008)
#define UMAC_TX_FLUSH (GENET_UMAC_OFF + 0x334)
#define UMAC_MDF_CTRL (GENET_UMAC_OFF + 0x650)
#define UMAC_MDF_ADDR (GENET_UMAC_OFF + 0x654)

/* MDF exact-match filter: 17 slots of two registers each, enabled from the top
 * of UMAC_MDF_CTRL down (slot k = bit MAX_MDF_FILTER-1-k). Two are permanently
 * spent on broadcast and the station address; the rest hold multicast. */
#define MAX_MDF_FILTER 17
#define GENET_MDF_MCAST_MAX (MAX_MDF_FILTER - 2)

/* MDIO controller (BCM2711). Command, status and data all share one register:
 * a transaction is started by setting MDIO_START_BUSY and is complete when the
 * hardware clears it. */
#define MDIO_CMD (GENET_UMAC_OFF + 0x614)
#define MDIO_START_BUSY BIT(29)
#define MDIO_READ_FAIL BIT(28)
#define MDIO_RD (2U << 26)
#define MDIO_WR BIT(26)
#define MDIO_PMD_SHIFT 21
#define MDIO_PMD_MASK 0x1fU
#define MDIO_REG_SHIFT 16
#define MDIO_REG_MASK 0x1fU

/* A clause-22 transaction on this controller takes ~25 us; the poll waits that
 * long before its first read. The timeout only has to cover a wedged bus. */
#define MDIO_C22_XFER_US 30U
#define MDIO_TIMEOUT_US 20000U

#define MIB_RESET_RX BIT(0)
#define MIB_RESET_RUNT BIT(1)
#define MIB_RESET_TX BIT(2)

/*
 * MIB counter block. Byte offsets are relative to genetBase (absolute).
 *
 * Three blocks, each opening with the same 10 packet-size buckets: RX at
 * +0x000, TX at +0x080, RUNT at +0x100. Linux reaches them as
 * UMAC_MIB_START + j + n * BCMGENET_STAT_OFFSET (0xc) — same layout,
 * spelled out here. Order below follows struct bcmgenet_rx_counters /
 * bcmgenet_tx_counters in the upstream bcmgenet.h so the two diff cleanly.
 *
 * All are free-running 32-bit and wrap; MIB_RESET_* zeroes the three blocks.
 */
#define UMAC_MIB_BASE       (GENET_UMAC_OFF + 0x400)

/* RX size buckets (struct bcmgenet_pkt_counters) — frames received per length
 * band, inclusive, counted whether good or bad. A traffic histogram: bulk
 * transfers pile into 1024-1518, bare ACKs into 64. */
#define UMAC_MIB_RX_CNT_64   (UMAC_MIB_BASE + 0x000) /* 64 octets exactly */
#define UMAC_MIB_RX_CNT_127  (UMAC_MIB_BASE + 0x004) /* 65-127 octets */
#define UMAC_MIB_RX_CNT_255  (UMAC_MIB_BASE + 0x008) /* 128-255 octets */
#define UMAC_MIB_RX_CNT_511  (UMAC_MIB_BASE + 0x00C) /* 256-511 octets */
#define UMAC_MIB_RX_CNT_1023 (UMAC_MIB_BASE + 0x010) /* 512-1023 octets */
#define UMAC_MIB_RX_CNT_1518 (UMAC_MIB_BASE + 0x014) /* 1024-1518 octets (to MTU) */
#define UMAC_MIB_RX_CNT_MGV  (UMAC_MIB_BASE + 0x018) /* MGV: 1519-1522, good VLAN-tagged */
#define UMAC_MIB_RX_CNT_2047 (UMAC_MIB_BASE + 0x01C) /* 1523-2047 octets */
#define UMAC_MIB_RX_CNT_4095 (UMAC_MIB_BASE + 0x020) /* 2048-4095 octets */
#define UMAC_MIB_RX_CNT_9216 (UMAC_MIB_BASE + 0x024) /* 4096-9216 octets (jumbo) */

/* RX named counters (RSV, Receive Status Vector — struct bcmgenet_rx_counters) */
#define UMAC_MIB_RX_PKT     (UMAC_MIB_BASE + 0x028) /* frames received, good or bad */
#define UMAC_MIB_RX_BYTES   (UMAC_MIB_BASE + 0x02C) /* octets received */
#define UMAC_MIB_RX_MCA     (UMAC_MIB_BASE + 0x030) /* MCA: multicast-addressed */
#define UMAC_MIB_RX_BCA     (UMAC_MIB_BASE + 0x034) /* BCA: broadcast-addressed */
#define UMAC_MIB_RX_FCS     (UMAC_MIB_BASE + 0x038) /* FCS: frame check sequence (CRC) bad */
#define UMAC_MIB_RX_CF      (UMAC_MIB_BASE + 0x03C) /* CF: MAC control frames */
#define UMAC_MIB_RX_PF      (UMAC_MIB_BASE + 0x040) /* PF: PAUSE frames (802.3x flow control) */
#define UMAC_MIB_RX_UO      (UMAC_MIB_BASE + 0x044) /* UO: control frame, unknown opcode */
#define UMAC_MIB_RX_ALN     (UMAC_MIB_BASE + 0x048) /* ALN: alignment — not a whole octet count */
#define UMAC_MIB_RX_FLR     (UMAC_MIB_BASE + 0x04C) /* FLR: frame length field out of range */
#define UMAC_MIB_RX_CDE     (UMAC_MIB_BASE + 0x050) /* CDE: code error — invalid PHY symbol */
#define UMAC_MIB_RX_FCR     (UMAC_MIB_BASE + 0x054) /* FCR: false carrier / carrier sense error */
#define UMAC_MIB_RX_OVR     (UMAC_MIB_BASE + 0x058) /* OVR: oversize, over max frame len, FCS ok */
#define UMAC_MIB_RX_JBR     (UMAC_MIB_BASE + 0x05C) /* JBR: jabber — oversize AND bad FCS */
#define UMAC_MIB_RX_MTUE    (UMAC_MIB_BASE + 0x060) /* MTUE: over UMAC_MAX_FRAME_LEN */
#define UMAC_MIB_RX_POK     (UMAC_MIB_BASE + 0x064) /* POK: packets OK — received without error */
#define UMAC_MIB_RX_UC      (UMAC_MIB_BASE + 0x068) /* UC: unicast-addressed */
#define UMAC_MIB_RX_PPP     (UMAC_MIB_BASE + 0x06C) /* PPP: PPP-over-Ethernet frames */
#define UMAC_MIB_RX_RCRC    (UMAC_MIB_BASE + 0x070) /* RCRC: frames whose CRC matched */

/* TX size buckets — same bands as RX, transmit side. Block starts at +0x080. */
#define UMAC_MIB_TX_CNT_64   (UMAC_MIB_BASE + 0x080) /* 64 octets exactly */
#define UMAC_MIB_TX_CNT_127  (UMAC_MIB_BASE + 0x084) /* 65-127 octets */
#define UMAC_MIB_TX_CNT_255  (UMAC_MIB_BASE + 0x088) /* 128-255 octets */
#define UMAC_MIB_TX_CNT_511  (UMAC_MIB_BASE + 0x08C) /* 256-511 octets */
#define UMAC_MIB_TX_CNT_1023 (UMAC_MIB_BASE + 0x090) /* 512-1023 octets */
#define UMAC_MIB_TX_CNT_1518 (UMAC_MIB_BASE + 0x094) /* 1024-1518 octets (to MTU) */
#define UMAC_MIB_TX_CNT_MGV  (UMAC_MIB_BASE + 0x098) /* MGV: 1519-1522, good VLAN-tagged */
#define UMAC_MIB_TX_CNT_2047 (UMAC_MIB_BASE + 0x09C) /* 1523-2047 octets */
#define UMAC_MIB_TX_CNT_4095 (UMAC_MIB_BASE + 0x0A0) /* 2048-4095 octets */
#define UMAC_MIB_TX_CNT_9216 (UMAC_MIB_BASE + 0x0A4) /* 4096-9216 octets (jumbo) */

/* TX named counters (TSV, Transmit Status Vector — struct bcmgenet_tx_counters).
 * The collision and deferral counters only move on a half-duplex link; on a
 * healthy full-duplex link every one of them should stay at zero. */
#define UMAC_MIB_TX_PKTS    (UMAC_MIB_BASE + 0x0A8) /* frames transmitted, good or bad */
#define UMAC_MIB_TX_MCA     (UMAC_MIB_BASE + 0x0AC) /* MCA: multicast-addressed */
#define UMAC_MIB_TX_BCA     (UMAC_MIB_BASE + 0x0B0) /* BCA: broadcast-addressed */
#define UMAC_MIB_TX_PF      (UMAC_MIB_BASE + 0x0B4) /* PF: PAUSE frames we sent */
#define UMAC_MIB_TX_CF      (UMAC_MIB_BASE + 0x0B8) /* CF: MAC control frames */
#define UMAC_MIB_TX_FCS     (UMAC_MIB_BASE + 0x0BC) /* FCS: frame check sequence errors */
#define UMAC_MIB_TX_OVR     (UMAC_MIB_BASE + 0x0C0) /* OVR: oversize, over max frame len */
#define UMAC_MIB_TX_DRF     (UMAC_MIB_BASE + 0x0C4) /* DRF: deferred — medium busy on first try */
#define UMAC_MIB_TX_EDF     (UMAC_MIB_BASE + 0x0C8) /* EDF: excessive deferral — gave up waiting */
#define UMAC_MIB_TX_SCL     (UMAC_MIB_BASE + 0x0CC) /* SCL: single collision, then sent */
#define UMAC_MIB_TX_MCL     (UMAC_MIB_BASE + 0x0D0) /* MCL: multiple (2-15) collisions, then sent */
#define UMAC_MIB_TX_LCL     (UMAC_MIB_BASE + 0x0D4) /* LCL: late collision, past the 512-bit slot
                                                     * time — duplex mismatch or over-long cable */
#define UMAC_MIB_TX_ECL     (UMAC_MIB_BASE + 0x0D8) /* ECL: excessive collisions (16), frame lost */
#define UMAC_MIB_TX_FRG     (UMAC_MIB_BASE + 0x0DC) /* FRG: fragments — undersize with bad FCS */
#define UMAC_MIB_TX_NCL     (UMAC_MIB_BASE + 0x0E0) /* NCL: number of collisions, running total */
#define UMAC_MIB_TX_JBR     (UMAC_MIB_BASE + 0x0E4) /* JBR: jabber — oversize AND bad FCS */
#define UMAC_MIB_TX_BYTES   (UMAC_MIB_BASE + 0x0E8) /* octets transmitted */
#define UMAC_MIB_TX_POK     (UMAC_MIB_BASE + 0x0EC) /* POK: packets OK — reached the wire intact */
#define UMAC_MIB_TX_UC      (UMAC_MIB_BASE + 0x0F0) /* UC: unicast-addressed */

/* RUNT block at +0x100. A runt is a frame shorter than the 64-octet minimum;
 * a valid-FCS one is a legitimate truncation upstream, a bad-FCS one is a
 * collision fragment or a wiring fault. */
#define UMAC_MIB_RX_RUNT           (UMAC_MIB_BASE + 0x100) /* runt frames received */
#define UMAC_MIB_RX_RUNT_FCS       (UMAC_MIB_BASE + 0x104) /* ...of those, FCS valid */
#define UMAC_MIB_RX_RUNT_FCS_ALIGN (UMAC_MIB_BASE + 0x108) /* ...FCS invalid or misaligned */
#define UMAC_MIB_RX_RUNT_BYTES     (UMAC_MIB_BASE + 0x10C) /* octets in runt frames */

/* Misc MAC counters. These sit outside the MIB block, are missed by
 * MIB_RESET_*, and saturate at ~0 rather than wrapping (write 0 to rearm).
 * RBUF pair uses the V3+ offsets — v1/v2 put them elsewhere. */
#define UMAC_RBUF_OVFL_CNT  (GENET_RBUF_OFF + 0x94)  /* RBUF: RX FIFO overflowed, frame lost
                                                      * before the DMA ring ever saw it */
#define UMAC_RBUF_ERR_CNT   (GENET_RBUF_OFF + 0x98)  /* RBUF internal errors */
#define UMAC_MDF_ERR_CNT    (GENET_UMAC_OFF + 0x638) /* MDF: multicast destination filter errors */

/* total number of Buffer Descriptors, same for Rx/Tx */
#define TOTAL_DESCS 256
#define RX_DESCS TOTAL_DESCS
#define TX_DESCS TOTAL_DESCS

#define DEFAULT_Q 0x10

/* Ethernet framing. Here rather than in device.h because ENET_MAX_MTU_SIZE
 * below is built from them: the register map has to stand on its own. */
#define ETH_HLEN 14		  /* Total octets in header					*/
#define VLAN_HLEN 4		  /* The additional bytes required by VLAN	*/
						  /* (in addition to the Ethernet header)	*/
#define ETH_FCS_LEN 4	  /* Octets in the FCS						*/
#define ETH_DATA_LEN 1500 /* Max. octets in payload					*/

/* Body(1500) + EH_SIZE(14) + VLANTAG(4) + BRCMTAG(6) + FCS(4) = 1528.
 * 1536 is multiple of 256 bytes
 */
#define ENET_BRCM_TAG_LEN 6
#define ENET_PAD 8
#define ENET_MAX_MTU_SIZE (ETH_DATA_LEN + ETH_HLEN +       \
						   VLAN_HLEN + ENET_BRCM_TAG_LEN + \
						   ETH_FCS_LEN + ENET_PAD)

/* Tx/Rx Dma Descriptor common bits */
#define DMA_EN BIT(0)
#define DMA_RING_BUF_EN_SHIFT 0x01
#define DMA_RING_BUF_EN_MASK 0xffffU
#define DMA_BUFLENGTH_MASK 0x0fffU
#define DMA_BUFLENGTH_SHIFT 16
#define DMA_RING_SIZE_SHIFT 16
#define DMA_OWN 0x8000U
#define DMA_EOP 0x4000U
#define DMA_SOP 0x2000U
#define DMA_WRAP 0x1000U
#define DMA_MAX_BURST_LENGTH 0x8U

/* Tx specific DMA descriptor bits */
#define DMA_TX_UNDERRUN 0x0200U
#define DMA_TX_APPEND_CRC 0x0040U
#define DMA_TX_OW_CRC 0x0020U
#define DMA_TX_DO_CSUM 0x0010U
#define DMA_TX_QTAG_SHIFT 7

/* DMA registers common definitions */
#define DMA_RW_POINTER_MASK 0x1FFU
#define DMA_P_INDEX_DISCARD_CNT_MASK 0xFFFFU
#define DMA_P_INDEX_DISCARD_CNT_SHIFT 16
#define DMA_BUFFER_DONE_CNT_MASK 0xFFFFU
#define DMA_BUFFER_DONE_CNT_SHIFT 16
#define DMA_P_INDEX_MASK 0xFFFFU
#define DMA_C_INDEX_MASK 0xFFFFU

/* DMA rings size */
#define DMA_RING_SIZE 0x40U
#define DMA_RINGS_SIZE (DMA_RING_SIZE * (DEFAULT_Q + 1U))

/* DMA descriptor */
#define DMA_DESC_LENGTH_STATUS 0x00
#define DMA_DESC_ADDRESS_LO 0x04
#define DMA_DESC_ADDRESS_HI 0x08
#define DMA_DESC_SIZE 12

#define GENET_RX_OFF 0x2000
#define GENET_RDMA_REG_OFF (GENET_RX_OFF + TOTAL_DESCS * DMA_DESC_SIZE)
#define GENET_TX_OFF 0x4000
#define GENET_TDMA_REG_OFF (GENET_TX_OFF + TOTAL_DESCS * DMA_DESC_SIZE)

#define DMA_FC_THRESH_HI (RX_DESCS >> 4)
#define DMA_FC_THRESH_LO 5
#define DMA_FC_THRESH_VALUE ((DMA_FC_THRESH_LO << 16) | DMA_FC_THRESH_HI)

#define DMA_XOFF_THRESHOLD_SHIFT 16

/* RDMA/TDMA ring registers and accessors
 * we merge the common fields and just prefix with T/D the registers
 * having different meaning depending on the direction
 */
#define TDMA_RING_REG_BASE \
	(GENET_TDMA_REG_OFF + DEFAULT_Q * DMA_RING_SIZE)
#define TDMA_READ_PTR (TDMA_RING_REG_BASE + 0x00)
#define TDMA_CONS_INDEX (TDMA_RING_REG_BASE + 0x08)
#define TDMA_PROD_INDEX (TDMA_RING_REG_BASE + 0x0c)
#define DMA_RING_BUF_SIZE 0x10
#define DMA_START_ADDR 0x14
#define DMA_END_ADDR 0x1c
#define DMA_MBUF_DONE_THRESH 0x24
#define TDMA_FLOW_PERIOD (TDMA_RING_REG_BASE + 0x28)
#define TDMA_WRITE_PTR (TDMA_RING_REG_BASE + 0x2c)

#define RDMA_RING_REG_BASE \
	(GENET_RDMA_REG_OFF + DEFAULT_Q * DMA_RING_SIZE)
#define RDMA_WRITE_PTR (RDMA_RING_REG_BASE + 0x00)
#define RDMA_PROD_INDEX (RDMA_RING_REG_BASE + 0x08)
#define RDMA_CONS_INDEX (RDMA_RING_REG_BASE + 0x0c)
#define RDMA_XON_XOFF_THRESH (RDMA_RING_REG_BASE + 0x28)
#define RDMA_READ_PTR (RDMA_RING_REG_BASE + 0x2c)

#define TDMA_REG_BASE (GENET_TDMA_REG_OFF + DMA_RINGS_SIZE)
#define RDMA_REG_BASE (GENET_RDMA_REG_OFF + DMA_RINGS_SIZE)
#define DMA_RING_CFG 0x00
#define DMA_CTRL 0x04
#define DMA_STATUS 0x08
#define DMA_SCB_BURST_SIZE 0x0C
#define DMA_ARB_CTRL 0x2C
#define DMA_PRIORITY_0 0x30
#define DMA_PRIORITY_1 0x34
#define DMA_PRIORITY_2 0x38

#define DMA_RING0_TIMEOUT 0x2C
#define DMA_RING1_TIMEOUT 0x30
#define DMA_RING2_TIMEOUT 0x34
#define DMA_RING3_TIMEOUT 0x38
#define DMA_RING4_TIMEOUT 0x3c
#define DMA_RING5_TIMEOUT 0x40
#define DMA_RING6_TIMEOUT 0x44
#define DMA_RING7_TIMEOUT 0x48
#define DMA_RING8_TIMEOUT 0x4c
#define DMA_RING9_TIMEOUT 0x50
#define DMA_RING10_TIMEOUT 0x54
#define DMA_RING11_TIMEOUT 0x58
#define DMA_RING12_TIMEOUT 0x5c
#define DMA_RING13_TIMEOUT 0x60
#define DMA_RING14_TIMEOUT 0x64
#define DMA_RING15_TIMEOUT 0x68
#define DMA_RING16_TIMEOUT 0x6C
#define DMA_INDEX2RING_0 0x70
#define DMA_INDEX2RING_1 0x74
#define DMA_INDEX2RING_2 0x78
#define DMA_INDEX2RING_3 0x7C
#define DMA_INDEX2RING_4 0x80
#define DMA_INDEX2RING_5 0x84
#define DMA_INDEX2RING_6 0x88
#define DMA_INDEX2RING_7 0x8C

/* DMA timeout register */
#define DMA_TIMEOUT_MASK 0xFFFFU
#define DMA_TIMEOUT_VAL 5000U /* micro seconds */

#define DMA_ARBITER_RR 0x00
#define DMA_ARBITER_WRR 0x01
#define DMA_ARBITER_SP 0x02

/* DMA interrupt threshold register */
#define DMA_INTR_THRESHOLD_MASK 0x01FFU

#define RX_BUF_LENGTH 2048

/* Rx Specific Dma descriptor bits */
#define DMA_RX_CHK_V3PLUS 0x8000U
#define DMA_RX_CHK_V12 0x1000U
#define DMA_RX_BRDCAST 0x0040U
#define DMA_RX_MULT 0x0020U
#define DMA_RX_LG 0x0010U
#define DMA_RX_NO 0x0008U
#define DMA_RX_RXER 0x0004U
#define DMA_RX_CRC_ERROR 0x0002U
#define DMA_RX_OV 0x0001U
#define DMA_RX_FI_MASK 0x001FU
#define DMA_RX_FI_SHIFT 0x0007U
#define DMA_DESC_ALLOC_MASK 0x00FFU

/* INTRL2 register block offsets */
#define GENET_INTRL2_0_OFF 0x0200
#define GENET_INTRL2_1_OFF 0x0240

/* uniMac intrl2 registers */
#define INTRL2_CPU_STAT 0x00
#define INTRL2_CPU_SET 0x04
#define INTRL2_CPU_CLEAR 0x08
#define INTRL2_CPU_MASK_STATUS 0x0C
#define INTRL2_CPU_MASK_SET 0x10
#define INTRL2_CPU_MASK_CLEAR 0x14

/* INTRL2 instance 0 definitions */
#define UMAC_IRQ_SCB BIT(0)
#define UMAC_IRQ_EPHY BIT(1)
#define UMAC_IRQ_PHY_DET_R BIT(2)
#define UMAC_IRQ_PHY_DET_F BIT(3)
#define UMAC_IRQ_LINK_UP BIT(4)
#define UMAC_IRQ_LINK_DOWN BIT(5)
#define UMAC_IRQ_LINK_EVENT (UMAC_IRQ_LINK_UP | UMAC_IRQ_LINK_DOWN)
#define UMAC_IRQ_UMAC BIT(6)
#define UMAC_IRQ_UMAC_TSV BIT(7)
#define UMAC_IRQ_TBUF_UNDERRUN BIT(8)
#define UMAC_IRQ_RBUF_OVERFLOW BIT(9)
#define UMAC_IRQ_HFB_SM BIT(10)
#define UMAC_IRQ_HFB_MM BIT(11)
#define UMAC_IRQ_MPD_R BIT(12)
#define UMAC_IRQ_WAKE_EVENT (UMAC_IRQ_HFB_SM | UMAC_IRQ_HFB_MM | \
							 UMAC_IRQ_MPD_R)
#define UMAC_IRQ_RXDMA_MBDONE BIT(13)
#define UMAC_IRQ_RXDMA_PDONE BIT(14)
#define UMAC_IRQ_RXDMA_BDONE BIT(15)
#define UMAC_IRQ_RXDMA_DONE UMAC_IRQ_RXDMA_MBDONE
#define UMAC_IRQ_TXDMA_MBDONE BIT(16)
#define UMAC_IRQ_TXDMA_PDONE BIT(17)
#define UMAC_IRQ_TXDMA_BDONE BIT(18)
#define UMAC_IRQ_TXDMA_DONE UMAC_IRQ_TXDMA_MBDONE

/* Only valid for GENETv3+ */
#define UMAC_IRQ_MDIO_DONE BIT(23)
#define UMAC_IRQ_MDIO_ERROR BIT(24)
#define UMAC_IRQ_MDIO_EVENT (UMAC_IRQ_MDIO_DONE | \
							 UMAC_IRQ_MDIO_ERROR)

/* INTRL2 instance 1 definitions */
#define UMAC_IRQ1_TX_INTR_MASK 0xFFFFU
#define UMAC_IRQ1_RX_INTR_MASK 0xFFFFU
#define UMAC_IRQ1_RX_INTR_SHIFT 16

#define GENMASK(h, l) \
	(((~0UL) << (l)) & (~0UL >> (sizeof(ULONG) * CHAR_BIT - 1 - (h))))

#define GENET_QTAG_MASK 0x3FU

#endif