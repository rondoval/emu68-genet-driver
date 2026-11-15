# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""The compare tier's criterion, the blocks a speed test is measured in, and what a profile build is known by."""
import pytest

from harness.compare import judge

pytestmark = pytest.mark.unit


def test_compare_throughput_against_the_reference_minimum():
    assert judge("tcp_tx", [930, 934, 938], [931, 925, 933], higher=True).ok
    verdict = judge("tcp_tx", [930, 934, 938], [920, 925, 929], higher=True)
    assert not verdict.ok and verdict.bound == 930 and "MISS" in str(verdict)


def test_compare_latency_against_the_reference_maximum():
    assert judge("rr_1.p50_us", [400, 410, 405], [410, 409, 412], higher=False).ok
    assert not judge("rr_1.p50_us", [400, 410, 405], [411, 412, 415], higher=False).ok


def test_compare_needs_both_sides():
    with pytest.raises(ValueError):
        judge("tcp_tx", [], [930], higher=True)


def test_a_block_gives_each_figure_per_run_the_cpu_where_stat_ran():
    from harness import sockbench, stat
    from harness.matrix import Block, key
    tcp = sockbench.MATRIX[0]
    samples = [(stat.Sample(10.0, 10.0, 1000), stat.Sample(15.126, 19.874, 10000))]
    block = Block(tcp, [sockbench.Throughput(930.1, 1), sockbench.Throughput(925.0, 1)], 0, samples)
    assert block.figures() == {"mbps": [930.1, 925.0], "cpu_pct": [34.2], "ctxsw_s": [600.0]}
    assert block.raw() == [{"before": {"used": 10.0, "idle": 10.0, "switches": 1000},
                            "after": {"used": 15.126, "idle": 19.874, "switches": 10000}, "seconds": 15.0, "kept": True,
                            "tasks": []}]
    rr = next(t for t in sockbench.MATRIX if t.name == "rr_1")
    runs = [sockbench.RequestResponse(2100, 470, 300, 405, 600, 900, 2000)]
    assert Block(rr, runs).figures() == {"tps": [2100.0], "p50_us": [405.0]}
    assert key("NETDEV", "rr_1", "p50_us") == "netdev.rr_1.p50_us"


def test_a_stat_window_stretched_by_a_wait_is_dropped():
    from harness import sockbench, stat
    from harness.matrix import Block
    tcp = sockbench.MATRIX[0]
    samples = [(stat.Sample(0.0, 0.0, 0), stat.Sample(9.0, 1.0, 700)),        # the run's own 10 s
               (stat.Sample(39.9, 82.09, 0), stat.Sample(52.2, 202.79, 96795))]  # 133 s, the 2026-11-14 one
    block = Block(tcp, [sockbench.Throughput(930.0, 1), sockbench.Throughput(931.0, 1)], 0, samples)
    assert block.figures()["cpu_pct"] == [90.0] and block.stretched() == 1
    assert [r["kept"] for r in block.raw()] == [True, False]
    rr = next(t for t in sockbench.MATRIX if t.name == "rr_1")
    assert not Block(rr, [], 0, samples[:1]).kept(stat.share(*samples[0]))      # 10 s for a 5 s run


def test_the_compare_tally_judges_every_figure_both_drivers_measured():
    from harness import sockbench
    from harness.compare import judge_all, tests_of
    from harness.matrix import Block, Tally, higher_is_better
    tcp = sockbench.MATRIX[0]
    tally = Tally()
    for label, values in (("reference", [930.0, 934.0]), ("build", [925.0, 928.0]), ("reference", [931.0])):
        tally.add(label, "NETDEV", Block(tcp, [sockbench.Throughput(v, 1) for v in values]))
    assert tally.of("reference") == {"netdev.tcp_tx.mbps": [930.0, 934.0, 931.0]}
    verdicts = judge_all(tally.of("reference"), tally.of("build"), higher_is_better)
    assert [(v.name, v.ok, v.bound) for v in verdicts] == [("netdev.tcp_tx.mbps", False, 930.0)]
    assert tests_of(verdicts) == {"tcp_tx"}
    assert higher_is_better("sana2.rr_1.tps") and not higher_is_better("sana2.rr_1.p50_us")


