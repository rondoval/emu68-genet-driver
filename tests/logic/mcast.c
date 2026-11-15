// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * mcast.c - the logic tier's program for sana2_mcast.c (tests/README.md): the
 * openers' multicast ranges, flattened into the reception filter, which
 * bcmgenet-mac.c programs into the MDF registers of a fake register block.
 *
 *     LogicMcast <step>...
 *
 * Each step, then the state after it as step.N.*:
 *
 *     add=<address>[-<address>]   S2_ADDMULTICASTADDRESS(ES) of an opener
 *     del=<address>[-<address>]   S2_DELMULTICASTADDRESS(ES)
 *     promiscuous=on|off          an opener opened with SANA2OPF_PROM comes or goes
 *     online=on|off               the SANA-II side online: the filter reaches the registers
 *     netdev=on|off               a netdev stack holds the unit, with a filter of its own
 *     nomem                       the next allocation fails
 *     wanted=<address>            sana2_multicast_wanted() for a frame to it
 *     clear                       sana2_multicast_clear(), the last close
 */

#include "genet.device/src/sana2_mcast.c"

#include <string.h>

#include <genet/unimac.h>

#include "kv.h"
#include "support.h"

#define SLOT_REGISTERS 2u     /* an MDF slot: the address's top two bytes, then the other four */
#define SLOT_HIGH_MASK 0xFFFFu /* the top two bytes, in the first register's low half */
#define REGISTER_BYTES 4u

/* The opener that opened promiscuously; on the unit's list while promiscuous=on. */
static struct Opener promiscuousOpener;
/* A netdev stack's ops: the unit is held by netdev while ndStackOps is set. */
static const struct NetDevStackOps netdevOps;
/* The netdev stack's own filter, which the SANA-II side must leave alone. */
static const UBYTE netdevGroup[MAC_ADDRESS_BYTES] = {0x01, 0x00, 0x5e, 0x7f, 0xff, 0xfa};

/**
 * range_request() - Make an S2_(ADD|DEL)MULTICASTADDRESS(ES) of @operands, and print its outcome.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: <address> or <address>-<address>.
 * @add: TRUE to add, FALSE to delete.
 *
 * Return: FALSE for operands that are not an address or a range.
 */
static BOOL range_request(struct GenetUnit *unit, const char *prefix, const char *operands, BOOL add)
{
    struct IOSana2Req io;

    memset(&io, 0, sizeof(io));
    ULONG addresses = logic_parse_range(operands, io.ios2_SrcAddr, io.ios2_DstAddr);
    if (addresses == 0)
        return FALSE;
    if (add)
        io.ios2_Req.io_Command = addresses == 2 ? S2_ADDMULTICASTADDRESSES : S2_ADDMULTICASTADDRESS;
    else
        io.ios2_Req.io_Command = addresses == 2 ? S2_DELMULTICASTADDRESSES : S2_DELMULTICASTADDRESS;

    ULONG outcome = add ? sana2_add_multicast(unit, &io) : sana2_del_multicast(unit, &io);
    logic_print_flag(prefix, ".processed", outcome == COMMAND_PROCESSED);
    logic_print_text(prefix, ".error", logic_error_name(io.ios2_Req.io_Error));
    logic_print_text(prefix, ".wire_error", logic_wire_error_name(io.ios2_WireError));
    return TRUE;
}

/**
 * step_add() - S2_ADDMULTICASTADDRESS(ES): add=<address>[-<address>].
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the address or range.
 *
 * Return: FALSE for operands that are neither.
 */
static BOOL step_add(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    return operands != NULL && range_request(unit, prefix, operands, TRUE);
}

/**
 * step_del() - S2_DELMULTICASTADDRESS(ES): del=<address>[-<address>].
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the address or range.
 *
 * Return: FALSE for operands that are neither.
 */
static BOOL step_del(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    return operands != NULL && range_request(unit, prefix, operands, FALSE);
}

/**
 * step_promiscuous() - The promiscuous opener comes or goes, and the filter is updated.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: on or off.
 *
 * Return: FALSE for neither.
 */
static BOOL step_promiscuous(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    struct ExecBase *SysBase = unit->sysBase;
    BOOL on;

    (void)prefix;
    if (!logic_switch(operands, &on))
        return FALSE;
    if (on == promiscuousOpener.promiscuous)
        return TRUE;
    promiscuousOpener.promiscuous = on;
    if (on)
        AddTailMinList(&unit->s2->openers, &promiscuousOpener.node);
    else
        RemoveMinNode(&promiscuousOpener.node);
    sana2_filter_update(unit);
    return TRUE;
}

/**
 * step_online() - The SANA-II side online or not: online, the filter reaches the registers.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: on or off.
 *
 * Return: FALSE for neither.
 */
static BOOL step_online(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    BOOL on;
    (void)prefix;
    if (!logic_switch(operands, &on))
        return FALSE;
    unit->s2->online = on;
    return TRUE;
}

/**
 * step_netdev() - A netdev stack holds the unit, with a filter of its own, or lets it go.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: on or off.
 *
 * Return: FALSE for neither.
 */
