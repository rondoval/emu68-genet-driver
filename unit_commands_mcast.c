// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#define __NOLIBBASE__

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/sana2.h>

#include <device.h>
#include <debug.h>

static inline uint64_t GetAddress(const UBYTE *addr)
{
    union
    {
        uint64_t u64;
        UBYTE u8[8];
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

int Do_S2_ADDMULTICASTADDRESSES(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    struct ExecBase *SysBase = unit->execBase;

    uint64_t lower_bound = GetAddress(io->ios2_SrcAddr);
    uint64_t upper_bound = (io->ios2_Req.io_Command == S2_ADDMULTICASTADDRESS) ? lower_bound : GetAddress(io->ios2_DstAddr);

    /* Go through already registered multicast ranges. If one is found, increase use count and return */
    struct MulticastRange *range = (struct MulticastRange *)unit->multicastRanges.mlh_Head;
    while(range->node.mln_Succ)
    {
        if (range->lowerBound == lower_bound && range->upperBound == upper_bound)
        {
            range->useCount++;
            return 1;
        }
        range = (struct MulticastRange *)range->node.mln_Succ;
    }

    /* No range was found. Create new one and add the multicast range on the WiFi module */
    //TODO mem pool range = AllocPooledClear(WiFiBase->w_MemPool, sizeof(struct MulticastRange));
    range = AllocMem(sizeof(struct MulticastRange), MEMF_CLEAR | MEMF_PUBLIC);
    range->useCount = 1;
    range->lowerBound = lower_bound;
    range->upperBound = upper_bound;
    AddHead((APTR)&unit->multicastRanges, (APTR)range);

    return 1;
}

int Do_S2_DELMULTICASTADDRESSES(struct IOSana2Req *io)
{
    struct GenetUnit *unit = (struct GenetUnit *)io->ios2_Req.io_Unit;
    struct ExecBase *SysBase = unit->execBase;

    uint64_t lower_bound = GetAddress(io->ios2_SrcAddr);
    uint64_t upper_bound = (io->ios2_Req.io_Command == S2_DELMULTICASTADDRESS) ? lower_bound : GetAddress(io->ios2_DstAddr);

    /* Go through already registered multicast ranges. Once found, decrease use count */
    struct MulticastRange *range = (struct MulticastRange *)unit->multicastRanges.mlh_Head;
    while(range->node.mln_Succ)
    {
        if (range->lowerBound == lower_bound && range->upperBound == upper_bound)
        {
            range->useCount--;

            /* No user of this multicast range. Remove it and unregister on WiFi module */
            if (range->useCount == 0)
            {
                Remove((APTR)range);
                FreeMem(range, sizeof(struct MulticastRange));
                // FreePooled(WiFiBase->w_MemPool, range, sizeof(struct MulticastRange));
            }
            return 1;
        }
        range = (struct MulticastRange *)range->node.mln_Succ;
    }

    return 1;
}
