# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""The SANA-II personality as the driver runs it (sana2_io.c, sana2_api.c and sana2_mcast.c under vamos):
which read a frame reaches, what it holds, orphans and the counters, the packet filter and the copy
functions, promiscuous delivery; the openers' lifecycle and foreign closes; aborts, S2_OFFLINE and
CMD_FLUSH; S2_DEVICEQUERY's sizes; S2_ONEVENT; the claim's refusals."""
from __future__ import annotations

import pytest

from harness.vamos import grouped

pytestmark = pytest.mark.logic

PROGRAM = "LogicSana2"
STATION = "02:00:00:00:00:01"           # the unit's address in the logic programs (unit.c)
OTHER = "02:00:00:00:00:77"             # a unicast address that is not the station's
GROUP = "01:00:5e:00:00:fb"
IPV4, ARP, IPV6, LENGTH_8023 = 0x0800, 0x0806, 0x86DD, 46
PAYLOAD_FIRST = 0x10111213              # the first payload bytes of every frame (sana2.c)
HEADER = 14
ABORTED = ("IOERR_ABORTED", "S2WERR_GENERIC_ERROR")
EVENT = {"ERROR": 0x1, "RX": 0x4, "ONLINE": 0x8, "OFFLINE": 0x10, "BUFF": 0x20, "SOFTWARE": 0x80, "CONFIGCHANGED": 0x100,
         "CONNECT": 0x200, "DISCONNECT": 0x400}


def online(*options: str) -> list[str]:
    """Steps that open an opener per entry of @options (comma-separated open options, "" for none) and
    bring the unit online through the first: link up, S2_CONFIGINTERFACE."""
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


def replied(step: dict[str, str]) -> list[int]:
    """The numbers of the requests replied during the step, in order."""
    return [int(reply["request"]) for reply in replies(step)]


def outcome(entry: dict[str, str]) -> tuple[str, str]:
    """A request's (io_Error, ios2_WireError)."""
    return entry["error"], entry["wire_error"]


@pytest.mark.covers("CMD_READ", "SANA-II RX routing")
def test_each_type_reaches_the_read_waiting_for_it(steps):
    found = steps(*online(""), f"read=a,{ARP}", f"read=a,{IPV6}", f"read=a,{IPV4}",
                  f"frame=station,{IPV6}", f"frame=station,{IPV4}", f"frame=station,{ARP}")[-3:]
    assert [replied(step) for step in found] == [[2], [3], [1]]
    assert [replies(step)[0]["type"] for step in found] == [hex(IPV6), hex(IPV4), hex(ARP)]


@pytest.mark.covers("CMD_READ", "SANA-II RX routing")
def test_the_oldest_read_of_the_type_takes_the_frame(steps):
    first, second = steps(*online(""), f"read=a,{IPV4}", f"read=a,{IPV4}", f"frame=station,{IPV4}",
                          f"frame=station,{IPV4}")[-2:]
    assert replied(first) == [1] and replied(second) == [2]


@pytest.mark.covers("CMD_READ", "SANA-II RX routing")
def test_every_opener_waiting_for_the_type_gets_a_copy(steps):
    step = steps(*online("", ""), f"read=a,{IPV4}", f"read=b,{IPV4}", f"frame=station,{IPV4}")[-1]
    assert replied(step) == [1, 2] and step["rx_packets"] == "1"
    assert step["rx_bytes"] == str(HEADER + 46)


@pytest.mark.covers("CMD_READ", "SANA-II RX routing")
def test_any_8023_read_takes_any_8023_frame_and_no_ethernet_ii_one(steps):
    taken, untouched = steps(*online(""), "read=a,100", f"frame=station,{LENGTH_8023}", "read=a,0x88b5",
                             f"frame=station,{LENGTH_8023}")[-3::2]
    assert replied(taken) == [1] and replies(taken)[0]["type"] == hex(LENGTH_8023)
    assert replied(untouched) == [] and untouched["rx_orphan"] == "1"


