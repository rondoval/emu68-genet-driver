# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""The code gates on the component's own C (harness/gates.py) and traceability (harness/trace.py), on
every host run."""
import pytest

from harness import gates, trace

pytestmark = pytest.mark.gates


def test_every_function_of_the_component_s_own_files_has_its_kernel_doc():
    assert gates.kerneldoc_findings() == []


def test_every_function_stays_within_its_complexity_limit_and_the_allow_list_only_goes_down():
    assert gates.complexity_findings() == []


def test_every_item_has_a_test_and_traceability_md_says_which():
    traced = trace.trace()
    trace.OUTPUT.write_text(trace.markdown(traced), encoding="utf-8")
    uncovered = [f"{item.group}: {item.name}" for item in traced if not item.tests]
    assert uncovered == [], "no test covers:\n" + "\n".join(uncovered)
