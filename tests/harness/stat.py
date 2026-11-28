# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
stat.py - Executive's Stat, the CPU accounting the AmiKit machines have
(CLAUDE.md: Executive skews timings; a test runs at priority 6, out of its
reach). Two Stat outputs around a run give the CPU share the run took and its
context switches per second.

    Total used CPU time:  0:00:09.02 (34.70%)
    Total idle CPU time:  0:00:16.97 (65.30%)
    Context switches:     73382

The output carries ANSI bold/underline sequences; they are stripped first.

Ps gives each task's CPU time, which splits a run's share between the genet
unit task, the stack's tasks and the rest:

      PID  PPID    TIME     CSW NAME
      395     0  2.819s   44063 genet ethernet dri
      241     0   2m26s   88725 bsdsocket.library

TIME has milliseconds below a minute and whole seconds above it (imprecise
for a 10 s run); names are cut to 18 characters.
"""
from __future__ import annotations

import re
from dataclasses import dataclass

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m|\x9b[0-9;]*m")
TIME_RE = re.compile(r"Total (used|idle) CPU time:\D*(\d+):(\d+):(\d+)\.(\d+)")
CTX_RE = re.compile(r"Context switches:\D*(\d+)")
PS_RE = re.compile(r"^\s*(\d+)\s+(\d+)\s+(\S+)\s+(\d+)\s(.*)$")
PS_TIME_RE = re.compile(r"^(?:(\d+)h)?(?:(\d+)m)?(?:(\d+)(?:\.(\d+))?s)?$")


@dataclass
class Sample:
    """One Stat output: used and idle CPU seconds, and the context switches so far."""

    used: float
    idle: float
    switches: int


@dataclass
class Share:
    """What a run took between two samples."""

    cpu_percent: float
    switches_per_s: float
    seconds: float = 0.0    # the window: used + idle CPU time between the two samples


def _seconds(hours: str, minutes: str, seconds: str, hundredths: str) -> float:
    return int(hours) * 3600 + int(minutes) * 60 + int(seconds) + int(hundredths) / 100


def parse(text: str) -> Sample | None:
    """A Stat output's totals; None when it is not one (no Executive)."""
    clean = ANSI_RE.sub("", text)
    times = {kind: _seconds(*rest) for kind, *rest in TIME_RE.findall(clean)}
    ctx = CTX_RE.search(clean)
    if "used" not in times or "idle" not in times or ctx is None:
        return None
    return Sample(times["used"], times["idle"], int(ctx.group(1)))


def share(before: Sample, after: Sample) -> Share:
    """The CPU share and context-switch rate between @before and @after."""
    used = after.used - before.used
    total = used + after.idle - before.idle
    if total <= 0:
        raise ValueError("the two Stat samples are not in order")
    return Share(100.0 * used / total, (after.switches - before.switches) / total, total)


@dataclass
class Task:
    """One line of Ps: a task's CPU time so far and its context switches."""

    pid: int
    ppid: int
    seconds: float
    precise: bool       # milliseconds (under a minute), not whole seconds
    switches: int
    name: str


def _ps_seconds(text: str) -> tuple[float, bool] | None:
    """Ps's TIME: '2.819s', '2m26s', '1h06m'; with whether it has milliseconds."""
    m = PS_TIME_RE.match(text)
    if m is None or not any(m.groups()):
        return None
    hours, minutes, seconds, millis = (int(g) if g else 0 for g in m.groups())
    precise = m.group(4) is not None
    return hours * 3600 + minutes * 60 + seconds + (millis / 1000 if precise else 0), precise


def parse_ps(text: str) -> dict[int, Task]:
    """Ps PLAIN FORMAT=PID,PPID,TIME,CSW,NAME, by pid; empty when it is not that."""
    found = {}
    for line in ANSI_RE.sub("", text).splitlines():
        m = PS_RE.match(line)
        time = _ps_seconds(m.group(3)) if m else None
        if m and time is not None:
            found[int(m.group(1))] = Task(int(m.group(1)), int(m.group(2)), time[0], time[1], int(m.group(4)),
                                          m.group(5).strip().strip("[]"))
    return found


def task_deltas(before: dict[int, Task], after: dict[int, Task]) -> list[dict]:
    """What each task that lived through the window used: a task started inside it counts from zero."""
    found = []
    for pid, task in sorted(after.items()):
        start = before.get(pid)
        if start is not None and start.name != task.name:
            start = None                                   # a pid used again
        seconds, switches = (task.seconds - start.seconds, task.switches - start.switches) if start else \
            (task.seconds, task.switches)
        precise = task.precise and (start is None or start.precise)
        found.append({"pid": pid, "ppid": task.ppid, "name": task.name, "seconds": round(seconds, 3),
                      "precise": precise, "switches": switches})
    return found
