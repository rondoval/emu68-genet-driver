# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""SANA-II writes as the driver sends them (sana2_tx.c under vamos, the TX ring a fake register block):
the frame on the ring, the refusals, a failed copy and the events it raises, the backlog of a full ring
in order, its aborts and flushes, the Miami rounding."""
from __future__ import annotations

import pytest

from harness.vamos import grouped

pytestmark = pytest.mark.logic

PROGRAM = "LogicSana2"
STATION = "02:00:00:00:00:01"           # the unit's address in the logic programs (unit.c)
OTHER = "02:00:00:00:00:77"
GROUP = "01:00:5e:00:00:fb"
BROADCAST = "ff:ff:ff:ff:ff:ff"
PAYLOAD_FIRST = 0x10111213              # what the openers' CopyFromBuff writes first (sana2.c)
HEADER = 14
MAX_DATA = 2048 - 2 - 64 - HEADER - 4   # S2_TX_MAX_DATALEN: a staging slot less its pad, TSB, header, round-up
RING_ROOM = 255                         # a 256-BD ring with one BD always left free
ABORTED = ("IOERR_ABORTED", "S2WERR_GENERIC_ERROR")
WRITER_EVENTS = 0x1 | 0x2 | 0x20 | 0x80  # S2EVENT_ERROR | TX | BUFF | SOFTWARE


def online(*options: str) -> list[str]:
    """Steps that open an opener per entry of @options and bring the unit online through the first."""
    return [f"open={o}" if o else "open" for o in options] + ["task", "link=up", "cmd=a,S2_CONFIGINTERFACE"]


@pytest.fixture
def steps(logic_program):
    """The SANA-II side after each of the steps given, as LogicSana2 prints it."""
    program = logic_program(PROGRAM)

    def run(*given: str) -> list[dict[str, str]]:
        found = program.run(*given).steps()
        assert len(found) == len(given)
        assert all(step["forbids"] == "0" for step in found), "Forbid/Permit left unbalanced"
        return found

    return run


def replies(step: dict[str, str]) -> list[dict[str, str]]:
    """The requests replied during the step, in order."""
    return grouped(step, "reply")


def outcome(entry: dict[str, str]) -> tuple[str, str]:
    """A request's (io_Error, ios2_WireError)."""
    return entry["error"], entry["wire_error"]


def ring(step: dict[str, str]) -> dict[str, str]:
    """What the step left on the TX ring: produced, doorbell and the last frame's fields."""
    return {key.removeprefix("ring."): value for key, value in step.items() if key.startswith("ring.")}


@pytest.mark.covers("CMD_WRITE", "SANA-II TX")
def test_a_write_goes_on_the_ring_behind_an_ethernet_header(steps):
    step = steps(*online(""), f"write=a,{OTHER},0x800,50")[-1]
    assert outcome(step) == ("S2ERR_NO_ERROR", "S2WERR_GENERIC_ERROR") and step["kept"] == "no"
    assert ring(step) == {"produced": "1", "doorbell": "1", "length": str(HEADER + 50), "destination": OTHER,
                          "source": STATION, "type": "0x800", "after_header": hex(PAYLOAD_FIRST)}
    assert [r["request"] for r in replies(step)] == [step["request"]] and step["tx_packets"] == "1"


@pytest.mark.covers("S2_BROADCAST", "S2_MULTICAST", "SANA-II TX")
def test_broadcast_and_multicast_writes_go_to_their_addresses(steps):
    broadcast, multicast = steps(*online(""), "write=a,broadcast,0x806,28", f"write=a,{GROUP},0x800,40")[-2:]
    assert ring(broadcast)["destination"] == BROADCAST and ring(broadcast)["length"] == str(HEADER + 28)
    assert ring(multicast)["destination"] == GROUP and ring(multicast)["produced"] == "2"


@pytest.mark.covers("SANA2IOF_RAW", "CMD_WRITE", "S2_CopyFromBuff")
def test_a_raw_write_sends_the_buffer_as_the_whole_frame(steps):
    step = steps(*online(""), f"write=a,{OTHER},0x800,60,raw")[-1]
    assert ring(step)["length"] == "60" and ring(step)["destination"] == "10:11:12:13:14:15"


@pytest.mark.covers("CMD_WRITE", "S2ERR_BAD_ARGUMENT", "S2ERR_NO_RESOURCES", "S2ERR_MTU_EXCEEDED")
@pytest.mark.parametrize("write, expected", [
    (f"write=null,{OTHER},0x800,50", ("S2ERR_BAD_ARGUMENT", "S2WERR_NULL_POINTER")),
    (f"write=a,{OTHER},0x800,0", ("S2ERR_NO_RESOURCES", "S2WERR_BUFF_ERROR")),
    (f"write=a,{OTHER},0x800,{MAX_DATA + 1}", ("S2ERR_MTU_EXCEEDED", "S2WERR_GENERIC_ERROR")),
], ids=["no opener", "no data", "too long"])
def test_a_write_that_can_never_be_sent_is_refused_before_the_ring(steps, write, expected):
    step = steps(*online(""), write)[-1]
    assert outcome(step) == expected and step["kept"] == "no" and ring(step)["produced"] == "0"


