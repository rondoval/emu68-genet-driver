// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/utility_protos.h>
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define UTILITY_BASE_NAME unit->device->utilityBase
#include <proto/utility.h>
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <genet/bcmgenet-regs.h>
#include <device.h>
#include <types.h>
#include <debug.h>
#include <runtime_config.h>

static inline void CopyPacket(struct IOSana2Req *io, u8 *packet, u32 packetLength, u16 dma_flags)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    KprintfT("[genet] %s: Copying packet of length %lu\n", __func__, (ULONG)packetLength);
    struct Opener *opener = io->ios2_BufferManagement;

    /* Copy Ethernet header: DST (6), SRC (6), EtherType (2)
     * packet is rxbuffer+RX_BUF_OFFSET(+2): +0 UWORD-aligned, +2/+6/+10 ULONG-aligned
     * ios2_DstAddr/SrcAddr are at 4-byte-aligned offsets in IOSana2Req
     */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
    *(UWORD *)io->ios2_DstAddr = *(const UWORD *)packet;
    *(ULONG *)(io->ios2_DstAddr + 2) = *(const ULONG *)(packet + 2);
    *(ULONG *)io->ios2_SrcAddr = *(const ULONG *)(packet + 6);
    *(UWORD *)(io->ios2_SrcAddr + 4) = *(const UWORD *)(packet + 10);
    io->ios2_PacketType = *(const UWORD *)(packet + 12);
#pragma GCC diagnostic pop
    KprintfT("[genet] %s: Source address: %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n", __func__,
             io->ios2_SrcAddr[0], io->ios2_SrcAddr[1], io->ios2_SrcAddr[2],
             io->ios2_SrcAddr[3], io->ios2_SrcAddr[4], io->ios2_SrcAddr[5]);
    KprintfT("[genet] %s: Destination address: %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n", __func__,
             io->ios2_DstAddr[0], io->ios2_DstAddr[1], io->ios2_DstAddr[2],
             io->ios2_DstAddr[3], io->ios2_DstAddr[4], io->ios2_DstAddr[5]);
    KprintfT("[genet] %s: Packet type: 0x%lx\n", __func__, io->ios2_PacketType);

    /* Clear broadcast and multicast flags */
    io->ios2_Req.io_Flags &= (UBYTE) ~(SANA2IOF_BCAST | SANA2IOF_MCAST);

    /* If dest address is FF:FF:FF:FF:FF:FF then it is a broadcast */
    if (dma_flags & DMA_RX_MULT)
    {
        KprintfT("[genet] %s: Packet is a multicast (DMA flag)\n", __func__);
        io->ios2_Req.io_Flags |= SANA2IOF_MCAST;
    }
    else if (dma_flags & DMA_RX_BRDCAST)
    {
        KprintfT("[genet] %s: Packet is a broadcast (DMA flag)\n", __func__);
        io->ios2_Req.io_Flags |= SANA2IOF_BCAST;
    }

    /*
        If RAW packet is requested, copy everything, otherwise copy only contents of
        the frame without ethernet header
        Unfortunately, forcing RAW packet on Roadshow does not work, so we have to copy
        if the flag is not set.
    */
    if (!(io->ios2_Req.io_Flags & SANA2IOF_RAW))
    {
        KprintfT("[genet] %s: Copying only data part of the packet\n", __func__);
        /* Copy only data part of the packet */
        packet += ETH_HLEN;
        packetLength -= ETH_HLEN;
    }

    /* Filter packet if CMD_READ and filter hook is set */
    BOOL packetFiltered = FALSE;
    if (opener->packetFilter && io->ios2_Req.io_Command == CMD_READ && !CallHookPkt(opener->packetFilter, io, packet))
    {
        KprintfT("[genet] %s: Packet filtered by hook\n", __func__);
        packetFiltered = TRUE;
    }

    /* Packet not filtered. Send it now and reply request. */
    if (likely(!packetFiltered))
    {
        u32 copyLen = unit->use_miami_workaround ? ((packetLength + 3u) & ~3u) : packetLength;
        if (unlikely(packetLength == 0 || !opener->CopyToBuff) || opener->CopyToBuff(io->ios2_Data, packet, copyLen) == 0)
        {
            KprintfT("[genet] %s: Failed to copy packet data to buffer\n", __func__);
            unit->internalStats.rx_buffer_errors++;
            io->ios2_WireError = S2WERR_BUFF_ERROR;
            io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            ReportEvents(unit, S2EVENT_BUFF | S2EVENT_RX | S2EVENT_SOFTWARE | S2EVENT_ERROR);
        }

        /* Set number of bytes received */
        io->ios2_DataLength = packetLength;

        ReplyMsg((struct Message *)io);
        KprintfT("[genet] %s: Packet copied and request replied\n", __func__);
    }
}

/* Drain the per-opener SPSC ring into per-type MinLists.
 * Consumer side — runs in the device task only.
 * Aborted entries (marked by abortIO under Forbid) are replied here.
 * Sets ln_Type = NT_MESSAGE on each entry so abortIO's MinList detection
 * heuristic (ln_Type == NT_MESSAGE && ln_Pred != NULL) treats it the same
 * as an IO that came in through PutMsg. */
