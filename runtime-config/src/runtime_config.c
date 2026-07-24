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

#include <types.h>
#include <strutil.h>
#include <prefs.h>
#include <runtime_config.h>
#include <debug.h>

/*
 * LINK_MODE values. The table is the whole grammar: a name, and the speed and
 * duplex it resolves to. "auto" resolves to LINK_MODE_NONE, which every
 * consumer reads as "advertise everything".
 *
 * 1000hd is deliberately absent. Half-duplex gigabit exists on paper only; no
 * switch worth pinning a link to offers it.
 */
struct link_mode_name
{
    const char *name;
    u16 speed;
    u8 duplex;
};

static const struct link_mode_name link_modes[] = {
    {"auto", LINK_MODE_NONE, DUPLEX_FULL_CFG},
    {"10hd", 10, DUPLEX_HALF_CFG},
    {"10fd", 10, DUPLEX_FULL_CFG},
    {"100hd", 100, DUPLEX_HALF_CFG},
    {"100fd", 100, DUPLEX_FULL_CFG},
    {"1000fd", 1000, DUPLEX_FULL_CFG},
};

/* TRUE and the config updated, FALSE for a name that is not in the table. */
static BOOL ParseLinkMode(struct GenetRuntimeConfig *config, const char *val)
{
    for (u32 i = 0; i < sizeof(link_modes) / sizeof(link_modes[0]); i++)
    {
        if (_Stricmp((CONST_STRPTR)val, (CONST_STRPTR)link_modes[i].name) != 0)
            continue;
        config->link_speed = link_modes[i].speed;
        config->link_duplex = link_modes[i].duplex;
        return TRUE;
    }
    return FALSE;
}

/* on/off, yes/no, true/false, 1/0. Returns FALSE for anything else, leaving
 * *out untouched. */
static BOOL ParseBool(const char *val, u8 *out)
{
    static const char *const on_names[] = {"on", "yes", "true", "1"};
    static const char *const off_names[] = {"off", "no", "false", "0"};

    for (u32 i = 0; i < sizeof(on_names) / sizeof(on_names[0]); i++)
    {
        if (_Stricmp((CONST_STRPTR)val, (CONST_STRPTR)on_names[i]) == 0)
        {
            *out = 1;
            return TRUE;
        }
        if (_Stricmp((CONST_STRPTR)val, (CONST_STRPTR)off_names[i]) == 0)
        {
            *out = 0;
            return TRUE;
        }
    }
    return FALSE;
}

static void ApplyDefaults(struct GenetRuntimeConfig *config)
{
    config->unit_task_priority = DEFAULT_UNIT_TASK_PRIORITY;
    config->unit_stack_bytes = DEFAULT_UNIT_STACK_BYTES;
    config->periodic_task_ms = DEFAULT_PERIODIC_TASK_MS;
    config->link_poll_ms = DEFAULT_LINK_POLL_MS;
    config->rx_coalesce_usecs = DEFAULT_RX_COALESCE_USECS;
    config->rx_coalesce_frames = DEFAULT_RX_COALESCE_FRAMES;
    config->tx_coalesce_frames = DEFAULT_TX_COALESCE_FRAMES;
    config->rx_pool_bufs = DEFAULT_RX_POOL_BUFS;
    config->link_speed = DEFAULT_LINK_SPEED;
    config->link_duplex = DEFAULT_LINK_DUPLEX;
    config->link_autoneg = DEFAULT_LINK_AUTONEG;
    config->flow_control = DEFAULT_FLOW_CONTROL;
}

