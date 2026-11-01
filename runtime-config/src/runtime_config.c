// SPDX-License-Identifier: GPL-2.0+
#include <exec/types.h>

#include <types.h>
#include <strutil.h>
#include <prefs.h>
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

void LoadGenetRuntimeConfig(struct GenetRuntimeConfig *config, struct ExecBase *SysBase)
{
    KprintfT("[genet] %s: Loading defaults\n", __func__);
    ApplyDefaults(config);

    struct prefs_file f;
    if (!prefs_open(&f, SysBase, "ENV:genet.prefs"))
    {
        KprintfT("[genet] %s: Nothing to read, keeping defaults\n", __func__);
        return;
    }
    KprintfT("[genet] %s: Reading ENV:genet.prefs\n", __func__);

    char *key, *val;
    while (prefs_next(&f, &key, &val))
    {
        LONG parsed;

        if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "UNIT_TASK_PRIORITY") == 0)
        {
            if (prefs_number(val, PREFS_NUMBER_LEAST, PREFS_NUMBER_MOST, &parsed))
            {
                if (parsed < UNIT_TASK_PRIORITY_MIN)
                    parsed = UNIT_TASK_PRIORITY_MIN;
                if (parsed > UNIT_TASK_PRIORITY_MAX)
                    parsed = UNIT_TASK_PRIORITY_MAX;
                config->unit_task_priority = (s8)parsed;
            }
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "UNIT_STACK_SIZE") == 0)
        {
            if (prefs_number(val, 1, PREFS_NUMBER_MOST, &parsed))
                config->unit_stack_bytes = (u32)parsed;
            if (config->unit_stack_bytes < 4096)
                config->unit_stack_bytes = 4096; /* floor */
            config->unit_stack_bytes &= ~3u;    /* 32-bit align */
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "USE_DMA") == 0)
        {
            if (prefs_number(val, 0, PREFS_NUMBER_MOST, &parsed))
                config->use_dma = (u8)parsed;
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "USE_MIAMI_WORKAROUND") == 0)
        {
            if (prefs_number(val, 0, PREFS_NUMBER_MOST, &parsed))
                config->use_miami_workaround = (u8)parsed;
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "BUDGET") == 0)
        {
            if (prefs_number(val, 1, PREFS_NUMBER_MOST, &parsed))
                config->budget = (u16)parsed;
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "PERIODIC_TASK_MS") == 0)
        {
            if (prefs_number(val, 0, PREFS_NUMBER_MOST, &parsed))
                config->periodic_task_ms = (u32)parsed;
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "RX_COALESCE_USECS") == 0)
        {
            if (prefs_number(val, 0, PREFS_NUMBER_MOST, &parsed))
                config->rx_coalesce_usecs = (u32)parsed;
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "RX_COALESCE_FRAMES") == 0)
        {
            if (prefs_number(val, 0, PREFS_NUMBER_MOST, &parsed))
                config->rx_coalesce_frames = (u32)parsed;
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "TX_COALESCE_FRAMES") == 0)
        {
            if (prefs_number(val, 0, PREFS_NUMBER_MOST, &parsed))
                config->tx_coalesce_frames = (u32)parsed;
        }
    }

    prefs_close(&f);
}

#ifdef DEBUG
void DumpGenetRuntimeConfig(const struct GenetRuntimeConfig *config)
{
    Kprintf("[genet] config: pri=%ld stack_bytes=%lu use_dma=%ld miami=%ld periodic_task_ms=%lu budget=%lu rx_coalesce_usecs=%lu rx_coalesce_frames=%lu tx_coalesce_frames=%lu\n",
            (LONG)config->unit_task_priority,
            (ULONG)config->unit_stack_bytes,
            (ULONG)config->use_dma,
            (ULONG)config->use_miami_workaround,
            (ULONG)config->periodic_task_ms,
            (ULONG)config->budget,
            (ULONG)config->rx_coalesce_usecs,
            (ULONG)config->rx_coalesce_frames,
            (ULONG)config->tx_coalesce_frames);
}
#endif /* DEBUG (DumpGenetRuntimeConfig) */
