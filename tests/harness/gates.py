# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
gates.py - the code gates on the component's own C (run.sh host): every
function's kernel-doc and its cyclomatic complexity.

The scope is what AGENTS.md calls ours: the device scaffolding, both
personalities, the datapath files, the prefs reader, genet-stats and the
suite's own programs. The upstream-diffable files (the PHY layer,
bcmgenet.c, bcmgenet-mac.c and the imported headers) are left out: a clean
diff against U-Boot and Linux is worth more there than our comments.

- Kernel-doc: a /** comment right above every function, "name() - what it
  does", an "@param:" line for each parameter, and "Return:" unless it
  returns void.
- Complexity (lizard's cyclomatic complexity): at most COMPLEXITY_LIMIT; a
  function in complexity-allow.txt at most the ceiling given there, which is
  at most COMPLEXITY_ALLOWED with a reason, or what the 5.0 code had (a reason
  starting "5.0:"). A ceiling stays at what the function needs: one above it
  is a finding until lowered, so the list only goes down.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

import lizard

from .vamos import COMPONENT_DIR

TESTS = COMPONENT_DIR / "tests"
ALLOW_LIST = TESTS / "complexity-allow.txt"
COMPLEXITY_LIMIT = 10
COMPLEXITY_ALLOWED = 15
INHERITED = "5.0:"                 # the reason of a ceiling the 5.0 code set
DRIVER_SOURCES = ("device", "unit", "unit_task", "netdev_api", "sana2_api", "sana2_io", "sana2_mcast", "sana2_stats",
                  "sana2_tx", "bcmgenet-link", "bcmgenet-mib", "bcmgenet-rxpool", "bcmgenet-rx", "bcmgenet-tx",
                  "bcmgenet-irq", "bcmgenet-dma", "devtree_parse")
OWN_FILES = ("genet.device/include/device.h", "genet.device/include/sana2_unit.h", "runtime-config/src/runtime_config.c",
             "runtime-config/include/runtime_config.h", "tools/genet-stats.c")
TEST_FOLDERS = ("tests/rig", "tests/common", "tests/logic")
REGISTER_BINDING = re.compile(r'asm\s*\("[^"]*"\)')
VOID_RESULT = re.compile(r"\b(void|VOID)\s*$")
ATTRIBUTE_MACRO = re.compile(r"^\s*[A-Z][A-Z0-9_]*\s*\([^;{}]*\)\s*$")   # EMU68_INTSERVER(bcmgenet_isr0)


def sources() -> list[Path]:
    """Every C file the gates cover."""
    found = [COMPONENT_DIR / "genet.device" / "src" / f"{name}.c" for name in DRIVER_SOURCES]
    found += [COMPONENT_DIR / name for name in OWN_FILES]
    for folder in TEST_FOLDERS:
        found += sorted((COMPONENT_DIR / folder).glob("*.[ch]"))
    return found


def where(path: Path, function) -> str:
    """A function as findings name it: <file>:<line> <name>."""
    return f"{path.relative_to(COMPONENT_DIR)}:{function.start_line} {function.name}"


def functions():
    """(file, its lines, lizard's function) for every function in the gates' sources."""
    for path in sources():
        lines = path.read_text(encoding="latin-1").splitlines()
        for function in lizard.analyze_file(str(path)).function_list:
            yield path, lines, function


@dataclass
class Ceiling:
    """An allow-list entry: the most a function may have, and why."""

    ceiling: int
    reason: str


def allowed(path: Path = ALLOW_LIST) -> dict[str, Ceiling]:
    """<file>:<function> -> its ceiling, from the allow list; ValueError for a line that is not an entry."""
    entries = {}
    for number, line in enumerate(path.read_text().splitlines() if path.exists() else [], 1):
        if not line.strip() or line.startswith("#"):
            continue
        found = re.match(r"^(\S+:\S+)\s+(\d+)\s+(\S.*)$", line)
        if found is None:
            raise ValueError(f"{path.name}:{number}: not '<file>:<function> <ceiling> <reason>'")
        entries[found.group(1)] = Ceiling(int(found.group(2)), found.group(3))
    return entries


def ceiling_finding(name: str, complexity: int, entry: Ceiling | None) -> str | None:
    """What is wrong with a function's complexity against its allow-list entry, or None."""
    if entry is None:
        return None if complexity <= COMPLEXITY_LIMIT else f"complexity {complexity} > {COMPLEXITY_LIMIT}"
    if entry.ceiling > COMPLEXITY_ALLOWED and not entry.reason.startswith(INHERITED):
        return f"ceiling {entry.ceiling} > {COMPLEXITY_ALLOWED}: only the 5.0 code's ceilings are higher"
    if complexity > entry.ceiling:
        return f"complexity {complexity} > its ceiling {entry.ceiling}"
    if complexity <= COMPLEXITY_LIMIT:
        return f"complexity {complexity}: needs no entry in {ALLOW_LIST.name} any more"
    if complexity < entry.ceiling:
        return f"complexity {complexity} < its ceiling {entry.ceiling}: lower the ceiling"
    return None


def complexity_findings(allow: dict[str, Ceiling] | None = None) -> list[str]:
    """Functions over their limit, ceilings to lower, and entries for functions that are gone."""
    allow = allowed() if allow is None else allow
    findings, seen = [], set()
    for path, _, function in functions():
        name = f"{path.relative_to(COMPONENT_DIR)}:{function.name}"
        seen.add(name)
        problem = ceiling_finding(name, function.cyclomatic_complexity, allow.get(name))
        if problem:
            findings.append(f"{where(path, function)}: {problem}")
    return findings + [f"{ALLOW_LIST.name}: {name} is no function of the gates' sources" for name in allow
                       if name not in seen]


def comment_before(lines: list[str], start: int) -> str:
    """The comment block that ends right above line @start (1-based), past blank lines and an
    attribute macro such as EMU68_INTSERVER(name), or ''."""
    index = start - 2
    while index >= 0 and (not lines[index].strip() or ATTRIBUTE_MACRO.match(lines[index])):
        index -= 1
    if index < 0 or not lines[index].rstrip().endswith("*/"):
        return ""
    end = index
    while index >= 0 and "/*" not in lines[index]:
        index -= 1
    return "\n".join(lines[index:end + 1]) if index >= 0 else ""