@pytest.mark.covers("SANA2IOF_RAW", "S2_CopyToBuff")
def test_a_raw_read_gets_the_whole_frame_and_any_other_the_payload(steps):
    step = steps(*online(""), f"read=a,{IPV4},raw", f"read=a,{IPV4}", f"read=a,{IPV4}",
                 f"frame=station,{IPV4},50", f"frame=station,{IPV4},50")
    raw, payload = replies(step[-2])[0], replies(step[-1])[0]
    assert (raw["length"], raw["copied"], int(raw["first"], 16)) == (str(HEADER + 50), str(HEADER + 50), 0x02000000)
    assert (payload["length"], payload["copied"], int(payload["first"], 16)) == ("50", "50", PAYLOAD_FIRST)
    assert raw["destination"] == payload["destination"] == STATION


@pytest.mark.covers("SANA2IOF_BCAST", "SANA2IOF_MCAST", "SANA-II RX routing")
def test_a_read_is_told_whether_the_frame_was_broadcast_or_multicast(steps):
    found = steps(*online(""), f"cmd=a,S2_ADDMULTICASTADDRESS,{GROUP}", *[f"read=a,{IPV4}"] * 3,
                  f"frame=broadcast,{IPV4}", f"frame={GROUP},{IPV4}", f"frame=station,{IPV4}")[-3:]
    assert [replies(step)[0]["cast"] for step in found] == ["broadcast", "multicast", "unicast"]


@pytest.mark.covers("S2_READORPHAN", "SANA-II RX routing")
def test_a_frame_no_read_waits_for_goes_to_the_orphan_reads_and_is_counted(steps):
    other, ip = steps(*online("", ""), "orphan=a", "orphan=b", f"frame=station,{IPV6}", f"frame=station,{IPV4}")[-2:]
    assert replied(other) == [1, 2] and replies(other)[0]["type"] == hex(IPV6)
    assert (other["rx_orphan"], other["rx_ip_arp_dropped"]) == ("1", "0")
    assert replied(ip) == [] and (ip["rx_orphan"], ip["rx_ip_arp_dropped"]) == ("2", "1")


@pytest.mark.covers("S2_PacketFilter", "CMD_READ", "S2_READORPHAN", "IOERR_ABORTED")
def test_a_declining_packet_filter_leaves_the_read_pending_and_the_orphan_reads_unfiltered(steps):
    declined, aborted = steps(*online("decline"), f"read=a,{IPV4}", "orphan=a", f"frame=station,{IPV4}",
                              "abort=1")[-2:]
    assert replied(declined) == [2], "the frame went to the orphan read, unfiltered"
    assert declined["rx_orphan"] == "1"
    assert replied(aborted) == [1] and outcome(replies(aborted)[0]) == ABORTED, "the read was still pending"


@pytest.mark.covers("prefs USE_MIAMI_WORKAROUND", "S2_CopyToBuff")
@pytest.mark.parametrize("payload, copied", [(46, 48), (48, 48), (49, 52)])
def test_the_miami_workaround_rounds_the_copy_up_to_a_longword(steps, payload, copied):
    step = steps(*online(""), "miami=on", f"read=a,{IPV4}", f"frame=station,{IPV4},{payload}")[-1]
    reply = replies(step)[0]
    assert (reply["length"], reply["copied"]) == (str(payload), str(copied))


@pytest.mark.covers("S2_CopyToBuff", "S2ERR_NO_RESOURCES", "S2EVENT_BUFF", "S2EVENT_RX", "S2EVENT_SOFTWARE",
                    "S2EVENT_ERROR")
@pytest.mark.parametrize("option", ["copyfail", "nocopy"])
def test_a_copy_that_fails_or_is_missing_answers_the_read_with_a_buffer_error(steps, option):
    events = EVENT["BUFF"] | EVENT["RX"] | EVENT["SOFTWARE"] | EVENT["ERROR"]
    step = steps(*online(option), f"cmd=a,S2_ONEVENT,{events:#x}", f"read=a,{IPV4}", f"frame=station,{IPV4}")[-1]
    read, event = sorted(replies(step), key=lambda r: int(r["request"]), reverse=True)
    assert outcome(read) == ("S2ERR_NO_RESOURCES", "S2WERR_BUFF_ERROR") and step["rx_buffer_errors"] == "1"
    assert event["events"] == "S2EVENT_ERROR|S2EVENT_RX|S2EVENT_BUFF|S2EVENT_SOFTWARE"


