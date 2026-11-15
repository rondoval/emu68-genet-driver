# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""NSCMD_DEVICEQUERY as the unit task answers it (unit_task.c under vamos): a SANA-II device whose
command list holds both personalities' commands, exactly those the driver's handlers serve."""
from __future__ import annotations

import re

import pytest

from harness.vamos import COMPONENT_DIR, listed

pytestmark = pytest.mark.logic

PROGRAM = "LogicUnitTask"
SOURCES = COMPONENT_DIR / "genet.device" / "src"
NSDEVTYPE_SANA2 = 7
RESULT_BYTES = 16                       # sizeof(struct NSDeviceQueryResult)
# BeginIO's own: the reads and writes it serves in the caller's task (device.c)
BEGINIO_COMMANDS = {"CMD_READ", "S2_READORPHAN", "CMD_WRITE", "S2_BROADCAST", "S2_MULTICAST"}


def handled(source: str, function: str) -> set[str]:
    """The commands @function in @source dispatches: its case labels."""
    text = (SOURCES / source).read_text()
    body = text[text.index(f"{function}(struct GenetUnit"):]
    body = body[:body.index("\n}\n")]
    return set(re.findall(r"case\s+(\w+):", body))


@pytest.fixture
def steps(logic_program):
    """The answers of the steps given, as LogicUnitTask prints them."""
    program = logic_program(PROGRAM)

    def run(*given: str) -> list[dict[str, str]]:
        found = program.run(*given).steps()
        assert len(found) == len(given)
        return found

    return run


@pytest.mark.covers("NSCMD_DEVICEQUERY")
def test_the_query_names_a_sana2_device(steps):
    (step,) = steps("query")
    assert (step["error"], step["actual"], step["size_available"]) == ("0", str(RESULT_BYTES), str(RESULT_BYTES))
    assert (step["device_type"], step["device_subtype"], step["format"]) == (str(NSDEVTYPE_SANA2), "0", "0")


@pytest.mark.covers("NSCMD_DEVICEQUERY", "lwip AUTO probe")
def test_the_command_list_is_both_personalities_commands_as_the_handlers_serve_them(steps):
    (step,) = steps("query")
    advertised = listed(step, "cmd")
    assert "?" not in advertised and len(advertised) == len(set(advertised)), advertised
    served = (handled("netdev_api.c", "netdev_process_command") | handled("sana2_api.c", "sana2_process_command")
              | BEGINIO_COMMANDS | {"NSCMD_DEVICEQUERY"})
    assert set(advertised) == served
    assert "NETDEV_CMD_ATTACH" in advertised, "how a stack tells the netdev ABI is there"


@pytest.mark.covers("NSCMD_DEVICEQUERY", "IOERR_BADLENGTH", "IOERR_BADADDRESS")
@pytest.mark.parametrize("operands, error", [(str(RESULT_BYTES - 1), "IOERR_BADLENGTH"), ("null", "IOERR_BADADDRESS")])
def test_a_short_or_missing_result_is_refused(steps, operands, error):
    (step,) = steps(f"query={operands}")
    assert step["error"] == error and step["actual"] == "0" and step["cmds"] == "0"
