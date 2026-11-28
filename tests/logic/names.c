// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * names.c - io_Command, io_Error (S2ERR_*, IOERR_*, NDERR_*), ios2_WireError and S2EVENT_* by name
 * (support.h), so the tests on the host read names, not numbers.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/netdev.h>
#include <devices/newstyle.h>
#include <devices/sana2.h>

#include <stdio.h>
#include <string.h>

#include "support.h"

/* A constant and its name. */
struct Named
{
    LONG value;       /* the constant */
    const char *name; /* its name in the NDK */
};

#define NAMED(constant) {(LONG)(constant), #constant}
#define COUNT_OF(table) (sizeof(table) / sizeof((table)[0]))
#define EVENT_BITS 32u

static const struct Named errors[] = {
    NAMED(S2ERR_NO_ERROR),    NAMED(S2ERR_NO_RESOURCES), NAMED(S2ERR_BAD_ARGUMENT), NAMED(S2ERR_BAD_STATE),
    NAMED(S2ERR_BAD_ADDRESS), NAMED(S2ERR_MTU_EXCEEDED), NAMED(S2ERR_NOT_SUPPORTED), NAMED(S2ERR_SOFTWARE),
    NAMED(S2ERR_OUTOFSERVICE), NAMED(S2ERR_TX_FAILURE), NAMED(IOERR_OPENFAIL),     NAMED(IOERR_ABORTED),
    NAMED(IOERR_NOCMD),       NAMED(IOERR_BADLENGTH),    NAMED(IOERR_BADADDRESS),   NAMED(IOERR_UNITBUSY),
    NAMED(NDERR_BADVERSION),  NAMED(NDERR_BUSY),         NAMED(NDERR_NOTATTACHED),  NAMED(NDERR_BADPARAMS),
    NAMED(NDERR_NOMEM),
};

static const struct Named wireErrors[] = {
    NAMED(S2WERR_GENERIC_ERROR),   NAMED(S2WERR_NOT_CONFIGURED), NAMED(S2WERR_UNIT_ONLINE),
    NAMED(S2WERR_UNIT_OFFLINE),    NAMED(S2WERR_ALREADY_TRACKED), NAMED(S2WERR_NOT_TRACKED),
    NAMED(S2WERR_BUFF_ERROR),      NAMED(S2WERR_SRC_ADDRESS),    NAMED(S2WERR_DST_ADDRESS),
    NAMED(S2WERR_BAD_BROADCAST),   NAMED(S2WERR_BAD_MULTICAST),  NAMED(S2WERR_MULTICAST_FULL),
    NAMED(S2WERR_BAD_EVENT),       NAMED(S2WERR_BAD_STATDATA),   NAMED(S2WERR_IS_CONFIGURED),
    NAMED(S2WERR_NULL_POINTER),    NAMED(S2WERR_TOO_MANY_RETRIES), NAMED(S2WERR_RCVREL_HDW_ERR),
    NAMED(S2WERR_UNIT_DISCONNECTED), NAMED(S2WERR_UNIT_CONNECTED),
};

