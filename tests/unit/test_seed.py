# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""values.json, and baselines.json seeded from the runs of the seeding boots."""
import json

import pytest

from harness import values
from emu68test.baselines import Baseline, Baselines
from harness.seed import FEW_BOOTS, seed

pytestmark = pytest.mark.unit

DRIVER = b"genet 5.0"


def run(tmp_path, name: str, boots: list[tuple[int, dict]], driver: bytes = DRIVER, machine: str = "amiga-pi4"):
    """A run directory with one test directory holding @boots in its values.json."""
    test_dir = tmp_path / name / "test_perf" / "test_x"
    test_dir.mkdir(parents=True)
    values.write(test_dir, machine, driver, boots)
    return tmp_path / name


def test_values_keep_each_boot_and_the_driver(tmp_path):
    values.write(tmp_path, "amiga-pi4", DRIVER, [(1, {"netdev.tcp_tx.mbps": [940.0]}), (2, {})])
    data = values.read(tmp_path / values.NAME)
    assert data["driver"] == values.digest(DRIVER) and data["boots"] == [
        {"boot": 1, "values": {"netdev.tcp_tx.mbps": [940.0]}}]


def test_a_judged_figure_is_bounded_by_its_worst_boot_median(tmp_path):
    first = run(tmp_path, "seed-1", [(1, {"netdev.tcp_tx.mbps": [944.0, 738.7, 945.0],
                                          "netdev.tcp_tx.cpu_pct": [60.0, 61.0, 62.0]})])
    second = run(tmp_path, "seed-2", [(1, {"netdev.tcp_tx.mbps": [930.0, 931.0, 929.0],
                                           "netdev.tcp_tx.cpu_pct": [66.0, 64.0, 65.0]}),
                                      (2, {"netdev.tcp_tx.mbps": [942.0, 940.0, 941.0],
                                           "netdev.tcp_tx.cpu_pct": [59.0, 60.0, 61.0]})])
    out = tmp_path / "baselines.json"
    lines = seed([first, second], "amiga-pi4", out)
    base = Baselines.load(out, "amiga-pi4").base
    assert base["netdev.tcp_tx.mbps"] == Baseline(940.0, 930.0, 3, 9)     # boot medians 944, 930, 941
    assert base["netdev.tcp_tx.cpu_pct"] == Baseline(61.0, 65.0, 3, 9)    # lower is better: the highest
    assert lines[0].startswith(f"{out}: 2 keys for amiga-pi4") and len(lines) == 1


def test_shown_figures_and_the_unjudged_tiers_keep_only_their_median(tmp_path):
    rundir = run(tmp_path, "seed-1", [(1, {"netdev.tcp_tx.ctxsw_s": [4000.0, 4500.0],
                                           "netdev.amispeedtest.download_mbps": [850.0],
                                           "netdev.tcp_tx.tx_submit.avg_us": [0.9]})])
    out = tmp_path / "baselines.json"
    seed([rundir], "amiga-pi4", out)
    data = json.loads(out.read_text())["amiga-pi4"]
    assert data["netdev.tcp_tx.ctxsw_s"] == {"median": 4250.0, "boots": 1, "runs": 2}
    assert "bound" not in data["netdev.amispeedtest.download_mbps"]
    assert "bound" not in data["netdev.tcp_tx.tx_submit.avg_us"]


def test_a_bound_from_few_boots_is_named(tmp_path):
    rundir = run(tmp_path, "seed-1", [(1, {"sana2.rr_1.tps": [960.0]})])
    lines = seed([rundir], "amiga-pi4", tmp_path / "baselines.json")
    assert lines[1:] == ["  sana2.rr_1.tps: 1 boot(s) only"] and FEW_BOOTS > 1


def test_a_key_measured_by_two_driver_builds_is_refused(tmp_path):
    first = run(tmp_path, "seed-1", [(1, {"sana2.rr_1.tps": [960.0]})])
    second = run(tmp_path, "seed-2", [(1, {"sana2.rr_1.tps": [950.0]})], driver=b"genet 5.1")
    with pytest.raises(SystemExit, match="1 keys measured by more than one driver build"):
        seed([first, second], "amiga-pi4", tmp_path / "baselines.json")
    seed([first, second], "amiga-pi4", tmp_path / "baselines.json", any_driver=True)


def test_the_profile_builds_keys_beside_the_release_builds(tmp_path):
    release = run(tmp_path, "perf", [(1, {"netdev.tcp_tx.mbps": [940.0]})])
    profile = run(tmp_path, "profile", [(1, {"netdev.tcp_tx.tx_submit.avg_us": [0.9]})], driver=b"profile")
    lines = seed([release, profile], "amiga-pi4", tmp_path / "baselines.json")
    assert ": 1 keys" in lines[0] and lines[0].count("driver ") == 2


def test_another_machines_runs_are_left_out(tmp_path):
    mine = run(tmp_path, "seed-1", [(1, {"sana2.rr_1.tps": [960.0]})])
    other = run(tmp_path, "seed-2", [(1, {"sana2.rr_1.tps": [10.0]})], machine="amiga-cm4")
    out = tmp_path / "baselines.json"
    seed([mine, other], "amiga-pi4", out)
    assert Baselines.load(out, "amiga-pi4").base["sana2.rr_1.tps"].median == 960.0
    with pytest.raises(SystemExit, match="no values.json"):
        seed([other], "amiga-pi4", out)


def test_the_tally_keeps_each_drivers_values_per_boot():
    from harness import sockbench
    from harness.matrix import Block, Tally
    tally = Tally()
    tcp = sockbench.MATRIX[0]
    tally.add("build", "NETDEV", Block(tcp, [sockbench.Throughput(940.0, 1)]), boot=2)
    tally.add("build", "NETDEV", Block(tcp, [sockbench.Throughput(941.0, 1)]), boot=4)
    tally.add("genet-4.4", "NETDEV", Block(tcp, [sockbench.Throughput(935.0, 1)]), boot=1)
    assert tally.boots_of("build") == [(2, {"netdev.tcp_tx.mbps": [940.0]}), (4, {"netdev.tcp_tx.mbps": [941.0]})]
