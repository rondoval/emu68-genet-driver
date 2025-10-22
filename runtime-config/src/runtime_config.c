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

struct DosLibrary *DOSBase = NULL;
struct GenetRuntimeConfig genetConfig;

static void ApplyDefaults()
{
    genetConfig.unit_task_priority = DEFAULT_UNIT_TASK_PRIORITY;
    genetConfig.unit_stack_bytes = DEFAULT_UNIT_STACK_BYTES;
    genetConfig.use_dma = DEFAULT_USE_DMA;
    genetConfig.use_miami_workaround = DEFAULT_USE_MIAMI_WORKAROUND;
    genetConfig.budget = DEFAULT_BUDGET;
    genetConfig.periodic_task_ms = DEFAULT_PERIODIC_TASK_MS;
    genetConfig.rx_coalesce_usecs = DEFAULT_RX_COALESCE_USECS;
    genetConfig.rx_coalesce_frames = DEFAULT_RX_COALESCE_FRAMES;
    genetConfig.tx_coalesce_frames = DEFAULT_TX_COALESCE_FRAMES;
}

void LoadGenetRuntimeConfig()
{
    Kprintf("[genet] %s: Loading defaults\n", __func__);
    ApplyDefaults();

    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR) "dos.library", 0);
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
                else if (!Stricmp((STRPTR)key, (STRPTR) "BUDGET"))
                {
                    if (StrToLong((STRPTR)val, &v) && v > 0)
                        genetConfig.budget = (UWORD)v;
                }
                else if (!Stricmp((STRPTR)key, (STRPTR) "PERIODIC_TASK_MS"))
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        genetConfig.periodic_task_ms = (ULONG)v;
                }
                else if (!Stricmp((STRPTR)key, (STRPTR) "RX_COALESCE_USECS"))
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        genetConfig.rx_coalesce_usecs = (ULONG)v;
                }
                else if (!Stricmp((STRPTR)key, (STRPTR) "RX_COALESCE_FRAMES"))
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        genetConfig.rx_coalesce_frames = (ULONG)v;
                }
                else if (!Stricmp((STRPTR)key, (STRPTR) "TX_COALESCE_FRAMES"))
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        genetConfig.tx_coalesce_frames = (ULONG)v;
                }
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
    Kprintf("[genet] config: pri=%ld stack_bytes=%lu use_dma=%ld miami=%ld periodic_task_ms=%lu budget=%lu rx_coalesce_usecs=%lu rx_coalesce_frames=%lu tx_coalesce_frames=%lu\n",
            genetConfig.unit_task_priority,
            genetConfig.unit_stack_bytes,
            (ULONG)genetConfig.use_dma,
            (ULONG)genetConfig.use_miami_workaround,
            genetConfig.periodic_task_ms,
            genetConfig.budget,
            genetConfig.rx_coalesce_usecs,
            genetConfig.rx_coalesce_frames,
            genetConfig.tx_coalesce_frames);
#endif
}
