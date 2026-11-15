# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""SANA-II multicast as the driver keeps it (sana2_mcast.c, and bcmgenet-mac.c's MDF programming, under
vamos): the ranges and their use counts, the flattening into exact-match slots, promiscuous past 15
addresses or with a promiscuous opener, the refusals, and a netdev stack's filter left alone."""
from __future__ import annotations

import pytest

from emu68test.vamos import grouped, listed

pytestmark = pytest.mark.logic

PROGRAM = "LogicMcast"
BROADCAST = "ff:ff:ff:ff:ff:ff"
STATION = "02:00:00:00:00:01"           # the unit's address in the logic programs (unit.c)
NETDEV_GROUP = "01:00:5e:7f:ff:fa"      # the netdev stack's own filter (mcast.c)
MDF_SLOTS = 17


def group(last: int) -> str:
    """An IPv4 multicast group address ending in @last."""
    return f"01:00:5e:00:00:{last:02x}"


def enabled(slots: int) -> int:
    """UMAC_MDF_CTRL with the first @slots MDF slots enabled: slot k is bit 16 - k."""
    return sum(1 << (MDF_SLOTS - 1 - slot) for slot in range(slots))


@pytest.fixture
def steps(logic_program):
    """The state after each of the steps given, as LogicMcast prints it."""
    program = logic_program(PROGRAM)

    def run(*given: str) -> list[dict[str, str]]:
        found = program.run(*given).steps()
        assert len(found) == len(given)
        assert all(step["forbids"] == "0" for step in found), "Forbid/Permit left unbalanced"
        return found

    return run


def ranges(step: dict[str, str]) -> list[tuple[str, str, int]]:
    """The step's ranges as (lower, upper, uses)."""
    return [(r["lower"], r["upper"], int(r["uses"])) for r in grouped(step, "range")]


def outcome(step: dict[str, str]) -> tuple[str, str]:
    """The step's request as (io_Error, ios2_WireError)."""
    return step["error"], step["wire_error"]


def events(step: dict[str, str]) -> list[str]:
    """The events the step reported, one entry per sana2_report_events() call."""
    return [call["value"] for call in grouped(step, "call") if call["kind"] == "events"]


@pytest.mark.covers("S2_ADDMULTICASTADDRESS", "multicast filter")
def test_an_address_added_offline_is_kept_and_reaches_the_registers_only_online(steps):
    offline, online, second = steps(f"add={group(1)}", "online=on", f"add={group(2)}")
    assert outcome(offline) == ("S2ERR_NO_ERROR", "S2WERR_GENERIC_ERROR") and offline["processed"] == "yes"
    assert ranges(offline) == [(group(1), group(1), 1)] and listed(offline, "slot") == [group(1)]
    assert int(offline["mdf_ctrl"], 16) == 0 and listed(offline, "mdf") == []
    assert int(online["mdf_ctrl"], 16) == 0
    assert int(second["mdf_ctrl"], 16) == enabled(4)
    assert listed(second, "mdf") == [BROADCAST, STATION, group(1), group(2)]
    assert second["promiscuous"] == "no" and second["umac_promiscuous"] == "no"


@pytest.mark.covers("S2_ADDMULTICASTADDRESSES", "multicast filter")
def test_a_range_takes_a_slot_per_address_in_order(steps):
    (step,) = steps(f"add={group(2)}-{group(5)}")
    assert ranges(step) == [(group(2), group(5), 1)]
    assert listed(step, "slot") == [group(2), group(3), group(4), group(5)]


@pytest.mark.covers("S2_ADDMULTICASTADDRESS", "S2_DELMULTICASTADDRESS")
def test_each_add_counts_a_use_and_each_delete_takes_one_back(steps):
    first, again, one_back, last = steps(f"add={group(1)}", f"add={group(1)}", f"del={group(1)}",
                                         f"del={group(1)}")
    assert ranges(first) == [(group(1), group(1), 1)]
    assert ranges(again) == [(group(1), group(1), 2)] and listed(again, "slot") == [group(1)]
    assert ranges(one_back) == [(group(1), group(1), 1)] and listed(one_back, "slot") == [group(1)]
    assert ranges(last) == [] and listed(last, "slot") == []


@pytest.mark.covers("S2_DELMULTICASTADDRESSES")
def test_a_range_is_taken_back_by_its_own_bounds_only(steps):
    added, inner, whole = steps(f"add={group(1)}-{group(3)}", f"del={group(2)}", f"del={group(1)}-{group(3)}")
    assert ranges(inner) == ranges(added) and outcome(inner) == ("S2ERR_NO_ERROR", "S2WERR_GENERIC_ERROR")
    assert ranges(whole) == []


@pytest.mark.covers("S2_DELMULTICASTADDRESS")
def test_deleting_what_was_never_added_is_no_error(steps):
    (step,) = steps(f"del={group(9)}")
    assert outcome(step) == ("S2ERR_NO_ERROR", "S2WERR_GENERIC_ERROR") and ranges(step) == []


