# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
bsdsocktest.py - bsdsocktest's TAP log (LOG file), the one its verdict is
read from; the screen output carries CSI sequences and progress lines.

    ok N - description
    not ok N - description
    not ok N - description  # KNOWN <id>: <why>     a known issue, not a failure
    ok N - # SKIP <reason>
    # Results: P passed, F failed, K known, S skipped (T total)
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

RESULTS_RE = re.compile(r"^# Results: (\d+) passed, (\d+) failed, (\d+) known, (\d+) skipped \((\d+) total\)", re.M)
NOT_OK_RE = re.compile(r"^not ok (\d+) - (.*)$", re.M)
KNOWN_MARK = "  # KNOWN "


@dataclass
class Outcome:
    """The suite's own count and the tests that failed."""

    passed: int
    failed: int
    known: int
    skipped: int
    total: int
    not_ok: list[str] = field(default_factory=list)

    @property
    def clean(self) -> bool:
        """Whether nothing failed beyond bsdsocktest's own known issues."""
        return self.failed == 0 and self.passed + self.known + self.skipped == self.total


def parse(log: str) -> Outcome | None:
    """The outcome in a TAP log; None when the run did not reach its Results line."""
    m = RESULTS_RE.search(log)
    if m is None:
        return None
    outcome = Outcome(*(int(x) for x in m.groups()))
    outcome.not_ok = [f"{n} - {d.strip()}" for n, d in NOT_OK_RE.findall(log) if KNOWN_MARK not in d]
    return outcome
