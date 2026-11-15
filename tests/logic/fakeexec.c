// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * fakeexec.c - exec and utility.library as the code under test sees them
 * (support.h). The memory and list functions go on to the real exec; Signal,
 * PutMsg and ReplyMsg are recorded; Forbid/Permit and Disable/Enable count
 * their nesting; CallHookPkt calls the hook, and the tag functions search the
 * list themselves (vamos has no CallHookPkt).
 */

#include <proto/exec.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>

#include "support.h"

/* The LVOs modelled, from the NDK's lvo/exec_lib.i and lvo/utility_lib.i. */
enum ExecLvo : LONG
{
    LVO_DISABLE = -120,
    LVO_ENABLE = -126,
    LVO_FORBID = -132,
    LVO_PERMIT = -138,
    LVO_ALLOC_MEM = -198,
    LVO_FREE_MEM = -210,
    LVO_INSERT = -234,
    LVO_ADD_HEAD = -240, /* AddHeadMinList too */
    LVO_ADD_TAIL = -246, /* AddTailMinList too */
    LVO_REMOVE = -252,   /* RemoveMinNode too */
    LVO_REM_HEAD = -258, /* RemHeadMinList too */
    LVO_REM_TAIL = -264, /* RemTailMinList too */
    LVO_FIND_TASK = -294,
    LVO_SIGNAL = -324,
    LVO_PUT_MSG = -366,
    LVO_REPLY_MSG = -378,
    LVO_COPY_MEM = -624,
    LVO_COPY_MEM_QUICK = -630,
    LVO_ALLOC_VEC = -684,
    LVO_FREE_VEC = -690,
    LVO_CREATE_POOL = -696,
    LVO_DELETE_POOL = -702,
    LVO_ALLOC_POOLED = -708,
    LVO_FREE_POOLED = -714,
    LVO_CACHE_PRE_DMA = -762,
    LVO_CACHE_POST_DMA = -768,
    LVO_NEW_MIN_LIST = -828,
};

enum UtilityLvo : LONG
{
    LVO_FIND_TAG_ITEM = -30,
    LVO_GET_TAG_DATA = -36,
    LVO_NEXT_TAG_ITEM = -48,
    LVO_CALL_HOOK_PKT = -102,
};

#define EXEC_SLOTS 140u   /* past NewMinList, -828 */
#define UTILITY_SLOTS 40u /* past CallHookPkt, -102 */
#define UTILITY_BASE_BYTES 64u

static struct LogicCall calls[LOGIC_CALLS_MAX];
static ULONG callCount;
static struct Message *posted[LOGIC_CALLS_MAX]; /* what PutMsg() posted, oldest first */
static ULONG postedFirst;
static ULONG postedCount;
static LONG forbids;
static ULONG failingAllocations;

/**
 * logic_record() - Record a call; past LOGIC_CALLS_MAX the oldest are kept.
 * @kind: what was called.
 * @target: the task, message or opener it was called on.
 * @value: its number.
 */
void logic_record(enum LogicCallKind kind, APTR target, ULONG value)
{
    if (callCount == LOGIC_CALLS_MAX)
        return;
    calls[callCount].kind = kind;
    calls[callCount].target = target;
    calls[callCount].value = value;
    callCount++;
}

/**
 * logic_calls() - The calls recorded since the last logic_calls_clear().
 * @found: set to the first.
 *
 * Return: how many there are.
 */
ULONG logic_calls(const struct LogicCall **found)
{
    *found = calls;
    return callCount;
}

/**
 * logic_calls_clear() - Forget the calls recorded so far.
 */
void logic_calls_clear(void)
{
    callCount = 0;
}

/**
 * logic_forbids() - The Forbid() and Disable() nesting the code under test left.
 *
 * Return: 0 when balanced.
 */
LONG logic_forbids(void)
{
    return forbids;
}

/**
 * logic_fail_allocations() - Make the next allocations of the code under test fail.
 * @count: how many.
 */
void logic_fail_allocations(ULONG count)
{
    failingAllocations = count;
}

/**
 * allocation_fails() - Whether this allocation is one logic_fail_allocations() asked to fail.
 *
 * Return: TRUE to fail it.
 */
static BOOL allocation_fails(void)
{
    if (failingAllocations == 0)
        return FALSE;
    failingAllocations--;
    return TRUE;
}

/**
 * stub_forbid() - Forbid() and Disable(): one level deeper.
 */
static void stub_forbid(void)
{
    forbids++;
}

/**
 * stub_permit() - Permit() and Enable(): one level back.
 */
static void stub_permit(void)
{
    forbids--;
}

