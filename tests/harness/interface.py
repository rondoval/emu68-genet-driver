# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
interface.py - genet's interface on the Amiga in the personality a test asks
for, and back as the machine had it once the session ends.

Each personality comes from a copy of the machine's DEVS:NetInterfaces/genet
in RAM:gt/<type>/genet with its TYPE= line set: AddNetInterface names an
interface after its file, so the copy brings up "genet" as the machine's file
would, in that personality, and the machine's file is never written. A switch
is a detached Script (the agent talks over the interface), then GenetTest
HOLDER proves who holds the unit.

Prefs tests write ENV:genet.prefs between a remove and an add (the driver
reads it at its first open) and put the machine's own back before their script
ends; restore() puts it back again in case a script did not get that far.
"""
from __future__ import annotations

from .amiga import RAM, Device
from .build import Artefacts
from .install import DRIVER, INTERFACE, PREFS
from .stack import AUTO, NAME, NETDEV, SANA2, Script, interface_type, rc_of, rescued, set_type
from .tools import genettest, push

MACHINE = "machine"  # the machine's own interface file, its own prefs
EXPECTED_HOLDER = {AUTO: "netdev", NETDEV: "netdev", SANA2: "sana2"}
ADD_UP = (0, 5)      # AddNetInterface: up, or up with its DHCP lease still coming
SCRIPT_LIMIT_S = 240


class SwitchFailed(AssertionError):
    """The interface did not come up in the personality asked for."""


class Interface:
    """The genet interface's personality on one Amiga."""

    def __init__(self, device: Device):
        self.device = device
        self.original = device.download(INTERFACE).decode("latin-1")
        self.machine_type = interface_type(self.original)
        self.prefs = self._read(PREFS)
        self.machine_prefs = f"{RAM}/prefs.machine" if self.prefs is not None else None
        self.failed: dict[str, str] = {}
        self.boots = 0      # fresh_boot()s this session: the boot a measurement was made on (values.json)
        self.rebooted()

    def fresh_boot(self, artefacts: Artefacts, driver: bytes | None = None) -> None:
        """A boot of its own, @driver installed first when given (the installer reboots when the file
        changed, and a reboot follows otherwise), then the tools and the RAM: copies back."""
        changed = driver is not None and any(p.changed for p in self.device.installer.install({DRIVER: driver}))
        if not changed:
            self.device.reboot()
        push(self.device, artefacts)
        self.failed.clear()  # the driver may be another one now
        self.boots += 1
        self.rebooted()

    def rebooted(self) -> None:
        """The RAM: copies made again after a boot emptied RAM:, the machine's own interface up (Startup)."""
        if self.prefs is not None:
            self.device.upload_bytes(self.machine_prefs, self.prefs)
        for kind in EXPECTED_HOLDER:
            self.device.exec(f"MakeDir >NIL: {RAM}/{kind.lower()}", timeout=15)
            self.device.upload_bytes(self.path(kind), set_type(self.original, kind).encode("latin-1"))
        self.current: str | None = MACHINE

    def _read(self, remote: str) -> bytes | None:
        try:
            return self.device.download(remote)
        except self.device.mod.AmigaError:
            return None

    @staticmethod
    def path(kind: str) -> str:
        """The RAM: interface file that brings genet up as @kind."""
        return f"{RAM}/{kind.lower()}/{NAME}"

    def holder(self) -> str:
        """Who holds the unit, by GenetTest HOLDER: netdev, sana2, none or unknown."""
        return genettest(self.device, "HOLDER", 30).kv.get("holder", "unknown")

    def prefs_file(self, name: str, text: str) -> str:
        """@text as a prefs file in RAM:gt (one key per line), for Script.prefs(); its path."""
        remote = f"{RAM}/prefs-{name}"
        self.device.upload_bytes(remote, text.encode("latin-1"))
        return remote

    def reconfigure(self, script: Script, label: str, kind: str, prefs: str | None = None) -> Script:
        """@script's steps: the interface down, ENV:genet.prefs from @prefs (a RAM: file; None = the
        machine's own), the interface up again as @kind. The add logs '<label> add rc <n>'."""
        script.remove(f"{label} remove")
        script.prefs(prefs if prefs is not None else self.machine_prefs)
        return script.add(f"{label} add", self.path(kind))

    def run(self, script: Script, ends_as: str | None, limit: float = SCRIPT_LIMIT_S) -> str:
        """@script run detached: its log. The interface is @ends_as after it; unknown when an add
        failed and the machine's own was put back, so the next use() switches again."""
        self.current = None
        log = self.device.script(script.name, script.text(), script.marker, limit)
        self.current = None if rescued(log) else ends_as
        return log

    def use(self, kind: str) -> None:
        """genet up as @kind (AUTO, NETDEV, SANA2), held by the personality that type means.

        A switch that failed is not tried again in the session: every test
        that needs that personality fails with the first reason.
        """
        if kind in self.failed:
            raise SwitchFailed(self.failed[kind])
        if self.current == kind:
            return
        script = Script(f"use-{kind.lower()}").remove().add("add", self.path(kind))
        log = self.run(script, kind)
        added = rc_of(log, "add")
        held = self.holder()
        if added in ADD_UP and held == EXPECTED_HOLDER[kind]:
            return
        self.current = None
        reason = f"TYPE={kind}: AddNetInterface rc {added}, the unit held by {held} (want {EXPECTED_HOLDER[kind]})"
        if rescued(log):
            reason += "; the machine's own interface was put back"
        self.failed[kind] = reason
        raise SwitchFailed(reason)

    def restore(self) -> str:
        """The machine's own interface file and prefs back in use, unless nothing changed them: what was done."""
        if self.current == MACHINE and self._read(PREFS) == self.prefs:
            return f"Interface: left as the machine had it (TYPE={self.machine_type})"
        script = Script("restore").remove()
        script.prefs(self.machine_prefs)
        script.add("add", NAME)
        log = self.run(script, MACHINE)
        prefs_back = self._read(PREFS) == self.prefs
        held = self.holder()
        note = (f"Interface: the machine's own back (TYPE={self.machine_type}), AddNetInterface rc "
                f"{rc_of(log, 'add')}, held by {held}, ENV:genet.prefs {'as it was' if prefs_back else 'DIFFERS'}")
        if rc_of(log, "add") not in ADD_UP or not prefs_back:
            raise AssertionError(note)
        return note
