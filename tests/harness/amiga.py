# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
amiga.py - the machine under test, reached through amimcp's Python client (its
server/amiga.py): commands, program runs, uploads with read-back, background
jobs, detached scripts, reboots.

The agent serves nothing while a command is within its deadline, so anything
longer than a minute is started with Run >NIL: and polled (Job). The agent
also talks over the interface under test: whatever takes the interface down
(RemoveNetInterface, a prefs change, S2_OFFLINE) runs as a detached script
(script()) that writes a RAM: log ending in a marker, and the host polls that
log until the agent answers again with the marker in it.

Everything temporary lives in RAM:gt: the programs in RAM:gt, their outputs in
RAM:gt/out. A reboot empties RAM:, so the device forgets what it uploaded.
"""
from __future__ import annotations

import json
import os
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from .results import HarnessError, Result, parse

RAM = "RAM:gt"
OUT = f"{RAM}/out"
PIECE = 12 << 20  # one agent frame carries at most 16 MiB
KV_TOOLS = ("GenetTest",)  # the programs that speak the grammar


def amiga_file_part(path: str) -> str:
    """The last component of an AmigaDOS path, after its volume and drawers (as dos FilePart() does)."""
    return re.split(r"[/:]", path)[-1]


class Unreachable(HarnessError):
    """The agent did not answer, even after waiting for it: the run cannot go on."""


class ToolTimeout(AssertionError):
    """A program ran past its deadline and was broken; the message carries what it had printed."""


def client(amimcp: str, server: str):
    """amimcp's module and a connected client for @server (address and token from AMIGA_*, else ~/.claude.json)."""
    sys.path.insert(0, os.path.join(amimcp, "server"))
    import amiga  # noqa: E402

    host = os.environ.get("AMIGA_HOST")
    token = os.environ.get("AMIGA_TOKEN", "")
    port = int(os.environ.get("AMIGA_PORT", "7846"))
    if not host:
        env = json.load(open(os.path.expanduser("~/.claude.json")))["mcpServers"][server]["env"]
        host, token, port = env["AMIGA_HOST"], env.get("AMIGA_TOKEN", ""), int(env.get("AMIGA_PORT", "7846"))
    return amiga, amiga.Amiga(host=host, port=port, token=token, timeout=120.0)


@dataclass
class Job:
    """A program started with Run >NIL:, its output in a RAM: file."""

    device: "Device"
    tool: str
    out: str
    started: float = field(default_factory=time.time)

    def output(self) -> str:
        """What the program printed; '' while it runs (the shell's redirection holds the file)."""
        return self.device.download_text(self.out)

    def done(self) -> bool:
        """Whether the program has ended: its verdict is in the file (a kv program), or its process is gone."""
        if self.tool in KV_TOOLS:
            return bool(re.search(r"^(PASS|FAIL)\s*$", self.output(), re.M))
        return not self.device.procs(self.tool)

    def wait(self, timeout: float) -> Result | str:
        """Poll until the program ends: its Result (a kv program) or its text. ToolTimeout after @timeout."""
        t0 = time.time()
        while not self.done():
            if time.time() - t0 > timeout:
                self.stop()
                raise ToolTimeout(f"{self.tool} still running after {timeout} s:\n{self.output()[-1200:]}")
            time.sleep(2)
        text = self.output()
        self.device.keep(self.out, text)
        return parse(text, 0 if re.search(r"^PASS\s*$", text, re.M) else 5) if self.tool in KV_TOOLS else text

    def stop(self, timeout: float = 20) -> None:
        """Ctrl-C to the program (its process found by its command name) and wait for it to end."""
        self.device.break_all(self.tool, timeout)


