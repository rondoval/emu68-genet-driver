# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
install.py - the build under test into its real places on the Amiga, the
machine's own files kept on the host first.

The first run against a machine copies each file it would replace into
tests/.machine/<server>/ and never overwrites those copies; --restore puts
them back. A passed build stays installed (the user's rule), so the next run
replaces the previous build, not the machine's original.

PFS3 delays its writes: a file that read back correctly was lost by a reboot a
second later, so every install waits 20 s, reads back, reboots, and reads back
again.
"""
from __future__ import annotations

import hashlib
import re
import time
from dataclasses import dataclass
from pathlib import Path

from .results import HarnessError

PFS3_SETTLE_S = 20

DRIVER = "DEVS:Networks/genet.device"
GENET_STATS = "C:genet-stats"
BSDSOCKET = "LIBS:bsdsocket.library"
INTERFACE = "DEVS:NetInterfaces/genet"
PREFS = "ENV:genet.prefs"


def digest(data: bytes) -> str:
    """A short fingerprint of @data, for the report."""
    return hashlib.sha256(data).hexdigest()[:16]


def backup_name(remote: str) -> str:
    """The host-side file name of @remote's original."""
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", remote)


@dataclass
class Placed:
    """One file the install wrote, or found already in place."""

    remote: str
    changed: bool
    digest: str
    version: str = ""


class Installer:
    """The machine's originals and the install of the build under test."""

    def __init__(self, device, backup_dir: Path):
        self.device = device
        self.backup_dir = Path(backup_dir)

    def original(self, remote: str) -> bytes | None:
        """The machine's own @remote as first seen; None when the machine had none."""
        path = self.backup_dir / backup_name(remote)
        return path.read_bytes() if path.exists() else None

    def keep_originals(self, remotes: list[str]) -> None:
        """Copy each of @remotes to the host once, before anything replaces it."""
        self.backup_dir.mkdir(parents=True, exist_ok=True)
        for remote in remotes:
            path = self.backup_dir / backup_name(remote)
            absent = self.backup_dir / (backup_name(remote) + ".absent")
            if path.exists() or absent.exists():
                continue
            try:
                path.write_bytes(self.device.download(remote))
            except self.device.mod.AmigaError:
                absent.write_text("the machine had no such file\n")

    def install(self, files: dict[str, bytes], reboot: bool = True) -> list[Placed]:
        """@files (remote path -> content) into place; a reboot when anything changed."""
        self.keep_originals(list(files))
        placed = []
        for remote, data in files.items():
            current = self._current(remote)
            changed = current != data
            if changed:
                self.device.upload_bytes(remote, data)
            placed.append(Placed(remote, changed, digest(data)))
        if any(p.changed for p in placed):
            time.sleep(PFS3_SETTLE_S)
            self._verify(files, "before the reboot")
            if reboot:
                self.device.reboot()
                self._verify(files, "after the reboot")
        for p in placed:
            p.version = self.device.version_of(p.remote)
        return placed

    def restore(self, remotes: list[str]) -> list[Placed]:
        """The machine as it was for @remotes: its originals back, what it never had deleted; a reboot."""
        files, absent = {}, []
        for remote in remotes:
            path = self.backup_dir / backup_name(remote)
            if path.exists():
                files[remote] = path.read_bytes()
            elif (self.backup_dir / (backup_name(remote) + ".absent")).exists():
                absent.append(remote)
        if absent:
            self.device.delete(*absent)
        return self.install(files)

    def _current(self, remote: str) -> bytes | None:
        try:
            return self.device.download(remote)
        except self.device.mod.AmigaError:
            return None

    def _verify(self, files: dict[str, bytes], when: str) -> None:
        for remote, data in files.items():
            if self._current(remote) != data:
                raise HarnessError(f"{remote} did not read back {when}")
