# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
seed.py - baselines.json from the runs of the seeding boots (run.sh seed):
every values.json under the run directories given, folded per key, each key
measured by one driver build (the perf tier's by the release build, the
profile tier's by a profile build). A judged figure's bound is the worst of
its boots' own medians (matrix.FIGURES says which way is better); a figure
that is only shown, AmiSpeedTest and the profile tier's per-call costs keep
the median.

    run.sh seed runs/seed-1 runs/seed-2 runs/seed-3 [--machine amiga-pi4]
"""
from __future__ import annotations

import argparse
import statistics
import sys
from collections import defaultdict
from pathlib import Path

from . import values
from emu68test.baselines import Baseline, write
from .matrix import FIGURES

TESTS = Path(__file__).resolve().parent.parent
FEW_BOOTS = 3       # a bound from fewer boots than this is noted: the next boot may well fall outside it


def collect(rundirs: list[Path], machine: str) -> tuple[dict[str, list[list[float]]], dict[str, set[str]]]:
    """Each key's values per boot from @machine's values.json files under @rundirs; the drivers that measured it."""
    per_boot: dict[str, dict[str, list[float]]] = defaultdict(dict)
    drivers: dict[str, set[str]] = defaultdict(set)
    for rundir in rundirs:
        for path in sorted(Path(rundir).glob(f"*/*/{values.NAME}")):
            data = values.read(path)
            if data["machine"] != machine:
                continue
            for boot in data["boots"]:
                for key, found in boot["values"].items():
                    per_boot[key].setdefault(f"{Path(rundir).name}#{boot['boot']}", []).extend(found)
                    drivers[key].add(data["driver"])
    return {key: list(boots.values()) for key, boots in per_boot.items()}, drivers


def fold(boots: list[list[float]], figure: str) -> Baseline:
    """One key's boots as its baseline: the median of every run; for a judged @figure, the worst boot median."""
    every = [v for boot in boots for v in boot]
    higher, judged = FIGURES.get(figure, (True, False))
    bound = None
    if judged:
        medians = [statistics.median(boot) for boot in boots if boot]
        bound = min(medians) if higher else max(medians)
    return Baseline(round(statistics.median(every), 2), None if bound is None else round(bound, 2),
                    len(boots), len(every))


def refuse_mixed(drivers: dict[str, set[str]]) -> None:
    """SystemExit when a key was measured by more than one driver build."""
    mixed = sorted(key for key, builds in drivers.items() if len(builds) > 1)
    if mixed:
        raise SystemExit(f"{len(mixed)} keys measured by more than one driver build ({mixed[0]}: "
                         f"{', '.join(sorted(drivers[mixed[0]]))}): seed each from one build (or --any-driver)")


def summary(out: Path, machine: str, found: dict[str, Baseline], drivers: dict[str, set[str]]) -> list[str]:
    """What was written, by driver build, and the bounds from fewer than FEW_BOOTS boots."""
    builds: dict[str, int] = defaultdict(int)
    for key in found:
        for build in drivers[key]:
            builds[build] += 1
    lines = [f"{out}: {len(found)} keys for {machine}; "
             + ", ".join(f"driver {build}: {count} keys" for build, count in sorted(builds.items()))]
    return lines + [f"  {key}: {b.boots} boot(s) only" for key, b in sorted(found.items())
                    if b.bound is not None and b.boots < FEW_BOOTS]


def seed(rundirs: list[Path], machine: str, out: Path, any_driver: bool = False) -> list[str]:
    """baselines.json for @machine from @rundirs, into @out; the lines worth reading."""
    per_boot, drivers = collect(rundirs, machine)
    if not per_boot:
        raise SystemExit(f"no {values.NAME} for {machine} under {', '.join(map(str, rundirs))}")
    if not any_driver:
        refuse_mixed(drivers)
    found = {key: fold(boots, key.rsplit(".", 1)[-1]) for key, boots in per_boot.items()}
    write(out, machine, found)
    return summary(out, machine, found, drivers)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="run.sh seed", description=__doc__.split("\n\n")[0])
    parser.add_argument("rundirs", nargs="+", type=Path, help="run directories of the seeding boots")
    parser.add_argument("--machine", default="amiga-pi4", help="the amiga MCP server the runs measured")
    parser.add_argument("--out", type=Path, default=TESTS / "baselines.json")
    parser.add_argument("--any-driver", action="store_true", help="let one key mix runs of several driver builds")
    args = parser.parse_args(argv)
    for line in seed(args.rundirs, args.machine, args.out, args.any_driver):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