static const struct Named commands[] = {
    NAMED(CMD_READ),
    NAMED(CMD_WRITE),
    NAMED(CMD_FLUSH),
    NAMED(S2_DEVICEQUERY),
    NAMED(S2_GETSTATIONADDRESS),
    NAMED(S2_CONFIGINTERFACE),
    NAMED(S2_ADDMULTICASTADDRESS),
    NAMED(S2_DELMULTICASTADDRESS),
    NAMED(S2_MULTICAST),
    NAMED(S2_BROADCAST),
    NAMED(S2_TRACKTYPE),
    NAMED(S2_UNTRACKTYPE),
    NAMED(S2_GETTYPESTATS),
    NAMED(S2_GETSPECIALSTATS),
    NAMED(S2_GETGLOBALSTATS),
    NAMED(S2_ONEVENT),
    NAMED(S2_READORPHAN),
    NAMED(S2_ONLINE),
    NAMED(S2_OFFLINE),
    NAMED(S2_ADDMULTICASTADDRESSES),
    NAMED(S2_DELMULTICASTADDRESSES),
    NAMED(S2_GETPEERADDRESS),
    NAMED(S2_GETDNSADDRESS),
    NAMED(S2_GETEXTENDEDGLOBALSTATS),
    NAMED(S2_CONNECT),
    NAMED(S2_DISCONNECT),
    NAMED(S2_SAMPLE_THROUGHPUT),
    NAMED(NSCMD_DEVICEQUERY),
    NAMED(NETDEV_CMD_ATTACH),
    NAMED(NETDEV_CMD_DETACH),
    NAMED(NETDEV_CMD_START),
    NAMED(NETDEV_CMD_STOP),
    NAMED(NETDEV_CMD_SET_RXFILTER),
    NAMED(NETDEV_CMD_SET_COALESCE),
    NAMED(NETDEV_CMD_GET_STATS),
    NAMED(NETDEV_CMD_GET_LINK),
    NAMED(NETDEV_CMD_SET_MAC),
    NAMED(NETDEV_CMD_GET_COUNTERS),
    NAMED(NETDEV_CMD_SET_RX_PROFILE),
};

static const struct Named events[] = {
    NAMED(S2EVENT_ERROR),   NAMED(S2EVENT_TX),       NAMED(S2EVENT_RX),       NAMED(S2EVENT_ONLINE),
    NAMED(S2EVENT_OFFLINE), NAMED(S2EVENT_BUFF),     NAMED(S2EVENT_HARDWARE), NAMED(S2EVENT_SOFTWARE),
    NAMED(S2EVENT_CONFIGCHANGED), NAMED(S2EVENT_CONNECT), NAMED(S2EVENT_DISCONNECT),
};

/**
 * name_of() - A constant's name from @table.
 * @table: the names.
 * @count: how many.
 * @value: the constant.
 *
 * Return: its name, or "?" for one not in the table.
 */
static const char *name_of(const struct Named *table, ULONG count, LONG value)
{
    for (ULONG index = 0; index < count; index++)
        if (table[index].value == value)
            return table[index].name;
    return "?";
}

/**
 * logic_error_name() - An io_Error by name: S2ERR_* or IOERR_*.
 * @error: the io_Error.
 *
 * Return: its name, "?" for one the NDK does not name.
 */
const char *logic_error_name(LONG error)
{
    return name_of(errors, COUNT_OF(errors), error);
}

/**
 * logic_wire_error_name() - An ios2_WireError by name.
 * @wireError: the wire error.
 *
 * Return: its name, "?" for one the NDK does not name.
 */
const char *logic_wire_error_name(ULONG wireError)
{
    return name_of(wireErrors, COUNT_OF(wireErrors), (LONG)wireError);
}

/**
 * logic_command_name() - An io_Command by name: CMD_*, S2_*, NSCMD_* or NETDEV_CMD_*.
 * @command: the io_Command.
 *
 * Return: its name, "?" for one not in the table.
 */
const char *logic_command_name(UWORD command)
{
    return name_of(commands, COUNT_OF(commands), command);
}

/**
 * logic_events_text() - S2EVENT_* bits as their names joined by '|', "0" for none.
 * @text: room for LOGIC_EVENTS_TEXT characters.
 * @mask: the events.
 */
void logic_events_text(char *text, ULONG mask)
{
    text[0] = '\0';
    for (ULONG bit = 0; bit < EVENT_BITS; bit++)
    {
        ULONG event = 1UL << bit;
        if (!(mask & event))
            continue;
        if (text[0] != '\0')
            strcat(text, "|");
        const char *name = name_of(events, COUNT_OF(events), (LONG)event);
        if (name[0] == '?')
            sprintf(text + strlen(text), "0x%lx", (unsigned long)event);
        else
            strcat(text, name);
    }
    if (text[0] == '\0')
        strcpy(text, "0");
}
