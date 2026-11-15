// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
/*
 * prefs.c - the logic tier's program for ENV:genet.prefs (tests/README.md):
 * the driver's own runtime_config.c, built for the host's vamos, prints the
 * settings LoadGenetRuntimeConfig() resolves. The test writes the file into a
 * drawer it assigns as ENV: for the run, and judges the settings.
 *
 *     LogicPrefs
 */

#include "runtime-config/src/runtime_config.c"

#include <proto/exec.h>

#include "kv.h"

/**
 * main() - Load the settings and print each of them.
 *
 * Return: KV_RC_PASS: the test judges what was printed.
 */
int main(void)
{
    struct GenetRuntimeConfig config;

    LoadGenetRuntimeConfig(&config, SysBase);
    kv_begin("prefs");
    kv_snum("prefs.unit_task_priority", config.unit_task_priority);
    kv_num("prefs.rx_pool_bufs", config.rx_pool_bufs);
    kv_num("prefs.link_speed", config.link_speed);
    kv_str("prefs.link_duplex", config.link_duplex == DUPLEX_FULL_CFG ? "full" : "half");
    kv_str("prefs.link_autoneg", config.link_autoneg ? "yes" : "no");
    kv_str("prefs.flow_control", config.flow_control ? "yes" : "no");
    kv_str("prefs.use_miami_workaround", config.use_miami_workaround ? "yes" : "no");
    return kv_end();
}
