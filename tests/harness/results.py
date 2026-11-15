# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
results.py - the output grammar of the suite's Amiga-side programs (GenetTest
and the logic tests under vamos):

    output  := "format=1" NL line* verdict
    line    := key "=" value NL        key at column 0: [a-z][a-z0-9_]*("."[a-z0-9_]+)*;
                                       indexed groups by number (check.0.name);
                                       values: decimal, 0x hex, yes/no, one-decimal
                                       rates, names verbatim
             | "  " prose NL           for people; kept for the report, never parsed
    verdict := "PASS" NL | "FAIL" NL   the last line

Every check a program makes is a group entry: check.N.name=<what> (the
same text every run), check.N.ok=yes|no and check.N.detail=<what was seen>;
the last facts are checks=<n> and checks_failed=<n>.

Return codes (AmigaDOS levels): 0 every check passed (PASS); 5 a check failed;
10 the environment (a device, port or timer did not open); 20 usage: ReadArgs
failed before the format line, and parse() raises HarnessError.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

FORMAT = 1

RC_PASS = 0
RC_CHECK_FAILED = 5
RC_ENVIRONMENT = 10
RC_USAGE = 20

KEY_RE = re.compile(r"^([a-z][a-z0-9_]*(?:\.[a-z0-9_]+)*)=(.*)$")
FORMAT_RE = re.compile(r"^format=(\d+)$")
INT_RE = re.compile(r"^-?\d+$")
FLOAT_RE = re.compile(r"^-?\d+\.\d+$")


class HarnessError(Exception):
    """A defect of the harness or of a program's output, not of the driver under test."""


@dataclass
class Check:
    """One check a program made."""

    name: str
    ok: bool
    detail: str = ""


@dataclass
class Result:
    """One program run: its facts, its prose, its verdict and return code."""

    kv: dict[str, str] = field(default_factory=dict)
    prose: list[str] = field(default_factory=list)
    verdict: str | None = None
    rc: int = RC_PASS
    raw: str = ""

    def has(self, key: str) -> bool:
        """Whether the program printed @key."""
        return key in self.kv

    def num(self, key: str, default: int | float | None = None) -> int | float:
        """The number under @key: decimal or 0x hex as int, one-decimal rates as float."""
        if key not in self.kv:
            if default is not None:
                return default
            raise HarnessError(f"{key} missing from the program's output")
        value = self.kv[key]
        if value.lower().startswith(("0x", "-0x")):
            return int(value, 16)
        if INT_RE.match(value):
            return int(value)
        if FLOAT_RE.match(value):
            return float(value)
        raise HarnessError(f"{key}={value!r} is not a number")

    def flag(self, key: str) -> bool:
        """The yes/no under @key."""
        value = self.kv.get(key)
        if value not in ("yes", "no"):
            raise HarnessError(f"{key}={value!r} is not yes or no")
        return value == "yes"

    def group(self, prefix: str) -> list[dict[str, str]]:
        """The indexed group @prefix.N.* as a list of dicts in index order."""
        found: dict[int, dict[str, str]] = {}
        head = re.compile(rf"^{re.escape(prefix)}\.(\d+)\.(.+)$")
        for key, value in self.kv.items():
            m = head.match(key)
            if m:
                found.setdefault(int(m.group(1)), {})[m.group(2)] = value
        return [found[i] for i in sorted(found)]

    def checks(self) -> list[Check]:
        """The checks the program made, in order."""
        return [Check(c.get("name", "?"), c.get("ok") == "yes", c.get("detail", "")) for c in self.group("check")]

    def check(self, name: str) -> Check:
        """The check called @name; HarnessError when the program made none."""
        for found in self.checks():
            if found.name == name:
                return found
        raise HarnessError(f"no check {name!r} in the program's output")

    def failures(self) -> list[str]:
        """The failed checks, each as 'name: detail'."""
        return [f"{c.name}: {c.detail}" if c.detail else c.name for c in self.checks() if not c.ok]

    @property
    def passed(self) -> bool:
        """Whether the program ended in PASS with return code 0."""
        return self.verdict == "PASS" and self.rc == RC_PASS

    def require(self, what: str) -> "Result":
        """This result when it passed; AssertionError naming the failed checks otherwise."""
        if not self.passed:
            failed = self.failures() or [self.kv.get("fail", "no check named")]
            raise AssertionError(f"{what}: rc {self.rc}, {self.verdict}: " + "; ".join(failed))
        return self


def _format_checked(lines: list[str]) -> None:
    """HarnessError unless @lines (the non-empty ones) start with this harness's format line."""
    if not lines:
        raise HarnessError("no format line: a usage error or the wrong program (empty output)")
    first = FORMAT_RE.match(lines[0])
    if first is None:
        raise HarnessError(f"no format line: a usage error or the wrong program ({lines[0]!r})")
    if int(first.group(1)) != FORMAT:
        raise HarnessError(f"output format {first.group(1)}, this harness reads {FORMAT}")


def parse(text: str, rc: int) -> Result:
    """Read a program's output into a Result; HarnessError when it is not in the grammar."""
    lines = [line.rstrip("\r") for line in text.splitlines() if line.strip()]
    _format_checked(lines)
    result = Result(rc=rc, raw=text)
    body = lines[1:]
    if body and body[-1] in ("PASS", "FAIL"):
        result.verdict = body.pop()
    for line in body:
        m = KEY_RE.match(line)
        if m:
            result.kv[m.group(1)] = m.group(2).strip()
        else:
            result.prose.append(line.strip())
    return result