class Device:
    """One Amiga, through the agent."""

    def __init__(self, mod, ami, rundir: Path):
        self.mod = mod
        self.ami = ami
        self.rundir = Path(rundir)
        self.ram_known: set[str] = set()
        self.test_name = "session"
        self.artifacts: Path | None = None
        self.runs = 0
        self.installed: dict[str, bytes] = {}  # what the session installed: remote path -> content
        self.installer = None                 # install.Installer, set by the session

    # -- commands

    def exec(self, cmd: str, timeout: float = 60) -> tuple[int, str]:
        """An AmigaDOS command whose output the agent collects; once more after the agent comes back from a stall."""
        try:
            return self.ami.exec_command(cmd, timeout=timeout)
        except (self.mod.AmigaUnreachable, OSError):
            self.wait_agent(180)
            try:
                return self.ami.exec_command(cmd, timeout=timeout)
            except (self.mod.AmigaUnreachable, OSError) as e:
                raise Unreachable(f"the agent is unreachable ({e})") from None

    def _out_name(self, tool: str) -> str:
        """The next output file of this test: RAM:gt/out/<test>-<n>-<tool>.txt, <tool> being the command's
        AmigaDOS file part (C:genet-stats -> genet-stats: a second colon makes no file name)."""
        self.runs += 1
        safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", self.test_name)[:40]
        return f"{OUT}/{safe}-{self.runs}-{amiga_file_part(tool)}.txt"

    def keep(self, remote: str, text: str) -> None:
        """A program's output into the test's artifact dir."""
        if self.artifacts is not None:
            (self.artifacts / remote.split("/")[-1]).write_text(text, encoding="latin-1")

    def run_text(self, command: str, args: str = "", timeout: float = 120) -> tuple[int, str]:
        """@command run to its end, its output redirected to a RAM: file and read back: (rc, text)."""
        out = self._out_name(command)
        try:
            rc, _ = self.exec(f"{command} >{out} {args}", timeout=timeout)
        except self.mod.AmigaError as e:
            if "still running" not in str(e):
                raise
            self.ami.break_command()
            self.reap()
            text = self.download_text(out)
            self.keep(out, text)
            raise ToolTimeout(f"{command} still running after {timeout} s (broken):\n{text[-1200:]}") from None
        text = self.download_text(out)
        self.keep(out, text)
        return rc, text

    def run(self, command: str, args: str = "", timeout: float = 120) -> Result:
        """A kv program run to its end, its output parsed."""
        rc, text = self.run_text(command, args, timeout)
        return parse(text, rc)

    def start(self, command: str, args: str = "") -> Job:
        """@command started with Run >NIL:, its output in a RAM: file: a Job to wait for or stop."""
        out = self._out_name(command)
        self.exec(f"Run >NIL: {command} >{out} {args}", timeout=15)
        return Job(self, amiga_file_part(command), out)

    def reap(self) -> None:
        """Wait for a broken command to be reaped: the next EXEC is refused while it lives."""
        for _ in range(30):
            time.sleep(1)
            try:
                self.ami.exec_command("Echo >NIL: reaped", timeout=5)
                return
            except self.mod.AmigaError:
                continue

    def procs(self, name: str) -> list[int]:
        """The CLI numbers of the processes whose command is @name, by any path (Status FULL)."""
        _, text = self.exec("Status FULL", timeout=15)
        found = []
        for num, command in re.findall(r"^Process\s+(\d+):.*Loaded as command: (.*)$", text, re.M):
            if re.split(r"[/:]", command.strip())[-1].lower() == name.lower():
                found.append(int(num))
        return found

    def break_all(self, name: str, timeout: float = 15) -> None:
        """Ctrl-C to every process of @name and wait for them to go."""
        for n in self.procs(name):
            self.exec(f"Break {n} C", timeout=10)
        t0 = time.time()
        while self.procs(name) and time.time() - t0 < timeout:
            time.sleep(1)

    def script(self, name: str, body: str, marker: str, limit: float) -> str:
        """@body as RAM:gt/<name>, run detached; its log RAM:gt/<name>.log polled until it holds @marker.

        The script takes the interface the agent talks over down and up again,
        so the agent is unreachable part of the time; polling waits it out.
        The script must append everything to the log and echo @marker last.
        """
        remote, log = f"{RAM}/{name}", f"{RAM}/{name}.log"
        self.upload_bytes(remote, body.encode("latin-1"))
        self.exec(f"Delete >NIL: {log} QUIET", timeout=20)
        self.exec(f"Run >NIL: Execute {remote}", timeout=20)
        t0, text = time.time(), ""
        while time.time() - t0 < limit:
            time.sleep(5)
            try:
                text = self.ami.read_file(log).decode("latin-1", "replace")
            except (self.mod.AmigaError, self.mod.AmigaUnreachable, OSError):
                continue
            if marker in text:
                break
        self.keep(log, text)
        if marker not in text:
            raise ToolTimeout(f"{name} did not reach {marker!r} within {limit} s:\n{text[-1500:]}")
        return text

    # -- files

    def _retry(self, fn):
        """@fn once more after the agent answers again: a transfer that stalls once costs a retry, not the test."""
        try:
            return fn()
        except (self.mod.AmigaUnreachable, OSError):
            self.wait_agent(180)
            return fn()

    def download(self, remote: str) -> bytes:
        """A file from the Amiga, in 4 MiB pieces."""
        def once():
            data = bytearray()
            piece = 4 << 20
            while True:
                chunk = self.ami.read_range(remote, len(data), piece)
                data += chunk
                if len(chunk) < piece:
                    return bytes(data)
        return self._retry(once)

    def download_text(self, remote: str) -> str:
        """A text file's content, '' when it does not exist or is held open."""
        try:
            return self.download(remote).decode("latin-1", "replace")
        except self.mod.AmigaError:
            return ""

    def upload_bytes(self, remote: str, data: bytes) -> None:
        """@data to @remote, read back and compared."""
        if len(data) > PIECE:
            raise HarnessError(f"{remote}: {len(data)} bytes is more than one agent frame carries")
        self._retry(lambda: self.ami.write_file(remote, data))
        if self.download(remote) != data:
            raise HarnessError(f"{remote}: read back differs")

    def upload_once(self, local: Path, remote: str) -> None:
        """A local file to RAM:, unless it went up since the last boot."""
        if remote not in self.ram_known:
            self.upload_bytes(remote, Path(local).read_bytes())
            self.ram_known.add(remote)

    def delete(self, *remotes: str) -> None:
        """Files on the Amiga, quietly."""
        if remotes:
            self.exec(f"Delete >NIL: {' '.join(remotes)} QUIET", timeout=30)
        for remote in remotes:
            self.ram_known.discard(remote)

    def version_of(self, remote: str) -> str:
        """The version Version FULL reports for a file, '' when none."""
        rc, text = self.exec(f"Version {remote} FULL", timeout=20)
        return text.strip() if rc == 0 else ""

    # -- the machine

    def ram_dirs(self) -> None:
        """RAM:gt and its out drawer."""
        for d in (RAM, OUT):
            self.exec(f"MakeDir >NIL: {d}", timeout=15)

    def reboot(self) -> None:
        """C:Reboot (the 68k only), wait for the agent and the network, forget RAM:."""
        mark = "RAM:gt-reboot-mark"
        self.ami.write_file(mark, b"1")
        try:
            self.ami.exec_command("C:Reboot", timeout=3)
            raise HarnessError("C:Reboot returned")
        except HarnessError:
            raise
        except Exception:
            pass  # the agent goes down with the reboot
        t0 = time.time()
        while time.time() - t0 < 60:
            try:
                self.ami.read_file(mark)
                time.sleep(1)
            except Exception:
                break
        else:
            raise HarnessError("C:Reboot: the Amiga never rebooted")
        self.wait_agent(300)
        time.sleep(60)  # AmiKit's start-up: the agent refuses connects for a minute right after it answers
        self.wait_agent(120)
        self.ram_known.clear()
        self.ram_dirs()

    def wait_agent(self, timeout: float) -> None:
        """Until the agent answers a ping."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                self.ami.ping()
                return
            except Exception:
                time.sleep(3)
        raise Unreachable(f"the agent did not come back in {timeout} s")

    def wait_reset(self, timeout: float) -> bool:
        """Until someone resets the machine and the agent answers again: TRUE when that happened.

        Only a reset empties RAM:; an agent that does not answer proves
        nothing, it may be stalled.
        """
        mark = "RAM:gt-reset-mark"
        self.ami.write_file(mark, b"1")
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                self.ami.read_file(mark)
            except self.mod.AmigaError:  # the agent answered, and the mark is gone
                time.sleep(60)
                self.ram_known.clear()
                self.ram_dirs()
                return True
            except (self.mod.AmigaUnreachable, OSError):
                pass
            time.sleep(2)
        return False
