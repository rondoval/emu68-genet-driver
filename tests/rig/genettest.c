// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * GenetTest - the genet.device suite's rig on the Amiga (tests/README.md).
 * Built, never installed. The first argument names the command, the second
 * is its number (seconds, a count) or ON / OFF:
 *
 *   QUERY            what any opener may ask, whoever holds the unit, and
 *                    every special stat as special.<name>=<count>
 *   HOLDER           who holds the unit: holder=netdev|sana2|none
 *   NETDEV           a netdev stack holds it: the refusals and the claim
 *   SANA2            a SANA-II stack holds it online: the claim, writes, reads
 *   BAREOPEN         the opens the driver takes and the ones it refuses
 *   CLOSE            closes carrying something other than the driver's Opener
 *   MCAST [secs]     the multicast filter's phases, the PC's sender running
 *   ONLINECYCLE [n]  S2_OFFLINE / S2_ONLINE and their events
 *   CABLE [secs]     the link events while someone pulls the cable and puts it back
 *   ABORT            every kind of kept request taken back
 *   MIAMI ON|OFF     the copy lengths with USE_MIAMI_WORKAROUND as set
 *
 * Output in kv.h's grammar; return codes 0 passed, 5 a check failed, 10 the
 * device or the timer did not open, 20 usage.
 */

#include "rig.h"
#include "../common/kv.h"

#include <proto/exec.h>

#include <exec/io.h>

#include <stdio.h>
#include <stdlib.h>

static const char verstag[] __attribute__((used)) = VERSTAG;

struct Device *TimerBase;

struct Command
{
    const char *name;
    void (*run)(struct Rig *rig, ULONG argument);
};

static const struct Command commands[] = {
    {"QUERY", command_query},
    {"HOLDER", command_holder},
    {"NETDEV", command_netdev},
    {"SANA2", command_sana2},
    {"BAREOPEN", command_bareopen},
    {"CLOSE", command_close},
    {"MCAST", command_mcast},
    {"ONLINECYCLE", command_onlinecycle},
    {"CABLE", command_cable},
    {"ABORT", command_abort},
    {"MIAMI", command_miami},
};

/**
 * find_command() - The command @word names.
 * @word: the first argument.
 *
 * Return: the command; NULL when there is none of that name.
 */
static const struct Command *find_command(const char *word)
{
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (rig_same_word(word, commands[i].name))
            return &commands[i];
    return NULL;
}

/**
 * argument_of() - The command's number from its second argument.
 * @word: the second argument, or NULL.
 * @argument: where the number goes; 0 without a second argument.
 *
 * Return: TRUE when @word is a number, ON or OFF, or absent.
 */
static BOOL argument_of(const char *word, ULONG *argument)
{
    *argument = 0;
    if (word == NULL || rig_same_word(word, "OFF"))
        return TRUE;
    if (rig_same_word(word, "ON"))
    {
        *argument = 1;
        return TRUE;
    }
    char *end;
    *argument = strtoul(word, &end, 10);
    return *end == '\0' && end != word;
}

/**
 * rig_open() - Open the timer and the rig's opener.
 * @rig: the rig, cleared.
 *
 * Return: TRUE when both are open; FALSE with the reason reported
 * (kv_environment()) and what was made left for rig_close().
 */
static BOOL rig_open(struct Rig *rig)
{
    rig->port = CreateMsgPort();
    rig->timerPort = CreateMsgPort();
    if (rig->port == NULL || rig->timerPort == NULL)
    {
        kv_environment("no message ports");
        return FALSE;
    }
    rig->timer = (struct timerequest *)CreateIORequest(rig->timerPort, sizeof(struct timerequest));
    rig->io = (struct IOSana2Req *)CreateIORequest(rig->port, sizeof(struct IOSana2Req));
    if (rig->timer == NULL || rig->io == NULL)
    {
        kv_environment("no memory for the requests");
        return FALSE;
    }
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)rig->timer, 0) != 0)
    {
        rig->timer->tr_node.io_Device = NULL;
        kv_environment("no timer.device");
        return FALSE;
    }
    TimerBase = rig->timer->tr_node.io_Device;
    rig->io->ios2_BufferManagement = rig_buffer_tags();
    BYTE error = OpenDevice((CONST_STRPTR)GENET_DEVICE, GENET_UNIT, (struct IORequest *)rig->io, 0);
    if (error != 0)
    {
        rig->io->ios2_Req.io_Device = NULL;
        kv_environment("%s unit %d does not open (error %d)", GENET_DEVICE, GENET_UNIT, error);
        return FALSE;
    }
    return TRUE;
}

/**
 * rig_close() - Close and free what rig_open() made.
 * @rig: the rig.
 */
static void rig_close(struct Rig *rig)
{
    if (rig->io != NULL)
    {
        if (rig->io->ios2_Req.io_Device != NULL)
            CloseDevice((struct IORequest *)rig->io);
        DeleteIORequest((struct IORequest *)rig->io);
    }
    if (rig->timer != NULL)
    {
        if (rig->timer->tr_node.io_Device != NULL)
            CloseDevice((struct IORequest *)rig->timer);
        DeleteIORequest((struct IORequest *)rig->timer);
    }
    if (rig->timerPort != NULL)
        DeleteMsgPort(rig->timerPort);
    if (rig->port != NULL)
        DeleteMsgPort(rig->port);
}

/**
 * usage() - Print the usage line.
 *
 * Return: KV_RC_USAGE.
 */
static int usage(void)
{
    printf("usage: GenetTest QUERY|HOLDER|NETDEV|SANA2|BAREOPEN|CLOSE|MCAST [secs]|ONLINECYCLE [n]|"
           "CABLE [secs]|ABORT|MIAMI ON|OFF\n");
    return KV_RC_USAGE;
}

/**
 * main() - Run the command the arguments name.
 * @argc: the argument count.
 * @argv: the command and its optional number.
 *
 * Return: kv_end()'s code; KV_RC_USAGE for a usage error.
 */
int main(int argc, char **argv)
{
    const struct Command *command = argc > 1 ? find_command(argv[1]) : NULL;
    ULONG argument;
    if (command == NULL || argc > 3 || !argument_of(argc > 2 ? argv[2] : NULL, &argument))
        return usage();

    kv_begin(command->name);
    struct Rig rig = {0};
    if (rig_open(&rig))
        command->run(&rig, argument);
    rig_close(&rig);
    return kv_end();
}
