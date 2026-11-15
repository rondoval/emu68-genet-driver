# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
values.py - every value a speed test measured, per boot: values.json in the
test's directory, what seed.py folds into baselines.json.

    {"format": 1, "machine": "amiga-pi4", "driver": "70d98f363bbd0dba",
     "boots": [{"boot": 1, "values": {"netdev.tcp_tx.mbps": [940.1, 944.0, 936.0]}}]}

A boot number is the session's (Interface.boots); seed.py tells the runs'
boots apart by their run directory.
"""
from __future__ import annotations

import json
from pathlib import Path

from emu68test.install import digest

FORMAT = 1
NAME = "values.json"


def write(directory: Path, machine: str, driver: bytes, boots: list[tuple[int, dict[str, list[float]]]]) -> None:
    """@boots, (boot, {key: values}), as values.json in @directory, for @driver on @machine."""
    entries = [{"boot": boot, "values": found} for boot, found in boots if found]
    (Path(directory) / NAME).write_text(json.dumps({"format": FORMAT, "machine": machine, "driver": digest(driver),
                                                    "boots": entries}, indent=1, sort_keys=True) + "\n")


def read(path: Path) -> dict:
    """A values.json; ValueError when it is not one this harness wrote."""
    data = json.loads(Path(path).read_text())
    if data.get("format") != FORMAT:
        raise ValueError(f"{path}: format {data.get('format')!r}, this harness reads {FORMAT}")
    return data
