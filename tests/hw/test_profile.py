# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
The profile tier: a profile build of the driver (genet in EMU68_PROFILE, the
pistorm debug backend), its perf slots read off the Pi's UART (uart.py) while
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

from harness import perf, values
from harness.build import profiled
from harness.install import DRIVER
from harness.sockbench import Throughput, matrix
from harness.stack import NETDEV, SANA2
from harness.tools import bench
from harness.uart import Capture

pytestmark = [pytest.mark.hw, pytest.mark.profile]

PROFILED = ("tcp_tx", "tcp_rx", "udp_tx_1472", "udp_rx_200", "udp_rx_64k", "udp_rx_64k_300")
REPORT_PERIOD_S = 2.5   # perf.h reports every ~2 s: one more after the run
TOLERANCE = "20%"


def cases():
    """(personality, test) for every profiled test the personality's matrix has."""
    return [pytest.param(kind, test, marks=pytest.mark.personality(kind), id=f"{kind.lower()}-{test.name}")
            for kind in (NETDEV, SANA2) for test in matrix(kind.lower()) if test.name in PROFILED]


@pytest.fixture(scope="session")
def capture(request, rundir, device):
    """The session's serial capture; skips the tier when the driver does not profile or the port is missing."""
    if not profiled(device.installed[DRIVER]):
        pytest.skip("the driver under test reports no perf slots: build genet with EMU68_PROFILE and BACKEND=pistorm")
    port = request.config.getoption("--serial")
    try:
        found = Capture(rundir / "serial.log", port)
    except Exception as e:  # noqa: BLE001 - no adapter, a busy port, no pyserial: the tier cannot run
        pytest.skip(f"no serial capture on {port}: {e}")
    yield found
    found.close()


@pytest.mark.parametrize("kind,test", cases())
@pytest.mark.covers("datapath")
def test_the_per_call_costs_hold_their_baselines(request, device, peer, baselines, capture, interface, test_dir,
                                                 kind, test, record_property):
    start = capture.position()
    run = bench(device, test.args.format(peer=peer), timeout=test.deadline)
    time.sleep(REPORT_PERIOD_S)
    text = capture.text_since(start)
    (test_dir / "serial.txt").write_text(text, encoding="latin-1")
    assert isinstance(run, Throughput), f"{test.name}: sockbench printed no total"
    slots = perf.totals(text)
    assert slots, "no [genet] perf report on the serial line while the test ran (adapter attached? its machine?)"
    misses = []
    found = {f"{kind.lower()}.{test.name}.{name}.avg_us": [round(slot.avg_us, 2)] for name, slot in slots.items()}
    values.write(test_dir, request.config.getoption("--server"), device.installed[DRIVER],
                 [(interface.boots, found)])
    for name, slot in sorted(slots.items()):
        record_property(name, f"{slot.avg_us:.2f} us x {slot.calls}")
        try:
            baselines.check_near(f"{kind.lower()}.{test.name}.{name}.avg_us", round(slot.avg_us, 2), False,
                                 TOLERANCE)
        except AssertionError as miss:
            misses.append(str(miss))
    assert not misses, "; ".join(misses)