/**
 * stub_alloc_mem() - AllocMem(), from the real exec unless it is to fail.
 * @size: its size.
 * @requirements: the MEMF_* flags.
 *
 * Return: the memory, or NULL.
 */
static APTR stub_alloc_mem(ULONG size asm("d0"), ULONG requirements asm("d1"))
{
    return allocation_fails() ? NULL : AllocMem(size, requirements);
}

/**
 * stub_free_mem() - FreeMem(), to the real exec.
 * @memory: the memory.
 * @size: its size.
 */
static void stub_free_mem(APTR memory asm("a1"), ULONG size asm("d0"))
{
    FreeMem(memory, size);
}

/**
 * stub_alloc_vec() - AllocVec(), from the real exec unless it is to fail.
 * @size: its size.
 * @requirements: the MEMF_* flags.
 *
 * Return: the memory, or NULL.
 */
static APTR stub_alloc_vec(ULONG size asm("d0"), ULONG requirements asm("d1"))
{
    return allocation_fails() ? NULL : AllocVec(size, requirements);
}

/**
 * stub_free_vec() - FreeVec(), to the real exec.
 * @memory: the memory.
 */
static void stub_free_vec(APTR memory asm("a1"))
{
    FreeVec(memory);
}

/**
 * stub_create_pool() - CreatePool(), from the real exec.
 * @requirements: the MEMF_* flags.
 * @puddle: the puddle size.
 * @threshold: the threshold size.
 *
 * Return: the pool, or NULL.
 */
static APTR stub_create_pool(ULONG requirements asm("d0"), ULONG puddle asm("d1"), ULONG threshold asm("d2"))
{
    return CreatePool(requirements, puddle, threshold);
}

/**
 * stub_delete_pool() - DeletePool(), to the real exec.
 * @pool: the pool.
 */
static void stub_delete_pool(APTR pool asm("a0"))
{
    DeletePool(pool);
}

/**
 * stub_alloc_pooled() - AllocPooled(), from the real exec unless it is to fail.
 * @pool: the pool.
 * @size: its size.
 *
 * Return: the memory, or NULL.
 */
static APTR stub_alloc_pooled(APTR pool asm("a0"), ULONG size asm("d0"))
{
    return allocation_fails() ? NULL : AllocPooled(pool, size);
}

/**
 * stub_free_pooled() - FreePooled(), to the real exec.
 * @pool: the pool.
 * @memory: the memory.
 * @size: its size.
 */
static void stub_free_pooled(APTR pool asm("a0"), APTR memory asm("a1"), ULONG size asm("d0"))
{
    FreePooled(pool, memory, size);
}

/**
 * stub_copy_mem() - CopyMem() and CopyMemQuick(), by the real exec.
 * @source: where from.
 * @destination: where to.
 * @size: its size.
 */
static void stub_copy_mem(APTR source asm("a0"), APTR destination asm("a1"), ULONG size asm("d0"))
{
    CopyMem(source, destination, size);
}

/**
 * stub_find_task() - FindTask(), by the real exec.
 * @name: the task's name, or NULL for the caller.
 *
 * Return: the task.
 */
static struct Task *stub_find_task(STRPTR name asm("a1"))
{
    return FindTask(name);
}

/**
 * stub_new_min_list() - NewMinList(): an empty list.
 * @list: the list.
 */
static void stub_new_min_list(struct MinList *list asm("a0"))
{
    list->mlh_Head = (struct MinNode *)&list->mlh_Tail;
    list->mlh_Tail = NULL;
    list->mlh_TailPred = (struct MinNode *)&list->mlh_Head;
}

/**
 * stub_insert() - Insert(): @node behind @after.
 * @list: the list.
 * @node: the node.
 * @after: the node to insert behind, or NULL for the head.
 */
static void stub_insert(struct MinList *list asm("a0"), struct MinNode *node asm("a1"), struct MinNode *after asm("a2"))
{
    struct MinNode *previous = after != NULL ? after : (struct MinNode *)&list->mlh_Head;
    struct MinNode *next = previous->mln_Succ;
    node->mln_Succ = next;
    node->mln_Pred = previous;
    next->mln_Pred = node;
    previous->mln_Succ = node;
}

/**
 * stub_add_head() - AddHead() and AddHeadMinList().
 * @list: the list.
 * @node: the node.
 */
static void stub_add_head(struct MinList *list asm("a0"), struct MinNode *node asm("a1"))
{
    stub_insert(list, node, NULL);
}

/**
 * stub_add_tail() - AddTail() and AddTailMinList().
 * @list: the list.
 * @node: the node.
 */
static void stub_add_tail(struct MinList *list asm("a0"), struct MinNode *node asm("a1"))
{
    stub_insert(list, node, list->mlh_TailPred);
}

