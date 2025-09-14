// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/dos_protos.h>
#include <clib/exec_protos.h>
#include <clib/utility_protos.h>
#else
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/utility.h>
#endif

#include <exec/types.h>
#include <exec/memory.h>

#include <runtime_config.h>
#include <debug.h>
#include <device.h>

struct DosLibrary *DOSBase = NULL;
struct GenetRuntimeConfig genetConfig;

static void ApplyDefaults()
{
    static const ULONG def_ladder[] = DEFAULT_POLL_LADDER;
    genetConfig.unit_task_priority = DEFAULT_UNIT_TASK_PRIORITY;
    genetConfig.unit_stack_bytes = DEFAULT_UNIT_STACK_BYTES;
    genetConfig.use_dma = DEFAULT_USE_DMA;
    genetConfig.use_miami_workaround = DEFAULT_USE_MIAMI_WORKAROUND;
    genetConfig.tx_pending_fast_ticks = DEFAULT_TX_PENDING_FAST_TICKS;
    genetConfig.tx_reclaim_soft_us = DEFAULT_TX_RECLAIM_SOFT_US;
    genetConfig.rx_poll_burst = DEFAULT_RX_POLL_BURST;
    genetConfig.rx_poll_burst_idle_break = DEFAULT_RX_POLL_BURST_IDLE_BREAK;
    genetConfig.poll_delay_len = sizeof(def_ladder) / sizeof(def_ladder[0]);
    for (UWORD i = 0; i < genetConfig.poll_delay_len && i < DEFAULT_POLL_LADDER_MAX; i++)
        genetConfig.poll_delay_us[i] = def_ladder[i];
}

static void ParsePollDelayList(char *val)
{
    UWORD count = 0;
    char *p = val;
    while (*p && count < DEFAULT_POLL_LADDER_MAX)
    {
        char *start = p;
        while (*p && *p != ',')
            p++;
        char saved = *p;
        if (*p)
            *p = '\0';
        LONG v;
        if (StrToLong((STRPTR)start, &v) && v >= 0)
            genetConfig.poll_delay_us[count++] = (ULONG)v;
        if (saved)
        {
            *p = saved; /* restore delimiter */
            if (saved == ',')
                p++; /* skip comma */
        }
    }
    if (count)
        genetConfig.poll_delay_len = count;
}

void LoadGenetRuntimeConfig()
{
    Kprintf("[genet] %s: Loading defaults\n", __func__);
    ApplyDefaults();

    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR) "dos.library", LIB_MIN_VERSION);
    if (!DOSBase)
        return;

    BPTR fh = Open((CONST_STRPTR) "ENV:genet.prefs", MODE_OLDFILE);
    if (!fh)
    {
        if (DOSBase)
        {
            CloseLibrary((struct Library *)DOSBase);
            DOSBase = NULL;
        }
        return;
    }
    Kprintf("[genet] %s: Reading ENV:genet.prefs\n", __func__);

    unsigned char linebuf[256];
    while (FGets(fh, (STRPTR)linebuf, sizeof(linebuf)))
    {
        char *line = (char *)linebuf;
        /* strip CR/LF */
        char *eol = line;
        while (*eol && *eol != '\n' && *eol != '\r')
            eol++;
        *eol = '\0';
        /* find '=' */
        char *eq = line;
        while (*eq && *eq != '=')
            eq++;
        if (*eq == '=')
        {
            *eq = '\0';
            char *key = line;
            char *val = eq + 1;
            while (*key == ' ' || *key == '\t')
                key++;
            while (*val == ' ' || *val == '\t')
                val++;
            char *end = val;
            while (*end)
                end++;
            while (end > val && (end[-1] == ' ' || end[-1] == '\t'))
                *--end = '\0';
            end = key;
            while (*end)
                end++;
            while (end > key && (end[-1] == ' ' || end[-1] == '\t'))
                *--end = '\0';
            if (*key && *val)
            {
                LONG v;
                if (!Stricmp((STRPTR)key, (STRPTR) "UNIT_TASK_PRIORITY"))
                {
                    if (StrToLong((STRPTR)val, &v))
                        genetConfig.unit_task_priority = v;
                }
                else if (!Stricmp((STRPTR)key, (STRPTR) "UNIT_STACK_SIZE"))
                {
                    if (StrToLong((STRPTR)val, &v) && v > 0)
                        genetConfig.unit_stack_bytes = (ULONG)v;
                    if (genetConfig.unit_stack_bytes < 4096)
                        genetConfig.unit_stack_bytes = 4096; /* floor */
                    genetConfig.unit_stack_bytes &= ~3UL;    /* 32-bit align */
                }
                else if (!Stricmp((STRPTR)key, (STRPTR) "USE_DMA"))
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        genetConfig.use_dma = (UBYTE)v;
                }
                else if (!Stricmp((STRPTR)key, (STRPTR) "USE_MIAMI_WORKAROUND"))
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        genetConfig.use_miami_workaround = (UBYTE)v;
                }
                else if (!Stricmp((STRPTR)key, (STRPTR) "TX_PENDING_FAST_TICKS"))
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        genetConfig.tx_pending_fast_ticks = (UWORD)v;
                }
                else if (!Stricmp((STRPTR)key, (STRPTR) "TX_RECLAIM_SOFT_US"))
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        genetConfig.tx_reclaim_soft_us = (ULONG)v;
                }
                else if (!Stricmp((STRPTR)key, (STRPTR) "RX_POLL_BURST"))
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        genetConfig.rx_poll_burst = (UWORD)v;
                }
                else if (!Stricmp((STRPTR)key, (STRPTR) "RX_POLL_BURST_IDLE_BREAK"))
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        genetConfig.rx_poll_burst_idle_break = (UWORD)v;
                }
                else if (!Stricmp((STRPTR)key, (STRPTR) "POLL_DELAY_US"))
                    ParsePollDelayList(val);
            }
        }
    }

    Close(fh);

    if (DOSBase)
    {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
}

void DumpGenetRuntimeConfig()
{
#ifdef DEBUG
    Kprintf("[genet] config: pri=%ld stack_bytes=%lu use_dma=%ld miami=%ld txFastTicks=%ld txSoftUs=%ld rxBurst=%ld/%ld ladder=",
            genetConfig.unit_task_priority,
            genetConfig.unit_stack_bytes,
            (ULONG)genetConfig.use_dma,
            (ULONG)genetConfig.use_miami_workaround,
            genetConfig.tx_pending_fast_ticks,
            genetConfig.tx_reclaim_soft_us,
            genetConfig.rx_poll_burst,
            genetConfig.rx_poll_burst_idle_break);
    for (UWORD i = 0; i < genetConfig.poll_delay_len; i++)
        Kprintf("%lu%s", genetConfig.poll_delay_us[i], (i + 1 < genetConfig.poll_delay_len) ? "," : "\n");
#endif
}
