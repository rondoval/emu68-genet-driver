// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/utility_protos.h>
#include <clib/exec_protos.h>
#else
#include <proto/utility.h>
#include <proto/exec.h>
#endif

#include <genet/bcmgenet-regs.h>
#include <device.h>
#include <compat.h>
#include <debug.h>
#include <runtime_config.h>

static inline void CopyPacket(struct IOSana2Req *io, UBYTE *packet, ULONG packetLength, ULONG dma_flags)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    KprintfH("[genet] %s: Copying packet of length %ld\n", __func__, packetLength);
    struct Opener *opener = io->ios2_BufferManagement;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
    /* Copy source and dest addresses */
    *(ULONG *)&io->ios2_DstAddr[0] = *(ULONG *)&packet[0];
    *(UWORD *)&io->ios2_DstAddr[4] = *(UWORD *)&packet[4];
    *(ULONG *)&io->ios2_SrcAddr[0] = *(ULONG *)&packet[6];
    *(UWORD *)&io->ios2_SrcAddr[4] = *(UWORD *)&packet[10];
#pragma GCC diagnostic pop
    io->ios2_PacketType = *(UWORD *)&packet[12];
    KprintfH("[genet] %s: Source address: %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n", __func__,
             io->ios2_SrcAddr[0], io->ios2_SrcAddr[1], io->ios2_SrcAddr[2],
             io->ios2_SrcAddr[3], io->ios2_SrcAddr[4], io->ios2_SrcAddr[5]);
    KprintfH("[genet] %s: Destination address: %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n", __func__,
             io->ios2_DstAddr[0], io->ios2_DstAddr[1], io->ios2_DstAddr[2],
             io->ios2_DstAddr[3], io->ios2_DstAddr[4], io->ios2_DstAddr[5]);
    KprintfH("[genet] %s: Packet type: 0x%lx\n", __func__, io->ios2_PacketType);

    /* Clear broadcast and multicast flags */
    io->ios2_Req.io_Flags &= ~(SANA2IOF_BCAST | SANA2IOF_MCAST);

    /* If dest address is FF:FF:FF:FF:FF:FF then it is a broadcast */
    if (dma_flags & DMA_RX_MULT)
    {
        KprintfH("[genet] %s: Packet is a multicast (DMA flag)\n", __func__);
        io->ios2_Req.io_Flags |= SANA2IOF_MCAST;
    }
    else if (dma_flags & DMA_RX_BRDCAST)
    {
        KprintfH("[genet] %s: Packet is a broadcast (DMA flag)\n", __func__);
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
        KprintfH("[genet] %s: Copying only data part of the packet\n", __func__);
        /* Copy only data part of the packet */
        packet += ETH_HLEN;
        packetLength -= ETH_HLEN;
    }

    /* Filter packet if CMD_READ and filter hook is set */
    UBYTE packetFiltered = FALSE;
    if (opener->packetFilter && io->ios2_Req.io_Command == CMD_READ && !CallHookPkt(opener->packetFilter, io, packet))
    {
        KprintfH("[genet] %s: Packet filtered by hook\n", __func__);
        packetFiltered = TRUE;
    }

    /* Packet not filtered. Send it now and reply request. */
    if (likely(!packetFiltered))
    {
        ULONG copyLen = genetConfig.use_miami_workaround ? ((packetLength + 3) & ~3u) : packetLength;
        if (unlikely(packetLength == 0 || !opener->CopyToBuff) || opener->CopyToBuff(io->ios2_Data, packet, copyLen) == 0)
        {
            KprintfH("[genet] %s: Failed to copy packet data to buffer\n", __func__);
            unit->internalStats.rx_dropped++;
            io->ios2_WireError = S2WERR_BUFF_ERROR;
            io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            ReportEvents(unit, S2EVENT_BUFF | S2EVENT_RX | S2EVENT_SOFTWARE | S2EVENT_ERROR);
        }

        /* Set number of bytes received */
        io->ios2_DataLength = packetLength;

        ReplyMsg((struct Message *)io);
        KprintfH("[genet] %s: Packet copied and request replied\n", __func__);
    }
}

