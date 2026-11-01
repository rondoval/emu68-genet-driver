// SPDX-License-Identifier: GPL-2.0+
#include <exec/types.h>

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

static void ApplyDefaults(struct GenetRuntimeConfig *config)
{
    config->unit_task_priority = DEFAULT_UNIT_TASK_PRIORITY;
    config->rx_pool_bufs = DEFAULT_RX_POOL_BUFS;
    config->link_speed = DEFAULT_LINK_SPEED;
    config->link_duplex = DEFAULT_LINK_DUPLEX;
    config->link_autoneg = DEFAULT_LINK_AUTONEG;
    config->flow_control = DEFAULT_FLOW_CONTROL;
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
        BOOL on;

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
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "LINK_MODE") == 0)
        {
            if (!ParseLinkMode(config, val))
                Kprintf("[genet] %s: unknown LINK_MODE '%s', keeping auto\n",
                        __func__, (ULONG)val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "AUTONEG") == 0)
        {
            if (prefs_bool(val, &on))
                config->link_autoneg = (u8)on;
            else
                Kprintf("[genet] %s: unknown AUTONEG '%s', keeping on\n",
                        __func__, (ULONG)val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "FLOW_CONTROL") == 0)
        {
            if (prefs_bool(val, &on))
                config->flow_control = (u8)on;
            else
                Kprintf("[genet] %s: unknown FLOW_CONTROL '%s', keeping on\n",
                        __func__, (ULONG)val);
        }
        else if (_Stricmp((CONST_STRPTR)key, (CONST_STRPTR) "RX_POOL_BUFS") == 0)
        {
            /* 0 = auto (sized at ATTACH from the stack's budget);
             * explicit values are an absolute operator override */
            if (prefs_number(val, 0, PREFS_NUMBER_MOST, &parsed))
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

    prefs_close(&f);

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
    Kprintf("[genet] config: pri=%ld rx_pool_bufs=%lu\n",
            (LONG)config->unit_task_priority,
            (ULONG)config->rx_pool_bufs);
    Kprintf("[genet] config: link_speed=%lu link_duplex=%s link_autoneg=%lu flow_control=%lu\n",
            (ULONG)config->link_speed,
            (ULONG)(config->link_duplex == DUPLEX_FULL_CFG ? "full" : "half"),
            (ULONG)config->link_autoneg,
            (ULONG)config->flow_control);
}
#endif /* DEBUG (DumpGenetRuntimeConfig) */
