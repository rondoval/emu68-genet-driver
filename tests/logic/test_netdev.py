# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""netdev's reception filter and station address as the driver keeps them (netdev_api.c under vamos):
exact multicast up to 15 groups, promiscuous past them or on the stack's flags, stored while stopped;
the address set before START, by the opener that attached, never while SANA-II holds the unit."""
from __future__ import annotations

import pytest

from harness.vamos import grouped, listed

pytestmark = pytest.mark.logic

PROGRAM = "LogicNetdev"
STATION = "02:00:00:00:00:01"           # the unit's address in the logic programs (unit.c)
NEW_ADDRESS = "02:00:00:00:00:42"
NDFF_PROMISC, NDFF_ALLMULTI = 1, 2
EXACT_SLOTS = 15                         # GENET_MDF_MCAST_MAX: 17 MDF slots less broadcast and the station


def group(last: int) -> str:
    """The groups the filter steps name: 01:00:5e:00:00:<last>."""
    return f"01:00:5e:00:00:{last:02x}"


@pytest.fixture
def steps(logic_program):
    """The unit after each of the steps given, as LogicNetdev prints it."""
    program = logic_program(PROGRAM)

    def run(*given: str) -> list[dict[str, str]]:
        found = program.run(*given).steps()
        assert len(found) == len(given)
        return found

    return run


def programmed(step: dict[str, str]) -> int:
    """How often the step programmed the reception filter into the hardware."""
    return sum(call["kind"] == "set_rx_mode" for call in grouped(step, "call"))


@pytest.mark.covers("NETDEV_CMD_SET_RXFILTER", "NDERR_NOTATTACHED", "NDERR_BADPARAMS")
def test_a_filter_needs_an_attached_stack_and_the_list_it_counts(steps):
    found = steps("filter=0,1", "holder=netdev", "filter=0,2,null", "filter=0,0,null")
    nobody, missing, empty = found[0], found[2], found[3]
    assert nobody["error"] == "NDERR_NOTATTACHED"
    assert missing["error"] == "NDERR_BADPARAMS"
    assert empty["error"] == "0" and empty["slots"] == "0" and empty["promiscuous"] == "no"


@pytest.mark.covers("NETDEV_CMD_SET_RXFILTER", "multicast filter")
def test_fifteen_groups_are_kept_exact_and_a_sixteenth_falls_back_to_promiscuous(steps):
    exact, overflow = steps("holder=netdev", f"filter=0,{EXACT_SLOTS}", f"filter=0,{EXACT_SLOTS + 1}")[1:]
    assert exact["promiscuous"] == "no" and listed(exact, "slot") == [group(n) for n in range(1, EXACT_SLOTS + 1)]
    assert overflow["error"] == "0" and overflow["promiscuous"] == "yes" and overflow["slots"] == "0"


@pytest.mark.covers("NETDEV_CMD_SET_RXFILTER")
@pytest.mark.parametrize("flags", [NDFF_PROMISC, NDFF_ALLMULTI], ids=["promisc", "allmulti"])
def test_the_stack_s_flags_make_the_unit_promiscuous(steps, flags):
    step = steps("holder=netdev", f"filter={flags},2")[-1]
    assert (step["error"], step["filter_flags"], step["promiscuous"], step["slots"]) == ("0", str(flags), "yes", "0")


@pytest.mark.covers("NETDEV_CMD_SET_RXFILTER", "NETDEV_CMD_START")
def test_a_stopped_unit_stores_the_filter_and_a_running_one_programs_it(steps):
    stored, running = steps("holder=netdev", "filter=0,2", "start", "filter=0,3")[1::2]
    assert programmed(stored) == 0 and stored["slots"] == "2"
    assert programmed(running) == 1 and running["slots"] == "3"


@pytest.mark.covers("NETDEV_CMD_SET_MAC")
def test_the_owner_sets_the_station_address_before_start(steps):
    set_, started, refused = steps("holder=netdev", f"mac={NEW_ADDRESS}", "start", "mac=02:00:00:00:00:43")[1:]
    assert set_["error"] == "0" and set_["mac"] == NEW_ADDRESS
    assert refused["error"] == "NDERR_BADPARAMS" and refused["mac"] == NEW_ADDRESS, "set before START only"


@pytest.mark.covers("NETDEV_CMD_SET_MAC", "NDERR_BUSY", "NDERR_BADPARAMS", "claim: SANA-II holds")
@pytest.mark.parametrize("setup, error", [(["holder=netdev", "owner=other"], "NDERR_BUSY"),
                                          (["holder=sana2"], "NDERR_BUSY"),
                                          (["holder=netdev"], "NDERR_BADPARAMS")],
                         ids=["another opener", "SANA-II holds", "short"])
def test_the_station_address_is_refused_to_others_and_short_requests(steps, setup, error):
    operand = "short" if error == "NDERR_BADPARAMS" else NEW_ADDRESS
    step = steps(*setup, f"mac={operand}")[-1]
    assert step["error"] == error and step["mac"] == STATION
