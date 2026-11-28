// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * sana2_mcast.c — SANA-II multicast: the openers' address ranges and the
 * reception filter they become. Unit task only.
 *
 * The ranges are flattened into the core's filter (ndMcastList / ndPromisc,
 * programmed by bcmgenet_set_rx_mode), the one netdev's NETDEV_CMD_SET_RXFILTER
 * writes too - never while a netdev stack holds the unit. Every address of
 * every range takes an exact-match slot; when they do not fit, or an opener
 * asked for promiscuous reception, the hardware passes everything, and the
 * delivery (sana2_io.c) sorts out who gets what.
 */

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function: see GenetUnit.sysBase */
#include <proto/exec.h>
#endif

#include <devices/sana2.h>

#include <debug.h>
#include <memory.h>
#include <types.h>
#include <device.h>
#include <sana2_unit.h>

#include <genet/bcmgenet.h>
#include <genet/bcmgenet-regs.h>

/* An Ethernet address as a number, for range arithmetic. */
#define MAC_ADDRESS_BYTES 6u
#define MULTICAST_BIT 0x01u /* the group bit, in the first byte on the wire */

/**
 * address_value() - Give an Ethernet address as a number.
 * @address: the MAC_ADDRESS_BYTES of the address, in wire order.
 *
 * Return: the address, its first byte the most significant.
 */
static u64 address_value(const u8 *address)
{
    u64 value = 0;
    for (u32 i = 0; i < MAC_ADDRESS_BYTES; i++)
        value = (value << 8) | address[i];
    return value;
}

/**
 * address_bytes() - Write a number back as an Ethernet address.
 * @value: the address, as address_value() gives it.
 * @address: the MAC_ADDRESS_BYTES to fill, in wire order.
 */
static void address_bytes(u64 value, u8 *address)
{
    for (u32 i = MAC_ADDRESS_BYTES; i-- > 0;)
    {
        address[i] = (u8)value;
        value >>= 8;
    }
}

/**
 * sana2_multicast_wanted() - Tell whether a multicast address falls in a
 * range an opener asked for.
 * @s2: the unit's SANA-II side.
 * @address: the frame's destination address.
 *
 * Unit task: the delivery's check while the hardware passes everything.
 *
 * Return: TRUE when @address is in a range, FALSE otherwise.
 */
BOOL sana2_multicast_wanted(const struct GenetS2 *s2, const u8 *address)
{
    u64 value = address_value(address);
    for (struct MinNode *node = s2->multicastRanges.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        const struct MulticastRange *range = (const struct MulticastRange *)node;
        if (value >= range->lowerBound && value <= range->upperBound)
            return TRUE;
    }
    return FALSE;
}

/**
 * sana2_filter_update() - Flatten the openers' multicast ranges and
 * promiscuous openings into the core's reception filter.
 * @unit: the unit.
 *
 * Unit task; nothing while a netdev stack is attached. Every address of every
 * range takes a slot of ndMcastList; a promiscuous opener, or more than
 * GENET_MDF_MCAST_MAX addresses, sets ndPromisc instead. The hardware is
 * programmed (bcmgenet_set_rx_mode()) only while the unit is online.
 */