static inline BOOL MulticastFilter(struct GenetUnit *unit, uint64_t destAddr)
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

BOOL ReceiveFrame(struct GenetUnit *unit, UBYTE *packet, ULONG packetLength, ULONG dma_flags)
{
    /* We only need to filter in software if MDF is not enabled */
    if (unlikely(!unit->mdfEnabled && (dma_flags & DMA_RX_MULT)))
    {
        uint64_t destAddr = ((uint64_t)*(UWORD *)&packet[0] << 32) | *(ULONG *)&packet[2];
        if (!MulticastFilter(unit, destAddr))
        {
            return FALSE; // Not a multicast address we accept, drop the packet
        }
    }

    unit->internalStats.rx_packets++;
    unit->internalStats.rx_bytes += packetLength;
    UWORD packetType = *(UWORD *)&packet[12];
    UBYTE orphan = TRUE;
    BOOL activity = FALSE;
    KprintfH("[genet] %s: Received packet of length %ld with type 0x%lx\n", __func__, packetLength, packetType);

    /* Fast path for common packet types */
    //TODO get rid of semaphores
    if (likely(packetType == 0x0800 || packetType == 0x0806))
    {
        for (struct MinNode *node = unit->openers.mlh_Head; node->mln_Succ; node = node->mln_Succ)
        {
            struct Opener *opener = (struct Opener *)node;
            struct MinList *queue = GetPacketTypeQueue(opener, packetType);
            ObtainSemaphore(&opener->openerSemaphore);
            struct IOSana2Req *io = (struct IOSana2Req *)RemHeadMinList(queue);
            ReleaseSemaphore(&opener->openerSemaphore);

            if (likely(io != NULL))
            {
                CopyPacket(io, packet, packetLength, dma_flags);
                orphan = FALSE;
                activity = TRUE;
                /* Continue to deliver to other openers */
            }
            else
            {
                unit->internalStats.rx_arp_ip_dropped++;
            }
        }
    }
    else
    {
        /* Fallback path for other packet types */
        for (struct MinNode *node = unit->openers.mlh_Head; node->mln_Succ; node = node->mln_Succ)
        {
            struct Opener *opener = (struct Opener *)node;
            ObtainSemaphore(&opener->openerSemaphore);
            /* Go through all IO read requests pending*/
            for (struct MinNode *ioNode = opener->readQueue.mlh_Head; ioNode->mln_Succ; ioNode = ioNode->mln_Succ)
            {
                struct IOSana2Req *io = (struct IOSana2Req *)ioNode;
                // EthernetII has packet type larger than 1500 (MTU),
                // 802.3 has no packet type but just length
                if (io->ios2_PacketType == packetType || (packetType <= 1500 && io->ios2_PacketType <= 1500))
                {
                    KprintfH("[genet] %s: Found opener for packet type 0x%lx\n", __func__, packetType);
                    Remove((struct Node *)io);
                    /* Match, copy packet, break loop for this opener */
                    CopyPacket(io, packet, packetLength, dma_flags);

                    /* The packet is sent at least to one opener, not an orphan anymore */
                    orphan = FALSE;
                    activity = TRUE;
                    break;
                }
            }
            ReleaseSemaphore(&opener->openerSemaphore);
        }
    }

    /* No receiver for this packet found? It's an orphan then */
    if (unlikely(orphan))
    {
        unit->internalStats.rx_dropped++;

        /* Go through all openers and offer orphan packet to anyone asking */
        for (struct MinNode *node = unit->openers.mlh_Head; node->mln_Succ; node = node->mln_Succ)
        {
            struct Opener *opener = (struct Opener *)node;
            /* Check if orphan port has any pending requests */
            struct IOSana2Req *io = (struct IOSana2Req *)RemHeadMinList(&opener->orphanQueue);
            if (unlikely(io != NULL))
            {
                KprintfH("[genet] %s: Found opener for orphan packet type 0x%lx\n", __func__, packetType);
                CopyPacket(io, packet, packetLength, dma_flags);
                activity = TRUE;
            }
            /* Continue to offer to other openers with orphan requests */
        }
    }
    return activity;
}
