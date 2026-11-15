# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
vamos.py - the logic tier's programs (tests/logic/): the driver's own sources
built by the stack as m68k CLI programs, run on the host under amitools'
vamos. A program prints the output grammar (results.py) and the test judges
what it printed.

The stack builds them into <build>/emu68-genet-driver-netdev/tests/logic/.
A program older than a file it was compiled from (its compiler's depfiles in
the build dir name them) is stale: the test fails rather than judge code that
is no longer there (scripts/docker-build.sh rebuilds it).
"""
from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from . import results
from .build import COMPONENT

VAMOS = "vamos"
CPU = "68040"
RAM_KIB = 8192               # room for SANA-II's TX staging (256 slots of 2 KB); vamos refuses more with custom chips mapped
TIMEOUT_S = 60
COMPONENT_DIR = Path(__file__).resolve().parents[2]
STACK_DIR = COMPONENT_DIR.parents[1]
CONTAINER_ROOT = "/work/"  # the stack checkout inside the build container (scripts/docker-build.sh)


def unavailable() -> str | None:
    """Why the logic tier cannot run on this host, or None when it can."""
    if shutil.which(VAMOS) is None:
        return "vamos is not installed (pipx install amitools)"
    return None


def programs_dir(build_dir: Path) -> Path:
    """Where the stack build at @build_dir puts the logic tier's programs."""
    return Path(build_dir) / COMPONENT / "tests" / "logic"


def dependencies(depfiles: list[Path]) -> list[Path]:
    """The stack's files the compiler's @depfiles name (the toolchain's left out), as host paths."""
    found: set[Path] = set()
    for depfile in depfiles:
        for token in depfile.read_text().replace("\\\n", " ").split():
            if token.startswith(CONTAINER_ROOT):
                found.add(STACK_DIR / token[len(CONTAINER_ROOT):])
    return sorted(found)


def listed(facts: dict[str, str], name: str) -> list[str]:
    """The values of @name.0, @name.1, ... in @facts (a step's), in index order."""
    found = {int(key[len(name) + 1:]): value for key, value in facts.items()
             if key.startswith(name + ".") and key[len(name) + 1:].isdigit()}
    return [found[index] for index in sorted(found)]


def grouped(facts: dict[str, str], name: str) -> list[dict[str, str]]:
    """The groups @name.N.* in @facts, each as a dict of what follows @name.N., in index order."""
    found: dict[int, dict[str, str]] = {}
    for key, value in facts.items():
        head, _, rest = key.partition(".")
        if head != name:
            continue
        index, _, field = rest.partition(".")
        if index.isdigit() and field:
            found.setdefault(int(index), {})[field] = value
    return [found[index] for index in sorted(found)]


@dataclass
class Run:
    """One run of a program: its result, and what vamos said on stderr."""

    result: results.Result
    stderr: str = ""

    def steps(self) -> list[dict[str, str]]:
        """The facts of each step, step.N.*, without the prefix; HarnessError when a step was not taken."""
        if "fail" in self.result.kv:
            raise results.HarnessError(f"the program refused its steps: {self.result.kv['fail']}")
        return self.result.group("step")


@dataclass
class Program:
    """A logic program in the build dir."""

    path: Path

    def stale(self) -> Path | None:
        """A file the program was compiled from that changed since, or went; None when it is current."""
        depfiles = sorted((self.path.parent / "CMakeFiles" / f"{self.path.name}.dir").rglob("*.d"))
        built = self.path.stat().st_mtime
        for source in dependencies(depfiles):
            if not source.exists() or source.stat().st_mtime > built:
                return source
        return None

    def command(self, args: list[str], volumes: dict[str, Path], assigns: dict[str, str]) -> list[str]:
        """vamos's command line for @args, with @volumes (name: host drawer) and @assigns (name: Amiga path)."""
        command = [VAMOS, "-q", "-C", CPU, "-m", str(RAM_KIB)]
        for name, drawer in volumes.items():
            command += ["-V", f"{name}:{Path(drawer).resolve()}"]
        for name, target in assigns.items():
            command += ["-a", f"{name}:{target}"]
        return command + [self.path.name, *args]

    def run(self, *args: str, volumes: dict[str, Path] | None = None, assigns: dict[str, str] | None = None) -> Run:
        """Run the program in its own drawer; AssertionError when it ends without a verdict (it crashed)."""
        command = self.command(list(args), volumes or {}, assigns or {})
        done = subprocess.run(command, cwd=self.path.parent, capture_output=True, text=True, timeout=TIMEOUT_S)
        result = results.parse(done.stdout, done.returncode)
        if result.verdict is None:
            raise AssertionError(f"{self.path.name} {' '.join(args)}: no verdict, rc {done.returncode} "
                                 f"(crashed under vamos?): {done.stderr.strip()[-500:]}")
        return Run(result, done.stderr)