void LoadGenetRuntimeConfig(struct GenetRuntimeConfig *config)
{
    KprintfT("[genet] %s: Loading defaults\n", __func__);
    ApplyDefaults(config);

    /* dos packet I/O needs a Process; OpenDevice is legal from a bare Task,
     * whose "pr_MsgPort" would be garbage beyond the Task structure */
    if (FindTask(NULL)->tc_Node.ln_Type != NT_PROCESS)
    {
        Kprintf("[genet] %s: caller is not a Process, keeping defaults\n", __func__);
        return;
    }

    struct DosLibrary *DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR) "dos.library", 0);
    if (!DOSBase)
        return;

    BPTR fh = Open((CONST_STRPTR) "ENV:genet.prefs", MODE_OLDFILE);
    if (!fh)
    {
        CloseLibrary((struct Library *)DOSBase);
        return;
    }
    KprintfT("[genet] %s: Reading ENV:genet.prefs\n", __func__);

    unsigned char linebuf[256];
    while (FGets(fh, (STRPTR)linebuf, sizeof(linebuf)))
    {
        char *key, *val;
        if (!prefs_split((char *)linebuf, &key, &val))
            continue;

        LONG parsed;

        if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "UNIT_TASK_PRIORITY") == 0)
        {
            if (StrToLong((STRPTR)val, &parsed))
                config->unit_task_priority = (s8)parsed;
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "UNIT_STACK_SIZE") == 0)
        {
            if (StrToLong((STRPTR)val, &parsed) && parsed > 0)
                config->unit_stack_bytes = (u32)parsed;
            if (config->unit_stack_bytes < 4096)
                config->unit_stack_bytes = 4096; /* floor */
            config->unit_stack_bytes &= ~3u;    /* 32-bit align */
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "PERIODIC_TASK_MS") == 0)
        {
            if (StrToLong((STRPTR)val, &parsed) && parsed >= 0)
                config->periodic_task_ms = (u32)parsed;
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "LINK_POLL_MS") == 0)
        {
            if (StrToLong((STRPTR)val, &parsed) && parsed >= 0)
                config->link_poll_ms = (u32)parsed;
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "RX_COALESCE_USECS") == 0)
        {
            if (StrToLong((STRPTR)val, &parsed) && parsed >= 0)
                config->rx_coalesce_usecs = (u32)parsed;
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "RX_COALESCE_FRAMES") == 0)
        {
            if (StrToLong((STRPTR)val, &parsed) && parsed >= 0)
                config->rx_coalesce_frames = (u32)parsed;
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "TX_COALESCE_FRAMES") == 0)
        {
            if (StrToLong((STRPTR)val, &parsed) && parsed >= 0)
                config->tx_coalesce_frames = (u32)parsed;
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "LINK_MODE") == 0)
        {
            if (!ParseLinkMode(config, val))
                Kprintf("[genet] %s: unknown LINK_MODE '%s', keeping auto\n",
                        __func__, (ULONG)val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "AUTONEG") == 0)
        {
            if (!ParseBool(val, &config->link_autoneg))
                Kprintf("[genet] %s: unknown AUTONEG '%s', keeping on\n",
                        __func__, (ULONG)val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "FLOW_CONTROL") == 0)
        {
            if (!ParseBool(val, &config->flow_control))
                Kprintf("[genet] %s: unknown FLOW_CONTROL '%s', keeping on\n",
                        __func__, (ULONG)val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "RX_POOL_BUFS") == 0)
        {
            /* 0 = auto (sized at ATTACH from the stack's budget);
             * explicit values are an absolute operator override */
            if (StrToLong((STRPTR)val, &parsed) && parsed >= 0)
            {
                if (parsed != 0)
                {
                    if (parsed < RX_POOL_BUFS_MIN)
                        parsed = RX_POOL_BUFS_MIN;
                    if (parsed > RX_POOL_BUFS_MAX)
                        parsed = RX_POOL_BUFS_MAX;
                }
                config->rx_pool_bufs = (u32)parsed;
            }
        }
    }

    Close(fh);
    CloseLibrary((struct Library *)DOSBase);

    /* Cross-key validation, once the whole file has been read: LINK_MODE and
     * AUTONEG may appear in either order.
     *
     * Forcing needs a speed to force, and 1000BASE-T cannot be forced at all —
     * negotiation is how the two ends agree which is master, so a forced
     * gigabit link only comes up against a peer forced to the opposite role.
     * Either way autonegotiation is left on; LINK_MODE still narrows the
     * advertisement, which is the standards-clean way to pin a link. */
    if (!config->link_autoneg && config->link_speed == LINK_MODE_NONE)
    {
        Kprintf("[genet] %s: AUTONEG=off needs a LINK_MODE speed, keeping autoneg\n", __func__);
        config->link_autoneg = 1;
    }
    else if (!config->link_autoneg && config->link_speed == 1000)
    {
        Kprintf("[genet] %s: 1000BASE-T cannot be forced, keeping autoneg (advertising 1000fd only)\n",
                __func__);
        config->link_autoneg = 1;
    }
}

#ifdef DEBUG
void DumpGenetRuntimeConfig(const struct GenetRuntimeConfig *config)
{
    Kprintf("[genet] config: pri=%ld stack_bytes=%lu periodic_task_ms=%lu link_poll_ms=%lu rx_coalesce_usecs=%lu rx_coalesce_frames=%lu tx_coalesce_frames=%lu rx_pool_bufs=%lu\n",
            (LONG)config->unit_task_priority,
            (ULONG)config->unit_stack_bytes,
            (ULONG)config->periodic_task_ms,
            (ULONG)config->link_poll_ms,
            (ULONG)config->rx_coalesce_usecs,
            (ULONG)config->rx_coalesce_frames,
            (ULONG)config->tx_coalesce_frames,
            (ULONG)config->rx_pool_bufs);
    Kprintf("[genet] config: link_speed=%lu link_duplex=%s link_autoneg=%lu flow_control=%lu\n",
            (ULONG)config->link_speed,
            (ULONG)(config->link_duplex == DUPLEX_FULL_CFG ? "full" : "half"),
            (ULONG)config->link_autoneg,
            (ULONG)config->flow_control);
}
#endif /* DEBUG (DumpGenetRuntimeConfig) */
