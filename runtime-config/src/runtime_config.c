// SPDX-License-Identifier: GPL-2.0+
#ifdef __INTELLISENSE__
#include <clib/dos_protos.h>
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#include <proto/dos.h>
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <exec/types.h>
#include <exec/memory.h>

#include <strutil.h>
#include <runtime_config.h>
#include <debug.h>

static void ApplyDefaults(struct GenetRuntimeConfig *config)
{
    config->unit_task_priority = DEFAULT_UNIT_TASK_PRIORITY;
    config->unit_stack_bytes = DEFAULT_UNIT_STACK_BYTES;
    config->use_dma = DEFAULT_USE_DMA;
    config->use_miami_workaround = DEFAULT_USE_MIAMI_WORKAROUND;
    config->budget = DEFAULT_BUDGET;
    config->periodic_task_ms = DEFAULT_PERIODIC_TASK_MS;
    config->rx_coalesce_usecs = DEFAULT_RX_COALESCE_USECS;
    config->rx_coalesce_frames = DEFAULT_RX_COALESCE_FRAMES;
    config->tx_coalesce_frames = DEFAULT_TX_COALESCE_FRAMES;
}

void LoadGenetRuntimeConfig(struct GenetRuntimeConfig *config)
{
    Kprintf("[genet] %s: Loading defaults\n", __func__);
    ApplyDefaults(config);

    struct DosLibrary *DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR) "dos.library", 0);
    if (!DOSBase)
        return;

    BPTR fh = Open((CONST_STRPTR) "ENV:genet.prefs", MODE_OLDFILE);
    if (!fh)
    {
        CloseLibrary((struct Library *)DOSBase);
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
                if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "UNIT_TASK_PRIORITY") == 0)
                {
                    if (StrToLong((STRPTR)val, &v))
                        config->unit_task_priority = v;
                }
                else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "UNIT_STACK_SIZE") == 0)
                {
                    if (StrToLong((STRPTR)val, &v) && v > 0)
                        config->unit_stack_bytes = (ULONG)v;
                    if (config->unit_stack_bytes < 4096)
                        config->unit_stack_bytes = 4096; /* floor */
                    config->unit_stack_bytes &= ~3UL;    /* 32-bit align */
                }
                else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "USE_DMA") == 0)
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        config->use_dma = (UBYTE)v;
                }
                else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "USE_MIAMI_WORKAROUND") == 0)
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        config->use_miami_workaround = (UBYTE)v;
                }
                else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "BUDGET") == 0)
                {
                    if (StrToLong((STRPTR)val, &v) && v > 0)
                        config->budget = (UWORD)v;
                }
                else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "PERIODIC_TASK_MS") == 0)
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        config->periodic_task_ms = (ULONG)v;
                }
                else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "RX_COALESCE_USECS") == 0)
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        config->rx_coalesce_usecs = (ULONG)v;
                }
                else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "RX_COALESCE_FRAMES") == 0)
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        config->rx_coalesce_frames = (ULONG)v;
                }
                else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "TX_COALESCE_FRAMES") == 0)
                {
                    if (StrToLong((STRPTR)val, &v) && v >= 0)
                        config->tx_coalesce_frames = (ULONG)v;
                }
            }
        }
    }

    Close(fh);
    CloseLibrary((struct Library *)DOSBase);
}

void DumpGenetRuntimeConfig(const struct GenetRuntimeConfig *config)
{
#ifdef DEBUG
    Kprintf("[genet] config: pri=%ld stack_bytes=%lu use_dma=%ld miami=%ld periodic_task_ms=%lu budget=%lu rx_coalesce_usecs=%lu rx_coalesce_frames=%lu tx_coalesce_frames=%lu\n",
            config->unit_task_priority,
            config->unit_stack_bytes,
            (ULONG)config->use_dma,
            (ULONG)config->use_miami_workaround,
            config->periodic_task_ms,
            config->budget,
            config->rx_coalesce_usecs,
            config->rx_coalesce_frames,
            config->tx_coalesce_frames);
#endif
}
