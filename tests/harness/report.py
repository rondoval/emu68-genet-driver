# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
report.py - the run's report.md: the header (the build, the files replaced on
the Amiga), one row per test with its outcome, time and notes
(record_property), then the baseline warnings and the values measured. It is
the page a person reads after a run; pytest's -ra summary sits beside it.
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Row:
    """One test's outcome."""

    nodeid: str
    outcome: str
    seconds: float
    reason: str = ""
    notes: list[str] = field(default_factory=list)


class Report:
    """What the session collects for report.md."""

    def __init__(self, rundir: Path, title: str):
        self.rundir = Path(rundir)
        self.title = title
        self.rows: list[Row] = []
        self.header: list[str] = []
        self.started = time.time()

    def add(self, row: Row) -> None:
        """A finished test."""
        self.rows.append(row)

    def _counts(self) -> str:
        counts: dict[str, int] = {}
        for row in self.rows:
            counts[row.outcome] = counts.get(row.outcome, 0) + 1
        return ", ".join(f"{n} {outcome}" for outcome, n in sorted(counts.items()))

    def _table(self) -> list[str]:
        lines = ["| test | outcome | s | notes |", "|---|---|---|---|"]
        for row in self.rows:
            notes = "; ".join(row.notes)
            if row.reason:
                notes = row.reason.splitlines()[0] + ("; " + notes if notes else "")
            notes = " ".join(notes.replace("|", "/").split())  # one table row: no newlines
            lines.append(f"| {row.nodeid} | {row.outcome} | {row.seconds:.0f} | {notes} |")
        return lines

    @staticmethod
    def _baselines(baselines) -> list[str]:
        lines: list[str] = []
        if baselines.warnings:
            lines += ["", "## Baselines", ""] + [f"- {w}" for w in baselines.warnings]
        if baselines.measured:
            lines += ["", "## Measured", "", "| key | value |", "|---|---|"]
            lines += [f"| {k} | {v} |" for k, v in sorted(baselines.measured.items())]
        return lines

    def write(self, baselines=None) -> Path:
        """report.md into the run dir."""
        lines = [f"# {self.title}", "",
                 f"{len(self.rows)} tests in {time.time() - self.started:.0f} s: {self._counts()}", ""]
        lines += self.header + [""] + self._table()
        if baselines is not None:
            lines += self._baselines(baselines)
        path = self.rundir / "report.md"
        path.write_text("\n".join(lines) + "\n")
        return path