@pytest.mark.covers("SANA2OPF_PROM", "SANA-II RX routing", "multicast filter")
def test_a_promiscuous_opener_alone_gets_what_is_for_nobody(steps):
    # requests: 0 S2_CONFIGINTERFACE, 1 the join of a's; then the reads, numbered as made
    found = steps(*online("", "prom"), f"cmd=a,S2_ADDMULTICASTADDRESS,{GROUP}",
                  f"read=a,{IPV4}", f"read=b,{IPV4}", f"frame={OTHER},{IPV4}",             # reads 2, 3
                  f"read=b,{IPV4}", f"frame=01:00:5e:00:00:01,{IPV4}",                    # read 4
                  f"frame=station,{IPV4}",
                  f"read=a,{IPV4}", f"read=b,{IPV4}", f"frame=broadcast,{IPV4}",          # reads 5, 6
                  f"read=a,{IPV4}", f"read=b,{IPV4}", f"frame={GROUP},{IPV4}")            # reads 7, 8
    other, stray, station, broadcast, joined = (found[i] for i in (8, 10, 11, 14, 17))
    assert found[5]["promiscuous"] == "yes"
    assert replied(other) == [3], "another station's frame: the promiscuous opener only"
    assert replied(stray) == [4], "a group nobody joined: the promiscuous opener only"
    assert replied(station) == [2], "a's first read was still waiting"
    assert replied(broadcast) == [5, 6] and replied(joined) == [7, 8]


@pytest.mark.covers("OpenDevice", "issued-opener list")
def test_an_open_is_handed_out_at_once_and_linked_by_the_unit_task(steps):
    opened, linked = steps("open", "task")
    assert (opened["issued"], opened["openers"], opened["posted"]) == ("1", "0", "1")
    assert (linked["issued"], linked["openers"]) == ("1", "1")


@pytest.mark.covers("CloseDevice", "the last close", "issued-opener list", "IOERR_ABORTED")
def test_a_close_answers_the_openers_requests_and_the_last_claimer_releases_the_unit(steps):
    closed, done = steps(*online(""), f"read=a,{IPV4}", "orphan=a", "close=a", "task")[-2:]
    assert closed["posted"] == "1" and closed["issued"] == "0"
    assert replied(done) == [1, 2] and all(outcome(r) == ABORTED for r in replies(done))
    assert (done["openers"], done["claimers"], done["online"], done["configured"]) == ("0", "0", "no", "no")


@pytest.mark.covers("CloseDevice", "issued-opener list")
@pytest.mark.parametrize("cookie", ["foreign", "null"])
def test_a_close_with_a_foreign_or_no_cookie_is_left_alone(steps, cookie):
    step = steps(*online(""), f"close={cookie}", "task")[-2]
    assert (step["posted"], step["issued"], step["openers"]) == ("0", "1", "1")


@pytest.mark.covers("CloseDevice", "issued-opener list")
def test_a_second_close_of_the_same_opener_is_left_alone(steps):
    again = steps("open", "open", "task", "close=a", "task", "close=a")[-1]
    assert (again["posted"], again["issued"], again["openers"]) == ("0", "1", "1")


@pytest.mark.covers("CMD_READ", "S2ERR_BAD_ARGUMENT", "S2ERR_OUTOFSERVICE")
def test_a_read_needs_an_opener_and_an_online_unit(steps):
    nobody, offline = steps("open", "task", f"read=null,{IPV4}", f"read=a,{IPV4}")[-2:]
    assert outcome(nobody) == ("S2ERR_BAD_ARGUMENT", "S2WERR_NULL_POINTER") and nobody["kept"] == "no"
    assert outcome(offline) == ("S2ERR_OUTOFSERVICE", "S2WERR_UNIT_OFFLINE") and offline["kept"] == "no"


