# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
suite.py - what this suite is, for emu68-common's harness (emu68test) and for
its own modules: where it lives, its component, its drawer on the Amiga, and
the files it replaces there.
"""
from __future__ import annotations

from pathlib import Path

from emu68test.plugin import Suite

TESTS = Path(__file__).resolve().parents[1]
COMPONENT_DIR = TESTS.parent
STACK_DIR = COMPONENT_DIR.parents[1]
COMPONENT = "emu68-genet-driver-netdev"

RAM = "RAM:gt"              # everything temporary on the Amiga
OUT = f"{RAM}/out"          # the programs' output files
BOOT_SETTLE_S = 60          # AmiKit's start-up: the agent refuses connects for a minute right after it answers

# the files the suite replaces on the Amiga, and those it reads to put back
DRIVER = "DEVS:Networks/genet.device"
GENET_STATS = "C:genet-stats"
BSDSOCKET = "LIBS:bsdsocket.library"
INTERFACE = "DEVS:NetInterfaces/genet"
PREFS = "ENV:genet.prefs"

SUITE = Suite(name="genet.device", component=COMPONENT, tests_dir=TESTS, ram=RAM, kv_programs=("GenetTest",),
              boot_settle_s=BOOT_SETTLE_S, run_options=("peer", "driver", "bsdsocket", "seeding"))
