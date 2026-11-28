# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
The link prefs on the wire (ENV:genet.prefs, read at the driver's first open),
in both personalities, one detached script each: LINK_MODE=100fd negotiates
100 Mb/s full duplex, where UDP sends outrun the wire (SANA-II's TX backlog
takes them; netdev hands them back to the stack when the ring is full); AUTONEG=off with
LINK_MODE=100hd forces a link the switch detects as the same; AUTONEG=off
alone, and FLOW_CONTROL=off, keep gigabit; and the machine's prefs bring
gigabit back. Which pause modes the PHY advertises is not visible without the
debug log: FLOW_CONTROL=off is checked to leave a working link, and the
logic tier checks its parsing.
"""
from __future__ import annotations

from dataclasses import dataclass

import pytest

from harness import netdevstats, sockbench
from harness.limits import (FAST, GIGABIT, HALF_DUPLEX_LEAST_MBPS, LINK_100_LEAST_MBPS, LINK_100_MOST_MBPS,
                            TCP_FLOOR_MBPS)
from emu68test.results import parse
from harness.stack import NETDEV, SANA2, Script, rc_of, section
from harness.tools import GENETTEST, PRIORITY

pytestmark = [pytest.mark.hw, pytest.mark.full]

BENCH_S = 5


@dataclass(frozen=True)
class Case:
    """One prefs setting: what goes into ENV:genet.prefs, the link it should give, the runs on it."""

    name: str
    prefs: str | None          # None: the machine's own
    speed: int
    full_duplex: bool
    benches: tuple[str, ...]   # sockbench commands, the peer and the length added


CASES = (
    Case("100fd", "LINK_MODE=100fd\n", FAST, True, ("tx", "udptx", "rx", "rr")),
    Case("forced-100hd", "AUTONEG=off\nLINK_MODE=100hd\n", FAST, False, ("rx",)),
    Case("autoneg-off-alone", "AUTONEG=off\n", GIGABIT, True, ("rx",)),
    Case("flow-control-off", "FLOW_CONTROL=off\n", GIGABIT, True, ("rx",)),
    Case("machine", None, GIGABIT, True, ("rx",)),
)
KINDS = (NETDEV, SANA2)

_logs: dict[str, str | Exception] = {}  # per personality: the round's log, or why it did not end


def bench_args(command: str, peer: str) -> str:
    """sockbench's arguments for @command: request-response takes 1-byte requests."""
    return f"rr {peer} 1 {BENCH_S}" if command == "rr" else f"{command} {peer} 1 {BENCH_S}"


def round_script(interface, kind: str, peer: str) -> Script:
    """Every case in turn, the interface re-added as @kind each time, the machine's prefs last."""
    script = Script(f"link-{kind.lower()}").pri(PRIORITY)
    for case in CASES:
        prefs = interface.prefs_file(case.name, case.prefs) if case.prefs is not None else None
        interface.reconfigure(script, case.name, kind, prefs)
        script.netdev_stats(f"{case.name} link")
        for command in case.benches:
            if command == "udptx":
                counted(script, kind, case.name, "before")
            script.bench(f"{case.name} {command}", bench_args(command, peer))
            if command == "udptx":
                counted(script, kind, case.name, "after")
    return script


def counted(script: Script, kind: str, case: str, when: str) -> None:
    """The driver's TX counters into the log (netdev-stats) and, beside SANA-II, its special stats into a file."""
    script.netdev_stats(f"{case} counters {when}", "COUNTERS")
    if kind == SANA2:
        script.program(f"{case} query {when}", GENETTEST, "QUERY", f"RAM:gt/out/link-{case}-{when}.txt")


@pytest.fixture
def link_log(device, interface, peer, personality) -> str:
    """The link round's log in this test's personality, run once per session (a round that did not end
    fails every test that reads it, without running again)."""
    if personality not in _logs:
        try:
            _logs[personality] = interface.run(round_script(interface, personality, peer), personality, limit=600)
        except AssertionError as e:
            _logs[personality] = e
    found = _logs[personality]
    if isinstance(found, Exception):
        raise AssertionError(f"the link round as {personality} did not end: {found}")
    return found


def throughput(log: str, label: str) -> sockbench.Throughput:
    """The sockbench total under @label in the round's log; AssertionError when there is none."""
    run = sockbench.parse(section(log, label) or "")
    assert isinstance(run, sockbench.Throughput), f"{label}: no sockbench total"
    return run


def tx_queued(device, case: str, when: str) -> int:
    """SANA-II's count of writes that waited in its TX backlog, from GenetTest QUERY's output."""
    out = f"RAM:gt/out/link-{case}-{when}.txt"
    text = device.download_text(out)
    device.keep(out, text)
    return parse(text, 0).num("special.tx_queued")


@pytest.mark.parametrize("kind", [pytest.param(k, marks=pytest.mark.personality(k), id=k.lower()) for k in KINDS])
@pytest.mark.parametrize("case", CASES, ids=[c.name for c in CASES])
@pytest.mark.covers("prefs LINK_MODE", "prefs AUTONEG", "prefs FLOW_CONTROL", "NETDEV_CMD_GET_LINK")
def test_the_link_comes_up_as_the_prefs_ask(link_log, kind, case, record_property):
    assert rc_of(link_log, f"{case.name} add") in (0, 5), f"{case.name}: the interface did not come up"
    stats = netdevstats.parse(section(link_log, f"{case.name} link") or "")
    assert (stats.link_up, stats.speed_mbps, stats.full_duplex) == (True, case.speed, case.full_duplex), stats
    for command in case.benches:
        label = f"{case.name} {command}"
        if command == "rr":
            run = sockbench.parse(section(link_log, label) or "")
            assert isinstance(run, sockbench.RequestResponse) and run.tps > 0, f"{label}: no exchange completed"
            continue
        mbps = throughput(link_log, label).mbps
        record_property(label, mbps)
        if case.speed == GIGABIT:
            assert mbps >= TCP_FLOOR_MBPS[kind], f"{label}: {mbps} Mb/s"
        elif case.full_duplex:
            assert LINK_100_LEAST_MBPS <= mbps <= LINK_100_MOST_MBPS, f"{label}: {mbps} Mb/s, not at the link rate"
        else:
            assert HALF_DUPLEX_LEAST_MBPS <= mbps <= LINK_100_MOST_MBPS, f"{label}: {mbps} Mb/s"


@pytest.mark.personality(SANA2)
@pytest.mark.covers("SANA-II TX backlog")
def test_udp_sends_that_outrun_a_100_mbps_wire_go_through_sana2_s_backlog(device, link_log, record_property):
    queued = tx_queued(device, "100fd", "after") - tx_queued(device, "100fd", "before")
    record_property("frames queued", queued)
    assert queued > 0, "no frame waited in the TX backlog"


@pytest.mark.personality(NETDEV)
@pytest.mark.covers("netdev TX ring full")
def test_udp_sends_that_outrun_a_100_mbps_wire_are_handed_back_to_netdev(link_log, record_property):
    before, after = (netdevstats.parse(section(link_log, f"100fd counters {when}") or "").counters
                     for when in ("before", "after"))
    assert "drv_tx_rejected" in before and "drv_tx_rejected" in after, "netdev-stats listed no drv_tx_rejected"
    rejected = after["drv_tx_rejected"] - before["drv_tx_rejected"]
    record_property("frames handed back, ring full", rejected)
    record_property("datagrams the stack refused", throughput(link_log, "100fd udptx").refused)
    assert rejected > 0, "the TX ring never filled: no frame handed back to the stack"
