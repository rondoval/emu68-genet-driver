# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
conftest.py - the suite's options, the report and the fixtures every tier
shares (README.md). The Amiga's own fixtures (device, install, stack, peers,
baselines) are in hw/conftest.py.
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

import pytest

TESTS = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS))

from harness.amiga import Unreachable  # noqa: E402
from harness.build import Artefacts, BuildInfo  # noqa: E402
from harness.matrix import PS, STAT  # noqa: E402
from harness.report import Report, Row  # noqa: E402

STACK = TESTS.parents[2]


def pytest_addoption(parser):
    g = parser.getgroup("amiga", "the machine under test (hardware tiers)")
    g.addoption("--server", default="amiga-pi4",
                help="the amiga MCP server name in ~/.claude.json; also the baselines key")
    g.addoption("--amimcp", default=os.environ.get("AMIMCP", str(STACK.parent / "amimcp")),
                help="the amimcp checkout (its server/amiga.py), by default beside the stack's")
    g.addoption("--install-dir", default=str(STACK / "install"), help="the stack's install tree")
    g.addoption("--build-dir", default=str(STACK / "build"), help="the stack's build dir")
    g.addoption("--bsdsocktest", default=str(STACK.parent / "bsdsocktest" / "bsdsocktest"),
                help="the bsdsocktest binary, by default from its checkout beside the stack's")
    g.addoption("--peer", default=os.environ.get("GENET_PEER"),
                help="the PC's LAN address, where the peers run (or GENET_PEER)")
    g.addoption("--peer-script", default=str(STACK / "components" / "lwip-amiga" / "scripts" / "tcp-bench-peer.py"),
                help="lwip-amiga's tcp-bench-peer.py")
    g.addoption("--bst-helper", default=str(STACK.parent / "bsdsocktest" / "host" / "bsdsocktest_helper.py"),
                help="bsdsocktest's host helper")
    g.addoption("--run-dir", default=None, help="where this run's report and outputs go (default tests/runs/<timestamp>)")
    g.addoption("--no-install", action="store_true",
                help="test what is installed; check it matches the install tree instead of replacing it")
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
    g.addoption("--serial", default="/dev/ttyUSB0", help="profile tier: the Pi's UART (921600 8N1)")


class Session:
    """What the hooks share with the fixtures."""

    def __init__(self, build_info: BuildInfo):
        self.build_info = build_info
        self.report: Report | None = None
        self.baselines = None
        self.rundir: Path | None = None


def pytest_configure(config):
    config._genet = Session(BuildInfo.read(Path(config.getoption("--build-dir"))))


def _reason(rep) -> str:
    """Why a test did not pass, in one line: a skip's reason or the failure's last line."""
    if rep.outcome == "skipped" and isinstance(rep.longrepr, tuple):
        return rep.longrepr[2]
    if rep.outcome != "passed" and rep.longrepr:
        return str(rep.longrepr).splitlines()[-1]
    return ""


def _row(item, rep) -> Row:
    """The report's row for a test's call, or for a setup that did not pass."""
    notes = [f"{k}: {v}" for k, v in item.user_properties]
    state = rep.outcome if rep.when == "call" or rep.outcome == "skipped" else f"{rep.outcome} in setup"
    return Row(item.nodeid, state, rep.duration, _reason(rep), notes)


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    if call.excinfo is not None and isinstance(call.excinfo.value, Unreachable):
        pytest.exit(f"the Amiga is unreachable: {call.excinfo.value}", returncode=3)
    report = item.config._genet.report
    if report is not None and (rep.when == "call" or (rep.when == "setup" and rep.outcome != "passed")):
        report.add(_row(item, rep))


def pytest_sessionfinish(session, exitstatus):
    genet = session.config._genet
    if genet.report is None:
        return
    if genet.baselines is not None:
        genet.baselines.write_measured(genet.rundir / "measured.json")
    path = genet.report.write(genet.baselines)
    session.config.pluginmanager.get_plugin("terminalreporter").write_line(f"report: {path}")


@pytest.fixture(scope="session")
def build_info(request) -> BuildInfo:
    """The stack build's flavour."""
    return request.config._genet.build_info


@pytest.fixture(scope="session")
def artefacts(request) -> Artefacts:
    """Where the files the suite pushes come from."""
    opt = request.config.getoption
    driver = opt("--driver")
    return Artefacts(Path(opt("--install-dir")), Path(opt("--build-dir")), Path(opt("--bsdsocktest")),
                     Path(driver) if driver else None)


@pytest.fixture(scope="session")
def rundir(request) -> Path:
    """This run's directory: report.md, run.json, measured.json, one dir per test."""
    chosen = request.config.getoption("--run-dir")
    path = Path(chosen) if chosen else TESTS / "runs" / time.strftime("%Y%m%d-%H%M%S")
    path.mkdir(parents=True, exist_ok=True)
    genet = request.config._genet
    genet.rundir = path
    genet.report = Report(path, f"genet.device tests, {time.strftime('%Y-%m-%d %H:%M')}")
    options = {k: v for k, v in vars(request.config.option).items()
               if k in ("server", "peer", "no_install", "driver", "bsdsocket", "seeding")}
    (path / "run.json").write_text(json.dumps({"build": genet.build_info.__dict__, "options": options},
                                              indent=2) + "\n")
    return path
