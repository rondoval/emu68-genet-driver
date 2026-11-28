# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
conftest.py - the suite's own options and the fixtures every tier shares
(README.md), on top of emu68-common's harness (emu68test.plugin: --server,
--install-dir, --build-dir, --run-dir, --no-install, --serial, the report,
rundir, build_info, logic_program, test_dir). The Amiga's own fixtures (device,
install, stack, peers, baselines) are in hw/conftest.py.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

TESTS = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS))
pytest.register_assert_rewrite("emu68test")

from harness.build import Artefacts  # noqa: E402
from harness.matrix import PS, STAT  # noqa: E402
from harness.suite import STACK_DIR as STACK, SUITE  # noqa: E402

from emu68test import plugin  # noqa: E402

plugin.configure(SUITE)
pytest_plugins = ["emu68test.plugin"]


def pytest_addoption(parser):
    g = parser.getgroup("genet", "genet.device's suite")
    g.addoption("--bsdsocktest", default=str(STACK.parent / "bsdsocktest" / "bsdsocktest"),
                help="the bsdsocktest binary, by default from its checkout beside the stack's")
    g.addoption("--peer", default=os.environ.get("GENET_PEER"),
                help="the PC's LAN address, where the peers run (or GENET_PEER)")
    g.addoption("--peer-script", default=str(STACK / "components" / "lwip-amiga" / "scripts" / "tcp-bench-peer.py"),
                help="lwip-amiga's tcp-bench-peer.py")
    g.addoption("--bst-helper", default=str(STACK.parent / "bsdsocktest" / "host" / "bsdsocktest_helper.py"),
                help="bsdsocktest's host helper")
    g.addoption("--driver", default=None,
                help="another genet.device to install and test (a reference build); default the install tree's")
    g.addoption("--bsdsocket", default=None, help="a bsdsocket.library to install with the driver")
    g.addoption("--restore", action="store_true",
                help="put the machine's own files back (from tests/.machine/<server>/) and test nothing")
    g.addoption("--seeding", action="store_true",
                help="perf tier: measure for run.sh seed, judging nothing and measuring no miss again")
    g.addoption("--reps", type=int, default=3, help="perf and compare tiers: runs of each sockbench test per block")
    g.addoption("--stat", default=STAT,
                help="Executive's Stat, for the CPU share; '' without Executive")
    g.addoption("--ps", default=PS,
                help="Executive's Ps, for the per-task split of the CPU share (shown, not judged); '' to skip it")
    g.addoption("--speedtest", default="AmiKit:Tools/AmiSpeedTest", help="AmiSpeedTest on the Amiga; '' to skip it")
    g.addoption("--reference-netdev", action="append", default=[],
                help="compare tier: a genet.device the build is judged against under netdev (e.g. 4.4); "
                     "again for more, booted in turn (a bisect)")
    g.addoption("--reference-sana2", action="append", default=[],
                help="compare tier: a genet.device the build is judged against under SANA-II (e.g. 3.17); "
                     "again for more")
    g.addoption("--compare-rounds", type=int, default=2,
                help="compare tier: rounds of boots, each every reference and then the build")
    g.addoption("--compare-tests", default="",
                help="compare tier: only these sockbench tests, comma-separated (default: the whole matrix)")


@pytest.fixture(scope="session")
def artefacts(request) -> Artefacts:
    """Where the files the suite pushes come from."""
    opt = request.config.getoption
    driver = opt("--driver")
    return Artefacts(Path(opt("--install-dir")), Path(opt("--build-dir")), Path(opt("--bsdsocktest")),
                     Path(driver) if driver else None)
