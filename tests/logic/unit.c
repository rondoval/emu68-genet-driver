// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * unit.c - the unit the logic tier's programs run the driver on (support.h),
 * and the printing they share.
 */

#include <proto/exec.h>
#include <exec/memory.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <iomem.h>
#include <device.h>
#include <sana2_unit.h>

#include <emu68test/kv.h>
#include "support.h"

#define POOL_PUDDLE_BYTES 4096u
#define POOL_THRESHOLD_BYTES 1024u
#define ADDRESS_BYTES 6u

/* A locally administered station address. */
static const UBYTE stationAddress[ADDRESS_BYTES] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

/* The unit task as Signal() sees it: never run, only named. */
static struct Task unitTask;

/**
 * allocated() - Memory the run cannot do without.
 * @size: its size, cleared.
 *
 * Return: the memory; the run ends with KV_RC_ENVIRONMENT without it.
 */
static APTR allocated(ULONG size)
{
    APTR memory = AllocVec(size, MEMF_CLEAR);
    if (memory == NULL)
    {
        kv_environment("no memory for the unit");
        exit(kv_end(KV_RC_ENVIRONMENT));
    }
    return memory;
}

/**
 * min_list_init() - Make @list empty.
 * @list: the list.
 */
static void min_list_init(struct MinList *list)
{
    list->mlh_Head = (struct MinNode *)&list->mlh_Tail;
    list->mlh_Tail = NULL;
    list->mlh_TailPred = (struct MinNode *)&list->mlh_Head;
}

/**
 * logic_unit_create() - A unit with its device, SANA-II side, register block and pool.
 *
 * Offline, unclaimed, held by no stack, its station address stationAddress.
 *
 * Return: the unit; the run ends with KV_RC_ENVIRONMENT without memory.
 */
struct GenetUnit *logic_unit_create(void)
{
    struct GenetDevice *device = allocated(sizeof(struct GenetDevice));
    struct GenetUnit *unit = allocated(sizeof(struct GenetUnit));
    struct GenetS2 *s2 = allocated(sizeof(struct GenetS2));

    device->sysBase = logic_exec();
    device->utilityBase = logic_utility();
    device->unit = unit;
    unit->sysBase = logic_exec();
    unit->device = device;
    unit->s2 = s2;
    unit->task = &unitTask;
    unit->rx_signal = LOGIC_RX_SIGNAL;
    unit->tx_signal = LOGIC_TX_SIGNAL;
    unit->genetBase = allocated(LOGIC_REGISTER_BYTES);
    unit->metaPool = CreatePool(MEMF_CLEAR, POOL_PUDDLE_BYTES, POOL_THRESHOLD_BYTES);
    if (unit->metaPool == NULL)
    {
        kv_environment("no memory for the unit's pool");
        exit(kv_end(KV_RC_ENVIRONMENT));
    }
    for (ULONG index = 0; index < ADDRESS_BYTES; index++)
        unit->currentMacAddress[index] = stationAddress[index];

    min_list_init(&s2->openers);
    min_list_init(&s2->issued);
    min_list_init(&s2->txBacklog);
    min_list_init(&s2->onlineWaiters);
    min_list_init(&s2->multicastRanges);
    return unit;
}

/**
 * logic_register() - Read a register of the unit's block, as the driver reads it.
 * @unit: the unit.
 * @offset: the register's offset.
 *
 * Return: its value.
 */
ULONG logic_register(const struct GenetUnit *unit, ULONG offset)
{
    return mmio_read32((const UBYTE *)unit->genetBase + offset);
}

/**
 * logic_register_set() - Set a register of the unit's block, as the hardware would.
 * @unit: the unit.
 * @offset: the register's offset.
 * @value: its value.
 */
void logic_register_set(struct GenetUnit *unit, ULONG offset, ULONG value)
{
    mmio_write32(value, (UBYTE *)unit->genetBase + offset);
}

/**
 * logic_format_address() - Write an Ethernet address as aa:bb:cc:dd:ee:ff.
 * @text: room for LOGIC_ADDRESS_TEXT characters.
 * @address: its six bytes.
 */
void logic_format_address(char *text, const UBYTE *address)
{
    sprintf(text, "%02x:%02x:%02x:%02x:%02x:%02x", address[0], address[1], address[2], address[3], address[4],
            address[5]);
}

/**
 * logic_print_address() - Print an Ethernet address as one fact.
 * @key: its key.
 * @address: its six bytes.
 */
void logic_print_address(const char *key, const UBYTE *address)
{
    char text[LOGIC_ADDRESS_TEXT];
    logic_format_address(text, address);
    kv_str(key, text);
}

/**
 * hex_digit() - One hexadecimal digit's value.
 * @character: the digit.
 *
 * Return: 0 to 15, or -1 for a character that is not one.
 */
static LONG hex_digit(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

/**
 * logic_parse_address() - Read an Ethernet address written aa:bb:cc:dd:ee:ff.
 * @text: the text, from the address on; what follows it is not read.
 * @address: its six bytes.
 *
 * Return: TRUE, FALSE when @text does not start with an address.
 */
BOOL logic_parse_address(const char *text, UBYTE *address)
{
    for (ULONG index = 0; index < ADDRESS_BYTES; index++)
    {
        LONG high = hex_digit(text[0]);
        LONG low = high < 0 ? -1 : hex_digit(text[1]);
        if (low < 0 || (index + 1 < ADDRESS_BYTES && text[2] != ':'))
            return FALSE;
        address[index] = (UBYTE)((high << 4) | low);
        text += 3;
    }
    return TRUE;
}

/**
 * logic_parse_range() - Read <address> or <address>-<address>.
 * @text: the text.
 * @lower: the first address.
 * @upper: the second, when there is one.
 *
 * Return: how many addresses: 1, 2, or 0 when @text is neither.
 */
ULONG logic_parse_range(const char *text, UBYTE *lower, UBYTE *upper)
{
    if (!logic_parse_address(text, lower))
        return 0;
    if (text[LOGIC_ADDRESS_TEXT - 1] == '\0')
        return 1;
    if (text[LOGIC_ADDRESS_TEXT - 1] != '-' || !logic_parse_address(text + LOGIC_ADDRESS_TEXT, upper))
        return 0;
    return 2;
}

/**
 * logic_name_call() - Name one of the driver stand-ins' calls, for logic_print_calls().
 * @call: the call.
 * @value: room for LOGIC_VALUE_TEXT characters: the events by name, else left empty (printed as hex).
 *
 * Return: the kind's name, or NULL for a kind that is not genet's.
 */
const char *logic_name_call(const struct LogicCall *call, char *value)
{
    switch (call->kind)
    {
    case LOGIC_EVENTS:
        logic_events_text(value, call->value);
        return "events";
    case LOGIC_SET_RX_MODE:
        return "set_rx_mode";
    case LOGIC_IRQ0_ENABLE:
        return "irq0_enable";
    case LOGIC_TX_ABORT:
        return "tx_abort";
    case LOGIC_TX_IRQ_ARM:
        return "tx_irq_arm";
    default:
        return NULL;
    }
}
