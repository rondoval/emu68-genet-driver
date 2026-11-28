# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
matrix.py - one sockbench test measured as the perf and compare tiers do it:
REPS runs at priority 6, 2 s apart, each with Executive's Stat before and
after it for its CPU share and context switches; then the figures each run
gives and the keys they are kept under.

    netdev.tcp_tx.mbps        throughput, the median of the runs (higher is better)
    netdev.rr_1.tps           request-response transactions per second (higher)
    netdev.rr_1.p50_us        its median latency (lower)
    netdev.tcp_tx.cpu_pct     the CPU share of each run (lower)
    netdev.tcp_tx.ctxsw_s     context switches per second of each run (shown, never judged)
    netdev.tcp_tx.unit_pct    the genet unit task's share of each run, from Ps (shown)
    netdev.tcp_tx.stack_pct   the stack's tasks' share (bsdsocket.library*: the stack, the SANA-II pump; shown)
"""
from __future__ import annotations

import itertools
import time
from dataclasses import dataclass, field

from . import stat
from emu68test.machine import Device
from .sockbench import RequestResponse, Test, Throughput
from .tools import bench

STAT = "SYS:Utilities/EXPANSION/Executive/Stat"
PS = "SYS:Utilities/EXPANSION/Executive/Ps PLAIN FORMAT=PID,PPID,TIME,CSW,NAME"
UNIT_TASK = "genet ethernet"        # Ps cuts names to 18 characters
STACK_TASKS = "bsdsocket.library"   # "bsdsocket.library stack" and "... sana2 pump"
REPS = 3
PAUSE_S = 2
STRETCH = 1.5   # a Stat window this much longer than the run measured a wait around it (a 133 s one, 2026-11-14)

# figure -> (higher is better, judged against the baseline's bound; False: shown only)
FIGURES = {
    "mbps": (True, True),
    "tps": (True, True),
    "p50_us": (False, True),
    "cpu_pct": (False, True),
    "ctxsw_s": (False, False),  # explains a CPU share; more switches are not by themselves worse
    "unit_pct": (False, False), # where the CPU share went: Executive's per-task split
    "stack_pct": (False, False),
}


def key(kind: str, test: str, figure: str) -> str:
    """The baselines key of @figure of @test under personality @kind."""
    return f"{kind.lower()}.{test}.{figure}"


def higher_is_better(name: str) -> bool:
    """Which way a key's figure is better, from its last part."""
    return FIGURES[name.rsplit(".", 1)[-1]][0]


def judged(name: str) -> bool:
    """Whether a key's figure is judged at all, or only shown (context switches)."""
    return FIGURES[name.rsplit(".", 1)[-1]][1]


@dataclass
class Block:
    """One test's runs, and what Stat said around each run that printed a result."""

    test: Test
    runs: list[Throughput | RequestResponse] = field(default_factory=list)
    missed: int = 0                 # runs that printed no result
    samples: list[tuple[stat.Sample, stat.Sample]] = field(default_factory=list)   # (before, after) per run
    tasks: list[list[dict]] = field(default_factory=list)   # per sampled run: stat.task_deltas(), [] without Ps

    def shares(self) -> list[stat.Share]:
        """Each sampled run's CPU share and context-switch rate, but for windows stretched by a wait (kept())."""
        return [s for s in (stat.share(before, after) for before, after in self.samples) if self.kept(s)]

    def task_shares(self, prefix: str) -> list[float]:
        """The share of each kept run's window that tasks named @prefix* used, where Ps timed them in ms."""
        found = []
        for (before, after), deltas in zip(self.samples, self.tasks):
            share = stat.share(before, after)
            mine = [d for d in deltas if d["name"].startswith(prefix)]
            if self.kept(share) and mine and all(d["precise"] for d in mine):
                found.append(round(100.0 * sum(d["seconds"] for d in mine) / share.seconds, 1))
        return found

    def kept(self, share: stat.Share) -> bool:
        """Whether @share's window is the run's own, not one stretched by a wait before or after it."""
        return share.seconds <= self.test.seconds * STRETCH

    def stretched(self) -> int:
        """How many of the Stat windows a wait stretched (dropped from the figures)."""
        return len(self.samples) - len(self.shares())

    def figures(self) -> dict[str, list[float]]:
        """Each figure's per-run values: mbps, or tps and p50_us; cpu_pct and ctxsw_s where Stat ran."""
        found = self._sockbench_figures()
        shares = self.shares()
        if shares:
            found["cpu_pct"] = [round(s.cpu_percent, 1) for s in shares]
            found["ctxsw_s"] = [float(round(s.switches_per_s)) for s in shares]
        for figure, prefix in (("unit_pct", UNIT_TASK), ("stack_pct", STACK_TASKS)):
            values = self.task_shares(prefix)
            if values:
                found[figure] = values
        return found

    def _sockbench_figures(self) -> dict[str, list[float]]:
        """What sockbench printed: tps and p50_us for a latency test, mbps otherwise."""
        if self.test.latency:
            answers = [r for r in self.runs if isinstance(r, RequestResponse)]
            return {"tps": [float(r.tps) for r in answers], "p50_us": [float(r.p50_us) for r in answers]}
        return {"mbps": [r.mbps for r in self.runs if isinstance(r, Throughput)]}

    def raw(self) -> list[dict]:
        """The Stat samples as they were read, one entry per sampled run, and whether it counted (stat.json)."""
        found = []
        for (before, after), deltas in itertools.zip_longest(self.samples, self.tasks, fillvalue=[]):
            share = stat.share(before, after)
            found.append({"before": vars(before), "after": vars(after), "seconds": round(share.seconds, 2),
                          "kept": self.kept(share), "tasks": deltas})
        return found


