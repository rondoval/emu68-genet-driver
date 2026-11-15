// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * fakelib.c - a library base whose jump table the program writes (support.h):
 * the functions it models jump to their stubs, every other slot calls
 * unexpected_call(), which names the library and the LVO and ends the run.
 */

#include <proto/exec.h>
#include <exec/libraries.h>
#include <exec/memory.h>

#include <stdlib.h>

#include "kv.h"
#include "support.h"

#define OPCODE_JMP_ABSOLUTE 0x4EF9u /* jmp (xxx).l: a modelled function */
#define OPCODE_JSR_ABSOLUTE 0x4EB9u /* jsr (xxx).l: an unexpected one, known by its return address */
#define FAKE_LIBRARIES_MAX 4u

/* One slot of a jump table. */
struct FakeEntry
{
    UWORD opcode; /* OPCODE_JMP_ABSOLUTE or OPCODE_JSR_ABSOLUTE */
    APTR target;  /* the stub, or unexpected_call() */
} __attribute__((packed));

/* A library made here. */
struct FakeLibrary
{
    const char *name;        /* for the failure line */
    struct FakeEntry *table; /* the first slot, the lowest address */
    struct Library *base;    /* just past the last slot */
};

static struct FakeLibrary fakes[FAKE_LIBRARIES_MAX];
static ULONG fakeCount;

/**
 * unexpected_call() - A slot that no stub models was called: name it and end the run.
 *
 * Reached by a jsr from the slot, so the return address is the slot's end.
 */
static void unexpected_call(void)
{
    const struct FakeEntry *entry = (const struct FakeEntry *)__builtin_return_address(0) - 1;

    for (ULONG index = 0; index < fakeCount; index++)
    {
        if (entry >= fakes[index].table && (APTR)entry < (APTR)fakes[index].base)
        {
            LONG lvo = (LONG)((const UBYTE *)entry - (const UBYTE *)fakes[index].base);
            kv_environment("%s LVO %ld called, which this program does not model", fakes[index].name, (long)lvo);
            exit(kv_end());
        }
    }
    kv_environment("an unexpected call from 0x%lx", (unsigned long)entry);
    exit(kv_end());
}

/**
 * fake_library_create() - Make a library base with its own jump table.
 * @name: the library's name, for the failure line.
 * @slots: how many LVOs the table holds (the largest |LVO| / 6).
 * @baseSize: the size of the base behind the table.
 * @image: what the base starts as (a real base to copy), or NULL for zeroes.
 * @functions: the modelled functions.
 * @count: how many there are.
 *
 * Return: the base; the run ends with KV_RC_ENVIRONMENT when there is no memory.
 */
struct Library *fake_library_create(const char *name, ULONG slots, ULONG baseSize, const void *image,
                                    const struct FakeFunction *functions, ULONG count)
{
    struct FakeEntry *table = AllocVec(slots * sizeof(struct FakeEntry) + baseSize, MEMF_CLEAR);
    if (table == NULL || fakeCount == FAKE_LIBRARIES_MAX)
    {
        kv_environment("no memory for the fake %s", name);
        exit(kv_end());
    }

    struct Library *base = (struct Library *)(table + slots);
    for (ULONG slot = 0; slot < slots; slot++)
    {
        table[slot].opcode = OPCODE_JSR_ABSOLUTE;
        table[slot].target = (APTR)unexpected_call;
    }
    for (ULONG index = 0; index < count; index++)
    {
        struct FakeEntry *entry = (struct FakeEntry *)((UBYTE *)base + functions[index].lvo);
        entry->opcode = OPCODE_JMP_ABSOLUTE;
        entry->target = functions[index].code;
    }
    if (image != NULL)
        CopyMem((APTR)image, base, baseSize);
    base->lib_NegSize = (UWORD)(slots * sizeof(struct FakeEntry));
    base->lib_PosSize = (UWORD)baseSize;
    CacheClearU();

    fakes[fakeCount].name = name;
    fakes[fakeCount].table = table;
    fakes[fakeCount].base = base;
    fakeCount++;
    return base;
}
