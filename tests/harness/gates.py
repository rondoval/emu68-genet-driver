# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
gates.py - which files emu68test's code gates cover here (run.sh host): what
AGENTS.md calls ours. The device scaffolding, both personalities, the datapath
files, the prefs reader, genet-stats and the suite's own programs. The
upstream-diffable files (the PHY layer, bcmgenet.c, bcmgenet-mac.c and the
imported headers) are left out: a clean diff against U-Boot and Linux is worth
more there than our comments.

The allow list is tests/complexity-allow.txt; its "5.0:" entries are what the
5.0 code had.
"""
from __future__ import annotations

from pathlib import Path

from emu68test.gates import Gates

from .suite import COMPONENT_DIR, TESTS

DRIVER_SOURCES = ("device", "unit", "unit_task", "netdev_api", "sana2_api", "sana2_io", "sana2_mcast", "sana2_stats",
                  "sana2_tx", "bcmgenet-link", "bcmgenet-mib", "bcmgenet-rxpool", "bcmgenet-rx", "bcmgenet-tx",
                  "bcmgenet-irq", "bcmgenet-dma", "devtree_parse")
OWN_FILES = ("genet.device/include/device.h", "genet.device/include/sana2_unit.h", "runtime-config/src/runtime_config.c",
             "runtime-config/include/runtime_config.h", "tools/genet-stats.c")
TEST_FOLDERS = ("tests/rig", "tests/logic")
INHERITED = "5.0:"                 # the reason of a ceiling the 5.0 code set


def sources() -> list[Path]:
    """Every C file the gates cover."""
    found = [COMPONENT_DIR / "genet.device" / "src" / f"{name}.c" for name in DRIVER_SOURCES]
    found += [COMPONENT_DIR / name for name in OWN_FILES]
    for folder in TEST_FOLDERS:
        found += sorted((COMPONENT_DIR / folder).glob("*.[ch]"))
    return found


GATES = Gates(COMPONENT_DIR, sources(), TESTS / "complexity-allow.txt", INHERITED)
