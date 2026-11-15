// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * netdev.c - the logic tier's program for netdev's commands (tests/README.md):
 * netdev_api.c's NETDEV_CMD_SET_COALESCE, SET_RX_PROFILE, SET_RXFILTER and
 * SET_MAC, with bcmgenet-dma.c's interrupt moderation (its limits, static
 * settings and RX burst ladder) over a fake register block.
 *
 *     LogicNetdev <step>...
 *
 * Each step, then the state after it as step.N.*:
 *
 *     valid=<tx>,<rx>,<usecs>      bcmgenet_coalesce_valid()
 *     coalesce=<tx>,<rx>,<usecs>   NETDEV_CMD_SET_COALESCE (ndcl_TxMaxFrames, ndcl_RxMaxFrames, ndcl_RxUsecs)
 *     profile=<n>                  NETDEV_CMD_SET_RX_PROFILE (NDRP_*)
 *     holder=netdev|sana2|none     who holds the unit
 *     start                        the unit goes online: ring init programs the moderation
 *     stop                         back to configured
 *     frames=<n>                   n frames wait in the RX ring (RDMA_PROD_INDEX)
 *     held=<n>, batch=<n>          RX buffers the stack holds, and its poll batch
 *     moderate                     bcmgenet_rx_moderate(): an RX pass is about to re-arm
 *     notask                       no unit task to signal
 *     filter=<flags>,<count>[,null]   NETDEV_CMD_SET_RXFILTER: NDFF_* flags and so many groups
 *                                  01:00:5e:00:00:01 on, or a NULL list
 *     mac=<address>|short          NETDEV_CMD_SET_MAC, or one 5 bytes short
 *     owner=other|none             another opener attached, or nobody did
 */

#include "genet.device/src/bcmgenet-dma.c"

#include <string.h>

#include <sana2_unit.h>

#include "kv.h"
#include "support.h"

#define DEFAULT_RX_BATCH 32u /* a stack's poll batch until a step says otherwise */
#define COALESCE_NUMBERS 3u  /* tx frames, rx frames, rx usecs */
#define FILTER_NUMBERS 2u    /* flags, groups */
#define GROUPS_MAX 32u       /* past GENET_MDF_MCAST_MAX, for the fallback */
#define ADDRESS_BYTES 6u

/* A netdev stack's ops: the unit is held by netdev while ndStackOps is set. */
static const struct NetDevStackOps netdevOps;
/* The reply port of another opener's ATTACH: the commands here come without one. */
static struct MsgPort otherOpener;
static UBYTE groups[GROUPS_MAX][ADDRESS_BYTES];

/**
 * send_command() - Send a netdev command with @data, as the unit task would, and print its io_Error.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @command: the NETDEV_CMD_*.
 * @data: its payload.
 * @length: the payload's size.
 *
 * The io_Error prints as 0, or as the error's name.
 */
static void send_command(struct GenetUnit *unit, const char *prefix, UWORD command, APTR data, ULONG length)
{
    struct IOStdReq io;

    memset(&io, 0, sizeof(io));
    io.io_Command = command;
    io.io_Data = data;
    io.io_Length = length;
    BYTE error = netdev_process_command(unit, &io);
    logic_print_text(prefix, ".error", error == 0 ? "0" : logic_error_name(error));
}

/**
 * step_valid() - Whether bcmgenet_coalesce_valid() takes <tx>,<rx>,<usecs>.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the three numbers.
 *
 * Return: FALSE for operands that are not three numbers.
 */
static BOOL step_valid(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    ULONG numbers[COALESCE_NUMBERS];
    (void)unit;
    if (!logic_numbers(operands, numbers, COALESCE_NUMBERS))
        return FALSE;
    logic_print_flag(prefix, ".valid", bcmgenet_coalesce_valid(numbers[0], numbers[1], numbers[2]) == GENET_OK);
    return TRUE;
}

/**
 * step_coalesce() - NETDEV_CMD_SET_COALESCE of <tx>,<rx>,<usecs>.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the three numbers.
 *
 * Return: FALSE for operands that are not three numbers.
 */
static BOOL step_coalesce(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    ULONG numbers[COALESCE_NUMBERS];
    if (!logic_numbers(operands, numbers, COALESCE_NUMBERS))
        return FALSE;
    struct NetDevCoalesce coalesce = {.ndcl_RxUsecs = numbers[2],
                                      .ndcl_RxMaxFrames = (UWORD)numbers[1],
                                      .ndcl_TxMaxFrames = (UWORD)numbers[0]};
    send_command(unit, prefix, NETDEV_CMD_SET_COALESCE, &coalesce, sizeof(coalesce));
    return TRUE;
}

/**
 * step_profile() - NETDEV_CMD_SET_RX_PROFILE of an NDRP_* number.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the number.
 *
 * Return: FALSE for operands that are not one number.
 */