void sana2_filter_update(struct GenetUnit *unit)
{
    struct GenetS2 *s2 = unit->s2;
    if (unit->ndStackOps != NULL)
        return; /* netdev's filter while it holds the unit */

    BOOL promiscuous = FALSE;
    for (struct MinNode *node = s2->openers.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
        if (((const struct Opener *)node)->promiscuous)
            promiscuous = TRUE;

    u64 addresses = 0;
    for (struct MinNode *node = s2->multicastRanges.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        const struct MulticastRange *range = (const struct MulticastRange *)node;
        addresses += range->upperBound - range->lowerBound + 1;
    }

    unit->ndFilterFlags = 0;
    if (promiscuous || addresses > GENET_MDF_MCAST_MAX)
    {
        unit->ndPromisc = TRUE;
        unit->ndMcastCount = 0;
    }
    else
    {
        UWORD slot = 0;
        for (struct MinNode *node = s2->multicastRanges.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
        {
            const struct MulticastRange *range = (const struct MulticastRange *)node;
            for (u64 value = range->lowerBound; value <= range->upperBound; value++)
                address_bytes(value, unit->ndMcastList[slot++]);
        }
        unit->ndMcastCount = slot;
        unit->ndPromisc = FALSE;
    }

    /* stopped, it is only stored: the next start programs it */
    if (s2->online)
        bcmgenet_set_rx_mode(unit);
}

/**
 * range_find() - Find the range with exactly these bounds.
 * @s2: the unit's SANA-II side.
 * @lower: the lower bound, as address_value() gives it.
 * @upper: the upper bound.
 *
 * Return: the range, or NULL when none has these bounds.
 */
static struct MulticastRange *range_find(struct GenetS2 *s2, u64 lower, u64 upper)
{
    for (struct MinNode *node = s2->multicastRanges.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        struct MulticastRange *range = (struct MulticastRange *)node;
        if (range->lowerBound == lower && range->upperBound == upper)
            return range;
    }
    return NULL;
}

/**
 * request_range() - Read the request's range: ios2_SrcAddr alone, or
 * ios2_SrcAddr to ios2_DstAddr for the ...ADDRESSES forms.
 * @io: the request.
 * @ranged: TRUE for S2_ADDMULTICASTADDRESSES / S2_DELMULTICASTADDRESSES.
 * @lower: receives the lower bound.
 * @upper: receives the upper bound.
 *
 * Return: TRUE for a multicast range in order; FALSE otherwise, with
 * io_Error S2ERR_BAD_ADDRESS / S2WERR_BAD_MULTICAST.
 */
static BOOL request_range(struct IOSana2Req *io, BOOL ranged, u64 *lower, u64 *upper)
{
    *lower = address_value(io->ios2_SrcAddr);
    *upper = ranged ? address_value(io->ios2_DstAddr) : *lower;
    if (!(io->ios2_SrcAddr[0] & MULTICAST_BIT) ||
        (ranged && !(io->ios2_DstAddr[0] & MULTICAST_BIT)) || *upper < *lower)
    {
        io->ios2_Req.io_Error = S2ERR_BAD_ADDRESS;
        io->ios2_WireError = S2WERR_BAD_MULTICAST;
        return FALSE;
    }
    return TRUE;
}

/**
 * sana2_add_multicast() - S2_ADDMULTICASTADDRESS(ES): take one more use of a
 * multicast range.
 * @unit: the unit.
 * @io: the request.
 *
 * Unit task. Refused with S2ERR_OUTOFSERVICE while a netdev stack is
 * attached. A range asked for already counts one more use; a new one comes
 * from metaPool and updates the filter. Out of memory fails with
 * S2ERR_NO_RESOURCES and reports S2EVENT_SOFTWARE | S2EVENT_ERROR.
 *
 * Return: COMMAND_PROCESSED, with io_Error set on failure (request_range()'s
 * included).
 */
u32 sana2_add_multicast(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct GenetS2 *s2 = unit->s2;
    u64 lower, upper;

    if (unit->ndStackOps != NULL)
        return sana2_refuse_netdev(io);
    if (!request_range(io, io->ios2_Req.io_Command == S2_ADDMULTICASTADDRESSES, &lower, &upper))
        return COMMAND_PROCESSED;

    struct MulticastRange *range = range_find(s2, lower, upper);
    if (range != NULL)
    {
        range->useCount++;
        return COMMAND_PROCESSED;
    }

    range = pool_alloc(unit->metaPool, sizeof(struct MulticastRange));
    if (range == NULL)
    {
        Kprintf("[genet] %s: no memory for a multicast range\n", __func__);
        io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        io->ios2_WireError = S2WERR_GENERIC_ERROR;
        sana2_report_events(unit, S2EVENT_SOFTWARE | S2EVENT_ERROR);
        return COMMAND_PROCESSED;
    }
    range->useCount = 1;
    range->lowerBound = lower;
    range->upperBound = upper;
    AddTailMinList(&s2->multicastRanges, &range->node);
    sana2_filter_update(unit);
    return COMMAND_PROCESSED;
}

/**
 * sana2_del_multicast() - S2_DELMULTICASTADDRESS(ES): take back one
 * S2_ADDMULTICASTADDRESS(ES) of the same range.
 * @unit: the unit.
 * @io: the request.
 *
 * Unit task. Refused with S2ERR_OUTOFSERVICE while a netdev stack is
 * attached. The last use frees the range and updates the filter; a range
 * never added is no error.
 *
 * Return: COMMAND_PROCESSED, with io_Error set on failure (request_range()'s).
 */
u32 sana2_del_multicast(struct GenetUnit *unit, struct IOSana2Req *io)
{
    struct ExecBase *SysBase = unit->sysBase;
    u64 lower, upper;

    if (unit->ndStackOps != NULL)
        return sana2_refuse_netdev(io);
    if (!request_range(io, io->ios2_Req.io_Command == S2_DELMULTICASTADDRESSES, &lower, &upper))
        return COMMAND_PROCESSED;

    struct MulticastRange *range = range_find(unit->s2, lower, upper);
    if (range == NULL || --range->useCount != 0)
        return COMMAND_PROCESSED;

    RemoveMinNode(&range->node);
    pool_free(unit->metaPool, range);
    sana2_filter_update(unit);
    return COMMAND_PROCESSED;
}

/**
 * sana2_multicast_clear() - Free every multicast range still listed.
 * @unit: the unit.
 *
 * The last close, in the closing task once the unit task has stopped: ranges
 * a stack added and never deleted. The filter is left alone; the hardware is
 * going down with the unit.
 */
void sana2_multicast_clear(struct GenetUnit *unit)
{
    struct ExecBase *SysBase = unit->sysBase;
    struct MinNode *node;
    while ((node = RemHeadMinList(&unit->s2->multicastRanges)) != NULL)
        pool_free(unit->metaPool, node);
}
