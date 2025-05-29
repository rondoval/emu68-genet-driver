// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#define __NOLIBBASE__

#include <proto/utility.h>
#include <proto/exec.h>

#include <device.h>
#include <debug.h>

int SendFrame(struct GenetUnit *unit, struct IOSana2Req *io)
{
    KprintfH("[genet] %s: unit %ld, io %lx, flags %lx\n", __func__, unit->unitNumber, io, io->ios2_Req.io_Flags);
    // TODO send multiple requests at a time
    //  just put into ring buffer
    //  and create an array of messages to reply to
    //  once the ring buffer index moves
    // Make sure we have place in TX
    // TODO send directly from stack buffer through DMA functions?
    struct Opener *opener = io->ios2_BufferManagement;

    UWORD length = io->ios2_DataLength;
    UBYTE *ptr = (UBYTE *)unit->txbuffer;

    if ((io->ios2_Req.io_Flags & SANA2IOF_RAW) == 0)
    {
        KprintfH("[genet] %s: adding ethernet header\n", __func__);
        length += ETH_HLEN;
        for (int i = 0; i < 6; i++)
            ptr[i] = io->ios2_DstAddr[i];

        for (int i = 0; i < 6; i++)
            ptr[6 + i] = io->ios2_SrcAddr[i];

        *(UWORD *)&ptr[12] = io->ios2_PacketType;
    }

    if (io->ios2_DataLength != 0 && opener->CopyFromBuff)
    {
        opener->CopyFromBuff(ptr + ETH_HLEN, io->ios2_Data, io->ios2_DataLength);
    }
    bcmgenet_gmac_eth_send(unit, ptr, length);
    unit->stats.PacketsSent++;
    return 1;
}

static inline void CopyPacket(struct IOSana2Req *io, UBYTE *packet, ULONG packetLength)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    struct ExecBase *SysBase = unit->execBase;
    KprintfH("[genet] %s: Copying packet of length %ld\n", __func__, packetLength);
    struct Opener *opener = io->ios2_BufferManagement;
    struct Library *UtilityBase = unit->utilityBase;

    /* Copy source and dest addresses */
    for (int i = 0; i < 6; i++)
        io->ios2_DstAddr[i] = packet[i];
    for (int i = 0; i < 6; i++)
        io->ios2_SrcAddr[i] = packet[6 + i];
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
    if (*(ULONG *)packet == 0xffffffff && *(UWORD *)(packet + 4) == 0xffff)
    {
        KprintfH("[genet] %s: Packet is a broadcast\n", __func__);
        io->ios2_Req.io_Flags |= SANA2IOF_BCAST;
    }
    /* If dest address has lowest bit of first addr byte set, then it is a multicast */
    else if (*packet & 0x01)
    {
        KprintfH("[genet] %s: Packet is a multicast\n", __func__);
        io->ios2_Req.io_Flags |= SANA2IOF_MCAST;
    }

    /*
        If RAW packet is requested, copy everything, otherwise copy only contents of
        the frame without ethernet header
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
    if (!packetFiltered)
    {
        if (packetLength != 0 && opener->CopyToBuff && opener->CopyToBuff(io->ios2_Data, packet, packetLength) == 0)
        {
            KprintfH("[genet] %s: Failed to copy packet data to buffer\n", __func__);
            io->ios2_WireError = S2WERR_BUFF_ERROR;
            io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            // TODO report error event
        }

        /* Set number of bytes received */
        io->ios2_DataLength = packetLength;

        ObtainSemaphore(&unit->semaphore);
        Remove((struct Node *)io);
        ReleaseSemaphore(&unit->semaphore);
        ReplyMsg((struct Message *)io);
        KprintfH("[genet] %s: Packet copied and request replied\n", __func__);
    }
}

void ReceiveFrame(struct GenetUnit *unit, UBYTE *packet, ULONG packetLength)
{
    struct ExecBase *SysBase = unit->execBase;

    // Get destination address and check if it is a multicast
    uint64_t destAddr = ((uint64_t)*(UWORD *)&packet[0] << 32) | *(ULONG *)&packet[2];
    if (destAddr != 0xffffffffffffULL && (destAddr & 0x010000000000ULL))
    {
        // struct MulticastRange *range;
        // accept = FALSE;
        return;

        // ForeachNode(&unit->wu_MulticastRanges, range)
        // {
        //     if (destAddr >= range->mr_LowerBound && destAddr <= range->mr_UpperBound)
        //     {
        //         accept = TRUE;
        //         break;
        //     }
        // }
    }

    unit->stats.PacketsReceived++;
    UWORD packetType = *(UWORD *)&packet[12];
    UBYTE orphan = TRUE;
    KprintfH("[genet] %s: Received packet of length %ld with type 0x%lx\n", __func__, packetLength, packetType);

    ObtainSemaphore(&unit->semaphore);
    /* Go through all openers */
    struct Opener *opener = (struct Opener *)unit->openers.mlh_Head;
    while (opener->node.mln_Succ)
    {
        /* Go through all IO read requests pending*/
        struct IOSana2Req *io = (struct IOSana2Req *)opener->readPort.mp_MsgList.lh_Head;
        while (io)
        {
            // EthernetII has packet type larger than 1500 (MTU),
            // 802.3 has no packet type but just length
            if (io->ios2_PacketType == packetType || (packetType <= 1500 && io->ios2_PacketType <= 1500))
            {
                KprintfH("[genet] %s: Found opener for packet type 0x%lx\n", __func__, packetType);
                /* Match, copy packet, break loop for this opener */
                CopyPacket(io, packet, packetLength);

                /* The packet is sent at least to one opener, not an orphan anymore */
                orphan = FALSE;
                break;
            }
            io = (struct IOSana2Req *)io->ios2_Req.io_Message.mn_Node.ln_Succ;
        }
        opener = (struct Opener *)opener->node.mln_Succ;
    }
    ReleaseSemaphore(&unit->semaphore);

    /* No receiver for this packet found? It's an orphan then */
    if (orphan)
    {
        unit->stats.UnknownTypesReceived++;

        ObtainSemaphore(&unit->semaphore);
        /* Go through all openers and offer orphan packet to anyone asking */
        struct Opener *opener = (struct Opener *)unit->openers.mlh_Head;
        while (opener->node.mln_Succ)
        {
            struct IOSana2Req *io = (APTR)opener->orphanPort.mp_MsgList.lh_Head;
            /*
                If this is a real node, ln_Succ will be not NULL, otherwise it is just
                protector node of empty list
            */
            if (io->ios2_Req.io_Message.mn_Node.ln_Succ)
            {
                KprintfH("[genet] %s: Found opener for orphan packet type 0x%lx\n", __func__, packetType);
                CopyPacket(io, packet, packetLength);
            }
            opener = (struct Opener *)opener->node.mln_Succ;
        }
        ReleaseSemaphore(&unit->semaphore);
    }
}