/**
 * stub_remove() - Remove() and RemoveMinNode().
 * @node: the node.
 */
static void stub_remove(struct MinNode *node asm("a1"))
{
    node->mln_Pred->mln_Succ = node->mln_Succ;
    node->mln_Succ->mln_Pred = node->mln_Pred;
}

/**
 * stub_rem_head() - RemHead() and RemHeadMinList().
 * @list: the list.
 *
 * Return: the node, or NULL for an empty list.
 */
static struct MinNode *stub_rem_head(struct MinList *list asm("a0"))
{
    struct MinNode *node = list->mlh_Head;
    if (node->mln_Succ == NULL)
        return NULL;
    stub_remove(node);
    return node;
}

/**
 * stub_rem_tail() - RemTail() and RemTailMinList().
 * @list: the list.
 *
 * Return: the node, or NULL for an empty list.
 */
static struct MinNode *stub_rem_tail(struct MinList *list asm("a0"))
{
    struct MinNode *node = list->mlh_TailPred;
    if (node->mln_Pred == NULL)
        return NULL;
    stub_remove(node);
    return node;
}

/**
 * stub_signal() - Signal(): recorded, nothing woken.
 * @task: the task.
 * @signals: the signals.
 */
static void stub_signal(struct Task *task asm("a1"), ULONG signals asm("d0"))
{
    logic_record(LOGIC_SIGNAL, task, signals);
}

/**
 * stub_put_msg() - PutMsg(): recorded, and kept for logic_port_take().
 * @port: the port.
 * @message: the message.
 */
static void stub_put_msg(struct MsgPort *port asm("a0"), struct Message *message asm("a1"))
{
    logic_record(LOGIC_PUTMSG, message, (ULONG)port);
    if (postedFirst + postedCount < LOGIC_CALLS_MAX)
        posted[postedFirst + postedCount++] = message;
}

/**
 * logic_port_take() - Take the oldest message PutMsg() posted, as the port's task would.
 *
 * Return: the message, or NULL when none is left.
 */
struct Message *logic_port_take(void)
{
    if (postedCount == 0)
        return NULL;
    struct Message *message = posted[postedFirst++];
    if (--postedCount == 0)
        postedFirst = 0; /* empty: the room is whole again */
    return message;
}

/**
 * stub_reply_msg() - ReplyMsg(): marked NT_REPLYMSG and recorded.
 * @message: the message.
 */
static void stub_reply_msg(struct Message *message asm("a1"))
{
    message->mn_Node.ln_Type = NT_REPLYMSG;
    logic_record(LOGIC_REPLYMSG, message, 0);
}

/* The CPU's caches are vamos's: nothing to push, the whole length is contiguous. */
/**
 * stub_cache_pre_dma() - CachePreDMA(): vamos has no caches to push.
 * @address: the buffer.
 * @length: its length, in and out.
 * @flags: the DMA_* flags.
 *
 * Return: @address, the whole length contiguous.
 */
static APTR stub_cache_pre_dma(APTR address asm("a0"), ULONG *length asm("a1"), ULONG flags asm("d0"))
{
    (void)length;
    (void)flags;
    return address;
}

/**
 * stub_cache_post_dma() - CachePostDMA(): nothing to invalidate.
 * @address: the buffer.
 * @length: its length, in and out.
 * @flags: the DMA_* flags.
 */
static void stub_cache_post_dma(APTR address asm("a0"), ULONG *length asm("a1"), ULONG flags asm("d0"))
{
    (void)address;
    (void)length;
    (void)flags;
}

static const struct FakeFunction execFunctions[] = {
    {LVO_DISABLE, (APTR)stub_forbid},
    {LVO_ENABLE, (APTR)stub_permit},
    {LVO_FORBID, (APTR)stub_forbid},
    {LVO_PERMIT, (APTR)stub_permit},
    {LVO_ALLOC_MEM, (APTR)stub_alloc_mem},
    {LVO_FREE_MEM, (APTR)stub_free_mem},
    {LVO_INSERT, (APTR)stub_insert},
    {LVO_ADD_HEAD, (APTR)stub_add_head},
    {LVO_ADD_TAIL, (APTR)stub_add_tail},
    {LVO_REMOVE, (APTR)stub_remove},
    {LVO_REM_HEAD, (APTR)stub_rem_head},
    {LVO_REM_TAIL, (APTR)stub_rem_tail},
    {LVO_FIND_TASK, (APTR)stub_find_task},
    {LVO_SIGNAL, (APTR)stub_signal},
    {LVO_PUT_MSG, (APTR)stub_put_msg},
    {LVO_REPLY_MSG, (APTR)stub_reply_msg},
    {LVO_COPY_MEM, (APTR)stub_copy_mem},
    {LVO_COPY_MEM_QUICK, (APTR)stub_copy_mem},
    {LVO_ALLOC_VEC, (APTR)stub_alloc_vec},
    {LVO_FREE_VEC, (APTR)stub_free_vec},
    {LVO_CREATE_POOL, (APTR)stub_create_pool},
    {LVO_DELETE_POOL, (APTR)stub_delete_pool},
    {LVO_ALLOC_POOLED, (APTR)stub_alloc_pooled},
    {LVO_FREE_POOLED, (APTR)stub_free_pooled},
    {LVO_CACHE_PRE_DMA, (APTR)stub_cache_pre_dma},
    {LVO_CACHE_POST_DMA, (APTR)stub_cache_post_dma},
    {LVO_NEW_MIN_LIST, (APTR)stub_new_min_list},
};

