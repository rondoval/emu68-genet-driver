# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
trace.py - traceability (run.sh host): what the suite must cover, the tests
that cover it, and TRACEABILITY.md, which says so.

The items, most read from the sources so a new one cannot be missed:
  - the netdev commands netdev_process_command() dispatches;
  - the SANA-II commands sana2_process_command() dispatches, and the reads
    and writes BeginIO serves itself;
  - NSCMD_DEVICEQUERY;
  - the S2EVENT_* the SANA-II side reports, and those S2_ONEVENT finds
    holding already;
  - the ENV:genet.prefs keys runtime_config.c reads, and its grammar, its
    defaults and the 3.x keys it ignores;
  - the claim's rules and the refusal codes AGENTS.md names (a fixed list).

A test covers an item when one of its @pytest.mark.covers() labels is the
item's name, or matches one of the item's patterns.

    python3 -m harness.trace     # writes TRACEABILITY.md; exits 1 when an item has no test
"""
from __future__ import annotations

import ast
import re
import sys
from dataclasses import dataclass, field

from .vamos import COMPONENT_DIR

TESTS = COMPONENT_DIR / "tests"
SOURCES = COMPONENT_DIR / "genet.device" / "src"
OUTPUT = TESTS / "TRACEABILITY.md"
SHOWN = 3                     # tests named per item; the rest counted
BEGINIO_COMMANDS = ("CMD_READ", "S2_READORPHAN", "CMD_WRITE", "S2_BROADCAST", "S2_MULTICAST")
PREFS_RULES = ("prefs defaults", "prefs grammar", "prefs 3.x keys")

# AGENTS.md's claim: who holds the unit, and what the other personality is told then
CLAIM_RULES = {
    "claim: nobody holds": "opening holds nothing; the interface removed, either personality takes the unit",
    "claim: netdev holds": "ATTACH to DETACH; SANA-II's claim, multicast and datapath answer S2ERR_OUTOFSERVICE",
    "claim: SANA-II holds": "an opener's S2_CONFIGINTERFACE or S2_ONLINE to its close; ATTACH answers NDERR_BUSY",
    "claim: SANA-II after netdev": "SANA-II takes the unit once netdev let go",
    "claim: netdev after SANA-II": "netdev takes the unit once the last claimer closed",
    "issued-opener list": "a close takes back only an Opener the open handed out",
}
REFUSALS = ("NDERR_BUSY", "NDERR_NOTATTACHED", "NDERR_BADPARAMS", "NDERR_BADVERSION", "S2ERR_OUTOFSERVICE",
            "S2ERR_BAD_STATE", "S2ERR_BAD_ARGUMENT", "S2ERR_BAD_ADDRESS", "S2ERR_NO_RESOURCES", "IOERR_OPENFAIL",
            "IOERR_UNITBUSY", "IOERR_BADLENGTH", "IOERR_ABORTED")


@dataclass
class Item:
    """One thing the suite must cover."""

    group: str
    name: str
    note: str = ""
    patterns: list[str] = field(default_factory=list)   # labels matching one cover it too
    tests: list[str] = field(default_factory=list)

    def covered_by(self, label: str) -> bool:
        """Whether @label covers this item."""
        return label == self.name or any(re.search(pattern, label) for pattern in self.patterns)


def labels() -> dict[str, list[str]]:
    """Every covers() label in the suite: label -> its tests, each as 'file::function'."""
    found: dict[str, list[str]] = {}
    for path in sorted(TESTS.rglob("test_*.py")):
        if ".venv" in path.parts:
            continue
        for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                for label in _covers(node):
                    found.setdefault(label, []).append(f"{path.relative_to(TESTS)}::{node.name}")
    return found


def _covers(node) -> list[str]:
    """The covers() labels on a test function."""
    return [argument.value for decorator in node.decorator_list
            if isinstance(decorator, ast.Call) and getattr(decorator.func, "attr", None) == "covers"
            for argument in decorator.args if isinstance(argument, ast.Constant) and isinstance(argument.value, str)]


def function_body(source: str, signature: str) -> str:
    """The body of the function whose definition starts with @signature in the driver file @source."""
    text = (SOURCES / source).read_text(encoding="utf-8")
    start = text.index(signature)
    return text[start:text.index("\n}\n", start)]


def dispatched(source: str, signature: str) -> list[str]:
    """The case labels of a dispatcher."""
    return re.findall(r"case\s+(\w+):", function_body(source, signature))


def events() -> list[str]:
    """The S2EVENT_* the SANA-II side reports or finds holding."""
    found = set()
    for path in sorted(SOURCES.glob("sana2_*.c")):
        text = path.read_text(encoding="utf-8")
        for argument in re.findall(r"sana2_report_events\(unit,\s*([^;]*)\);", text):
            found |= set(re.findall(r"S2EVENT_\w+", argument))
        for value in re.findall(r"pendingEvents\s*\|=\s*([^;]*);", text):
            found |= set(re.findall(r"S2EVENT_\w+", value))
    found |= set(re.findall(r"S2EVENT_\w+", function_body("sana2_api.c", "static u32 s2_events_present(")))
    return sorted(found)


def prefs_keys() -> list[str]:
    """The keys LoadGenetRuntimeConfig() compares against."""
    text = (COMPONENT_DIR / "runtime-config" / "src" / "runtime_config.c").read_text(encoding="utf-8")
    return re.findall(r'_Stricmp\(\(CONST_STRPTR\)key, \(CONST_STRPTR\) "(\w+)"\)', text)


def items() -> list[Item]:
    """Every item to cover."""
    listed = [Item("netdev commands", name) for name in dispatched("netdev_api.c", "BYTE netdev_process_command(")]
    sana2 = dict.fromkeys(dispatched("sana2_api.c", "u32 sana2_process_command(") + list(BEGINIO_COMMANDS))
    listed += [Item("SANA-II commands", name) for name in sana2]
    listed += [Item("new-style commands", "NSCMD_DEVICEQUERY")]
    listed += [Item("SANA-II events", name) for name in events()]
    listed += [Item("prefs keys", f"prefs {key}") for key in prefs_keys()]
    listed += [Item("prefs keys", rule) for rule in PREFS_RULES]
    listed += [Item("the claim", name, note) for name, note in CLAIM_RULES.items()]
    listed += [Item("refusal codes", name) for name in REFUSALS]
    return listed


def trace() -> list[Item]:
    """Every item, with the tests that cover it."""
    found = labels()
    listed = items()
    for item in listed:
        item.tests = sorted({test for label, tests in found.items() if item.covered_by(label) for test in tests})
    return listed


def markdown(traced: list[Item]) -> str:
    """TRACEABILITY.md's text."""
    uncovered = [item for item in traced if not item.tests]
    lines = ["# genet.device: traceability", "",
             "Written by `harness/trace.py` (`run.sh host` rewrites it): every item the suite must cover, "
             "with the tests that cover it.", "", f"**Uncovered: {len(uncovered)}** of {len(traced)} items.", ""]
    for group in dict.fromkeys(item.group for item in traced):
        lines += [f"## {group}", "", "| Item | Tests |", "|---|---|"]
        for item in (i for i in traced if i.group == group):
            shown = ", ".join(f"`{test}`" for test in item.tests[:SHOWN])
            more = f" and {len(item.tests) - SHOWN} more" if len(item.tests) > SHOWN else ""
            name = f"{item.name} ({item.note})" if item.note else item.name
            lines.append(f"| {name} | {shown + more if item.tests else '**none**'} |")
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    traced = trace()
    OUTPUT.write_text(markdown(traced), encoding="utf-8")
    uncovered = [f"{item.group}: {item.name}" for item in traced if not item.tests]
    print("\n".join(uncovered) if uncovered else f"all {len(traced)} items covered")
    return 1 if uncovered else 0


if __name__ == "__main__":
    sys.exit(main())