def declaration(lines: list[str], function) -> str:
    """The function's declaration, from its first line to the parameter list's ')'."""
    text = " ".join(lines[function.start_line - 1:function.start_line + 6])
    return text[:text.index("{")] if "{" in text else text


def parameter_names(lines: list[str], function) -> list[str]:
    """A function's parameter names from its source: lizard takes a register binding (asm("d0")) for a name."""
    text = declaration(lines, function)
    inside, depth, parameters, current = text[text.index("(") + 1:], 0, [], ""
    for character in inside:
        if character == ")" and depth == 0:
            break
        depth += {"(": 1, ")": -1}.get(character, 0)
        if character == "," and depth == 0:
            parameters, current = parameters + [current], ""
        else:
            current += character
    names = [re.findall(r"(\w+)\s*(?:\[[^]]*\])?$", REGISTER_BINDING.sub("", p).strip()) for p in parameters + [current]]
    return [found[0] for found in names if found and found[0] != "void"]


def returns_void(lines: list[str], function) -> bool:
    """Whether the function's declaration says it returns void."""
    before = declaration(lines, function).split(function.name + "(", 1)[0]
    return VOID_RESULT.search(before) is not None


def kerneldoc_problems(lines: list[str], function) -> list[str]:
    """What the function's comment lacks, by kernel-doc."""
    comment = comment_before(lines, function.start_line)
    if not comment.lstrip().startswith("/**") or f"{function.name}() - " not in comment:
        return ["no kernel-doc comment"]
    problems = [f"@{name} not described" for name in parameter_names(lines, function) if f"@{name}:" not in comment]
    if not returns_void(lines, function) and "Return:" not in comment:
        problems.append("no Return: line")
    return problems


def kerneldoc_findings() -> list[str]:
    """Functions whose comment is missing or does not follow kernel-doc."""
    return [f"{where(path, function)}: {problem}" for path, lines, function in functions()
            for problem in kerneldoc_problems(lines, function)]
