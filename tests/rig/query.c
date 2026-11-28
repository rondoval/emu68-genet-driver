// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * query.c - GenetTest QUERY and HOLDER: what any opener may ask in any mode,
 * and who holds the unit.
 */

#include "rig.h"
#include <emu68test/kv.h>

#include <proto/exec.h>

#include <devices/netdev.h>
#include <devices/newstyle.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define ETHER_HEADER_BYTES 14
#define MAX_SPECIAL_STATS 64
#define SPECIAL_KEY_BYTES 48
#define QUERY_GUARD 0xa5a5a5a5UL
#define HOLDER_PROBE_GROUP {0x01, 0x00, 0x5e, 0x7f, 0x7f, 0x7f} /* an IPv4 group nobody joins */

/* The fields every SANA-II revision has: a stack built with the older
 * headers asks with this much. */
#define DEVICEQUERY_BASE_BYTES (offsetof(struct Sana2DeviceQuery, HardwareType) + sizeof(ULONG))

/**
 * check_device_query_full() - Check S2_DEVICEQUERY with the whole structure:
 * Ethernet, an MTU of EXPECTED_MTU and a RawMTU that holds its header.
 * @io: the opened request.
 */
static void check_device_query_full(struct IOSana2Req *io)
{
    struct Sana2DeviceQuery query;
    memset(&query, 0, sizeof(query));
    query.SizeAvailable = sizeof(query);
    io->ios2_StatData = &query;
    BYTE error = rig_command(io, S2_DEVICEQUERY);
    kv_unum("query.mtu", query.MTU);
    kv_unum("query.raw_mtu", query.RawMTU);
    kv_unum("query.bps", query.BPS);
    kv_check(error == 0 && query.HardwareType == S2WireType_Ethernet && query.MTU == EXPECTED_MTU &&
                 query.SizeSupplied == sizeof(query) && query.RawMTU >= EXPECTED_MTU + ETHER_HEADER_BYTES,
             "S2_DEVICEQUERY answers in full", "error %d, type %lu, MTU %lu, raw MTU %lu, %lu of %lu bytes", error,
             query.HardwareType, query.MTU, query.RawMTU, query.SizeSupplied, (ULONG)sizeof(query));
}

/**
 * check_device_query_base() - Check S2_DEVICEQUERY with the older, shorter
 * structure: what fits is filled, and nothing past it.
 * @io: the opened request.
 */
static void check_device_query_base(struct IOSana2Req *io)
{
    struct Sana2DeviceQuery query;
    memset(&query, 0, sizeof(query));
    query.SizeAvailable = DEVICEQUERY_BASE_BYTES;
    query.RawMTU = QUERY_GUARD; /* the field the older structure does not have */
    io->ios2_StatData = &query;
    BYTE error = rig_command(io, S2_DEVICEQUERY);
    kv_check(error == 0 && query.MTU == EXPECTED_MTU && query.SizeSupplied == DEVICEQUERY_BASE_BYTES &&
                 query.RawMTU == QUERY_GUARD,
             "S2_DEVICEQUERY fills an older query and nothing past it",
             "error %d, MTU %lu, %lu of %lu bytes, field past it %s", error, query.MTU, query.SizeSupplied,
             (ULONG)DEVICEQUERY_BASE_BYTES, query.RawMTU == QUERY_GUARD ? "untouched" : "written");
}

/**
 * check_device_query_short() - Check that S2_DEVICEQUERY refuses a structure
 * short of the older one with S2ERR_BAD_ARGUMENT / S2WERR_BAD_STATDATA.
 * @io: the opened request.
 */
static void check_device_query_short(struct IOSana2Req *io)
{
    struct Sana2DeviceQuery query;
    memset(&query, 0, sizeof(query));
    query.SizeAvailable = DEVICEQUERY_BASE_BYTES - 1;
    io->ios2_StatData = &query;
    BYTE error = rig_command(io, S2_DEVICEQUERY);
    kv_check(error == S2ERR_BAD_ARGUMENT && io->ios2_WireError == S2WERR_BAD_STATDATA,
             "S2_DEVICEQUERY refuses a query short of its fields", "error %d, wire error %lu", error,
             io->ios2_WireError);
}

