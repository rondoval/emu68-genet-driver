# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""The gates themselves (harness/gates.py, harness/trace.py): each fails on what it is there to catch."""
import lizard
import pytest

from harness import gates, trace

pytestmark = pytest.mark.unit

DOCUMENTED = """/**
 * add() - Add two numbers.
 * @a: the first.
 * @b: the second.
 *
 * Return: their sum.
 */
static int add(int a, int b)
{
    return a + b;
}
"""


def analysed(text: str):
    """(lines, the first function) of a C source given as text."""
    return text.splitlines(), lizard.analyze_file.analyze_source_code("x.c", text).function_list[0]


def test_a_complete_kernel_doc_passes():
    assert gates.kerneldoc_problems(*analysed(DOCUMENTED)) == []


def test_a_function_without_kernel_doc_fails():
    assert gates.kerneldoc_problems(*analysed("/* a plain comment */\nstatic void f(void)\n{\n}\n")) == \
        ["no kernel-doc comment"]


def test_a_parameter_or_a_return_left_out_fails():
    text = DOCUMENTED.replace(" * @b: the second.\n", "").replace(" * Return: their sum.\n", "")
    assert gates.kerneldoc_problems(*analysed(text)) == ["@b not described", "no Return: line"]


def test_register_bound_parameters_are_read_by_name_and_an_attribute_line_is_skipped():
    text = ("/**\n * isr() - A server.\n * @unit: the unit.\n * @irq: unused.\n *\n * Return: 1.\n */\n"
            "EMU68_INTSERVER(isr)\nULONG isr(struct GenetUnit *unit asm(\"a1\"), ULONG irq asm(\"d0\"))\n{\n"
            "    return 1;\n}\n")
    assert gates.kerneldoc_problems(*analysed(text)) == []


@pytest.mark.parametrize("complexity, entry, problem", [
    (10, None, None),
    (11, None, "complexity 11 > 10"),
    (14, gates.Ceiling(14, "a reason"), None),
    (15, gates.Ceiling(14, "a reason"), "complexity 15 > its ceiling 14"),
    (12, gates.Ceiling(14, "a reason"), "complexity 12 < its ceiling 14: lower the ceiling"),
    (9, gates.Ceiling(14, "a reason"), "complexity 9: needs no entry in complexity-allow.txt any more"),
    (20, gates.Ceiling(20, "a reason"), "ceiling 20 > 15: only the 5.0 code's ceilings are higher"),
    (20, gates.Ceiling(20, "5.0: inherited"), None),
], ids=["at the limit", "over it", "at its ceiling", "over its ceiling", "under its ceiling", "simple again",
        "a new ceiling over 15", "an inherited one"])
def test_the_complexity_rules(complexity, entry, problem):
    assert gates.ceiling_finding("x.c:f", complexity, entry) == problem


def test_an_allow_list_entry_for_a_function_that_is_gone_is_a_finding():
    findings = gates.complexity_findings({"genet.device/src/device.c:no_such_function": gates.Ceiling(12, "5.0: x")} |
                                         gates.allowed())
    assert findings == ["complexity-allow.txt: genet.device/src/device.c:no_such_function is no function of the "
                        "gates' sources"]


def test_a_malformed_allow_list_line_is_refused(tmp_path):
    path = tmp_path / "allow.txt"
    path.write_text("# a comment\nfile.c:f 12 a reason\nnot an entry\n")
    with pytest.raises(ValueError, match="allow.txt:3"):
        gates.allowed(path)


def test_an_item_is_covered_by_its_own_label_or_a_pattern():
    item = trace.Item("group", "S2_ONLINE", patterns=[r"^claim: "])
    assert item.covered_by("S2_ONLINE") and item.covered_by("claim: netdev holds")
    assert not item.covered_by("S2_ONLINE_WAIT") and not item.covered_by("S2_OFFLINE")


def test_the_table_counts_and_marks_what_no_test_covers():
    text = trace.markdown([trace.Item("g", "covered", tests=["a::t"]), trace.Item("g", "bare")])
    assert "**Uncovered: 1** of 2 items." in text and "| bare | **none** |" in text and "| covered | `a::t` |" in text
