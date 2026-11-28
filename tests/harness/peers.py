# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
peers.py - the PC side of a round: lwip-amiga's tcp-bench-peer.py (ports
5001-5003, sockbench's partner), bsdsocktest's helper (8700-8704) and the
multicast sender for GenetTest MCAST.

They run on Windows, not in WSL: WSL's own address is NAT'd, out of the
Amiga's reach, and its NAT does not pass LAN multicast. The suite asks Windows
which ports listen, starts the peers that do not (Start-Process, minimised,
logs in the user's profile), and stops at the end what it started.
"""
from __future__ import annotations

import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path

BENCH_PORT = 5001
HELPER_PORT = 8700
START_WAIT_S = 30

HERE = Path(__file__).resolve().parent


def powershell(command: str, timeout: float = 30) -> str:
    """@command run by Windows PowerShell; its output."""
    done = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
                          capture_output=True, text=True, timeout=timeout)
    return done.stdout


def windows_path(path: Path) -> str:
    """@path as Windows names it (\\\\wsl.localhost\\...)."""
    return subprocess.run(["wslpath", "-w", str(path)], capture_output=True, text=True, check=True).stdout.strip()


def listener(port: int) -> int | None:
    """The Windows process listening on TCP @port; None when nothing does."""
    out = powershell(f"(Get-NetTCPConnection -State Listen -LocalPort {port} -ErrorAction SilentlyContinue)"
                     ".OwningProcess | Select-Object -First 1")
    out = out.strip()
    return int(out) if out.isdigit() else None


@dataclass
class Peer:
    """One PC program the round needs, known by the port it listens on."""

    name: str
    script: Path
    port: int


@dataclass
class Peers:
    """The PC side of the session."""

    peers: list[Peer]
    started: list[int] = field(default_factory=list)

    def ensure(self) -> list[str]:
        """Each peer listening, started where it was not: what it did, in plain words."""
        notes = []
        for peer in self.peers:
            if listener(peer.port) is not None:
                notes.append(f"{peer.name}: already listening on {peer.port}")
                continue
            if not peer.script.is_file():
                raise FileNotFoundError(f"{peer.name}: {peer.script} not found")
            self._start(peer)
            pid = self._wait(peer)
            self.started.append(pid)
            notes.append(f"{peer.name}: started (pid {pid})")
        return notes

    def _start(self, peer: Peer) -> None:
        log = f"$env:USERPROFILE\\genet-suite-{peer.name}"
        command = (f"Start-Process py -ArgumentList '-u','{windows_path(peer.script)}' -WindowStyle Minimized "
                   f"-RedirectStandardOutput {log}.log -RedirectStandardError {log}.err")
        subprocess.Popen(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def _wait(self, peer: Peer) -> int:
        t0 = time.time()
        while time.time() - t0 < START_WAIT_S:
            pid = listener(peer.port)
            if pid is not None:
                return pid
            time.sleep(2)
        raise TimeoutError(f"{peer.name} did not listen on {peer.port} within {START_WAIT_S} s")

    def stop(self) -> None:
        """Stop what ensure() started."""
        for pid in self.started:
            powershell(f"Stop-Process -Id {pid} -ErrorAction SilentlyContinue")
        self.started.clear()


def mcast_sender(interface: str, seconds: int) -> subprocess.Popen:
    """mcast_send.ps1 on Windows for @seconds from the PC's LAN @interface (its address)."""
    script = windows_path(HERE / "mcast_send.ps1")
    return subprocess.Popen(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script,
                             "-Interface", interface, "-Seconds", str(seconds)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
