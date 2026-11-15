# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
compare.py - the release verdict of the compare tier: the build under test
against one or more reference drivers, measured on alternating boots
(reference, new, reference, new) in one session. A run on a later boot comes out up to 2 %
faster than the same driver earlier, and the peer and the network drift
between sessions, so only an A/B/A/B pair in one session is a verdict.

Per figure: the new driver's median must reach the reference's worst run
(throughput: its minimum; latency and CPU share: its maximum). A miss is
measured once more before it counts.
"""
from __future__ import annotations

import statistics
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from .matrix import Tally, higher_is_better, judged

BUILD = "build"     # the build's label in a Tally; the references are labelled by labels()


@dataclass
class Verdict:
    """One figure's comparison."""

    name: str
    ok: bool
    new_median: float
    bound: float
    higher: bool

    def __str__(self) -> str:
        relation = ">=" if self.higher else "<="
        state = "ok" if self.ok else "MISS"
        return f"{self.name}: new median {self.new_median:g} {relation} reference {self.bound:g}: {state}"


def judge(name: str, reference: list[float], new: list[float], higher: bool) -> Verdict:
    """@new's median against the reference's worst run; @higher says which way is better."""
    if not reference or not new:
        raise ValueError(f"{name}: nothing measured to compare")
    median = statistics.median(new)
    bound = min(reference) if higher else max(reference)
    ok = median >= bound if higher else median <= bound
    return Verdict(name, ok, median, bound, higher)


def judge_all(reference: dict[str, list[float]], new: dict[str, list[float]],
              higher_of: Callable[[str], bool]) -> list[Verdict]:
    """Every figure both drivers measured, judged (judge()); @higher_of tells each key's better way."""
    return [judge(name, reference[name], new[name], higher_of(name))
            for name in sorted(reference) if reference[name] and new.get(name)]


def tests_of(verdicts: list[Verdict]) -> set[str]:
    """The tests the missed figures belong to: 'netdev.tcp_tx.mbps' -> 'tcp_tx'."""
    return {v.name.split(".")[1] for v in verdicts if not v.ok}


def labels(paths: list[str]) -> list[str]:
    """A label per reference file: its name without the suffix, numbered when it repeats one."""
    found: list[str] = []
    for path in paths:
        stem = Path(path).stem
        found.append(stem if stem not in found and stem != BUILD else f"{stem}-{len(found) + 1}")
    return found


def against(tally: Tally, reference: str) -> list[Verdict]:
    """The build's judged figures against @reference's."""
    judged_lists = {name: values for name, values in tally.of(reference).items() if judged(name)}
    return judge_all(judged_lists, tally.of(BUILD), higher_is_better)


def table(reference: str, verdicts: list[Verdict], tally: Tally) -> str:
    """compare.md's section for @reference: one row per figure both measured, judged or only shown."""
    lines = [f"## {BUILD} against {reference}", "",
             f"| figure | {reference} runs | {BUILD} runs | {BUILD} median | bound | verdict |",
             "|---|---|---|---|---|---|"]
    by_name = {v.name: v for v in verdicts}
    ref_lists, new_lists = tally.of(reference), tally.of(BUILD)
    for name in sorted(set(ref_lists) & set(new_lists)):
        ref = ", ".join(f"{x:g}" for x in ref_lists[name])
        new = ", ".join(f"{x:g}" for x in new_lists[name])
        v = by_name.get(name)
        if v is None:
            lines.append(f"| {name} | {ref} | {new} | {statistics.median(new_lists[name]):g} | | shown |")
        else:
            lines.append(f"| {name} | {ref} | {new} | {v.new_median:g} | {v.bound:g} | {'ok' if v.ok else 'MISS'} |")
    return "\n".join(lines) + "\n"


def ratio(verdict: Verdict) -> float:
    """The build's median against the bound, better-is-above-1 either way."""
    if not verdict.bound or not verdict.new_median:
        return 1.0
    return verdict.new_median / verdict.bound if verdict.higher else verdict.bound / verdict.new_median
