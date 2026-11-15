# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
stack.py - the network stack on the Amiga and its genet interface.

lwip-amiga picks genet's personality from the interface file's TYPE= line
(AUTO, NETDEV, SANA2); Roadshow always speaks SANA-II. AddNetInterface names
an interface after its file, so a copy of the machine's file in RAM: with
another TYPE= brings up the same "genet" in that personality, and the
machine's own file is never touched (interface.py). Changing the personality
means taking the interface down and up again, which drops the agent's own
connection, so it runs as a detached script (Script, Device.script()). Prefs
reach the driver only at its first open: the script writes ENV:genet.prefs
between the remove and the add.
"""
from __future__ import annotations

import re
from dataclasses import dataclass

from .install import PREFS

LWIP = "lwip"
ROADSHOW = "roadshow"

ADD = "C:AddNetInterface"
REMOVE = "C:RemoveNetInterface"
NAME = "genet"
ADD_TIMEOUT_S = 30
RC_ERROR = 10  # AddNetInterface: 5 = up, its DHCP lease still coming; 10 and more = not up
RESCUE = "rescue"
SOCKBENCH = "RAM:gt/sockbench"
NETDEV_STATS = "RAM:gt/netdev-stats"

AUTO = "AUTO"
NETDEV = "NETDEV"
SANA2 = "SANA2"

# lwip-amiga's bsdsocket.library is 4.1xx; Roadshow's is 4.3xx
ROADSHOW_MINOR = 300

TYPE_RE = re.compile(r"^\s*TYPE\s*=\s*(\S+)", re.I | re.M)
VERSION_RE = re.compile(r"bsdsocket\.library (\d+)\.(\d+)")
NETINFO_RE = re.compile(r"^(\S+)\t(UP|DOWN)  \((\S+) unit (\d+)\)\n\tinet (\S+)", re.M)


def stack_of(version: str) -> str | None:
    """Which stack a 'Version LIBS:bsdsocket.library' answer names; None when neither."""
    m = VERSION_RE.search(version)
    if m is None:
        return None
    return ROADSHOW if int(m.group(2)) >= ROADSHOW_MINOR else LWIP


def interface_type(text: str) -> str:
    """The TYPE= of a NetInterfaces file; AUTO when it has none (lwip-amiga's default)."""
    m = TYPE_RE.search(text)
    return m.group(1).upper() if m else AUTO


def set_type(text: str, kind: str) -> str:
    """@text with its TYPE= line replaced by TYPE=@kind, or one added."""
    out, done = [], False
    for line in text.splitlines():
        if TYPE_RE.match(line):
            if not done:
                out.append(f"TYPE={kind}")
                done = True
            continue
        out.append(line)
    if not done:
        out.append(f"TYPE={kind}")
    return "\n".join(out) + "\n"


@dataclass
class Netinfo:
    """lwip-amiga's netinfo line for one interface."""

    up: bool
    device: str
    unit: int
    address: str


def netinfo(text: str, name: str = NAME) -> Netinfo | None:
    """@name's entry in netinfo's output; None when it is not there."""
    for found, state, device, unit, address in NETINFO_RE.findall(text):
        if found == name:
            return Netinfo(state == "UP", device, int(unit), address)
    return None


class Script:
    """A detached AmigaDOS script that logs every step to RAM:gt/<name>.log and ends in a marker."""

    def __init__(self, name: str):
        self.name = name
        self.log = f"RAM:gt/{name}.log"
        self.marker = f"{name.upper()} DONE"
        self.lines = ["FailAt 21", f'Echo >{self.log} "{name} start"',
                      "Wait 5"]  # the agent answers the Run before the interface goes

    def say(self, text: str) -> "Script":
        """An Echo of @text into the log."""
        self.lines.append(f'Echo >>{self.log} "{text}"')
        return self

    def run(self, label: str, command: str, args: str = "") -> "Script":
        """@command with its output in the log, then '<label> rc <n>'."""
        self.lines.append(f"{command} >>{self.log} {args}".rstrip())
        self.lines.append(f'Echo >>{self.log} "{label} rc $RC"')
        return self

    def remove(self, label: str = "remove") -> "Script":
        """The genet interface down."""
        self.run(label, REMOVE, f"{NAME} FORCE")
        self.lines.append("Wait 2")
        return self

    def add(self, label: str = "add", interface: str = NAME) -> "Script":
        """An interface up, from DEVS:NetInterfaces/@interface or a file path; '<label> rc <n>' logged.

        The agent talks over this interface: when the add fails, the machine's
        own interface goes back up ('rescue rc <n>'), so the host can reach
        the Amiga again and read the log.
        """
        # the shell ends a variable's name at an underscore: $add_rc would print as it stands
        self.lines += [f"{ADD} >>{self.log} {interface} TIMEOUT={ADD_TIMEOUT_S}",
                       "Set addrc $RC",
                       f'Echo >>{self.log} "{label} rc $addrc"',
                       f"If $addrc GE {RC_ERROR} VAL",
                       f'  Echo >>{self.log} "{label} failed, the machine interface goes back"',
                       f"  {REMOVE} >>{self.log} {NAME} FORCE",
                       f"  {ADD} >>{self.log} {NAME} TIMEOUT={ADD_TIMEOUT_S}",
                       f'  Echo >>{self.log} "{RESCUE} rc $RC"',
                       "EndIf"]
        return self

    def pri(self, priority: int) -> "Script":
        """The script's own priority, which the programs it runs inherit (out of Executive's reach at 6)."""
        self.lines.append(f"ChangeTaskPri {priority}")
        return self

    def program(self, label: str, command: str, args: str, out: str) -> "Script":
        """@command with its output in a file of its own (@out, for the host to parse), then '<label> rc <n>'."""
        self.lines.append(f"{command} >{out} {args}".rstrip())
        self.lines.append(f'Echo >>{self.log} "{label} rc $RC"')
        return self

    def bench(self, label: str, args: str) -> "Script":
        """A section '== <label>' holding one sockbench run's output (section())."""
        self.say(f"== {label}")
        return self.run(label, SOCKBENCH, args)

    def netdev_stats(self, label: str, args: str = "") -> "Script":
        """A section '== <label>' holding one netdev-stats run's output."""
        self.say(f"== {label}")
        return self.run(label, NETDEV_STATS, args)

    def copy(self, source: str, target: str) -> "Script":
        """@source over @target (a prefs or interface file the host uploaded before)."""
        self.lines.append(f"Copy {source} {target} QUIET")
        return self

    def prefs(self, source: str | None) -> "Script":
        """ENV:genet.prefs replaced by @source, or deleted for None (the defaults)."""
        if source is None:
            self.lines.append(f"Delete {PREFS} QUIET")
            return self
        return self.copy(source, PREFS)

    def text(self) -> str:
        """The script, with netinfo and the marker at its end."""
        tail = [f"C:netinfo >>{self.log}", f'Echo >>{self.log} "{self.marker}"']
        return "\n".join(self.lines + tail) + "\n"


def rc_of(log: str, label: str) -> int | None:
    """The return code a Script logged for @label; None when that step never ran."""
    m = re.search(rf"^{re.escape(label)} rc (\d+)", log, re.M)
    return int(m.group(1)) if m else None


def rescued(log: str) -> bool:
    """Whether an add in the script failed and the machine's own interface was put back."""
    return re.search(rf"^{RESCUE} rc ", log, re.M) is not None


def section(log: str, label: str) -> str | None:
    """What a Script logged under '== @label', up to the next section; None when it never got there."""
    head = f"== {label}\n"
    at = log.find(head)
    if at < 0:
        return None
    start = at + len(head)
    end = log.find("\n== ", start)
    return log[start:] if end < 0 else log[start:end + 1]
