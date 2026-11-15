# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""Interrupt moderation as the driver programs it (bcmgenet-dma.c and netdev_api.c under vamos): the
limits of NETDEV_CMD_SET_COALESCE, what it stores and when it reaches the registers, the RX profiles,
the latency profile's burst ladder, the bulk level for a consumer that is behind, and the operator's
pin."""
from __future__ import annotations

import math

import pytest

pytestmark = pytest.mark.logic

PROGRAM = "LogicNetdev"
TX_FRAMES, RX_FRAMES, RX_USECS, BULK_USECS = 32, 64, 500, 1000   # device.h's GENET_COAL_*
BATCH = 32                                                       # the stack's poll batch (netdev.c)
LADDER = [(1, 8), (64, 100), (64, 200), (64, 400), (64, RX_USECS), (64, BULK_USECS)]
TOP, BULK = 4, 5
UNIT_TASK_RX_SIGNAL = 1 << 8                                     # support.h's LOGIC_RX_SIGNAL


def ticks(usecs: int) -> int:
    """The RDMA timeout for @usecs: the 125 MHz reference / 1024, 8.192 us a tick, rounded up."""
    return math.ceil(usecs * 1000 / 8192)


def rx_registers(step: dict[str, str]) -> tuple[int, int]:
    """What the step left in the RX frame threshold and timeout registers."""
    return int(step["reg_rx_frames"]), int(step["reg_rx_ticks"])


def programmed(frames: int, usecs: int) -> tuple[int, int]:
    """The RX registers for @frames and @usecs."""
    return frames, ticks(usecs)


def signals(step: dict[str, str]) -> list[int]:
    """The signals the step sent the unit task."""
    return [int(value, 16) for key, value in step.items() if key.endswith(".value")
            and step[key.replace(".value", ".kind")] == "signal"]


@pytest.fixture
def steps(logic_program):
    """The moderation after each of the steps given, as LogicNetdev prints it."""
    program = logic_program(PROGRAM)

    def run(*given: str) -> list[dict[str, str]]:
        found = program.run(*given).steps()
        assert len(found) == len(given)
        assert all(step["forbids"] == "0" for step in found), "Forbid/Permit left unbalanced"
        return found

    return run


@pytest.mark.covers("NETDEV_CMD_SET_COALESCE")
def test_the_limits_are_those_of_the_hardware(steps):
    cases = {"1,64,500": "yes", "0,64,500": "no", "511,511,500": "yes", "512,64,500": "no", "32,512,500": "no",
             "32,0,0": "no", "32,0,1": "yes", "32,1,0": "yes", "32,64,524281": "yes", "32,64,524282": "no"}
    found = steps(*(f"valid={case}" for case in cases))
    assert {case: step["valid"] for case, step in zip(cases, found)} == cases


@pytest.mark.covers("NETDEV_CMD_SET_COALESCE", "NDERR_NOTATTACHED")
def test_setting_coalescing_needs_a_holder_of_either_personality(steps):
    nobody, sana2 = steps("coalesce=16,8,250", "holder=sana2", "coalesce=16,8,250")[0::2]
    assert nobody["error"] == "NDERR_NOTATTACHED"
    assert (nobody["tx_frames"], nobody["rx_frames"], nobody["rx_usecs"]) == ("32", "64", "500")
    assert sana2["error"] == "0" and (sana2["tx_frames"], sana2["rx_frames"], sana2["rx_usecs"]) == ("16", "8", "250")


@pytest.mark.covers("NETDEV_CMD_SET_COALESCE", "NDERR_BADPARAMS")
@pytest.mark.parametrize("values", ["600,0,0", "32,600,0", "32,0,600000"], ids=["tx frames", "rx frames", "rx usecs"])
def test_values_past_the_limits_are_refused_and_change_nothing(steps, values):
    step = steps("holder=netdev", "start", f"coalesce={values}")[-1]
    assert step["error"] == "NDERR_BADPARAMS"
    assert (step["tx_frames"], step["rx_frames"], step["rx_usecs"], step["pinned"]) == ("32", "64", "500", "no")
    assert rx_registers(step) == programmed(RX_FRAMES, RX_USECS)


@pytest.mark.covers("NETDEV_CMD_SET_COALESCE")
def test_zero_keeps_the_default_and_only_explicit_rx_values_pin(steps):
    defaults, tx_only, rx_frames, rx_usecs = steps("holder=netdev", "coalesce=0,0,0", "coalesce=16,0,0",
                                                   "coalesce=0,8,0", "coalesce=0,0,250")[1:]
    assert (defaults["tx_frames"], defaults["rx_frames"], defaults["rx_usecs"], defaults["pinned"]) == \
        ("32", "64", "500", "no")
    assert tx_only["tx_frames"] == "16" and tx_only["pinned"] == "no"
    assert rx_frames["rx_frames"] == "8" and rx_frames["pinned"] == "yes"
    assert rx_usecs["rx_usecs"] == "250" and rx_usecs["rx_frames"] == "64" and rx_usecs["pinned"] == "yes"


@pytest.mark.covers("NETDEV_CMD_SET_COALESCE", "NETDEV_CMD_START")
def test_a_stopped_unit_stores_the_values_and_start_programs_them(steps):
    stored, started = steps("holder=netdev", "coalesce=16,8,250", "start")[1:]
    assert (stored["reg_tx_frames"], stored["reg_rx_frames"], stored["reg_rx_ticks"]) == ("0", "0", "0")
    assert int(started["reg_tx_frames"]) == 16 and rx_registers(started) == programmed(8, 250)


@pytest.mark.covers("NETDEV_CMD_SET_COALESCE")
def test_a_running_unit_takes_the_values_at_once(steps):
    step = steps("holder=netdev", "start", "coalesce=24,12,300")[-1]
    assert int(step["reg_tx_frames"]) == 24 and rx_registers(step) == programmed(12, 300)


@pytest.mark.covers("NETDEV_CMD_SET_RX_PROFILE", "NDERR_NOTATTACHED", "NDERR_BADPARAMS")
def test_a_profile_needs_a_netdev_stack_and_a_known_profile(steps):
    nobody, sana2, unknown = steps("profile=1", "holder=sana2", "profile=1", "holder=netdev", "profile=3")[0::2]
    assert nobody["error"] == sana2["error"] == "NDERR_NOTATTACHED"
    assert unknown["error"] == "NDERR_BADPARAMS" and unknown["profile"] == "0"


@pytest.mark.covers("NETDEV_CMD_SET_RX_PROFILE")
def test_each_profile_programs_its_moderation(steps):
    unstated, throughput, latency, back = steps("holder=netdev", "start", "profile=1", "profile=2", "profile=0")[1:]
    assert rx_registers(unstated) == programmed(RX_FRAMES, RX_USECS) and unstated["ladder"] == "no"
    assert rx_registers(throughput) == programmed(RX_FRAMES, BULK_USECS) and throughput["ladder"] == "no"
    assert rx_registers(latency) == programmed(*LADDER[0]) and latency["ladder"] == "yes"
    assert rx_registers(back) == programmed(RX_FRAMES, RX_USECS) and back["ladder"] == "no"


@pytest.mark.covers("NETDEV_CMD_SET_RX_PROFILE", "NETDEV_CMD_START")
def test_a_profile_set_while_stopped_is_programmed_at_start(steps):
    stored, started = steps("holder=netdev", "profile=1", "start")[1:]
    assert stored["reg_rx_frames"] == "0" and rx_registers(started) == programmed(RX_FRAMES, BULK_USECS)


@pytest.mark.covers("RX burst ladder")
def test_a_burst_climbs_a_level_per_busy_rearm_and_stops_at_the_top(steps):
    found = steps("holder=netdev", "start", "profile=2", "frames=3", *["moderate"] * 6)[4:]
    assert [int(step["level"]) for step in found] == [1, 2, 3, 4, 4, 4]
    assert [rx_registers(step) for step in found[:4]] == [programmed(*LADDER[level]) for level in (1, 2, 3, 4)]
    assert found[-1]["level_changes"] == "4"


@pytest.mark.covers("RX burst ladder")
def test_a_quiet_ring_drops_to_one_frame_at_once(steps):
    climbed, quiet = steps("holder=netdev", "start", "profile=2", "frames=3", "moderate", "moderate", "frames=0",
                           "moderate")[5::2]
    assert climbed["level"] == "2"
    assert quiet["level"] == "0" and rx_registers(quiet) == programmed(*LADDER[0])
    assert signals(quiet) == []


@pytest.mark.covers("RX burst ladder")
def test_the_registers_are_written_only_when_the_level_changes(steps):
    first, again = steps("holder=netdev", "start", "profile=2", "moderate", "moderate")[3:]
    assert first["level"] == again["level"] == "0"
    assert first["level_changes"] == again["level_changes"] == "0"


@pytest.mark.covers("RX burst ladder")
def test_a_consumer_behind_by_two_batches_gets_the_bulk_level_until_it_catches_up(steps):
    found = steps("holder=netdev", "start", "profile=2", f"held={2 * BATCH}", "moderate", "frames=3", "moderate",
                  "frames=0", f"held={2 * BATCH - 1}", "moderate")
    behind, burst, caught_up = found[4], found[6], found[9]
    assert behind["level"] == str(BULK) and rx_registers(behind) == programmed(*LADDER[BULK])
    assert behind["bulk_arms"] == "1"
    assert burst["level"] == str(BULK), "a burst that began batched stays batched"
    assert caught_up["level"] == "0" and caught_up["bulk_arms"] == "1"


@pytest.mark.covers("NETDEV_CMD_SET_COALESCE", "NETDEV_CMD_SET_RX_PROFILE", "RX burst ladder")
def test_explicit_rx_values_outrank_the_profile_until_an_all_default_setting(steps):
    pinned, latency, released = steps("holder=netdev", "start", "coalesce=32,16,250", "profile=2",
                                      "coalesce=32,0,0")[2:]
    assert pinned["pinned"] == "yes" and rx_registers(pinned) == programmed(16, 250)
    assert latency["ladder"] == "no" and rx_registers(latency) == programmed(16, 250)
    assert released["pinned"] == "no" and released["ladder"] == "yes"
    assert rx_registers(released) == programmed(*LADDER[0])


@pytest.mark.covers("RX burst ladder", "NETDEV_CMD_SET_RX_PROFILE")
def test_switching_to_the_ladder_with_frames_waiting_wakes_the_unit_task(steps):
    waking = steps("holder=netdev", "start", "frames=2", "profile=2")[-1]
    assert signals(waking) == [UNIT_TASK_RX_SIGNAL]
    without = steps("holder=netdev", "start", "frames=2", "notask", "profile=2")[-1]
    assert signals(without) == []
