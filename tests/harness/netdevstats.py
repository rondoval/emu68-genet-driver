# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
netdevstats.py - lwip-amiga's netdev-stats: the driver's portable counters,
link state, the full counter list (COUNTERS) and the coalescing setting. It
opens the device with NETDEV_CMD_GET_* and SET_COALESCE only, so it answers
beside either personality.

    rx : pkts <n>  bytes <n>
         errs <n>  drops <n>
    tx : pkts <n>  bytes <n>
         errs <n>  drops <n>
    loss: rdma <n>  rbovfl <n>
    link: up|down <speed> Mb/s FD|HD
    counters:
      <name>  <value>
    coalesce set: rx <u> us / <f> frames, tx <t> frames
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

DIR_RE = re.compile(r"^(rx|tx) : pkts (\d+)  bytes (\d+).*\n\s+errs (\d+)  drops (\d+)", re.M)
LOSS_RE = re.compile(r"^loss: rdma (\d+)  rbovfl (\d+)", re.M)
LINK_RE = re.compile(r"^link: (up|down) (\d+) Mb/s (FD|HD)", re.M)
COUNTER_RE = re.compile(r"^  (\S+)\s+(\d+)", re.M)
COALESCE_RE = re.compile(r"^coalesce set: rx (\d+) us / (\d+) frames, tx (\d+) frames", re.M)


@dataclass
class Direction:
    """One direction's portable counters."""

    packets: int
    bytes: int
    errors: int
    drops: int


@dataclass
class Stats:
    """One netdev-stats run."""

    rx: Direction | None = None
    tx: Direction | None = None
    rdma_overruns: int = 0
    fifo_overflows: int = 0
    link_up: bool | None = None
    speed_mbps: int = 0
    full_duplex: bool = False
    counters: dict[str, int] = field(default_factory=dict)


def parse(text: str) -> Stats:
    """What a netdev-stats run printed (fields stay None or empty where it printed nothing)."""
    stats = Stats()
    for tag, *values in DIR_RE.findall(text):
        setattr(stats, tag, Direction(*(int(v) for v in values)))
    loss = LOSS_RE.search(text)
    if loss:
        stats.rdma_overruns, stats.fifo_overflows = int(loss.group(1)), int(loss.group(2))
    link = LINK_RE.search(text)
    if link:
        stats.link_up = link.group(1) == "up"
        stats.speed_mbps = int(link.group(2))
        stats.full_duplex = link.group(3) == "FD"
    at = text.find("counters:\n")
    if at >= 0:
        stats.counters = {name: int(value) for name, value in COUNTER_RE.findall(text[at:])}
    return stats


def coalescing(text: str) -> tuple[int, int, int] | None:
    """The setting a coalescing run reports: (rx us, rx frames, tx frames); None when it failed."""
    m = COALESCE_RE.search(text)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else None