/**
 * check_station_address() - Check that S2_GETSTATIONADDRESS answers with a
 * unicast address whose first three bytes are not all zero.
 * @io: the opened request.
 */
static void check_station_address(struct IOSana2Req *io)
{
    BYTE error = rig_command(io, S2_GETSTATIONADDRESS);
    const UBYTE *address = io->ios2_SrcAddr;
    BOOL unicast = (address[0] & ETHER_GROUP_BIT) == 0 && (address[0] | address[1] | address[2]) != 0;
    kv_check(error == 0 && unicast, "S2_GETSTATIONADDRESS answers", "error %d, %s", error,
             unicast ? "a unicast address" : "not a unicast address");
}

/**
 * check_global_stats() - Check S2_GETGLOBALSTATS and S2_GETEXTENDEDGLOBALSTATS.
 * @io: the opened request.
 */
static void check_global_stats(struct IOSana2Req *io)
{
    struct Sana2DeviceStats stats;
    memset(&stats, 0, sizeof(stats));
    io->ios2_StatData = &stats;
    BYTE error = rig_command(io, S2_GETGLOBALSTATS);
    kv_unum("stats.packets_received", stats.PacketsReceived);
    kv_unum("stats.packets_sent", stats.PacketsSent);
    kv_check(error == 0 && stats.PacketsReceived != 0, "S2_GETGLOBALSTATS answers",
             "error %d, %lu frames received, %lu sent", error, stats.PacketsReceived, stats.PacketsSent);

    struct Sana2ExtDeviceStats extended;
    memset(&extended, 0, sizeof(extended));
    extended.s2xds_Length = sizeof(extended);
    io->ios2_StatData = &extended;
    error = rig_command(io, S2_GETEXTENDEDGLOBALSTATS);
    kv_unum("stats.reconfigurations", extended.s2xds_Reconfigurations.s2q_Low);
    kv_check(error == 0 && extended.s2xds_Actual == sizeof(extended), "S2_GETEXTENDEDGLOBALSTATS answers",
             "error %d, %lu of %lu bytes", error, extended.s2xds_Actual, (ULONG)sizeof(extended));
}

/**
 * key_word() - Tell whether @name can be a key's last part (kv.h): lower
 * case letters, digits and underscores.
 * @name: a special stat's String, or NULL.
 *
 * Return: TRUE when it can.
 */
static BOOL key_word(CONST_STRPTR name)
{
    if (name == NULL || *name == '\0')
        return FALSE;
    for (; *name != '\0'; name++)
        if (!((*name >= 'a' && *name <= 'z') || (*name >= '0' && *name <= '9') || *name == '_'))
            return FALSE;
    return TRUE;
}

/**
 * check_special_stats() - Check that S2_GETSPECIALSTATS answers with records,
 * and print each as special.<name>=<count> for the host to compare.
 * @io: the opened request.
 */
static void check_special_stats(struct IOSana2Req *io)
{
    static struct
    {
        struct Sana2SpecialStatHeader header;
        struct Sana2SpecialStatRecord records[MAX_SPECIAL_STATS];
    } special;
    memset(&special, 0, sizeof(special));
    special.header.RecordCountMax = MAX_SPECIAL_STATS;
    io->ios2_StatData = &special;
    BYTE error = rig_command(io, S2_GETSPECIALSTATS);
    ULONG named = 0;
    for (ULONG i = 0; error == 0 && i < special.header.RecordCountSupplied; i++)
    {
        const struct Sana2SpecialStatRecord *record = &special.records[i];
        if (!key_word(record->String))
            continue;
        char key[SPECIAL_KEY_BYTES];
        snprintf(key, sizeof(key), "special.%s", (const char *)record->String);
        kv_unum(key, record->Count);
        named++;
    }
    kv_check(error == 0 && special.header.RecordCountSupplied != 0, "S2_GETSPECIALSTATS answers",
             "error %d, %lu records, %lu named", error, special.header.RecordCountSupplied, named);
}

/**
 * lists() - Tell whether a command list has @command.
 * @commands: the zero-ended list.
 * @command: the command.
 *
 * Return: TRUE when it is in the list.
 */
static BOOL lists(const UWORD *commands, UWORD command)
{
    for (; *commands != 0; commands++)
        if (*commands == command)
            return TRUE;
    return FALSE;
}