static BOOL step_profile(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    ULONG number;
    if (!logic_numbers(operands, &number, 1))
        return FALSE;
    struct NetDevRxProfile profile = {.ndrp_Profile = (UWORD)number};
    send_command(unit, prefix, NETDEV_CMD_SET_RX_PROFILE, &profile, sizeof(profile));
    return TRUE;
}

/**
 * step_holder() - A netdev stack, a SANA-II claimer or nobody holds the unit.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: netdev, sana2 or none.
 *
 * Return: FALSE for another word.
 */
static BOOL step_holder(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    (void)prefix;
    if (operands == NULL)
        return FALSE;
    BOOL netdev = strcmp(operands, "netdev") == 0;
    BOOL sana2 = strcmp(operands, "sana2") == 0;
    unit->ndStackOps = netdev ? &netdevOps : NULL;
    unit->s2->claimers = sana2 ? 1u : 0u;
    return netdev || sana2 || strcmp(operands, "none") == 0;
}

/**
 * step_start() - The unit goes online, and ring init programs the moderation (bcmgenet_apply_coalesce()).
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: none.
 *
 * Return: FALSE for operands.
 */
static BOOL step_start(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    (void)prefix;
    unit->state = STATE_ONLINE;
    bcmgenet_apply_coalesce(unit);
    return operands == NULL;
}

/**
 * step_stop() - The unit goes back to configured.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: none.
 *
 * Return: FALSE for operands.
 */
static BOOL step_stop(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    (void)prefix;
    unit->state = STATE_CONFIGURED;
    return operands == NULL;
}

/**
 * step_frames() - So many frames wait in the RX ring: RDMA_PROD_INDEX past the consumer.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the number.
 *
 * Return: FALSE for operands that are not one number.
 */
static BOOL step_frames(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    ULONG frames;
    (void)prefix;
    if (!logic_numbers(operands, &frames, 1))
        return FALSE;
    logic_register_set(unit, RDMA_PROD_INDEX, (unit->rx_ring.rx_cons_index + frames) & DMA_P_INDEX_MASK);
    return TRUE;
}

/**
 * step_held() - The stack holds so many RX buffers.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the number.
 *
 * Return: FALSE for operands that are not one number.
 */
static BOOL step_held(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    ULONG held;
    (void)prefix;
    if (!logic_numbers(operands, &held, 1))
        return FALSE;
    unit->ndRxHeld = held;
    return TRUE;
}

/**
 * step_batch() - The stack's poll batch.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the number.
 *
 * Return: FALSE for operands that are not one number.
 */
static BOOL step_batch(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    ULONG batch;
    (void)prefix;
    if (!logic_numbers(operands, &batch, 1))
        return FALSE;
    unit->ndRxBatch = (u16)batch;
    return TRUE;
}

/**
 * step_moderate() - An RX pass is about to re-arm its interrupt: bcmgenet_rx_moderate().
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: none.
 *
 * Return: FALSE for operands.
 */
static BOOL step_moderate(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    (void)prefix;
    bcmgenet_rx_moderate(unit);
    return operands == NULL;
}

/**
 * step_filter() - NETDEV_CMD_SET_RXFILTER: filter=<flags>,<count>[,null].
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the NDFF_* flags, how many groups (01:00:5e:00:00:01 on), and null for a NULL list.
 *
 * Return: FALSE for operands that are not that.
 */
static BOOL step_filter(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    ULONG numbers[FILTER_NUMBERS];
    char given[LOGIC_KEY_TEXT];
    const char *null = operands != NULL ? strstr(operands, ",null") : NULL;
    size_t length = operands == NULL ? 0 : null != NULL ? (size_t)(null - operands) : strlen(operands);

    if (operands == NULL || length >= sizeof(given))
        return FALSE;
    memcpy(given, operands, length);
    given[length] = '\0';
    if (!logic_numbers(given, numbers, FILTER_NUMBERS) || numbers[1] > GROUPS_MAX)
        return FALSE;
    for (ULONG index = 0; index < numbers[1]; index++)
    {
        static const UBYTE first[ADDRESS_BYTES] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x00};
        memcpy(groups[index], first, ADDRESS_BYTES);
        groups[index][ADDRESS_BYTES - 1] = (UBYTE)(index + 1);
    }
    struct NetDevRxFilter filter = {.ndrx_Flags = (UWORD)numbers[0], .ndrx_NumMcast = (UWORD)numbers[1],
                                    .ndrx_McastList = null != NULL ? NULL : (const UBYTE(*)[ADDRESS_BYTES])groups};
    send_command(unit, prefix, NETDEV_CMD_SET_RXFILTER, &filter, sizeof(filter));
    return TRUE;
}

/**
 * step_mac() - NETDEV_CMD_SET_MAC: mac=<address>, or mac=short for a request one byte short.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: the address, or short.
 *
 * Return: FALSE for operands that are neither.
 */