@pytest.mark.covers("multicast filter", "S2_ADDMULTICASTADDRESSES")
def test_fifteen_addresses_fit_and_a_sixteenth_makes_the_unit_promiscuous_until_it_goes(steps):
    exact, overflow, back = steps("online=on", f"add={group(1)}-{group(15)}", f"add={group(16)}",
                                  f"del={group(16)}")[1:]
    assert exact["promiscuous"] == "no" and len(listed(exact, "slot")) == 15
    assert int(exact["mdf_ctrl"], 16) == enabled(MDF_SLOTS)
    assert listed(exact, "mdf")[2:] == [group(n) for n in range(1, 16)]
    assert overflow["promiscuous"] == "yes" and overflow["umac_promiscuous"] == "yes"
    assert int(overflow["mdf_ctrl"], 16) == 0 and listed(overflow, "slot") == []
    assert back["promiscuous"] == "no" and back["umac_promiscuous"] == "no"
    assert int(back["mdf_ctrl"], 16) == enabled(MDF_SLOTS)


@pytest.mark.covers("multicast filter", "SANA2OPF_PROM")
def test_a_promiscuous_opener_makes_the_unit_promiscuous_until_it_goes(steps):
    added, promiscuous, gone = steps("online=on", f"add={group(1)}", "promiscuous=on", "promiscuous=off")[1:]
    assert promiscuous["promiscuous"] == "yes" and promiscuous["umac_promiscuous"] == "yes"
    assert ranges(promiscuous) == ranges(added)
    assert gone["promiscuous"] == "no" and gone["umac_promiscuous"] == "no"
    assert listed(gone, "mdf") == [BROADCAST, STATION, group(1)]


@pytest.mark.covers("S2_ADDMULTICASTADDRESS", "S2_DELMULTICASTADDRESS", "S2_ADDMULTICASTADDRESSES",
                    "S2_DELMULTICASTADDRESSES", "S2ERR_BAD_ADDRESS")
@pytest.mark.parametrize("command", ["add", "del"])
@pytest.mark.parametrize("operand", ["02:00:5e:00:00:01", f"{group(1)}-02:00:5e:00:00:09", f"{group(5)}-{group(4)}"],
                         ids=["unicast", "unicast upper bound", "upper below lower"])
def test_a_request_that_is_not_a_multicast_range_is_refused(steps, command, operand):
    (step,) = steps(f"{command}={operand}")
    assert outcome(step) == ("S2ERR_BAD_ADDRESS", "S2WERR_BAD_MULTICAST") and ranges(step) == []


@pytest.mark.covers("claim: netdev holds", "S2ERR_OUTOFSERVICE", "multicast filter")
def test_a_netdev_stack_holding_the_unit_refuses_multicast_and_keeps_its_filter(steps):
    held, added, deleted, promiscuous = steps("netdev=on", f"add={group(1)}", f"del={group(1)}", "promiscuous=on")
    assert listed(held, "slot") == [NETDEV_GROUP]
    for step in (added, deleted):
        assert outcome(step) == ("S2ERR_OUTOFSERVICE", "S2WERR_GENERIC_ERROR") and ranges(step) == []
    assert listed(promiscuous, "slot") == [NETDEV_GROUP] and promiscuous["promiscuous"] == "no"


@pytest.mark.covers("S2_ADDMULTICASTADDRESS", "S2ERR_NO_RESOURCES", "S2EVENT_SOFTWARE", "S2EVENT_ERROR")
def test_no_memory_for_a_range_fails_the_add_and_reports_a_software_error(steps):
    failed, retried = steps("nomem", f"add={group(1)}", f"add={group(1)}")[1:]
    assert outcome(failed) == ("S2ERR_NO_RESOURCES", "S2WERR_GENERIC_ERROR") and ranges(failed) == []
    assert events(failed) == ["S2EVENT_ERROR|S2EVENT_SOFTWARE"]
    assert ranges(retried) == [(group(1), group(1), 1)] and events(retried) == []


@pytest.mark.covers("multicast filter")
def test_a_frame_is_wanted_inside_a_range_bounds_included(steps):
    found = steps(f"add={group(2)}-{group(4)}", *(f"wanted={group(n)}" for n in range(1, 6)))[1:]
    assert [step["wanted"] for step in found] == ["no", "yes", "yes", "yes", "no"]


@pytest.mark.covers("the last close", "multicast filter")
def test_the_last_close_frees_every_range_and_leaves_the_filter(steps):
    added, cleared = steps("online=on", f"add={group(1)}", f"add={group(2)}", "clear")[2:]
    assert ranges(cleared) == []
    assert listed(cleared, "slot") == listed(added, "slot") and listed(cleared, "mdf") == listed(added, "mdf")