/* A hook's entry, as utility.library calls it. */
typedef ULONG (*HookEntry)(struct Hook *hook asm("a0"), APTR object asm("a2"), APTR message asm("a1"));

/**
 * stub_call_hook_pkt() - CallHookPkt(): the hook's entry, called as utility.library calls it.
 * @hook: the hook.
 * @object: the object (a2).
 * @message: the message.
 *
 * Return: what the hook returns.
 */
static ULONG stub_call_hook_pkt(struct Hook *hook asm("a0"), APTR object asm("a2"), APTR message asm("a1"))
{
    return ((HookEntry)(void (*)(void))hook->h_Entry)(hook, object, message);
}

/**
 * stub_next_tag_item() - NextTagItem(): the next item, past TAG_IGNORE, TAG_MORE and TAG_SKIP.
 * @cursor: the walk's place.
 *
 * Return: the item, or NULL at the end.
 */
static struct TagItem *stub_next_tag_item(struct TagItem **cursor asm("a0"))
{
    while (*cursor != NULL)
    {
        struct TagItem *item = (*cursor)++;
        switch (item->ti_Tag)
        {
        case TAG_DONE:
            *cursor = NULL;
            return NULL;
        case TAG_IGNORE:
            break;
        case TAG_MORE:
            *cursor = (struct TagItem *)item->ti_Data;
            break;
        case TAG_SKIP:
            *cursor += item->ti_Data;
            break;
        default:
            return item;
        }
    }
    return NULL;
}

/**
 * stub_find_tag_item() - FindTagItem().
 * @tag: the tag.
 * @list: the list.
 *
 * Return: the item, or NULL.
 */
static struct TagItem *stub_find_tag_item(Tag tag asm("d0"), struct TagItem *list asm("a0"))
{
    struct TagItem *cursor = list;
    struct TagItem *item;
    while ((item = stub_next_tag_item(&cursor)) != NULL)
        if (item->ti_Tag == tag)
            return item;
    return NULL;
}

/**
 * stub_get_tag_data() - GetTagData().
 * @tag: the tag.
 * @fallback: what to give without it.
 * @list: the list.
 *
 * Return: the tag's data, or @fallback.
 */
static ULONG stub_get_tag_data(Tag tag asm("d0"), ULONG fallback asm("d1"), struct TagItem *list asm("a0"))
{
    struct TagItem *item = stub_find_tag_item(tag, list);
    return item != NULL ? item->ti_Data : fallback;
}

static const struct FakeFunction utilityFunctions[] = {
    {LVO_FIND_TAG_ITEM, (APTR)stub_find_tag_item},
    {LVO_GET_TAG_DATA, (APTR)stub_get_tag_data},
    {LVO_NEXT_TAG_ITEM, (APTR)stub_next_tag_item},
    {LVO_CALL_HOOK_PKT, (APTR)stub_call_hook_pkt},
};

/**
 * logic_exec() - The fake exec, a copy of the real base behind the modelled jump table.
 *
 * Return: the base.
 */
struct ExecBase *logic_exec(void)
{
    static struct ExecBase *fake;
    if (fake == NULL)
        fake = (struct ExecBase *)fake_library_create("exec.library", EXEC_SLOTS, sizeof(struct ExecBase), SysBase,
                                                      execFunctions, sizeof(execFunctions) / sizeof(execFunctions[0]));
    return fake;
}

/**
 * logic_utility() - The fake utility.library.
 *
 * Return: the base.
 */
struct Library *logic_utility(void)
{
    static struct Library *fake;
    if (fake == NULL)
        fake = fake_library_create("utility.library", UTILITY_SLOTS, UTILITY_BASE_BYTES, NULL, utilityFunctions,
                                   sizeof(utilityFunctions) / sizeof(utilityFunctions[0]));
    return fake;
}
