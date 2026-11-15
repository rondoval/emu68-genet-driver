# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
baselines.py - what the driver measured when its baselines were seeded, per
machine and stack; a later build must do no worse.

tests/baselines.json, written by seed.py from the runs of the seeding boots:

    {"format": 2,
     "amiga-pi4": {"netdev.tcp_tx.mbps": {"median": 940.1, "bound": 928.8, "boots": 9, "runs": 27},
                   "netdev.tcp_tx.ctxsw_s": {"median": 4250.0, "boots": 6, "runs": 18}},
     "amiga-pi4-roadshow": {...}}

median is every seeding run's median; bound, for a judged figure, the worst
of the seeding boots' own medians (the lowest where higher is better). A run
passes when its median reaches the bound: as good as the driver was on its
worst seeding boot, or better. A figure that is only shown has no bound. The
key of a machine is its amiga MCP server name, with "-roadshow" when the
Roadshow card is in. Which way a figure is better is said where it is
checked (matrix.FIGURES).
"""
from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path

FORMAT = 2


@dataclass
class Baseline:
    """One figure as the seeding boots measured it."""

    median: float
    bound: float | None = None      # the worst seeding boot's median; None: shown, not judged
    boots: int = 0
    runs: int = 0

    def as_json(self) -> dict:
        """The entry in baselines.json, without a bound it does not have."""
        return {k: v for k, v in asdict(self).items() if v is not None}


class Baselines:
    """The baselines of one machine, the values this run measured, and the warnings."""

    def __init__(self, machine: str, base: dict[str, Baseline] | None = None, seeding: bool = False):
        self.machine = machine
        self.base: dict[str, Baseline] = dict(base or {})
        self.seeding = seeding          # measuring for seed.py: nothing judged, nothing warned
        self.measured: dict[str, float] = {}
        self.warnings: list[str] = []

    @classmethod
    def load(cls, path: str | Path, machine: str) -> "Baselines":
        """The baselines of @machine from the JSON at @path; none when the file does not exist."""
        path = Path(path)
        if not path.exists():
            return cls(machine)
        data = json.loads(path.read_text())
        if data.get("format") != FORMAT:
            raise ValueError(f"{path}: format {data.get('format')!r}, this harness reads {FORMAT} (run.sh seed)")
        return cls(machine, {k: Baseline(**v) for k, v in data.get(machine, {}).items()})

    @staticmethod
    def tolerance_of(spec: float | str, base: float) -> float:
        """An absolute tolerance from a number, or from an 'N%' string relative to @base."""
        if isinstance(spec, str):
            if not spec.endswith("%"):
                raise ValueError(f"tolerance {spec!r}: a number, or a percentage like '5%'")
            return abs(base) * float(spec[:-1]) / 100.0
        return float(spec)

    def _known(self, key: str, value: float) -> Baseline | None:
        """@value recorded under @key; its baseline, or None (seeding, or none yet: a warning)."""
        self.measured[key] = value
        if self.seeding:
            return None
        found = self.base.get(key)
        if found is None:
            self.warnings.append(f"{key}: no baseline for {self.machine}, measured {value} (recorded only)")
        return found

    def check(self, key: str, value: float, higher: bool) -> None:
        """Record @value under @key; it must reach the baseline's bound, the worst seeding boot (@higher
        says which way is better). Without a baseline it is only recorded."""
        base = self._known(key, value)
        if base is None:
            return
        if base.bound is None:
            raise ValueError(f"{key}: the baseline has no bound (a figure that is only shown)")
        if higher and value < base.bound:
            raise AssertionError(f"{key}: {value:g} is below {base.bound:g}, the worst of {base.boots} seeding "
                                 f"boots (median {base.median:g}; higher is better)")
        if not higher and value > base.bound:
            raise AssertionError(f"{key}: {value:g} is above {base.bound:g}, the worst of {base.boots} seeding "
                                 f"boots (median {base.median:g}; lower is better)")

    def check_near(self, key: str, value: float, higher: bool, tolerance: float | str) -> None:
        """Record @value under @key; it may fall short of the baseline's median by @tolerance (a number, or
        'N%' of the median): the profile tier's per-call costs, one sample per boot."""
        base = self._known(key, value)
        if base is None:
            return
        allowed = self.tolerance_of(tolerance, base.median)
        if higher and value < base.median - allowed:
            raise AssertionError(f"{key}: {value:g} is below the baseline {base.median:g} by more than "
                                 f"{allowed:g} (higher is better, tolerance {tolerance})")
        if not higher and value > base.median + allowed:
            raise AssertionError(f"{key}: {value:g} is above the baseline {base.median:g} by more than "
                                 f"{allowed:g} (lower is better, tolerance {tolerance})")

    def record(self, key: str, value: float) -> None:
        """Record @value under @key without judging it (AmiSpeedTest: the internet decides)."""
        self.measured[key] = value

    def write_measured(self, path: str | Path) -> None:
        """The values this run measured, as JSON with sorted keys."""
        Path(path).write_text(json.dumps({"format": FORMAT, "machine": self.machine, "measured": self.measured},
                                         indent=2, sort_keys=True) + "\n")


def write(path: str | Path, machine: str, found: dict[str, Baseline]) -> None:
    """@found as @machine's baselines in the file at @path, the other machines' kept."""
    path = Path(path)
    data = json.loads(path.read_text()) if path.exists() else {}
    if data and data.get("format") != FORMAT:
        data = {}       # a format-1 file: numbers without bounds, replaced whole
    data["format"] = FORMAT
    data[machine] = {k: v.as_json() for k, v in sorted(found.items())}
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