static BOOL step_netdev(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    BOOL on;
    (void)prefix;
    if (!logic_switch(operands, &on))
        return FALSE;
    unit->ndStackOps = on ? &netdevOps : NULL;
    if (on)
    {
        unit->ndPromisc = FALSE;
        unit->ndMcastCount = 1;
        memcpy(unit->ndMcastList[0], netdevGroup, sizeof(netdevGroup));
    }
    return TRUE;
}

/**
 * step_nomem() - The next allocation fails.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: none.
 *
 * Return: FALSE for operands.
 */
static BOOL step_nomem(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    (void)unit;
    (void)prefix;
    logic_fail_allocations(1);
    return operands == NULL;
}

/**
 * step_wanted() - Whether sana2_multicast_wanted() takes a frame to an address.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the address.
 *
 * Return: FALSE for operands that are no address.
 */
static BOOL step_wanted(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    UBYTE address[MAC_ADDRESS_BYTES];
    if (operands == NULL || !logic_parse_address(operands, address))
        return FALSE;
    logic_print_flag(prefix, ".wanted", sana2_multicast_wanted(unit->s2, address));
    return TRUE;
}

/**
 * step_clear() - sana2_multicast_clear(), as the last close makes it.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: none.
 *
 * Return: FALSE for operands.
 */
static BOOL step_clear(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    (void)prefix;
    sana2_multicast_clear(unit);
    return operands == NULL;
}

/**
 * print_ranges() - The ranges listed, each with its use count.
 * @unit: the unit.
 * @prefix: the step's prefix.
 */
static void print_ranges(const struct GenetUnit *unit, const char *prefix)
{
    char key[LOGIC_KEY_TEXT];
    UBYTE address[MAC_ADDRESS_BYTES];
    ULONG count = 0;

    for (struct MinNode *node = unit->s2->multicastRanges.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        const struct MulticastRange *range = (const struct MulticastRange *)node;
        address_bytes(range->lowerBound, address);
        logic_key(key, prefix, ".range.%lu.lower", (unsigned long)count);
        logic_print_address(key, address);
        address_bytes(range->upperBound, address);
        logic_key(key, prefix, ".range.%lu.upper", (unsigned long)count);
        logic_print_address(key, address);
        logic_key(key, prefix, ".range.%lu.uses", (unsigned long)count);
        kv_num(key, range->useCount);
        count++;
    }
    logic_print_number(prefix, ".ranges", count);
}

/**
 * print_registers() - What the MDF registers and UMAC_CMD hold: the enable
 * mask, each enabled slot's address, and the promiscuous bit.
 * @unit: the unit.
 * @prefix: the step's prefix.
 */
static void print_registers(const struct GenetUnit *unit, const char *prefix)
{
    char key[LOGIC_KEY_TEXT];
    UBYTE address[MAC_ADDRESS_BYTES];
    ULONG enabled = logic_register(unit, UMAC_MDF_CTRL);

    logic_key(key, prefix, ".mdf_ctrl");
    kv_hex(key, enabled);
    for (ULONG slot = 0; slot < MAX_MDF_FILTER; slot++)
    {
        if (!(enabled & (1UL << (MAX_MDF_FILTER - 1 - slot))))
            continue;
        ULONG high = logic_register(unit, UMAC_MDF_ADDR + slot * SLOT_REGISTERS * REGISTER_BYTES);
        ULONG low = logic_register(unit, UMAC_MDF_ADDR + (slot * SLOT_REGISTERS + 1) * REGISTER_BYTES);
        address_bytes(((u64)(high & SLOT_HIGH_MASK) << 32) | low, address);
        logic_key(key, prefix, ".mdf.%lu", (unsigned long)slot);
        logic_print_address(key, address);
    }
    logic_print_flag(prefix, ".umac_promiscuous", (logic_register(unit, UMAC_CMD) & CMD_PROMISC) != 0);
}

/**
 * print_state() - The unit's filter after a step: ranges, the core's filter, the registers, the calls.
 * @unit: the unit.
 * @prefix: the step's prefix.
 */
static void print_state(const struct GenetUnit *unit, const char *prefix)
{
    char key[LOGIC_KEY_TEXT];

    print_ranges(unit, prefix);
    logic_print_flag(prefix, ".promiscuous", unit->ndPromisc);
    for (ULONG slot = 0; slot < unit->ndMcastCount; slot++)
    {
        logic_key(key, prefix, ".slot.%lu", (unsigned long)slot);
        logic_print_address(key, unit->ndMcastList[slot]);
    }
    logic_print_number(prefix, ".slots", unit->ndMcastCount);
    print_registers(unit, prefix);
    logic_key(key, prefix, ".forbids");
    kv_snum(key, logic_forbids());
    logic_print_calls(prefix);
}

static const struct LogicStep steps[] = {
    {"add", step_add},       {"del", step_del},       {"promiscuous", step_promiscuous},
    {"online", step_online}, {"netdev", step_netdev}, {"nomem", step_nomem},
    {"wanted", step_wanted}, {"clear", step_clear},
};

static const struct LogicProgram program = {"mcast", steps, LOGIC_COUNT_OF(steps), print_state};

/**
 * main() - Make each step and print the filter after it.
 * @argc: the argument count.
 * @argv: the steps.
 *
 * Return: logic_main()'s.
 */
int main(int argc, char **argv)
{
    return logic_main(&program, logic_unit_create(), argc, argv);
}
