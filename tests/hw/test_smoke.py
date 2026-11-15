# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
The smoke tier: the build installed; genet up under TYPE=AUTO as netdev,
answering the queries any opener may make, refusing the SANA-II commands that
would take the unit, and moving TCP both ways; then the same interface as
TYPE=SANA2, held by SANA-II, doing the same. The interface goes back as the
machine had it when the session ends (interface.py).
"""
from __future__ import annotations

import pytest

from harness import netdevstats, sockbench, stack
from harness.limits import GIGABIT, SHORT, TCP_FLOOR_MBPS
from harness.stack import AUTO, SANA2
from harness.tools import bench, counters, genettest, netdev_stats

pytestmark = [pytest.mark.hw, pytest.mark.smoke]

BOTH = [pytest.param(AUTO, marks=pytest.mark.personality(AUTO), id="netdev"),
        pytest.param(SANA2, marks=pytest.mark.personality(SANA2), id="sana2")]


@pytest.mark.install
def test_the_build_under_test_is_installed(device, installed, record_property):
    for remote, data in installed.items():
        assert device.download(remote) == data, f"{remote} is not the build under test"
        record_property(remote, device.version_of(remote))


@pytest.mark.personality(AUTO)
@pytest.mark.covers("NSCMD_DEVICEQUERY", "NETDEV_CMD_ATTACH", "lwip AUTO probe")
def test_auto_brings_genet_up_as_netdev(device, interface):
    # the personality fixture brought it up and GenetTest HOLDER said netdev
    assert interface.holder() == "netdev"
    _, text = device.exec("C:netinfo", timeout=20)
    entry = stack.netinfo(text)
    assert entry is not None and entry.up, f"genet is not up:\n{text}"
    assert entry.address != "0.0.0.0", f"genet has no address:\n{text}"


@pytest.mark.parametrize("kind", BOTH)
@pytest.mark.covers("S2_DEVICEQUERY", "S2_GETSTATIONADDRESS", "S2_GETGLOBALSTATS", "S2_GETEXTENDEDGLOBALSTATS",
                    "S2_GETSPECIALSTATS", "NSCMD_DEVICEQUERY")
def test_any_opener_s_queries_answer(device, kind):
    genettest(device, "QUERY").require("GenetTest QUERY")


@pytest.mark.personality(AUTO)
@pytest.mark.covers("claim: netdev holds", "S2ERR_OUTOFSERVICE", "NDERR_BUSY")
def test_sana2_cannot_take_the_unit_netdev_holds(device):
    genettest(device, "NETDEV").require("GenetTest NETDEV")


@pytest.mark.personality(SANA2)
@pytest.mark.covers("claim: SANA-II holds", "NDERR_BUSY", "CMD_WRITE", "S2_BROADCAST", "S2_MULTICAST", "CMD_READ",
                    "S2_ONEVENT")
def test_sana2_holds_the_unit_and_netdev_cannot_attach(device):
    genettest(device, "SANA2").require("GenetTest SANA2")


@pytest.mark.parametrize("kind", BOTH)
@pytest.mark.parametrize("direction", ["tx", "rx"])
@pytest.mark.covers("datapath")
def test_tcp_moves_at_gigabit_speed(device, peer, kind, direction, record_property):
    run = bench(device, f"{direction} {peer} {SHORT}")
    assert isinstance(run, sockbench.Throughput), f"sockbench {direction} printed no total"
    record_property("Mb/s", run.mbps)
    assert run.mbps >= TCP_FLOOR_MBPS[kind], f"TCP {direction}: {run.mbps} Mb/s, under {TCP_FLOOR_MBPS[kind]}"


@pytest.mark.parametrize("kind", BOTH)
@pytest.mark.covers("NETDEV_CMD_GET_STATS", "NETDEV_CMD_GET_LINK", "NETDEV_CMD_GET_COUNTERS")
def test_netdev_stats_counts_the_traffic_and_sees_the_link(device, peer, kind):
    before = counters(device)
    run = bench(device, f"rx {peer} {SHORT}")
    after = counters(device)
    assert isinstance(run, sockbench.Throughput), "sockbench rx printed no total"
    assert before.rx and after.rx and before.tx and after.tx, "netdev-stats printed no rx/tx counters"
    assert after.rx.packets > before.rx.packets and after.rx.bytes - before.rx.bytes >= run.bytes
    assert after.tx.packets > before.tx.packets, "the ACKs sent are not counted"
    assert (after.link_up, after.speed_mbps, after.full_duplex) == (True, GIGABIT, True), after
    rc, text = netdev_stats(device, "COUNTERS")
    listed = netdevstats.parse(text).counters
    assert rc == 0 and "drv_rx_packets" in listed, f"netdev-stats COUNTERS: rc {rc}:\n{text[-600:]}"
    assert listed["drv_rx_packets"] >= after.rx.packets, "the driver's own count is behind the summary's"
