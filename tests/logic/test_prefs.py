# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""ENV:genet.prefs as the driver reads it (runtime_config.c under vamos): every key, the grammar,
the clamps, the 3.x keys and the LINK_MODE/AUTONEG rules (README.md, Runtime configuration)."""
from __future__ import annotations

import pytest

pytestmark = pytest.mark.logic

PROGRAM = "LogicPrefs"
DEFAULTS = {"unit_task_priority": "15", "rx_pool_bufs": "0", "link_speed": "0", "link_duplex": "full",
            "link_autoneg": "yes", "flow_control": "yes", "use_miami_workaround": "no"}
OLD_KEYS = ("BUDGET", "PERIODIC_TASK_MS", "RX_COALESCE_USECS", "RX_COALESCE_FRAMES", "TX_COALESCE_FRAMES",
            "UNIT_STACK_SIZE", "USE_DMA")


@pytest.fixture
def load(tmp_path, logic_program):
    """The settings the driver resolves from a genet.prefs holding the text given (None: no file)."""
    program = logic_program(PROGRAM)

    def settings(text: str | None) -> dict[str, str]:
        env = tmp_path / "env"
        env.mkdir(exist_ok=True)
        prefs = env / "genet.prefs"
        prefs.unlink(missing_ok=True)
        if text is not None:
            prefs.write_bytes(text.encode("latin-1"))
        run = program.run(volumes={"genetenv": env}, assigns={"ENV": "genetenv:"})
        return {key.removeprefix("prefs."): value for key, value in run.result.kv.items()
                if key.startswith("prefs.")}

    return settings


def changed(**settings: str) -> dict[str, str]:
    """The defaults with @settings in place of theirs."""
    return DEFAULTS | settings


@pytest.mark.covers("prefs defaults")
@pytest.mark.parametrize("text", [None, "", "\n\n", "# nothing set\n"], ids=["no file", "empty", "blank", "comment"])
def test_nothing_set_gives_the_defaults(load, text):
    assert load(text) == DEFAULTS


@pytest.mark.covers("prefs UNIT_TASK_PRIORITY", "prefs RX_POOL_BUFS", "prefs LINK_MODE", "prefs AUTONEG",
                    "prefs FLOW_CONTROL", "prefs USE_MIAMI_WORKAROUND")
def test_every_key_is_read(load):
    text = ("UNIT_TASK_PRIORITY = 12\nRX_POOL_BUFS = 1024\nLINK_MODE = 100hd\nAUTONEG = off\n"
            "FLOW_CONTROL = off\nUSE_MIAMI_WORKAROUND = on\n")
    assert load(text) == {"unit_task_priority": "12", "rx_pool_bufs": "1024", "link_speed": "100",
                          "link_duplex": "half", "link_autoneg": "no", "flow_control": "no",
                          "use_miami_workaround": "yes"}


@pytest.mark.covers("prefs grammar")
def test_keys_and_values_are_read_in_any_case_with_blanks_and_crlf(load):
    text = "  unit_task_priority=10\r\n\tLink_Mode   =   10FD  \r\nFlow_Control\t=\tOFF\r\n"
    assert load(text) == changed(unit_task_priority="10", link_speed="10", flow_control="no")


@pytest.mark.covers("prefs grammar")
def test_comments_and_lines_without_a_setting_are_skipped(load):
    text = ("# UNIT_TASK_PRIORITY = 8\n; FLOW_CONTROL = off\n   # RX_POOL_BUFS = 600\nLINK_MODE 100fd\n"
            "= off\nAUTONEG =\nUSE_MIAMI_WORKAROUND =   \nUNIT_TASK_PRIORITY = 11\n")
    assert load(text) == changed(unit_task_priority="11")


@pytest.mark.covers("prefs grammar")
def test_a_later_line_overrides_an_earlier_one(load):
    assert load("LINK_MODE = 10hd\nLINK_MODE = 1000fd\n") == changed(link_speed="1000")


@pytest.mark.covers("prefs 3.x keys")
def test_the_3x_keys_and_unknown_ones_are_ignored(load):
    text = "".join(f"{key} = 7\n" for key in OLD_KEYS) + "NO_SUCH_KEY = on\n"
    assert load(text) == DEFAULTS


@pytest.mark.covers("prefs UNIT_TASK_PRIORITY")
@pytest.mark.parametrize("value, expected", [("6", "6"), ("19", "19"), ("5", "6"), ("0", "6"), ("-5", "6"),
                                             ("20", "19"), ("200", "19"), ("+12", "12"),
                                             ("-2147483648", "6"), ("2147483647", "19")])
def test_the_unit_task_priority_is_clamped_to_6_to_19(load, value, expected):
    assert load(f"UNIT_TASK_PRIORITY = {value}\n")["unit_task_priority"] == expected


@pytest.mark.covers("prefs UNIT_TASK_PRIORITY")
@pytest.mark.parametrize("value", ["high", "12abc", "1 2", "-", "2147483648", "0x10"])
def test_a_priority_that_is_not_a_number_keeps_the_default(load, value):
    assert load(f"UNIT_TASK_PRIORITY = {value}\n")["unit_task_priority"] == "15"


@pytest.mark.covers("prefs RX_POOL_BUFS")
@pytest.mark.parametrize("value, expected", [("0", "0"), ("1", "512"), ("511", "512"), ("512", "512"),
                                             ("2048", "2048"), ("4096", "4096"), ("4097", "4096"),
                                             ("2147483647", "4096")])
def test_an_explicit_rx_pool_is_clamped_to_512_to_4096_and_0_is_auto(load, value, expected):
    assert load(f"RX_POOL_BUFS = {value}\n")["rx_pool_bufs"] == expected


@pytest.mark.covers("prefs RX_POOL_BUFS")
@pytest.mark.parametrize("value", ["-1", "many", "1k"])
def test_an_rx_pool_that_is_not_a_count_keeps_auto(load, value):
    assert load(f"RX_POOL_BUFS = {value}\n")["rx_pool_bufs"] == "0"


@pytest.mark.covers("prefs LINK_MODE")
@pytest.mark.parametrize("value, speed, duplex", [("auto", "0", "full"), ("10hd", "10", "half"),
                                                  ("10fd", "10", "full"), ("100hd", "100", "half"),
                                                  ("100fd", "100", "full"), ("1000fd", "1000", "full")])
def test_each_link_mode_narrows_to_its_speed_and_duplex(load, value, speed, duplex):
    assert load(f"LINK_MODE = {value}\n") == changed(link_speed=speed, link_duplex=duplex)


@pytest.mark.covers("prefs LINK_MODE")
@pytest.mark.parametrize("value", ["1000hd", "fast", "100"])
def test_an_unknown_link_mode_keeps_auto(load, value):
    assert load(f"LINK_MODE = {value}\n") == DEFAULTS


@pytest.mark.covers("prefs AUTONEG", "prefs LINK_MODE")
@pytest.mark.parametrize("text", ["AUTONEG = off\nLINK_MODE = 10hd\n", "LINK_MODE = 10hd\nAUTONEG = off\n"],
                         ids=["autoneg first", "link mode first"])
def test_autoneg_off_forces_the_link_mode_in_either_order(load, text):
    assert load(text) == changed(link_speed="10", link_duplex="half", link_autoneg="no")


@pytest.mark.covers("prefs AUTONEG")
def test_autoneg_off_without_a_link_mode_speed_keeps_autonegotiation(load):
    assert load("AUTONEG = off\n") == DEFAULTS
    assert load("AUTONEG = off\nLINK_MODE = auto\n") == DEFAULTS


@pytest.mark.covers("prefs AUTONEG")
def test_autoneg_off_at_1000fd_keeps_autonegotiation_advertising_1000fd_only(load):
    assert load("LINK_MODE = 1000fd\nAUTONEG = off\n") == changed(link_speed="1000")


@pytest.mark.covers("prefs AUTONEG", "prefs FLOW_CONTROL", "prefs USE_MIAMI_WORKAROUND")
@pytest.mark.parametrize("value, on", [("on", True), ("ON", True), ("yes", True), ("True", True), ("1", True),
                                       ("off", False), ("No", False), ("FALSE", False), ("0", False)])
def test_switches_take_every_spelling(load, value, on):
    text = f"LINK_MODE = 100fd\nAUTONEG = {value}\nFLOW_CONTROL = {value}\nUSE_MIAMI_WORKAROUND = {value}\n"
    found = load(text)
    word = "yes" if on else "no"
    assert (found["link_autoneg"], found["flow_control"], found["use_miami_workaround"]) == (word, word, word)


@pytest.mark.covers("prefs AUTONEG", "prefs FLOW_CONTROL", "prefs USE_MIAMI_WORKAROUND")
@pytest.mark.parametrize("value", ["maybe", "2", "onn"])
def test_a_switch_that_is_neither_keeps_its_default(load, value):
    text = f"LINK_MODE = 100fd\nAUTONEG = {value}\nFLOW_CONTROL = {value}\nUSE_MIAMI_WORKAROUND = {value}\n"
    assert load(text) == changed(link_speed="100")
