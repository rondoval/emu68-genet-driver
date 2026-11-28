# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
The profile tier: a profile build of the driver (genet in EMU68_PROFILE, the
pistorm debug backend), its perf slots read off the debug log (the Pi's UART) while
one sockbench run of each profiled test goes, in each personality; each slot's
time per call judged against its baseline's median, within 20 % (one sample
per boot: a diagnosis, not the speed verdict), and kept in values.json. Only
for a driver whose binary reports perf slots (build.profiled()): any other
skips.

    netdev.udp_tx_1472.tx_submit.avg_us     the slot's mean time per call, in us
"""
from __future__ import annotations

import time

import pytest

from emu68test import perf

from harness import values
from harness.build import profiled
from harness.sockbench import Throughput, matrix
from harness.stack import NETDEV, SANA2
from harness.suite import DRIVER
from harness.tools import bench

pytestmark = [pytest.mark.hw, pytest.mark.profile]

PROFILED = ("tcp_tx", "tcp_rx", "udp_tx_1472", "udp_rx_200", "udp_rx_64k", "udp_rx_64k_300")
REPORT_PERIOD_S = 2.5   # perf.h reports every ~2 s: one more after the run
TOLERANCE = "20%"
LOG_PREFIX = "genet"    # the driver's lines in the debug log: [genet] ...


def cases():
    """(personality, test) for every profiled test the personality's matrix has."""
    return [pytest.param(kind, test, marks=pytest.mark.personality(kind), id=f"{kind.lower()}-{test.name}")
            for kind in (NETDEV, SANA2) for test in matrix(kind.lower()) if test.name in PROFILED]


@pytest.fixture(scope="session")
def perf_log(installed, capture):
    """The session's debug log, with genet's perf reports in it; skips the tier when the driver does not
    profile or the log is not captured."""
    if not profiled(installed[DRIVER]):
        pytest.skip("the driver under test reports no perf slots: build genet with EMU68_PROFILE and BACKEND=pistorm")
    if not capture.ok:
        pytest.skip(f"no debug log capture: {capture.why}")
    return capture


@pytest.mark.parametrize("kind,test", cases())
@pytest.mark.covers("datapath")
def test_the_per_call_costs_hold_their_baselines(request, device, installed, peer, baselines, perf_log, interface,
                                                 test_dir, kind, test, record_property):
    start = perf_log.mark()
    run = bench(device, test.args.format(peer=peer), timeout=test.deadline)
    time.sleep(REPORT_PERIOD_S)
    text = perf_log.text(start)
    (test_dir / "serial.txt").write_text(text, encoding="latin-1")
    assert isinstance(run, Throughput), f"{test.name}: sockbench printed no total"
    slots = perf.totals(text, LOG_PREFIX)
    assert slots, "no [genet] perf report on the serial line while the test ran (adapter attached? its machine?)"
    misses = []
    found = {f"{kind.lower()}.{test.name}.{name}.avg_us": [round(slot.avg_us, 2)] for name, slot in slots.items()}
    values.write(test_dir, request.config.getoption("--server"), installed[DRIVER],
                 [(interface.boots, found)])
    for name, slot in sorted(slots.items()):
        record_property(name, f"{slot.avg_us:.2f} us x {slot.calls}")
        try:
            baselines.check_near(f"{kind.lower()}.{test.name}.{name}.avg_us", round(slot.avg_us, 2), False,
                                 TOLERANCE)
        except AssertionError as miss:
            misses.append(str(miss))
    assert not misses, "; ".join(misses)
