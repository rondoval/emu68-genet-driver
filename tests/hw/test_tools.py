# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
genet-stats, the statistics window the package installs in C:, beside each
personality: it opens its window, keeps updating it, and quits on Ctrl-C.
What it reads (S2_GETEXTENDEDGLOBALSTATS, S2_GETSPECIALSTATS) GenetTest QUERY
checks; this checks the program.
"""
from __future__ import annotations

import time

import pytest

from harness.install import GENET_STATS
from harness.stack import NETDEV, SANA2

pytestmark = [pytest.mark.hw, pytest.mark.full]

TITLE = "genet.device Statistics"
RUN_S = 4  # a few of its 1 s refreshes
WAKE_TRIES = 10
WAKE_STEP_S = 0.5
WAKE_PIXELS = 20


def front_tree(device) -> str:
    """The front screen's window tree once the window is there: ui_tree reads the front screen only, and a
    screen blanker in front gives the front back only on mouse movement, so move it until the window shows
    (up to WAKE_TRIES times)."""
    tree = device.ami.ui_tree()
    for _ in range(WAKE_TRIES):
        if TITLE in tree:
            break
        device.ami.input_move_rel(WAKE_PIXELS, 0)
        device.ami.input_move_rel(-WAKE_PIXELS, 0)
        time.sleep(WAKE_STEP_S)
        tree = device.ami.ui_tree()
    return tree


@pytest.mark.parametrize("kind", [pytest.param(k, marks=pytest.mark.personality(k), id=k.lower())
                                  for k in (NETDEV, SANA2)])
@pytest.mark.covers("genet-stats")
def test_genet_stats_shows_its_window_and_quits_on_ctrl_c(device, kind):
    job = device.start(GENET_STATS)
    try:
        time.sleep(RUN_S)
        assert device.procs("genet-stats"), f"genet-stats ended at once:\n{job.output()}"
        tree = front_tree(device)
        assert TITLE in tree, f"no '{TITLE}' window on the front screen:\n{tree[:1500]}"
    finally:
        job.stop()
    assert not device.procs("genet-stats"), "genet-stats did not quit on Ctrl-C"
