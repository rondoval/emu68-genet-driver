# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
bsdsocktest's whole suite (ALL: loopback and network tiers, its helper on the
PC) over genet in each personality: bsdsocket.library conformance with this
driver under it. Judged from its TAP log; its own known issues are not
failures. It runs for several minutes, detached, at priority 6.
"""
from __future__ import annotations

import pytest

from harness import bsdsocktest
from harness.stack import NETDEV, SANA2
from harness.tools import BSDSOCKTEST, PRIORITY

pytestmark = [pytest.mark.hw, pytest.mark.full]

LIMIT_S = 1800


@pytest.mark.timeout(LIMIT_S + 300)
@pytest.mark.parametrize("kind", [pytest.param(k, marks=pytest.mark.personality(k), id=k.lower())
                                  for k in (NETDEV, SANA2)])
@pytest.mark.covers("bsdsocktest")
def test_bsdsocktest_passes_over_genet(device, artefacts, peer, kind, record_property):
    device.upload_once(artefacts.bsdsocktest, BSDSOCKTEST)
    name = f"bsdsocktest-{kind.lower()}"
    tap, screen, log = f"RAM:gt/out/{name}.tap", f"RAM:gt/out/{name}.out", f"RAM:gt/{name}.log"
    marker = "BSDSOCKTEST DONE"
    body = (f"ChangeTaskPri {PRIORITY}\n"
            f"{BSDSOCKTEST} >{screen} ALL HOST {peer} LOG {tap} NOPAGE\n"
            f'Echo >{log} "rc $RC"\n'
            f'Echo >>{log} "{marker}"\n')
    device.script(name, body, marker, LIMIT_S)
    text = device.download_text(tap)
    device.keep(tap, text)
    device.keep(screen, device.download_text(screen))
    outcome = bsdsocktest.parse(text)
    assert outcome is not None, f"bsdsocktest did not reach its Results line:\n{text[-1500:]}"
    record_property("result", f"{outcome.passed} passed, {outcome.failed} failed, {outcome.known} known, "
                              f"{outcome.skipped} skipped of {outcome.total}")
    assert outcome.clean, "failed: " + "; ".join(outcome.not_ok)