def test_a_profile_build_is_known_by_its_perf_report_format():
    from harness.build import profiled
    assert profiled(b"\x00[%s] %s: n=%lu sum=%luus avg=%lu.%luus max=%luus\x00")
    assert not profiled(b"$VER: emu68-genet-driver 5.0")


def test_stat_is_sampled_around_each_run_that_printed_a_result(monkeypatch):
    from harness import matrix, sockbench, stat
    samples = iter(stat.Sample(float(n), 100.0, n) for n in range(6))
    runs = iter([sockbench.Throughput(930.0, 1), None, sockbench.Throughput(931.0, 1)])
    monkeypatch.setattr(matrix, "sample", lambda device, command: next(samples))
    monkeypatch.setattr(matrix, "bench", lambda device, args, timeout: next(runs))
    monkeypatch.setattr(matrix.time, "sleep", lambda s: None)
    block = matrix.measure(None, sockbench.MATRIX[0], "peer", reps=3)
    assert block.missed == 1 and [r.mbps for r in block.runs] == [930.0, 931.0]
    assert [(b.switches, a.switches) for b, a in block.samples] == [(0, 1), (4, 5)]


def test_the_tally_keeps_every_stat_sample_with_its_driver_and_boot():
    from harness import sockbench, stat
    from harness.matrix import Block, Tally
    tally = Tally()
    samples = [(stat.Sample(1.0, 1.0, 0), stat.Sample(2.0, 2.0, 50))]
    tally.add("genet-4.4", "NETDEV", Block(sockbench.MATRIX[0], [sockbench.Throughput(930.0, 1)], 0, samples), boot=3)
    assert [(s["driver"], s["boot"], s["test"], s["run"], s["after"]["switches"]) for s in tally.stats] == \
        [("genet-4.4", 3, "tcp_tx", 0, 50)]


def test_context_switches_are_shown_beside_the_verdicts_never_judged():
    from harness import compare, sockbench, stat
    from harness.matrix import Block, Tally, judged
    assert judged("netdev.tcp_tx.cpu_pct") and not judged("netdev.tcp_tx.ctxsw_s")
    tally = Tally()
    for label, switches in (("genet-4.4", 1000), (compare.BUILD, 9000)):
        samples = [(stat.Sample(0.0, 0.0, 0), stat.Sample(5.0, 5.0, switches))]
        tally.add(label, "NETDEV", Block(sockbench.MATRIX[0], [sockbench.Throughput(930.0, 1)], 0, samples))
    verdicts = compare.against(tally, "genet-4.4")
    assert [(v.name, v.ok) for v in verdicts] == [("netdev.tcp_tx.cpu_pct", True), ("netdev.tcp_tx.mbps", True)]
    md = compare.table("genet-4.4", verdicts, tally)
    assert "| netdev.tcp_tx.ctxsw_s | 100 | 900 | 900 | | shown |" in md
    assert md.startswith("## build against genet-4.4") and "| netdev.tcp_tx.mbps | 930 | 930 | 930 | 930 | ok |" in md


def test_each_reference_is_labelled_by_its_file_name():
    from harness.compare import labels
    paths = ["a/genet-3.17.device", "b/genet-combined.device", "c/genet-3.17.device", "d/build.device"]
    assert labels(paths) == ["genet-3.17", "genet-combined", "genet-3.17-3", "build-4"]


def test_the_per_task_split_counts_only_millisecond_times_of_kept_windows():
    from harness import sockbench, stat
    from harness.matrix import Block
    tcp = sockbench.MATRIX[0]
    samples = [(stat.Sample(0.0, 0.0, 0), stat.Sample(9.0, 1.0, 700))] * 2
    unit = {"name": "genet ethernet dri", "seconds": 2.5, "precise": True}
    stack = [{"name": "bsdsocket.library", "seconds": 1.0, "precise": True},
             {"name": "bsdsocket.library", "seconds": 3.0, "precise": True}]
    coarse = {"name": "bsdsocket.library", "seconds": 4.0, "precise": False}
    block = Block(tcp, [sockbench.Throughput(440.0, 1)] * 2, 0, samples, [[unit, *stack], [unit, coarse]])
    figures = block.figures()
    assert figures["unit_pct"] == [25.0, 25.0] and figures["stack_pct"] == [40.0]
    assert block.raw()[1]["tasks"] == [unit, coarse]
