# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
tools.py - the programs the hardware tiers run on the Amiga, from RAM:gt:
GenetTest (the rig), lwip-amiga's sockbench and netdev-stats, bsdsocktest;
pushed once per boot, run with the agent up.

Every measuring run goes through an Execute script at priority 6, out of
Executive's reach (CLAUDE.md); the programs a detached Script runs inherit its
priority instead (Script.pri()).
"""
from __future__ import annotations

from emu68test.machine import Device
from emu68test.results import Result

from . import netdevstats, sockbench
from .build import Artefacts
from .stack import NETDEV_STATS, SOCKBENCH
from .suite import RAM

GENETTEST = f"{RAM}/GenetTest"
BSDSOCKTEST = f"{RAM}/bsdsocktest"
BENCH = f"{RAM}/bench1"
PRIORITY = 6


def push(device: Device, artefacts: Artefacts) -> None:
    """GenetTest, sockbench, netdev-stats and the bench script into RAM:gt, unless they went up this boot."""
    device.upload_once(artefacts.genettest, GENETTEST)
    device.upload_once(artefacts.sockbench, SOCKBENCH)
    device.upload_once(artefacts.netdev_stats, NETDEV_STATS)
    if BENCH not in device.ram_known:
        device.upload_bytes(BENCH, sockbench.script(PRIORITY))
        device.ram_known.add(BENCH)


def genettest(device: Device, command: str, timeout: float = 60) -> Result:
    """GenetTest @command (its name and number) run to its end, its output parsed."""
    return device.run(GENETTEST, command, timeout)


def bench(device: Device, args: str, timeout: float = 60) -> sockbench.Throughput | sockbench.RequestResponse | None:
    """One sockbench run at priority 6 (@args after the command, the peer in them): what it measured, or None."""
    _, text = device.run_text("Execute", f"{BENCH} {args}", timeout)
    return sockbench.parse(text)


def netdev_stats(device: Device, args: str = "") -> tuple[int, str]:
    """netdev-stats with @args: its return code and output."""
    return device.run_text(NETDEV_STATS, args, 30)


def counters(device: Device) -> netdevstats.Stats:
    """netdev-stats' counters and link, parsed; AssertionError when it did not answer."""
    rc, text = netdev_stats(device)
    assert rc == 0, f"netdev-stats rc {rc}:\n{text[-600:]}"
    return netdevstats.parse(text)
