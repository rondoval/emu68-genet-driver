# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
What only a SANA-II holder can do: the multicast filter's phases against the
PC's sender, interrupt coalescing set through netdev-stats beside it, S2_OFFLINE
and S2_ONLINE with their events, every kind of kept request taken back, and the
copy lengths with USE_MIAMI_WORKAROUND on and off.
"""
from __future__ import annotations

import time

import pytest

from harness import netdevstats, sockbench
from harness.limits import SHORT, TCP_FLOOR_MBPS
from harness.peers import mcast_sender
from harness.results import parse
from harness.stack import SANA2, Script, rc_of, section
from harness.tools import GENETTEST, PRIORITY, bench, genettest, netdev_stats

pytestmark = [pytest.mark.hw, pytest.mark.full, pytest.mark.personality(SANA2)]

PHASE_S = 6
PHASES = 3
SENDER_SLACK_S = 25        # the sender outlasts the phases and GenetTest's start
SENDER_LEAD_S = 3          # the sender is on the wire before the first phase
CYCLES = 3
COALESCE = (200, 16, 32)   # rx us, rx frames, tx frames
MIAMI_STEPS = ("on", "off")


def outcome(device, log: str, label: str, out: str):
    """The Result of a GenetTest run a Script made, its output in @out and its rc in the log."""
    text = device.download_text(out)
    device.keep(out, text)
    return parse(text, rc_of(log, label))


@pytest.mark.covers("S2_ADDMULTICASTADDRESSES", "S2_DELMULTICASTADDRESSES", "multicast filter", "SANA2IOF_MCAST",
                    "SANA2IOF_BCAST")
def test_the_multicast_filter_passes_what_was_asked_for(device, peer, record_property):
    sender = mcast_sender(peer, PHASES * PHASE_S + SENDER_SLACK_S)
    try:
        time.sleep(SENDER_LEAD_S)
        result = genettest(device, f"MCAST {PHASE_S}", timeout=PHASES * PHASE_S + 60)
    finally:
        sent = sender.communicate(timeout=120)[0].strip()
    record_property("PC sender", sent)
    result.require("GenetTest MCAST")


@pytest.mark.covers("NETDEV_CMD_SET_COALESCE")
def test_coalescing_set_beside_sana2_and_released(device, peer):
    rx_us, rx_frames, tx_frames = COALESCE
    rc, text = netdev_stats(device, f"RXUSECS {rx_us} RXFRAMES {rx_frames} TXFRAMES {tx_frames}")
    assert rc == 0 and netdevstats.coalescing(text) == COALESCE, f"rc {rc}:\n{text}"
    run = bench(device, f"rx {peer} {SHORT}")
    assert isinstance(run, sockbench.Throughput) and run.mbps >= TCP_FLOOR_MBPS[SANA2], run
    rc, text = netdev_stats(device, "RXUSECS 0 RXFRAMES 0 TXFRAMES 0")
    assert rc == 0 and netdevstats.coalescing(text) == (0, 0, 0), f"the release: rc {rc}:\n{text}"


@pytest.mark.covers("S2_OFFLINE", "S2_ONLINE", "S2EVENT_OFFLINE", "S2EVENT_ONLINE", "Reconfigurations")
def test_offline_and_online_raise_their_events_and_the_stack_recovers(device, interface, peer, record_property):
    out = "RAM:gt/out/onlinecycle.txt"
    script = Script("onlinecycle").pri(PRIORITY)
    script.program("cycle", GENETTEST, f"ONLINECYCLE {CYCLES}", out)
    script.bench("after the cycles", f"rx {peer} {SHORT}")
    interface.reconfigure(script, "again", SANA2)
    script.bench("after a re-add", f"rx {peer} {SHORT}")
    log = interface.run(script, SANA2)
    outcome(device, log, "cycle", out).require("GenetTest ONLINECYCLE")
    before = sockbench.parse(section(log, "after the cycles") or "")
    record_property("lwip after the cycles, no re-add",
                    f"{before.mbps} Mb/s" if isinstance(before, sockbench.Throughput) else "no data")
    after = sockbench.parse(section(log, "after a re-add") or "")
    assert isinstance(after, sockbench.Throughput) and after.mbps >= TCP_FLOOR_MBPS[SANA2], \
        f"no data after the interface came back:\n{log[-1500:]}"


@pytest.mark.covers("AbortIO", "CMD_READ", "S2_READORPHAN", "S2_ONEVENT", "S2_SAMPLE_THROUGHPUT", "S2ERR_NO_RESOURCES")
def test_every_kept_request_is_taken_back(device):
    genettest(device, "ABORT").require("GenetTest ABORT")


@pytest.mark.covers("prefs USE_MIAMI_WORKAROUND", "S2_CopyToBuff", "S2_CopyFromBuff")
def test_the_miami_workaround_rounds_every_copy_to_longwords(device, interface):
    script = Script("miami")
    outs = {}
    for step in MIAMI_STEPS:
        prefs = interface.prefs_file(f"miami-{step}", f"USE_MIAMI_WORKAROUND={'1' if step == 'on' else '0'}\n")
        interface.reconfigure(script, f"miami {step}", SANA2, prefs)
        outs[step] = f"RAM:gt/out/miami-{step}.txt"
        script.program(f"miami {step} run", GENETTEST, f"MIAMI {step.upper()}", outs[step])
    interface.reconfigure(script, "defaults", SANA2)
    log = interface.run(script, SANA2, limit=300)
    for step in MIAMI_STEPS:
        assert rc_of(log, f"miami {step} add") in (0, 5), f"SANA-II did not come up with the workaround {step}"
        outcome(device, log, f"miami {step} run", outs[step]).require(f"GenetTest MIAMI {step.upper()}")