@pytest.mark.covers("CMD_WRITE")
def test_the_longest_write_a_slot_holds_is_sent(steps):
    step = steps(*online(""), f"write=a,{OTHER},0x800,{MAX_DATA}")[-1]
    assert outcome(step)[0] == "S2ERR_NO_ERROR" and ring(step)["length"] == str(HEADER + MAX_DATA)


@pytest.mark.covers("CMD_WRITE", "S2ERR_OUTOFSERVICE")
def test_an_offline_unit_refuses_writes(steps):
    step = steps("open", "task", f"write=a,{OTHER},0x800,50")[-1]
    assert outcome(step) == ("S2ERR_OUTOFSERVICE", "S2WERR_UNIT_OFFLINE") and ring(step)["produced"] == "0"


@pytest.mark.covers("S2_CopyFromBuff", "S2ERR_NO_RESOURCES", "S2EVENT_TX", "S2EVENT_BUFF", "S2EVENT_ERROR",
                    "S2EVENT_SOFTWARE")
def test_a_failing_copy_drops_the_write_and_the_unit_task_reports_its_events(steps):
    waiting, failed, reported = steps(*online("sendfail"), f"cmd=a,S2_ONEVENT,{WRITER_EVENTS:#x}",
                                      f"write=a,{OTHER},0x800,50", "service")[-3:]
    assert waiting["kept"] == "yes"
    assert outcome(failed) == ("S2ERR_NO_RESOURCES", "S2WERR_BUFF_ERROR") and failed["tx_dropped"] == "1"
    assert ring(failed)["produced"] == "0" and ring(failed)["doorbell"] == "0", "nothing was published"
    events = [r for r in replies(reported) if "events" in r]
    assert [r["events"] for r in events] == ["S2EVENT_ERROR|S2EVENT_TX|S2EVENT_BUFF|S2EVENT_SOFTWARE"]


@pytest.mark.covers("SANA-II TX backlog", "CMD_WRITE")
def test_a_full_ring_keeps_writes_waiting_in_order_until_the_hardware_has_sent(steps):
    filled, first, second, consumed, drained = steps(*online(""), f"writes=a,{RING_ROOM}",
                                                     f"write=a,{OTHER},0x800,50", f"write=a,{OTHER},0x806,28",
                                                     "consumed", "service")[-5:]
    assert ring(filled)["produced"] == str(RING_ROOM)
    assert (first["kept"], second["kept"], second["tx_waiting"]) == ("yes", "yes", "2")
    assert replies(first) == [] and replies(consumed) == []
    assert [r["request"] for r in replies(drained)] == [first["request"], second["request"]]
    assert ring(drained)["produced"] == str(RING_ROOM + 2) and ring(drained)["type"] == "0x806"
    assert drained["tx_waiting"] == "0"


@pytest.mark.covers("AbortIO", "IOERR_ABORTED", "SANA-II TX backlog")
def test_aborting_a_waiting_write_answers_it_aborted(steps):
    waiting, aborted = steps(*online(""), f"writes=a,{RING_ROOM}", f"write=a,{OTHER},0x800,50",
                             f"abort={1 + RING_ROOM}")[-2:]
    assert waiting["kept"] == "yes"
    assert [r["request"] for r in replies(aborted)] == [waiting["request"]] and outcome(replies(aborted)[0]) == ABORTED
    assert aborted["tx_waiting"] == "0"


@pytest.mark.covers("CloseDevice", "CMD_FLUSH", "IOERR_ABORTED", "SANA-II TX backlog")
@pytest.mark.parametrize("answer", [["close=a", "task"], ["cmd=a,CMD_FLUSH"]], ids=["close", "flush"])
def test_a_close_or_a_flush_answers_the_waiting_writes_aborted(steps, answer):
    found = steps(*online(""), f"writes=a,{RING_ROOM}", f"write=a,{OTHER},0x800,50", *answer)
    waiting, answered = found[-1 - len(answer)], found[-1]
    aborted = [r for r in replies(answered) if r["request"] == waiting["request"]]
    assert len(aborted) == 1 and outcome(aborted[0]) == ABORTED and answered["tx_waiting"] == "0"


@pytest.mark.covers("prefs USE_MIAMI_WORKAROUND", "S2_CopyFromBuff")
def test_the_miami_workaround_rounds_the_write_s_copy_up_and_sends_its_length(steps):
    step = steps(*online(""), "miami=on", f"write=a,{OTHER},0x800,49")[-1]
    assert replies(step)[0]["copied"] == "52" and ring(step)["length"] == str(HEADER + 49)
