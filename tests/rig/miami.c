// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * miami.c - GenetTest MIAMI ON|OFF: what USE_MIAMI_WORKAROUND does to the
 * copies between the driver and the stack. With it on, every copy is rounded
 * up to a multiple of 4 bytes, as Miami DX expects; off, each copy is the
 * frame's own length. A SANA-II stack holds the unit online; the host set
 * the prefs and brought the interface up again.
 */

#include "rig.h"
#include "../common/kv.h"

#include <proto/exec.h>

#include <string.h>

#define LONGWORD_BYTES 4
#define FIRST_ODD_LENGTH 61   /* 61..64: every remainder modulo 4 */
#define LENGTH_COUNT 4
#define READS_WANTED 8
#define READ_MICROS 3000000UL /* the agent's own traffic brings IPv4 frames */

static UBYTE payload[FRAME_BUFFER_BYTES];
static UBYTE read_buffer[FRAME_BUFFER_BYTES];

/**
 * expected_copy() - The copy length the driver should ask for.
 * @length: the data's own length.
 * @miami: whether the workaround is on.
 *
 * Return: @length, rounded up to a longword when @miami.
 */
static ULONG expected_copy(ULONG length, BOOL miami)
{
    return miami ? (length + LONGWORD_BYTES - 1) & ~(ULONG)(LONGWORD_BYTES - 1) : length;
}

/**
 * check_writes() - Write each remainder's length and check what the driver
 * asked S2_CopyFromBuff for.
 * @io: the opened request.
 * @miami: whether the workaround is on.
 */
static void check_writes(struct IOSana2Req *io, BOOL miami)
{
    UBYTE own[ETHER_ADDRESS_BYTES];
    rig_station_address(io, own);
    ULONG right = 0;
    for (ULONG length = FIRST_ODD_LENGTH; length < FIRST_ODD_LENGTH + LENGTH_COUNT; length++)
    {
        rig_write_setup(io, own, payload, length);
        BYTE error = rig_command(io, CMD_WRITE);
        if (error == 0 && rig_copy_log.lastFromBuffLength == expected_copy(length, miami))
            right++;
        else
            kv_note("write of %lu bytes: error %d, copy of %lu", length, error, rig_copy_log.lastFromBuffLength);
    }
    kv_check(right == LENGTH_COUNT, miami ? "writes copy whole longwords" : "writes copy their own length",
             "%lu of %d", right, LENGTH_COUNT);
}

/**
 * check_reads() - Read IPv4 frames and check what the driver asked
 * S2_CopyToBuff for against each frame's length.
 * @rig: the rig.
 * @miami: whether the workaround is on.
 */
static void check_reads(struct Rig *rig, BOOL miami)
{
    struct IOSana2Req *io = rig->io;
    ULONG reads = 0;
    ULONG right = 0;
    for (ULONG i = 0; i < READS_WANTED; i++)
    {
        io->ios2_Req.io_Command = CMD_READ;
        io->ios2_Req.io_Flags = 0;
        io->ios2_Req.io_Error = 0;
        io->ios2_PacketType = ETHERTYPE_IPV4;
        io->ios2_Data = read_buffer;
        if (!rig_wait((struct IORequest *)io, rig->timer, READ_MICROS) || io->ios2_Req.io_Error != 0)
            break;
        reads++;
        if (rig_copy_log.lastToBuffLength == expected_copy(io->ios2_DataLength, miami))
            right++;
    }
    kv_check(reads != 0 && right == reads, miami ? "reads copy whole longwords" : "reads copy their own length",
             "%lu of %lu frames", right, reads);
}

/**
 * command_miami() - GenetTest MIAMI: the copy lengths, with the workaround
 * as the host set it.
 * @rig: the rig.
 * @argument: 1 when the host turned USE_MIAMI_WORKAROUND on, 0 when off.
 */
void command_miami(struct Rig *rig, ULONG argument)
{
    BOOL miami = argument != 0;
    kv_str("miami", miami ? "on" : "off");
    check_writes(rig->io, miami);
    check_reads(rig, miami);
}
