# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
The claim rules (AGENTS.md): netdev holds the unit while a stack is attached,
SANA-II while an opener has configured it or taken it online; nobody holds it
with the interface removed; each personality takes it once the other let go.
Beside both: the opens the driver takes and refuses, and closes that carry
something other than the driver's own Opener (the issued-opener list).
"""
from __future__ import annotations

import pytest

from emu68test.results import parse
from harness.stack import NETDEV, SANA2, Script, rc_of
from harness.tools import GENETTEST, genettest

pytestmark = [pytest.mark.hw, pytest.mark.full]

BOTH = [pytest.param(NETDEV, marks=pytest.mark.personality(NETDEV), id="netdev"),
        pytest.param(SANA2, marks=pytest.mark.personality(SANA2), id="sana2")]


@pytest.mark.personality(NETDEV)
@pytest.mark.covers("claim: nobody holds", "NDERR_BADVERSION")
def test_nobody_holds_the_unit_once_the_interface_is_removed(device, interface):
    out = "RAM:gt/out/holder-removed.txt"
    script = Script("holder-removed").remove()
    script.program("holder", GENETTEST, "HOLDER", out)
    script.add("add", interface.path(NETDEV))
    log = interface.run(script, NETDEV)
    text = device.download_text(out)
    device.keep(out, text)
    result = parse(text, rc_of(log, "holder"))
    assert result.passed and result.kv.get("holder") == "none", result.raw


@pytest.mark.personality(NETDEV)
@pytest.mark.covers("claim: netdev holds", "S2ERR_OUTOFSERVICE", "NDERR_BUSY", "S2_CONFIGINTERFACE", "S2_ONLINE",
                    "S2_OFFLINE", "S2_ADDMULTICASTADDRESS", "S2_DELMULTICASTADDRESS", "S2_ADDMULTICASTADDRESSES",
                    "S2_DELMULTICASTADDRESSES", "CMD_WRITE", "S2_BROADCAST", "S2_MULTICAST", "CMD_READ",
                    "S2_READORPHAN", "S2_ONEVENT")
def test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev(device):
    genettest(device, "NETDEV").require("GenetTest NETDEV")


@pytest.mark.parametrize("kind", BOTH)
@pytest.mark.covers("OpenDevice", "IOERR_OPENFAIL", "IOERR_UNITBUSY", "SANA2OPF_MINE")
def test_the_opens_the_driver_takes_and_refuses(device, kind):
    genettest(device, "BAREOPEN").require("GenetTest BAREOPEN")


@pytest.mark.parametrize("kind", BOTH)
@pytest.mark.covers("CloseDevice", "issued-opener list")
def test_a_close_carrying_a_foreign_pointer_or_null_leaves_the_unit_working(device, kind):
    genettest(device, "CLOSE").require("GenetTest CLOSE")
    genettest(device, "QUERY").require("GenetTest QUERY after the closes")


@pytest.mark.personality(NETDEV, last=True)
@pytest.mark.covers("claim: SANA-II after netdev")
def test_sana2_takes_the_unit_once_netdev_let_go(interface):
    interface.use(SANA2)  # removes the netdev interface, adds it as SANA-II, checks HOLDER
    assert interface.holder() == "sana2"


@pytest.mark.personality(SANA2, last=True)
@pytest.mark.covers("claim: netdev after SANA-II")
def test_netdev_takes_the_unit_once_sana2_let_go(interface):
    interface.use(NETDEV)
    assert interface.holder() == "netdev"
