// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom GENET (Gigabit Ethernet) controller driver
 *
 * Copyright (c) 2014-2025 Broadcom
 */
#define __NOLIBBASE__

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <device.h>
#include <bcmgenet.h>
#include <bcmgenet-regs.h>
#include <compat.h>
#include <debug.h>

/* Combined address + length/status setter */
static inline void dmadesc_set(APTR descriptor_address, APTR addr, ULONG val)
{
	writel((ULONG)addr, descriptor_address + DMA_DESC_ADDRESS_LO);
	writel(val, descriptor_address + DMA_DESC_LENGTH_STATUS);
}

static inline struct enet_cb *bcmgenet_get_txcb(struct GenetUnit *priv,
												struct bcmgenet_tx_ring *ring)
{
	struct enet_cb *tx_cb_ptr;

	tx_cb_ptr = ring->tx_control_block;
	tx_cb_ptr += ring->write_ptr;
	KprintfH("[genet] %s: tx_cb_ptr %lx, write_ptr %ld\n", __func__, tx_cb_ptr, ring->write_ptr);

	/* Advancing local write pointer */
	ring->write_ptr++;

	return tx_cb_ptr;
}

/* Simple helper to free a transmit control block's resources
 * Returns an skb when the last transmit control block associated with the
 * skb is freed.  The skb should be freed by the caller if necessary.
 */
static inline struct IOSana2Req *bcmgenet_free_tx_cb(struct enet_cb *cb)
{
	struct IOSana2Req *ioReq = cb->ioReq;

	if (ioReq)
	{
		cb->ioReq = NULL;
		return ioReq;
	}
	return NULL;
}

/* Unlocked version of the reclaim routine */
static void bcmgenet_tx_reclaim(struct GenetUnit *priv)
{
	KprintfH("[genet] %s: Reclaiming TX buffers\n", __func__);
	struct ExecBase *SysBase = priv->execBase;
	struct bcmgenet_tx_ring *ring = &priv->tx_ring;
	/* Compute how many buffers are transmitted since last xmit call */
	UWORD tx_cons_index = readl((ULONG)priv->genetBase + TDMA_CONS_INDEX) & DMA_C_INDEX_MASK;
	UWORD txbds_ready = (tx_cons_index - ring->tx_cons_index) & DMA_C_INDEX_MASK;

	/* Reclaim transmitted buffers */
	UWORD txbds_processed = 0;
	ULONG bytes_compl = 0;
	UWORD pkts_compl = 0;
	KprintfH("[genet] %s: clean_ptr %ld, tx_cons_index %ld, txbds_ready %ld\n", __func__, ring->clean_ptr, ring->tx_cons_index, txbds_ready);
	while (txbds_processed < txbds_ready)
	{
		struct IOSana2Req *io = bcmgenet_free_tx_cb(&ring->tx_control_block[ring->clean_ptr]);
		if (io)
		{
			pkts_compl++;
			bytes_compl += io->ios2_DataLength;
			KprintfH("[genet] %s: Reclaimed tx buffer %lx, length %ld\n", __func__, io, io->ios2_DataLength);
			ReplyMsg((struct Message *)io);
		}

		txbds_processed++;
		ring->clean_ptr++;
	}

	ring->free_bds += txbds_processed;
	ring->tx_cons_index = tx_cons_index;
	KprintfH("[genet] %s: tx_cons_index %ld, clean_ptr %ld, free_bds %ld\n",
			 __func__, ring->tx_cons_index, ring->clean_ptr, ring->free_bds);

	priv->stats.PacketsSent += pkts_compl;
	priv->internalStats.tx_packets += pkts_compl;
	priv->internalStats.tx_bytes += bytes_compl;

	// Print every time we cross a multiple of 5000 PacketsSent
	static ULONG last_printed = 0;
	ULONG packets = priv->stats.PacketsSent;
	if (packets - last_printed >= 5000)
	{
		last_printed = packets;
		// print all internalStats tx_
		KprintfH("[genet] %s: tx_packets %ld, tx_dma %ld, tx_copy %ld, tx_bytes %ld, tx_dropped %ld\n",
				 __func__, priv->internalStats.tx_packets,
				 priv->internalStats.tx_dma,
				 priv->internalStats.tx_copy,
				 priv->internalStats.tx_bytes,
				 priv->internalStats.tx_dropped);
	}
}

