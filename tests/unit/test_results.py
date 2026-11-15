# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""The kv grammar GenetTest and the logic programs print."""
from pathlib import Path

import pytest

from harness.results import RC_CHECK_FAILED, HarnessError, parse

pytestmark = pytest.mark.unit

SAMPLES = Path(__file__).parent / "samples"


def test_a_passing_run():
    result = parse((SAMPLES / "genettest_pass.txt").read_text(), 0)
    assert result.passed and result.kv["command"] == "netdev"
    assert [c.name for c in result.checks()] == ["S2_DEVICEQUERY answers", "S2_CONFIGINTERFACE refused beside netdev"]
    assert result.num("attach.error") == 41
    assert result.prose == ["the unit is held by netdev"]
    assert result.check("S2_DEVICEQUERY answers").detail == "error 0, MTU 1500"
    assert result.num("checks") == 2 and result.num("checks_failed") == 0
    assert result.require("netdev") is result
    with pytest.raises(HarnessError):
        result.check("no such check")


def test_a_failing_run_names_its_failed_check():
    result = parse((SAMPLES / "genettest_fail.txt").read_text(), RC_CHECK_FAILED)
    assert not result.passed and result.verdict == "FAIL"
    assert result.failures() == ["exact filter: 239.255.77.17 delivered (12 frames), want none"]
    with pytest.raises(AssertionError, match="exact filter"):
        result.require("mcast")


def test_numbers_and_flags():
    result = parse("format=1\na=0x29\nb=-3\nc=93.5\nd=yes\nPASS\n", 0)
    assert (result.num("a"), result.num("b"), result.num("c"), result.flag("d")) == (41, -3, 93.5, True)
    assert result.num("missing", 7) == 7
    with pytest.raises(HarnessError):
        result.num("missing")


def test_a_usage_error_is_not_in_the_grammar():
    with pytest.raises(HarnessError, match="no format line"):
        parse("GenetTest: required argument missing\n", 20)


def test_a_later_format_is_refused():
    with pytest.raises(HarnessError, match="format 2"):
        parse("format=2\nPASS\n", 0)


def test_pass_counts_only_as_the_last_line():
    result = parse("format=1\nPASS\nx=1\n", 0)
    assert result.verdict is None and "PASS" in result.prose