@pytest.mark.covers("CMD_READ", "SANA-II read ring")
def test_a_read_past_a_full_ring_goes_through_the_unit_port_behind_the_others(steps):
    full, overflow, _, frame = steps(*online(""), f"reads=a,{IPV4},512", f"read=a,{IPV4}", "task",
                                     f"frame=station,{IPV4}")[-4:]
    assert full["posted"] == "0"
    assert (overflow["request"], overflow["kept"], overflow["posted"]) == ("513", "yes", "1")
    assert replied(frame) == [1], "the oldest read in the ring, not the one that went round"


@pytest.mark.covers("AbortIO", "CMD_READ", "IOERR_ABORTED")
def test_an_aborted_read_is_answered_once(steps):
    aborted, again = steps(*online(""), f"read=a,{IPV4}", "abort=1", "abort=1")[-2:]
    assert replied(aborted) == [1] and outcome(replies(aborted)[0]) == ABORTED
    assert replied(again) == []


@pytest.mark.covers("S2_OFFLINE", "S2EVENT_OFFLINE", "CMD_READ", "IOERR_ABORTED")
def test_going_offline_answers_the_reads_and_reports_offline_to_the_waiters(steps):
    step = steps(*online(""), f"read=a,{IPV4}", f"cmd=a,S2_ONEVENT,{EVENT['OFFLINE']:#x}", "cmd=a,S2_OFFLINE")[-1]
    found = {int(r["request"]): r for r in replies(step)}
    assert found[2].get("events") == "S2EVENT_OFFLINE"
    assert outcome(found[1]) == ABORTED and step["online"] == "no" and step["claimers"] == "1"


@pytest.mark.covers("CMD_FLUSH", "AbortIO", "IOERR_ABORTED")
def test_flush_answers_every_kept_request_with_aborted(steps):
    step = steps(*online(""), f"read=a,{IPV4}", "orphan=a", f"cmd=a,S2_ONEVENT,{EVENT['ERROR']:#x}",
                 "cmd=a,CMD_FLUSH")[-1]
    assert sorted(replied(step)) == [1, 2, 3, 4]
    assert all(outcome(r) == ABORTED for r in replies(step) if r["request"] != "4")


@pytest.mark.covers("S2_DEVICEQUERY")
def test_device_query_fills_the_whole_structure_when_there_is_room(steps):
    (step,) = steps("cmd=null,S2_DEVICEQUERY")
    assert outcome(step) == ("S2ERR_NO_ERROR", "S2WERR_GENERIC_ERROR")
    assert {k: step[f"query.{k}"] for k in ("size_supplied", "address_bits", "mtu", "bps", "hardware_type",
                                            "raw_mtu")} == {"size_supplied": "34", "address_bits": "48",
                                                            "mtu": "1500", "bps": "1000000000",
                                                            "hardware_type": "1", "raw_mtu": "1518"}


@pytest.mark.covers("S2_DEVICEQUERY")
def test_device_query_leaves_raw_mtu_out_of_a_30_byte_structure(steps):
    (step,) = steps("cmd=null,S2_DEVICEQUERY,30")
    assert (step["query.size_supplied"], step["query.raw_mtu"], step["query.mtu"]) == ("30", "0", "1500")


@pytest.mark.covers("S2_DEVICEQUERY", "S2ERR_BAD_ARGUMENT")
@pytest.mark.parametrize("argument, wire", [("29", "S2WERR_BAD_STATDATA"), ("null", "S2WERR_NULL_POINTER")])
def test_device_query_refuses_a_short_or_missing_structure(steps, argument, wire):
    (step,) = steps(f"cmd=null,S2_DEVICEQUERY,{argument}")
    assert outcome(step) == ("S2ERR_BAD_ARGUMENT", wire) and step["query.size_supplied"] == "0"


