# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
The perf tier, the verdict on speed: sockbench's matrix in each personality,
each on a boot of its own, every test run --reps times at priority 6 with
Executive's Stat and Ps around each run. Each judged figure's median must
reach its baseline's bound, the worst of the seeding boots (baselines.py): no
worse than the driver was when the baselines were seeded. A test with a miss
is measured once more on a fresh boot, and the median of both boots' runs is
judged. The context-switch rate and the per-task split are recorded beside
them. values.json in the test's directory has every run's values per boot
(what run.sh seed reads), stat.json the Stat and Ps samples. --seeding
measures without judging. AmiSpeedTest is recorded, never judged: the internet
decides most of it.
"""
from __future__ import annotations

import json
import statistics

import pytest

from harness import speedtest, values
from harness.amiga import RAM
from harness.install import DRIVER
from harness.matrix import FIGURES, Block, key, measure
from harness.sockbench import matrix
from harness.stack import NETDEV, SANA2
from harness.tools import PRIORITY

pytestmark = [pytest.mark.hw, pytest.mark.perf]

KINDS = (NETDEV, SANA2)
SPEEDTEST_S = 240
SPEEDTEST_SCRIPT = f"{RAM}/speedtest"


def cases():
    """(personality, test) for every matrix test, each personality on a boot of its own."""
    return [pytest.param(kind, test, marks=pytest.mark.personality(kind, boot=True), id=f"{kind.lower()}-{test.name}")
            for kind in KINDS for test in matrix(kind.lower())]


def merged(blocks: list[tuple[int, Block]]) -> dict[str, list[float]]:
    """Every figure's values over the boots of @blocks."""
    found: dict[str, list[float]] = {}
    for _, block in blocks:
        for figure, runs in block.figures().items():
            found.setdefault(figure, []).extend(runs)
    return found


def misses_of(baselines, kind: str, test: str, figures: dict[str, list[float]]) -> list[str]:
    """Each judged figure's median checked against its baseline's bound (the others recorded): the misses."""
    misses = []
    for figure, runs in figures.items():
        if not runs:
            continue
        higher, judged = FIGURES[figure]
        median = statistics.median(runs)
        if not judged:
            baselines.record(key(kind, test, figure), median)
            continue
        try:
            baselines.check(key(kind, test, figure), median, higher)
        except AssertionError as miss:
            misses.append(str(miss))
    return misses


def kept(test_dir, machine: str, driver: bytes, kind: str, test: str, blocks: list[tuple[int, Block]]) -> None:
    """values.json and stat.json of @blocks in @test_dir."""
    boots = [(boot, {key(kind, test, f): runs for f, runs in block.figures().items() if runs})
             for boot, block in blocks]
    values.write(test_dir, machine, driver, boots)
    (test_dir / "stat.json").write_text(json.dumps([{"boot": boot, "runs": block.raw()} for boot, block in blocks],
                                                   indent=1))


@pytest.mark.parametrize("kind,test", cases())
@pytest.mark.covers("datapath")
def test_the_matrix_holds_its_baselines(request, device, peer, baselines, interface, artefacts, test_dir, kind,
                                        test, record_property):
    opt = request.config.getoption
    blocks = [(interface.boots, measure(device, test, peer, opt("--reps"), opt("--stat"), opt("--ps")))]
    assert blocks[0][1].runs, f"{test.name}: no run printed a result ({blocks[0][1].missed} tried)"
    misses = misses_of(baselines, kind, test.name, merged(blocks))
    if misses:
        record_property("measured again", "a miss on the first boot: " + "; ".join(misses))
        interface.fresh_boot(artefacts)
        interface.use(kind)
        blocks.append((interface.boots, measure(device, test, peer, opt("--reps"), opt("--stat"), opt("--ps"))))
        misses = misses_of(baselines, kind, test.name, merged(blocks))
    kept(test_dir, opt("--server"), device.installed[DRIVER], kind, test.name, blocks)
    for figure, runs in merged(blocks).items():
        record_property(figure, f"{statistics.median(runs):g} ({', '.join(f'{v:g}' for v in runs)})")
    for name, count in (("runs without a result", sum(b.missed for _, b in blocks)),
                        ("Stat windows stretched by a wait, dropped", sum(b.stretched() for _, b in blocks))):
        if count:
            record_property(name, count)
    assert not misses, "; ".join(misses)


@pytest.mark.parametrize("kind", [pytest.param(k, marks=pytest.mark.personality(k, boot=True), id=k.lower())
                                  for k in KINDS])
def test_amispeedtest_is_recorded(request, device, baselines, interface, test_dir, kind, record_property):
    command = request.config.getoption("--speedtest")
    if not command:
        pytest.skip("no AmiSpeedTest (--speedtest '')")
    device.upload_bytes(SPEEDTEST_SCRIPT, f"ChangeTaskPri {PRIORITY}\n{command} UNIT=MEGABIT\n".encode("latin-1"))
    found: dict[str, list[float]] = {}
    for _ in range(request.config.getoption("--reps")):
        _, text = device.run_text("Execute", SPEEDTEST_SCRIPT, SPEEDTEST_S)
        for direction, mbps in speedtest.parse(text).items():
            found.setdefault(key(kind, "amispeedtest", f"{direction}_mbps"), []).append(mbps)
    assert found, "AmiSpeedTest printed no average"
    values.write(test_dir, request.config.getoption("--server"), device.installed[DRIVER],
                 [(interface.boots, found)])
    for name, runs in found.items():
        median = statistics.median(runs)
        baselines.record(name, median)
        record_property(name.rsplit(".", 1)[-1], f"{median:g} ({', '.join(f'{v:g}' for v in runs)})")
