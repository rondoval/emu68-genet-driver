# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
RemoveNetInterface and AddNetInterface, three times over in each personality:
every add comes up, and the interface moves data after each at full rate (the
unit's last close and the next first open, with nothing left behind).
"""
from __future__ import annotations

import pytest

from harness import sockbench
from harness.limits import SHORT, TCP_FLOOR_MBPS
from harness.stack import NETDEV, SANA2, Script, rc_of, section
from harness.tools import PRIORITY

pytestmark = [pytest.mark.hw, pytest.mark.full]

CYCLES = 3
DIRECTIONS = ("rx", "tx")


@pytest.mark.parametrize("kind", [pytest.param(k, marks=pytest.mark.personality(k), id=k.lower())
                                  for k in (NETDEV, SANA2)])
@pytest.mark.covers("NETDEV_CMD_DETACH", "NETDEV_CMD_STOP", "CloseDevice", "the last close", "the first open")
def test_the_interface_comes_back_after_every_remove(device, interface, peer, kind, record_property):
    script = Script(f"cycles-{kind.lower()}").pri(PRIORITY)
    for n in range(1, CYCLES + 1):
        interface.reconfigure(script, f"cycle {n}", kind)
        script.bench(f"cycle {n} data", f"{DIRECTIONS[n % 2]} {peer} {SHORT}")
    log = interface.run(script, kind, limit=400)
    for n in range(1, CYCLES + 1):
        assert rc_of(log, f"cycle {n} add") in (0, 5), f"cycle {n}: the add failed:\n{log[-1500:]}"
        run = sockbench.parse(section(log, f"cycle {n} data") or "")
        assert isinstance(run, sockbench.Throughput), f"cycle {n}: no data"
        record_property(f"cycle {n} Mb/s", run.mbps)
        assert run.mbps >= TCP_FLOOR_MBPS[kind], f"cycle {n}: {run.mbps} Mb/s"