/**
 * check_nsd() - Check NSCMD_DEVICEQUERY: a SANA-II device whose command list
 * has both personalities' commands.
 * @io: the opened request.
 */
static void check_nsd(struct IOSana2Req *io)
{
    struct NSDeviceQueryResult result;
    memset(&result, 0, sizeof(result));
    struct IOStdReq *standard = (struct IOStdReq *)io; /* NSD's view of the request */
    standard->io_Data = &result;
    standard->io_Length = sizeof(result);
    BYTE error = rig_command(io, NSCMD_DEVICEQUERY);
    const UWORD *commands = result.nsdqr_SupportedCommands;
    BOOL both = commands != NULL && lists(commands, NETDEV_CMD_ATTACH) && lists(commands, S2_DEVICEQUERY) &&
                lists(commands, S2_SAMPLE_THROUGHPUT) && lists(commands, NSCMD_DEVICEQUERY);
    kv_check(error == 0 && result.nsdqr_DeviceType == NSDEVTYPE_SANA2 && both,
             "NSCMD_DEVICEQUERY names SANA-II and lists both command sets", "error %d, type %u, %s", error,
             result.nsdqr_DeviceType, both ? "both sets listed" : "a set missing");
}

/**
 * command_query() - GenetTest QUERY: everything an opener may ask whoever
 * holds the unit (genet-stats asks it beside either stack).
 * @rig: the rig.
 * @argument: unused.
 */
void command_query(struct Rig *rig, ULONG argument)
{
    (void)argument;
    check_device_query_full(rig->io);
    check_device_query_base(rig->io);
    check_device_query_short(rig->io);
    check_station_address(rig->io);
    check_global_stats(rig->io);
    check_special_stats(rig->io);
    check_nsd(rig->io);
}

/**
 * holder_by_multicast() - Tell netdev from SANA-II once the unit is held: a
 * multicast address is refused beside netdev, and taken (then given back)
 * beside SANA-II. A driver without SANA-II commands (genet 4.x) can only be
 * held by netdev.
 * @io: the opened request.
 *
 * Return: "netdev", "sana2", or "unknown" with the error noted.
 */
static const char *holder_by_multicast(struct IOSana2Req *io)
{
    static const UBYTE group[ETHER_ADDRESS_BYTES] = HOLDER_PROBE_GROUP;
    memcpy(io->ios2_SrcAddr, group, ETHER_ADDRESS_BYTES);
    BYTE error = rig_command(io, S2_ADDMULTICASTADDRESS);
    if (error == S2ERR_OUTOFSERVICE)
        return "netdev";
    if (error == IOERR_NOCMD)
    {
        kv_note("the driver knows no SANA-II commands: only netdev can hold it");
        return "netdev";
    }
    if (error == 0)
    {
        memcpy(io->ios2_SrcAddr, group, ETHER_ADDRESS_BYTES);
        rig_command(io, S2_DELMULTICASTADDRESS);
        return "sana2";
    }
    kv_note("S2_ADDMULTICASTADDRESS answered error %d", error);
    return "unknown";
}

/**
 * command_holder() - GenetTest HOLDER: who holds the unit, as holder=netdev,
 * sana2 or none, without taking it.
 * @rig: the rig.
 * @argument: unused.
 *
 * NETDEV_CMD_ATTACH with ABI version 0 answers NDERR_BUSY when anyone holds
 * the unit and NDERR_BADVERSION when nobody does; which personality holds it
 * then shows in how a multicast address is answered. A driver without netdev
 * (genet 3.x) answers IOERR_NOCMD, and only the multicast answer tells.
 */
void command_holder(struct Rig *rig, ULONG argument)
{
    (void)argument;
    BYTE error = rig_attach_probe(rig->io);
    const char *holder = "unknown";
    if (error == NDERR_BADVERSION)
        holder = "none";
    else if (error == NDERR_BUSY || error == IOERR_NOCMD) /* NOCMD: a SANA-II-only driver (genet 3.x) */
        holder = holder_by_multicast(rig->io);
    kv_str("holder", holder);
    kv_check(strcmp(holder, "unknown") != 0, "the holder is known", "NETDEV_CMD_ATTACH error %d, %s", error, holder);
}