void DrainReadRing(struct Opener *opener)
{
    ULONG p = opener->readRing.producer;
    asm volatile("nop");
    ULONG c = opener->readRing.consumer;
    while (c != p)
    {
        struct IOSana2Req *io = opener->readRing.entries[c & OPENER_READ_RING_MASK];
        c++;
        if (unlikely(io->ios2_Req.io_Error == IOERR_ABORTED))
        {
            io->ios2_WireError = S2WERR_GENERIC_ERROR;
            ReplyMsg((struct Message *)io);
            continue;
        }
        io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        if (io->ios2_Req.io_Command == S2_READORPHAN)
        {
            AddTailMinList(&opener->orphanQueue, (struct MinNode *)io);
        }
        else
        {
            AddTailMinList(GetPacketTypeQueue(opener, (u16)io->ios2_PacketType),
                           (struct MinNode *)io);
        }
    }
    opener->readRing.consumer = c;
}

static inline BOOL MulticastFilter(struct GenetUnit *unit, u64 destAddr)
{
    // TODO this looks slow
    for (struct MinNode *node = unit->multicastRanges.mlh_Head; node->mln_Succ; node = node->mln_Succ)
    {
        // Check if this is a multicast address we accept
        struct MulticastRange *range = (struct MulticastRange *)node;
        if (destAddr >= range->lowerBound && destAddr <= range->upperBound)
        {
            return TRUE; /* Multicast on our list */
        }
    }
    return FALSE; /* Multicast not on our list */
}

BOOL ReceiveFrame(struct GenetUnit *unit, u8 *packet, u32 packetLength, u16 dma_flags)
{
    /* We only need to filter in software if MDF is not enabled */
    if (unlikely(!unit->mdfEnabled && (dma_flags & DMA_RX_MULT)))
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
        u64 destAddr = ((u64)*(const UWORD *)packet << 32) | *(const ULONG *)(packet + 2);
#pragma GCC diagnostic pop
        if (!MulticastFilter(unit, destAddr))
        {
            return FALSE; // Not a multicast address we accept, drop the packet
        }
    }

    unit->internalStats.rx_packets++;
    unit->internalStats.rx_bytes += packetLength;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
    u16 packetType = *(const UWORD *)(packet + 12);
#pragma GCC diagnostic pop
    BOOL orphan = TRUE;
    BOOL activity = FALSE;
    KprintfT("[genet] %s: Received packet of length %lu with type 0x%lx\n", __func__, (ULONG)packetLength, (ULONG)packetType);

    /* MinLists are pre-drained by UnitTask before bcmgenet_gmac_eth_rx. */

    /* Fast path for common packet types */
    if (likely(packetType == 0x0800 || packetType == 0x0806))
    {
        for (struct MinNode *node = unit->openers.mlh_Head; node->mln_Succ; node = node->mln_Succ)
        {
            struct Opener *opener = (struct Opener *)node;
            struct MinList *queue = GetPacketTypeQueue(opener, packetType);
            struct IOSana2Req *io = (struct IOSana2Req *)RemHeadMinList(queue);

            if (likely(io != NULL))
            {
                CopyPacket(io, packet, packetLength, dma_flags);
                orphan = FALSE;
                activity = TRUE;
                /* Continue to deliver to other openers */
            }
        }
        if (orphan)
        {
            unit->internalStats.rx_arp_ip_dropped++;
        }
    }
    else
    {
        /* Fallback path for other packet types */
        for (struct MinNode *node = unit->openers.mlh_Head; node->mln_Succ; node = node->mln_Succ)
        {
            struct Opener *opener = (struct Opener *)node;
            /* Go through all IO read requests pending*/
            for (struct MinNode *ioNode = opener->readQueue.mlh_Head; ioNode->mln_Succ; ioNode = ioNode->mln_Succ)
            {
                struct IOSana2Req *io = (struct IOSana2Req *)ioNode;
                // EthernetII has packet type larger than 1500 (MTU),
                // 802.3 has no packet type but just length
                if (io->ios2_PacketType == packetType || (packetType <= 1500 && io->ios2_PacketType <= 1500))
                {
                    KprintfT("[genet] %s: Found opener for packet type 0x%lx\n", __func__, (ULONG)packetType);
                    Remove((struct Node *)io);
                    /* Match, copy packet, break loop for this opener */
                    CopyPacket(io, packet, packetLength, dma_flags);

                    /* The packet is sent at least to one opener, not an orphan anymore */
                    orphan = FALSE;
                    activity = TRUE;
                    break;
                }
            }
        }
    }

    /* No receiver for this packet found? It's an orphan then */
    if (unlikely(orphan))
    {
        unit->internalStats.rx_orphan++;

        /* Go through all openers and offer orphan packet to anyone asking */
        for (struct MinNode *node = unit->openers.mlh_Head; node->mln_Succ; node = node->mln_Succ)
        {
            struct Opener *opener = (struct Opener *)node;
            /* Check if orphan port has any pending requests */
            struct IOSana2Req *io = (struct IOSana2Req *)RemHeadMinList(&opener->orphanQueue);
            if (unlikely(io != NULL))
            {
                KprintfT("[genet] %s: Found opener for orphan packet type 0x%lx\n", __func__, (ULONG)packetType);
                CopyPacket(io, packet, packetLength, dma_flags);
                activity = TRUE;
            }
            /* Continue to offer to other openers with orphan requests */
        }
    }
    return activity;
}
