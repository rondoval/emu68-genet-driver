// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/sana2.h>

#include <types.h>
#include <device.h>
#include <debug.h>
#include <memory.h>
#include <minlist.h>

static inline u64 GetAddress(const u8 *addr)
{
    union
    {
        u64 u64;
        u8 u8[8];
    } u;

    u.u8[0] = u.u8[1] = 0;
    u.u8[2] = addr[0];
    u.u8[3] = addr[1];
    u.u8[4] = addr[2];
    u.u8[5] = addr[3];
    u.u8[6] = addr[4];
    u.u8[7] = addr[5];

    return u.u64;
}

u32 Do_S2_ADDMULTICASTADDRESSES(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
#ifdef DEBUG_HIGH
    if (io->ios2_Req.io_Command == S2_ADDMULTICASTADDRESSES)
    {
        KprintfH("[genet] %s: Adding multicast address range %02lx:%02lx:%02lx:%02lx:%02lx:%02lx - %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                 __func__,
                 io->ios2_SrcAddr[0], io->ios2_SrcAddr[1], io->ios2_SrcAddr[2],
                 io->ios2_SrcAddr[3], io->ios2_SrcAddr[4], io->ios2_SrcAddr[5],
                 io->ios2_DstAddr[0], io->ios2_DstAddr[1], io->ios2_DstAddr[2],
                 io->ios2_DstAddr[3], io->ios2_DstAddr[4], io->ios2_DstAddr[5]);
    }
    else
    {
        KprintfH("[genet] %s: Adding multicast address %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                 __func__,
                 io->ios2_SrcAddr[0], io->ios2_SrcAddr[1], io->ios2_SrcAddr[2],
                 io->ios2_SrcAddr[3], io->ios2_SrcAddr[4], io->ios2_SrcAddr[5]);
    }
#endif

    u64 lower_bound = GetAddress(io->ios2_SrcAddr);
    u64 upper_bound = (io->ios2_Req.io_Command == S2_ADDMULTICASTADDRESS) ? lower_bound : GetAddress(io->ios2_DstAddr);

    /* Go through already registered multicast ranges. If one is found, increase use count and return */
    for (struct MinNode *node = unit->multicastRanges.mlh_Head; node->mln_Succ; node = node->mln_Succ)
    {
        struct MulticastRange *range = (struct MulticastRange *)node;
        if (range->lowerBound == lower_bound && range->upperBound == upper_bound)
        {
            range->useCount++;
            return COMMAND_PROCESSED;
        }
    }

    /* No range was found. Create new one and add the multicast range on the WiFi module */
    struct MulticastRange *range = pool_alloc(unit->metaPool, sizeof(struct MulticastRange));
    if (!range)
    {
        Kprintf("[genet] %s: Failed to allocate memory for multicast range\n", __func__);
        io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        ReportEvents(unit, S2EVENT_SOFTWARE | S2EVENT_ERROR);
        return COMMAND_PROCESSED;
    }
    memset(range, 0, sizeof(struct MulticastRange));
    range->useCount = 1;
    range->lowerBound = lower_bound;
    range->upperBound = upper_bound;
    AddHeadMinList(&unit->multicastRanges, (struct MinNode *)range);

    u32 count = (u32)(upper_bound - lower_bound + 1);
    unit->multicastCount += count;

    /* Update PROMISC and MDF filter */
    bcmgenet_set_rx_mode(unit);
    return COMMAND_PROCESSED;
}

u32 Do_S2_DELMULTICASTADDRESSES(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
#ifdef DEBUG_HIGH
    if (io->ios2_Req.io_Command == S2_DELMULTICASTADDRESSES)
    {
        KprintfH("[genet] %s: Removing multicast address range %02lx:%02lx:%02lx:%02lx:%02lx:%02lx - %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                 __func__,
                 io->ios2_SrcAddr[0], io->ios2_SrcAddr[1], io->ios2_SrcAddr[2],
                 io->ios2_SrcAddr[3], io->ios2_SrcAddr[4], io->ios2_SrcAddr[5],
                 io->ios2_DstAddr[0], io->ios2_DstAddr[1], io->ios2_DstAddr[2],
                 io->ios2_DstAddr[3], io->ios2_DstAddr[4], io->ios2_DstAddr[5]);
    }
    else
    {
        KprintfH("[genet] %s: Removing multicast address %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                 __func__,
                 io->ios2_SrcAddr[0], io->ios2_SrcAddr[1], io->ios2_SrcAddr[2],
                 io->ios2_SrcAddr[3], io->ios2_SrcAddr[4], io->ios2_SrcAddr[5]);
    }
#endif

    u64 lower_bound = GetAddress(io->ios2_SrcAddr);
    u64 upper_bound = (io->ios2_Req.io_Command == S2_DELMULTICASTADDRESS) ? lower_bound : GetAddress(io->ios2_DstAddr);

    /* Go through already registered multicast ranges. Once found, decrease use count */
    for (struct MinNode *node = unit->multicastRanges.mlh_Head; node->mln_Succ; node = node->mln_Succ)
    {
        struct MulticastRange *range = (struct MulticastRange *)node;
        if (range->lowerBound == lower_bound && range->upperBound == upper_bound)
        {
            range->useCount--;

            /* No user of this multicast range. Remove it and unregister on WiFi module */
            if (range->useCount == 0)
            {
                RemoveMinNode((struct MinNode *)range);
                pool_free(unit->metaPool, range);

                u32 count = (u32)(upper_bound - lower_bound + 1);
                unit->multicastCount -= count;

                /* Update PROMISC and MDF filter */
                bcmgenet_set_rx_mode(unit);
            }
            return COMMAND_PROCESSED;
        }
    }
    return COMMAND_PROCESSED;
}