@pytest.mark.covers("S2_ONEVENT", "S2EVENT_CONNECT", "S2EVENT_DISCONNECT")
def test_an_event_wait_ends_at_once_on_a_state_that_holds_and_later_on_a_change(steps):
    unknown, offline = steps("open", "task", "cmd=a,S2_ONEVENT,0x100000",
                             f"cmd=a,S2_ONEVENT,{EVENT['OFFLINE']:#x}")[-2:]
    assert outcome(unknown) == ("S2ERR_NOT_SUPPORTED", "S2WERR_BAD_EVENT")
    assert offline["kept"] == "no" and replies(offline)[0]["events"] == "S2EVENT_OFFLINE"
    connected, waiting, lost = steps(*online(""), f"cmd=a,S2_ONEVENT,{EVENT['CONNECT']:#x}",
                                     f"cmd=a,S2_ONEVENT,{EVENT['DISCONNECT']:#x}", "link=down")[-3:]
    assert connected["kept"] == "no" and replies(connected)[0]["events"] == "S2EVENT_CONNECT"
    assert waiting["kept"] == "yes" and replies(lost)[0]["events"] == "S2EVENT_DISCONNECT"


@pytest.mark.covers("S2_CONFIGINTERFACE", "S2_ONLINE")
def test_configuring_without_a_link_parks_until_the_link_comes_up(steps):
    parked, up = steps("open", "task", "cmd=a,S2_CONFIGINTERFACE", "link=up")[-2:]
    assert parked["kept"] == "yes" and parked["online"] == "yes" and parked["configured"] == "yes"
    assert replied(up) == [0] and outcome(replies(up)[0]) == ("S2ERR_NO_ERROR", "S2WERR_GENERIC_ERROR")


@pytest.mark.covers("S2_CONFIGINTERFACE", "S2EVENT_CONFIGCHANGED", "S2EVENT_ONLINE", "S2EVENT_CONNECT")
def test_configuring_reports_the_change_then_online_then_the_link(steps):
    waits = [f"cmd=b,S2_ONEVENT,{EVENT[name]:#x}" for name in ("CONFIGCHANGED", "ONLINE", "CONNECT")]
    found = steps("open", "open", "task", *waits, "link=up", "cmd=a,S2_CONFIGINTERFACE")
    assert all(step["kept"] == "yes" for step in found[3:6]), "none of the three holds on an offline unit"
    assert [(r["request"], r.get("events")) for r in replies(found[-1])] == [
        ("0", "S2EVENT_CONFIGCHANGED"), ("1", "S2EVENT_ONLINE"), ("2", "S2EVENT_CONNECT"), ("3", None)]


@pytest.mark.covers("S2_ONLINE", "S2_CONFIGINTERFACE", "S2ERR_BAD_STATE")
def test_online_needs_a_configured_unit_and_configuring_happens_once(steps):
    early = steps("open", "task", "cmd=a,S2_ONLINE")[-1]
    assert outcome(early) == ("S2ERR_BAD_STATE", "S2WERR_NOT_CONFIGURED")
    again = steps(*online(""), "cmd=a,S2_CONFIGINTERFACE")[-1]
    assert outcome(again) == ("S2ERR_BAD_STATE", "S2WERR_IS_CONFIGURED")


@pytest.mark.covers("claim: netdev holds", "S2ERR_OUTOFSERVICE", "S2_CONFIGINTERFACE", "S2_ONLINE")
@pytest.mark.parametrize("command", ["S2_CONFIGINTERFACE", "S2_ONLINE"])
def test_a_netdev_stack_holding_the_unit_refuses_the_claim(steps, command):
    step = steps("open", "task", "netdev=on", f"cmd=a,{command}")[-1]
    assert outcome(step) == ("S2ERR_OUTOFSERVICE", "S2WERR_GENERIC_ERROR") and step["claimers"] == "0"


@pytest.mark.covers("S2_CONFIGINTERFACE", "S2_ONLINE", "S2_OFFLINE", "Reconfigurations")
def test_the_claim_lasts_from_configuration_to_the_last_close(steps):
    configured, offline, again = steps(*online(""), "cmd=a,S2_OFFLINE", "cmd=a,S2_ONLINE")[-3:]
    assert (configured["claimers"], configured["online"]) == ("1", "yes")
    assert (offline["claimers"], offline["online"]) == ("1", "no")
    assert (again["claimers"], again["online"]) == ("1", "yes")
