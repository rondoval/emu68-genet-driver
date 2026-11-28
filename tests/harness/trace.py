# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
trace.py - what this suite must cover (run.sh host), for emu68test's
traceability, which finds the tests that cover it and writes TRACEABILITY.md.

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
"""
from __future__ import annotations

import re

from emu68test.trace import Item, case_labels, function_body

from .suite import COMPONENT_DIR, TESTS

SOURCES = COMPONENT_DIR / "genet.device" / "src"
OUTPUT = TESTS / "TRACEABILITY.md"
TITLE = "genet.device"
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



def events() -> list[str]:
    """The S2EVENT_* the SANA-II side reports or finds holding."""
    found = set()
    for path in sorted(SOURCES.glob("sana2_*.c")):
        text = path.read_text(encoding="utf-8")
        for argument in re.findall(r"sana2_report_events\(unit,\s*([^;]*)\);", text):
            found |= set(re.findall(r"S2EVENT_\w+", argument))
        for value in re.findall(r"pendingEvents\s*\|=\s*([^;]*);", text):
            found |= set(re.findall(r"S2EVENT_\w+", value))
    found |= set(re.findall(r"S2EVENT_\w+", function_body(SOURCES / "sana2_api.c", "static u32 s2_events_present(")))
    return sorted(found)


def prefs_keys() -> list[str]:
    """The keys LoadGenetRuntimeConfig() compares against."""
    text = (COMPONENT_DIR / "runtime-config" / "src" / "runtime_config.c").read_text(encoding="utf-8")
    return re.findall(r'_Stricmp\(\(CONST_STRPTR\)key, \(CONST_STRPTR\) "(\w+)"\)', text)


def items() -> list[Item]:
    """Every item to cover."""
    listed = [Item("netdev commands", name) for name in case_labels(SOURCES / "netdev_api.c", "BYTE netdev_process_command(")]
    sana2 = dict.fromkeys(case_labels(SOURCES / "sana2_api.c", "u32 sana2_process_command(") + list(BEGINIO_COMMANDS))
    listed += [Item("SANA-II commands", name) for name in sana2]
    listed += [Item("new-style commands", "NSCMD_DEVICEQUERY")]
    listed += [Item("SANA-II events", name) for name in events()]
    listed += [Item("prefs keys", f"prefs {key}") for key in prefs_keys()]
    listed += [Item("prefs keys", rule) for rule in PREFS_RULES]
    listed += [Item("the claim", name, note) for name, note in CLAIM_RULES.items()]
    listed += [Item("refusal codes", name) for name in REFUSALS]
    return listed