def sample(device: Device, command: str) -> stat.Sample | None:
    """Executive's totals now; None without Executive (or with --stat '')."""
    if not command:
        return None
    rc, text = device.exec(command, timeout=20)
    return stat.parse(text) if rc == 0 else None


def tasks_now(device: Device, command: str) -> dict[int, stat.Task]:
    """Executive's per-task CPU times now; empty without Ps (or with --ps '')."""
    if not command:
        return {}
    rc, text = device.exec(command, timeout=20)
    return stat.parse_ps(text) if rc == 0 else {}


def measure(device: Device, test: Test, peer: str, reps: int = REPS, stat_command: str = STAT,
            ps_command: str = "") -> Block:
    """@test run @reps times against @peer, Stat around each run, and Ps around that when @ps_command is given."""
    block = Block(test)
    for _ in range(reps):
        tasks_before = tasks_now(device, ps_command)
        before = sample(device, stat_command)
        run = bench(device, test.args.format(peer=peer), timeout=test.deadline)
        after = sample(device, stat_command)
        tasks_after = tasks_now(device, ps_command)
        if run is None:
            block.missed += 1
        else:
            block.runs.append(run)
            if before is not None and after is not None:
                block.samples.append((before, after))
                block.tasks.append(stat.task_deltas(tasks_before, tasks_after) if tasks_after else [])
        time.sleep(PAUSE_S)
    return block


class Tally:
    """Each driver's per-run values under their keys, as the compare tier collects them boot by boot."""

    def __init__(self):
        self.values: dict[str, dict[str, list[float]]] = {}
        self.by_boot: dict[str, dict[int, dict[str, list[float]]]] = {}   # driver -> boot -> key -> values
        self.stats: list[dict] = []     # every raw Stat sample, with its driver, boot and test (stat.json)

    def add(self, driver: str, kind: str, block: Block, boot: int = 0) -> None:
        """@block's figures into @driver's lists; its Stat samples kept as measured on @boot."""
        lists = self.values.setdefault(driver, {})
        boot_lists = self.by_boot.setdefault(driver, {}).setdefault(boot, {})
        for figure, values in block.figures().items():
            lists.setdefault(key(kind, block.test.name, figure), []).extend(values)
            boot_lists.setdefault(key(kind, block.test.name, figure), []).extend(values)
        for run, raw in enumerate(block.raw()):
            self.stats.append({"driver": driver, "boot": boot, "test": block.test.name, "run": run, **raw})

    def of(self, driver: str) -> dict[str, list[float]]:
        """@driver's lists (empty when it ran nothing)."""
        return self.values.get(driver, {})

    def boots_of(self, driver: str) -> list[tuple[int, dict[str, list[float]]]]:
        """@driver's lists per boot, (boot, {key: values}), as values.json keeps them."""
        return sorted(self.by_boot.get(driver, {}).items())
