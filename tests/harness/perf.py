# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
perf.py - the profile build's perf slots on the serial log (emu68-common's
perf.h). Every ~2 s each active slot prints what it counted since its previous
report, and starts again from zero:

    [genet] rx_drain: n=2116 sum=267366us avg=126.3us max=894us

So a test's cost is the sum of the reports captured while it ran. The
"[genet] hist <name>:" bucket lines are another grammar and pass unread.
"""
from __future__ import annotations

import re
from dataclasses import dataclass

SLOT_RE = re.compile(r"^\[(\w+)\] (\w+): n=(\d+) sum=(\d+)us avg=[\d.]+us max=(\d+)us", re.M)


@dataclass
class Slot:
    """One probe's totals over a stretch of reports."""

    calls: int
    sum_us: int
    max_us: int

    @property
    def avg_us(self) -> float:
        """The mean time per call, in us."""
        return self.sum_us / self.calls if self.calls else 0.0


def totals(text: str, prefix: str = "genet") -> dict[str, Slot]:
    """Each @prefix probe's reports in @text added up: calls and time summed, the worst call kept."""
    found: dict[str, Slot] = {}
    for owner, name, calls, total, worst in SLOT_RE.findall(text):
        if owner != prefix:
            continue
        slot = found.setdefault(name, Slot(0, 0, 0))
        slot.calls += int(calls)
        slot.sum_us += int(total)
        slot.max_us = max(slot.max_us, int(worst))
    return found