static BOOL step_mac(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    static UBYTE address[ADDRESS_BYTES];
    BOOL whole = operands != NULL && strcmp(operands, "short") != 0;
    if (operands == NULL || (whole && !logic_parse_address(operands, address)))
        return FALSE;
    send_command(unit, prefix, NETDEV_CMD_SET_MAC, address, whole ? ADDRESS_BYTES : ADDRESS_BYTES - 1);
    return TRUE;
}

/**
 * step_owner() - Another opener attached, or nobody did (ndOwnerPort).
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: other or none.
 *
 * Return: FALSE for neither.
 */
static BOOL step_owner(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    (void)prefix;
    if (operands == NULL)
        return FALSE;
    unit->ndOwnerPort = strcmp(operands, "other") == 0 ? &otherOpener : NULL;
    return unit->ndOwnerPort != NULL || strcmp(operands, "none") == 0;
}

/**
 * step_notask() - There is no unit task to signal.
 * @unit: the unit.
 * @prefix: the step's prefix.
 * @operands: none.
 *
 * Return: FALSE for operands.
 */
static BOOL step_notask(struct GenetUnit *unit, const char *prefix, const char *operands)
{
    (void)prefix;
    unit->task = NULL;
    return operands == NULL;
}

/**
 * print_state() - The unit after a step: its moderation, the registers, its filter and address.
 * @unit: the unit.
 * @prefix: the step's prefix.
 */
static void print_state(const struct GenetUnit *unit, const char *prefix)
{
    char key[LOGIC_KEY_TEXT];

    logic_print_number(prefix, ".tx_frames", unit->coalTxFrames);
    logic_print_number(prefix, ".rx_frames", unit->coalRxFrames);
    logic_print_number(prefix, ".rx_usecs", unit->coalRxUsecs);
    logic_print_flag(prefix, ".pinned", unit->coalPinned);
    logic_print_number(prefix, ".profile", unit->rxProfile);
    logic_print_flag(prefix, ".ladder", unit->rxLadder);
    logic_print_number(prefix, ".level", unit->rxLevel);
    logic_print_number(prefix, ".level_changes", unit->internalStats.rx_level_changes);
    logic_print_number(prefix, ".bulk_arms", unit->internalStats.rx_bulk_arms);
    logic_print_number(prefix, ".reg_tx_frames", logic_register(unit, TDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH));
    logic_print_number(prefix, ".reg_rx_frames", logic_register(unit, RDMA_RING_REG_BASE + DMA_MBUF_DONE_THRESH));
    logic_print_number(prefix, ".reg_rx_ticks",
                       logic_register(unit, RDMA_REG_BASE + DMA_RING16_TIMEOUT) & DMA_TIMEOUT_MASK);
    logic_print_number(prefix, ".filter_flags", unit->ndFilterFlags);
    logic_print_flag(prefix, ".promiscuous", unit->ndPromisc);
    for (ULONG slot = 0; slot < unit->ndMcastCount; slot++)
    {
        logic_key(key, prefix, ".slot.%lu", (unsigned long)slot);
        logic_print_address(key, unit->ndMcastList[slot]);
    }
    logic_print_number(prefix, ".slots", unit->ndMcastCount);
    logic_key(key, prefix, ".mac");
    logic_print_address(key, unit->currentMacAddress);
    logic_key(key, prefix, ".forbids");
    kv_snum(key, logic_forbids());
    logic_print_calls(prefix);
}

static const struct LogicStep steps[] = {
    {"filter", step_filter}, {"mac", step_mac},         {"owner", step_owner},
    {"valid", step_valid}, {"coalesce", step_coalesce}, {"profile", step_profile}, {"holder", step_holder},
    {"start", step_start}, {"stop", step_stop},         {"frames", step_frames},   {"held", step_held},
    {"batch", step_batch}, {"moderate", step_moderate}, {"notask", step_notask},
};

static const struct LogicProgram program = {"netdev", steps, LOGIC_COUNT_OF(steps), print_state};

/**
 * main() - Seed the unit's moderation as UnitOpen() does, then make each step.
 * @argc: the argument count.
 * @argv: the steps.
 *
 * Return: logic_main()'s.
 */
int main(int argc, char **argv)
{
    struct GenetUnit *unit = logic_unit_create();

    unit->coalTxFrames = GENET_COAL_TX_FRAMES;
    unit->coalRxFrames = GENET_COAL_RX_FRAMES;
    unit->coalRxUsecs = GENET_COAL_RX_USECS;
    unit->rxProfile = NDRP_UNSTATED;
    unit->state = STATE_CONFIGURED;
    unit->ndRxBatch = DEFAULT_RX_BATCH;
    return logic_main(&program, unit, argc, argv);
}