static int bcmgenet_xmit(struct IOSana2Req *io, struct GenetUnit *unit)
{
	KprintfH("[genet] %s: unit %ld, io %lx, flags %lx\n", __func__, unit->unitNumber, io, io->ios2_Req.io_Flags);
	struct ExecBase *SysBase = unit->execBase;
	struct Opener *opener = io->ios2_BufferManagement;
	struct bcmgenet_tx_ring *ring = &unit->tx_ring;

	UBYTE bds_required = (io->ios2_Req.io_Flags & SANA2IOF_RAW) ? 1 : 2;
	if (unlikely(ring->free_bds <= bds_required))
	{
		KprintfH("[genet] %s: Not enough free BDs\n", __func__);
		unit->internalStats.tx_dropped++;
		io->ios2_WireError = S2WERR_BUFF_ERROR;
		io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
		ReportEvents(unit, S2EVENT_BUFF | S2EVENT_TX | S2EVENT_SOFTWARE | S2EVENT_ERROR);
		return COMMAND_PROCESSED;
	}

	if (unlikely(io->ios2_DataLength == 0))
	{
		// TODO handle a case where DMA is not available - use copy
		KprintfH("[genet] %s: No data to send\n", __func__);
		unit->internalStats.tx_dropped++;
		io->ios2_WireError = S2WERR_BUFF_ERROR;
		io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
		ReportEvents(unit, S2EVENT_BUFF | S2EVENT_TX | S2EVENT_SOFTWARE | S2EVENT_ERROR);
		return COMMAND_PROCESSED;
	}

	if (likely((io->ios2_Req.io_Flags & SANA2IOF_RAW) == 0))
	{
		KprintfH("[genet] %s: adding ethernet header\n", __func__);

		struct enet_cb *tx_cb_ptr = bcmgenet_get_txcb(unit, ring);
		UBYTE *ptr = (UBYTE *)tx_cb_ptr->internal_buffer;
		tx_cb_ptr->data_buffer = NULL;
		tx_cb_ptr->ioReq = NULL;

		for (int i = 0; i < 6; i++)
			ptr[i] = io->ios2_DstAddr[i];

		for (int i = 0; i < 6; i++)
			ptr[6 + i] = io->ios2_SrcAddr[i];

		*(UWORD *)&ptr[12] = io->ios2_PacketType;

		ULONG len_stat = (ETH_HLEN << DMA_BUFLENGTH_SHIFT) | (GENET_QTAG_MASK << DMA_TX_QTAG_SHIFT);
		/* Note: if we ever change from DMA_TX_APPEND_CRC below we
		 * will need to restore software padding of "runt" packets
		 */
		len_stat |= DMA_TX_APPEND_CRC;
		len_stat |= DMA_SOP;

		dmadesc_set(tx_cb_ptr->descriptor_address, tx_cb_ptr->internal_buffer, len_stat);

		ULONG len = ETH_HLEN;
		CachePreDMA(tx_cb_ptr->internal_buffer, &len, DMA_ReadFromRAM);

		/* Decrement total BD count and advance our write pointer */
		ring->free_bds--;
		ring->tx_prod_index++;
		ring->tx_prod_index &= DMA_P_INDEX_MASK;
	}

	// Then the body from upstream
	struct enet_cb *tx_cb_ptr = bcmgenet_get_txcb(unit, ring);
	tx_cb_ptr->ioReq = io;

	if (likely(opener->DMACopyFromBuff && (tx_cb_ptr->data_buffer = (APTR)opener->DMACopyFromBuff(io->ios2_Data)) != NULL))
	{
		KprintfH("[genet] %s: Using DMA copy from buffer\n", __func__);
		unit->internalStats.tx_dma++;
	}
	else
	{
		KprintfH("[genet] %s: Using software copy from buffer\n", __func__);
		if (!opener->CopyFromBuff || opener->CopyFromBuff(tx_cb_ptr->internal_buffer, io->ios2_Data, io->ios2_DataLength) == 0)
		{
			KprintfH("[genet] %s: Failed to copy packet data from buffer\n", __func__);
			io->ios2_WireError = S2WERR_BUFF_ERROR;
			io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
			ReportEvents(unit, S2EVENT_BUFF | S2EVENT_TX | S2EVENT_SOFTWARE | S2EVENT_ERROR);
			return COMMAND_PROCESSED;
		}
		tx_cb_ptr->data_buffer = tx_cb_ptr->internal_buffer;
		unit->internalStats.tx_copy++;
	}

	ULONG len_stat = (io->ios2_DataLength << DMA_BUFLENGTH_SHIFT) | (GENET_QTAG_MASK << DMA_TX_QTAG_SHIFT);
	/* Note: if we ever change from DMA_TX_APPEND_CRC below we
	 * will need to restore software padding of "runt" packets
	 */
	len_stat |= DMA_TX_APPEND_CRC;
	if (unlikely(io->ios2_Req.io_Flags & SANA2IOF_RAW))
	{
		len_stat |= DMA_SOP;
	}
	len_stat |= DMA_EOP;
	KprintfH("[genet] %s: Setting descriptor address %lx, data buffer %lx, len_stat %lx\n",
			 __func__, tx_cb_ptr->descriptor_address, tx_cb_ptr->data_buffer, len_stat);

	dmadesc_set(tx_cb_ptr->descriptor_address, tx_cb_ptr->data_buffer, len_stat);

	CachePreDMA(tx_cb_ptr->data_buffer, &io->ios2_DataLength, DMA_ReadFromRAM);

	/* Decrement total BD count and advance our write pointer */
	ring->free_bds--;
	ring->tx_prod_index++;
	ring->tx_prod_index &= DMA_P_INDEX_MASK;

	writel(ring->tx_prod_index, (ULONG)unit->genetBase + TDMA_PROD_INDEX);
	KprintfH("[genet] %s: Transmitting packet, tx_prod_index %ld, free_bds %ld\n",
			 __func__, ring->tx_prod_index, ring->free_bds);
	return COMMAND_SCHEDULED;
}

int bcmgenet_tx_poll(struct GenetUnit *unit, struct IOSana2Req *io)
{
	struct ExecBase *SysBase = unit->execBase;
	struct bcmgenet_tx_ring *ring = &unit->tx_ring;

	if (ring->free_bds > 2) // we usually send two fragments
	{
		return bcmgenet_xmit(io, unit);
	}

	bcmgenet_tx_reclaim(unit);
	if (ring->free_bds > 2) // we usually send two fragments
	{
		return bcmgenet_xmit(io, unit);
	}
	// Can't process right now
	PutMsg(&unit->unit.unit_MsgPort, (struct Message *)io);
	return COMMAND_SCHEDULED;
}

void bcmgenet_timeout(struct GenetUnit *unit)
{
	bcmgenet_tx_reclaim(unit);
	// TODO unit->internalStats.tx_errors++;
}
