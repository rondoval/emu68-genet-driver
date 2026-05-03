// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom GENET (Gigabit Ethernet) controller driver
 *
 * Copyright (c) 2014-2025 Broadcom
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <iomem.h>
#include <types.h>
#include <debug.h>
#include <device.h>
#include <runtime_config.h>

#include <genet/bcmgenet.h>
#include <genet/bcmgenet-regs.h>
#include <genet/bcmgenet-irq.h>

/* Combined address + length/status setter */
static inline void dmadesc_set(APTR descriptor_address, dma_addr_t addr, u32 val)
{
	mmio_write32((u32)addr, descriptor_address + DMA_DESC_ADDRESS_LO);
	mmio_write32(val, descriptor_address + DMA_DESC_LENGTH_STATUS);
}

static inline struct enet_cb *bcmgenet_get_txcb(struct bcmgenet_tx_ring *ring)
{
	struct enet_cb *tx_cb_ptr = &ring->tx_control_block[ring->write_ptr++];
	KprintfH("[genet] %s: tx_cb_ptr 0x%lx, write_ptr %ld\n", __func__, tx_cb_ptr, ring->write_ptr - 1);
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
u16 bcmgenet_tx_reclaim(struct GenetUnit *unit, u16 budget)
{
	struct bcmgenet_tx_ring *ring = &unit->tx_ring;

	if (budget == 0U)
		return 0;

	/* Compute how many buffers are transmitted since last xmit call */
	u16 tx_cons_index = mmio_read32(BCMGENET_REG(unit, TDMA_CONS_INDEX)) & DMA_C_INDEX_MASK;
	u16 txbds_ready = (u16)((u32)(tx_cons_index - ring->tx_cons_index) & DMA_C_INDEX_MASK);

	/* Reclaim transmitted buffers */
	u16 txbds_processed = 0;
	u32 bytes_compl = 0;
	u16 pkts_compl = 0;
	while (txbds_processed < txbds_ready && pkts_compl < budget)
	{
		struct IOSana2Req *io = bcmgenet_free_tx_cb(&ring->tx_control_block[ring->clean_ptr]);
		++txbds_processed;
		++ring->clean_ptr;

		if (io)
		{
			pkts_compl++;
			bytes_compl += io->ios2_DataLength;
			KprintfH("[genet] %s: Reclaimed tx buffer 0x%lx, length %ld\n", __func__, io, io->ios2_DataLength);
			ReplyMsg((struct Message *)io);
		}
	}

	ring->tx_cons_index = (u16)((u32)(ring->tx_cons_index + txbds_processed) & DMA_C_INDEX_MASK);

	unit->internalStats.tx_packets += pkts_compl;
	unit->internalStats.tx_bytes += bytes_compl;

	return pkts_compl;
}

u32 bcmgenet_xmit(struct IOSana2Req *io, struct GenetUnit *unit)
{
	KprintfH("[genet] %s: unit %lu, io 0x%lx, flags 0x%lx\n", __func__, unit->unitNumber, io, io->ios2_Req.io_Flags);
	struct Opener *opener = io->ios2_BufferManagement;
	struct bcmgenet_tx_ring *ring = &unit->tx_ring;
	ObtainSemaphore(&ring->tx_ring_sem);

	KprintfH("[genet] %s: pre: tx_prod_index %ld, write_ptr %ld\n", __func__, ring->tx_prod_index, ring->write_ptr);

	u16 free_bds = (u16)(TX_DESCS - ((u32)(ring->tx_prod_index - ring->tx_cons_index) & DMA_P_INDEX_MASK));
	u8 bds_required = (io->ios2_Req.io_Flags & SANA2IOF_RAW) ? 1 : 2;
	if (unlikely(free_bds <= bds_required))
	{
		KprintfH("[genet] %s: Not enough free BDs\n", __func__);
		goto ret_error;
	}

	if (unlikely(io->ios2_DataLength == 0))
	{
		KprintfH("[genet] %s: No data to send\n", __func__);
		goto ret_error;
	}

	if (likely((io->ios2_Req.io_Flags & SANA2IOF_RAW) == 0))
	{
		KprintfH("[genet] %s: adding ethernet header\n", __func__);

		struct enet_cb *tx_cb_ptr = bcmgenet_get_txcb(ring);
		u8 *ptr = (u8 *)tx_cb_ptr->internal_buffer;
		tx_cb_ptr->data_buffer = NULL;
		tx_cb_ptr->ioReq = NULL;

		/* Copy Ethernet header: DST (6), SRC (6), EtherType (2) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
		*(ULONG *)ptr        = *(const ULONG *)io->ios2_DstAddr;
		*(UWORD *)(ptr + 4)  = *(const UWORD *)&io->ios2_DstAddr[4];
		*(ULONG *)(ptr + 6)  = *(const ULONG *)unit->currentMacAddress;
		*(UWORD *)(ptr + 10) = *(const UWORD *)&unit->currentMacAddress[4];
		*(UWORD *)(ptr + 12) = (UWORD)io->ios2_PacketType;
#pragma GCC diagnostic pop

		u32 len_stat = (ETH_HLEN << DMA_BUFLENGTH_SHIFT) | (GENET_QTAG_MASK << DMA_TX_QTAG_SHIFT);
		/* Note: if we ever change from DMA_TX_APPEND_CRC below we
		 * will need to restore software padding of "runt" packets
		 */
		len_stat |= DMA_TX_APPEND_CRC;
		len_stat |= DMA_SOP;

		dmadesc_set(tx_cb_ptr->descriptor_address, tx_cb_ptr->internal_buffer, len_stat);

		u32 len = ETH_HLEN;
		CachePreDMA((APTR)tx_cb_ptr->internal_buffer, &len, DMA_ReadFromRAM);

		/* Advance our write pointer */
		ring->tx_prod_index++;
		ring->tx_prod_index &= DMA_P_INDEX_MASK;
		KprintfH("[genet] %s: ETH header sent type: 0x%lx dst addr: %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n", __func__, io->ios2_PacketType,
				 io->ios2_DstAddr[0], io->ios2_DstAddr[1], io->ios2_DstAddr[2],
				 io->ios2_DstAddr[3], io->ios2_DstAddr[4], io->ios2_DstAddr[5]);
	}

	// Then the body from upstream
	struct enet_cb *tx_cb_ptr = bcmgenet_get_txcb(ring);
	tx_cb_ptr->ioReq = io;
	/* We'll use the ln_Pred pointer to mark it is on the TX ring now and can't be aborted */
	io->ios2_Req.io_Message.mn_Node.ln_Pred = NULL;

	if (unlikely(opener->DMACopyFromBuff) && (tx_cb_ptr->data_buffer = (dma_addr_t)opener->DMACopyFromBuff(io->ios2_Data)) != 0)
	{
		if (unlikely(tx_cb_ptr->data_buffer <= 0x1FFFFFUL))
		{
			KprintfH("[genet] %s: Cannot use buffers in CHIP memory, falling back to copying.\n", __func__);
			// opener->DMACopyFromBuff = NULL; // Disable DMA copy
			goto use_software_copy;
		}
		KprintfH("[genet] %s: Using DMA copy from buffer 0x%lx\n", __func__, tx_cb_ptr->data_buffer);
		unit->internalStats.tx_dma++;
	}
	else
	{
	use_software_copy:
		KprintfH("[genet] %s: Using software copy from buffer\n", __func__);
		u32 copy_length = io->ios2_DataLength;
		if (unit->use_miami_workaround)
			copy_length = (copy_length + 3U) & ~3U;
		if (!opener->CopyFromBuff || opener->CopyFromBuff((APTR)tx_cb_ptr->internal_buffer, io->ios2_Data, copy_length) == 0)
		{
			KprintfH("[genet] %s: Failed to copy packet data from buffer\n", __func__);
			goto ret_error;
		}
		tx_cb_ptr->data_buffer = tx_cb_ptr->internal_buffer;
		unit->internalStats.tx_copy++;
	}

	u32 len_stat = (io->ios2_DataLength << DMA_BUFLENGTH_SHIFT) | (GENET_QTAG_MASK << DMA_TX_QTAG_SHIFT);
	/* Note: if we ever change from DMA_TX_APPEND_CRC below we
	 * will need to restore software padding of "runt" packets
	 */
	len_stat |= DMA_TX_APPEND_CRC;
	if (unlikely(io->ios2_Req.io_Flags & SANA2IOF_RAW))
	{
		len_stat |= DMA_SOP;
	}
	len_stat |= DMA_EOP;
	KprintfH("[genet] %s: Setting descriptor address 0x%lx, data buffer 0x%lx, len_stat 0x%lx\n",
			 __func__, tx_cb_ptr->descriptor_address, tx_cb_ptr->data_buffer, len_stat);

	dmadesc_set(tx_cb_ptr->descriptor_address, tx_cb_ptr->data_buffer, len_stat);

	CachePreDMA((APTR)tx_cb_ptr->data_buffer, &io->ios2_DataLength, DMA_ReadFromRAM);

	/* Advance our write pointer */
	ring->tx_prod_index++;
	ring->tx_prod_index &= DMA_P_INDEX_MASK;

	mmio_write32(ring->tx_prod_index, BCMGENET_REG(unit, TDMA_PROD_INDEX));
	KprintfH("[genet] %s: Transmitting packet, tx_prod_index %ld\n", __func__, ring->tx_prod_index);

	ReleaseSemaphore(&ring->tx_ring_sem);
	return COMMAND_SCHEDULED;

ret_error:
	unit->internalStats.tx_dropped++;
	io->ios2_WireError = S2WERR_BUFF_ERROR;
	io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
	UnitSubmitControlAsync(unit, UNIT_CTRL_EVENT_REPORT, (union UnitControlPayload){.eventSet = S2EVENT_BUFF | S2EVENT_TX | S2EVENT_SOFTWARE | S2EVENT_ERROR});
	return COMMAND_PROCESSED;
}
