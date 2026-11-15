# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""The parsers of the Amiga programs' outputs, on outputs recorded on the PI4 (sanitised)."""
from pathlib import Path

import pytest

from harness import bsdsocktest, netdevstats, perf, sockbench, speedtest, stat

pytestmark = pytest.mark.unit

SAMPLES = Path(__file__).parent / "samples"


def sample(name: str) -> str:
    return (SAMPLES / name).read_text(encoding="latin-1")


def test_sockbench_throughput():
    run = sockbench.parse(sample("sockbench_tcp_tx.txt"))
    assert run == sockbench.Throughput(926.3, 1158036388, 0)


def test_sockbench_udp_rx_reads_the_total_not_the_target():
    assert sockbench.parse(sample("sockbench_udp_rx_200.txt")).mbps == 186.3


def test_sockbench_counts_refused_datagrams():
    assert sockbench.parse(sample("sockbench_udp_tx_refused.txt")).refused == 37


def test_sockbench_request_response():
    run = sockbench.parse(sample("sockbench_rr_1.txt"))
    assert run == sockbench.RequestResponse(1783, 538, 317, 496, 721, 1082, 3195)


def test_sockbench_without_a_result():
    assert sockbench.parse("sockbench: cannot connect to 192.0.2.100:5001\n") is None


def test_matrix_keeps_each_stack_to_what_it_can_take():
    names = {where: {t.name for t in sockbench.matrix(where)} for where in ("netdev", "sana2", "roadshow")}
    assert "udp_rx_64k" in names["netdev"] and "udp_rx_64k" not in names["sana2"]
    assert "udp_rx_16k_300" in names["roadshow"] and "udp_rx_64k_300" not in names["roadshow"]
    assert {"tcp_tx", "tcp_rx", "rr_1"} <= names["roadshow"] & names["sana2"] & names["netdev"]


def test_series_median_and_latencies():
    rr = sockbench.Test("rr_1", "rr {peer} 1 5", 30)
    runs = [sockbench.RequestResponse(t, 0, 0, p50, 0, 0, 0) for t, p50 in ((1900, 410), (2100, 400), (2000, 405))]
    series = sockbench.Series(rr, runs)
    assert rr.latency and series.median == 2000 and series.latencies() == [410, 400, 405]


def test_stat_share_between_two_samples():
    before, after = stat.parse(sample("stat_before.txt")), stat.parse(sample("stat_after.txt"))
    assert before == stat.Sample(9.02, 16.97, 73382)
    share = stat.share(before, after)
    assert share.cpu_percent == pytest.approx(57.4, abs=0.1)
    assert share.switches_per_s == pytest.approx(3421, abs=1)
    assert share.seconds == pytest.approx(12.0)


def test_stat_without_executive():
    assert stat.parse("Unknown command Stat\n") is None


def test_stat_out_of_order():
    with pytest.raises(ValueError):
        stat.share(stat.Sample(10, 10, 0), stat.Sample(10, 10, 0))


def test_bsdsocktest_clean_run():
    outcome = bsdsocktest.parse(sample("bsdsocktest_clean.log"))
    assert (outcome.passed, outcome.total, outcome.clean) == (142, 142, True)


def test_bsdsocktest_failures_leave_known_issues_out():
    outcome = bsdsocktest.parse(sample("bsdsocktest_failures.log"))
    assert not outcome.clean
    assert outcome.not_ok == ["2 - select(): writable after connect [BSD 4.4]"]
    assert (outcome.failed, outcome.known, outcome.skipped) == (1, 1, 1)


def test_bsdsocktest_unfinished_run():
    assert bsdsocktest.parse("TAP version 12\nok 1 - socket()\n") is None


def test_netdev_stats_summary():
    stats = netdevstats.parse(sample("netdevstats.txt"))
    assert stats.rx == netdevstats.Direction(112448, 167883208, 0, 0)
    assert stats.tx.packets == 4469
    assert (stats.link_up, stats.speed_mbps, stats.full_duplex) == (True, 1000, True)


def test_netdev_stats_counters_and_coalescing():
    text = "counters:\n  rx_packets                   120\n  mib_rx_gd_pkts               118  +5 (+2/s)\n"
    assert netdevstats.parse(text).counters == {"rx_packets": 120, "mib_rx_gd_pkts": 118}
    assert netdevstats.coalescing("coalesce set: rx 200 us / 16 frames, tx 32 frames\n") == (200, 16, 32)
    assert netdevstats.coalescing("SET_COALESCE failed (3)\n") is None


def test_amispeedtest_averages():
    assert speedtest.parse(sample("amispeedtest.txt")) == {"download": 852.8, "upload": 81.6}


def test_perf_totals_add_the_reports_up():
    slots = perf.totals(sample("perf_serial.txt"))
    assert slots["rx_wake"].calls == 2124 + 489
    assert slots["rx_wake"].max_us == 7555
    assert slots["rx_reply"].avg_us == pytest.approx(49689 / 4448)
    assert "hist" not in slots


PS_SAMPLE = """\x1b[4m  PID  PPID    TIME     CSW NAME              \x1b[24m
   13     0  0.211s   15984 [Executive]
  241     0   2m26s   88725 bsdsocket.library
  253     0   1h06m  141052 Matrix
  395     0  2.819s   44063 genet ethernet dri
"""


def test_ps_gives_each_task_its_time_and_whether_it_has_milliseconds():
    tasks = stat.parse_ps(PS_SAMPLE)
    assert sorted(tasks) == [13, 241, 253, 395]
    assert tasks[395] == stat.Task(395, 0, 2.819, True, 44063, "genet ethernet dri")
    assert (tasks[241].seconds, tasks[241].precise) == (146.0, False)
    assert (tasks[253].seconds, tasks[13].name) == (3960.0, "Executive")
    assert stat.parse_ps("Unknown command Ps\n") == {}


def test_task_deltas_count_a_new_task_from_zero_and_a_reused_pid_afresh():
    before = stat.parse_ps(PS_SAMPLE)
    after = stat.parse_ps(PS_SAMPLE.replace("2.819s   44063", "7.000s   50000")
                          .replace("0.211s   15984 [Executive]", "0.300s     10 [Ps]"))
    deltas = {d["pid"]: d for d in stat.task_deltas(before, after)}
    assert (deltas[395]["seconds"], deltas[395]["switches"], deltas[395]["precise"]) == (4.181, 5937, True)
    assert (deltas[13]["name"], deltas[13]["seconds"]) == ("Ps", 0.3)
    assert deltas[241]["precise"] is False
