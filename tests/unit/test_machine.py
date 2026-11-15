# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""The interface file, the stack, the detached scripts, the build flavour and the install, without an Amiga."""
from pathlib import Path

import pytest

from harness import install, stack
from harness.build import BuildInfo
from harness.results import HarnessError

pytestmark = pytest.mark.unit

SAMPLES = Path(__file__).parent / "samples"
INTERFACE = "device=genet.device\nunit=0\nconfigure=dhcp\n"


def test_interface_type_defaults_to_auto():
    assert stack.interface_type(INTERFACE) == "AUTO"
    assert stack.interface_type(INTERFACE + "type = sana2\n") == "SANA2"


def test_set_type_replaces_the_one_line():
    text = stack.set_type(INTERFACE + "TYPE=NETDEV\nTYPE=AUTO\n", "SANA2")
    assert text.count("TYPE=") == 1 and stack.interface_type(text) == "SANA2"
    assert stack.set_type(INTERFACE, "NETDEV").endswith("TYPE=NETDEV\n")


def test_netinfo():
    entry = stack.netinfo((SAMPLES / "netinfo.txt").read_text())
    assert entry == stack.Netinfo(True, "genet.device", 0, "192.0.2.101")
    assert stack.netinfo("no interfaces\n") is None


def test_which_stack():
    assert stack.stack_of("bsdsocket.library 4.109 (10.11.26)") == stack.LWIP
    assert stack.stack_of("bsdsocket.library 4.364 (2.2.24)") == stack.ROADSHOW
    assert stack.stack_of("object not found") is None


def test_script_logs_every_step_and_ends_in_its_marker():
    body = stack.Script("switch").remove().prefs("RAM:gt/p1").add().text()
    lines = body.splitlines()
    assert lines[0] == "FailAt 21" and lines[-1] == 'Echo >>RAM:gt/switch.log "SWITCH DONE"'
    assert "C:RemoveNetInterface >>RAM:gt/switch.log genet FORCE" in lines
    assert "Copy RAM:gt/p1 ENV:genet.prefs QUIET" in lines
    for line in lines:  # one redirection, right after the command
        assert line.count(">") <= 2


def test_rc_of():
    log = "switch start\nremove rc 0\nadd rc 10\n"
    assert (stack.rc_of(log, "remove"), stack.rc_of(log, "add"), stack.rc_of(log, "gone")) == (0, 10, None)


def test_build_info(tmp_path):
    (tmp_path / "CMakeCache.txt").write_text("EMU68_DEBUG_BACKEND:STRING=pistorm\nEMU68_TIER:STRING=off\n"
                                             "EMU68_PROFILE:STRING=emu68-genet-driver-netdev;lwip-amiga\n"
                                             "EMU68_FORCE_LVO_CACHE_OPS:BOOL=OFF\n")
    info = BuildInfo.read(tmp_path)
    assert info.profile and info.flavour == "rangeops"
    assert not BuildInfo.read(tmp_path / "none").profile


class FakeDevice:
    """Files in a dict, a reboot that only counts."""

    class mod:
        class AmigaError(Exception):
            pass

    def __init__(self, files):
        self.files = dict(files)
        self.reboots = 0
        self.lose_writes = False

    def download(self, remote):
        if remote not in self.files:
            raise self.mod.AmigaError("object not found")
        return self.files[remote]

    def upload_bytes(self, remote, data):
        if not self.lose_writes:
            self.files[remote] = data

    def delete(self, *remotes):
        for remote in remotes:
            self.files.pop(remote, None)

    def reboot(self):
        self.reboots += 1

    def version_of(self, remote):
        return f"{remote} 5.0"


@pytest.fixture
def quick(monkeypatch):
    monkeypatch.setattr(install, "PFS3_SETTLE_S", 0)


def test_install_keeps_the_originals_once(tmp_path, quick):
    device = FakeDevice({install.DRIVER: b"4.4"})
    installer = install.Installer(device, tmp_path)
    placed = installer.install({install.DRIVER: b"5.0", install.GENET_STATS: b"stats"})
    assert [p.changed for p in placed] == [True, True] and device.reboots == 1
    assert installer.original(install.DRIVER) == b"4.4" and installer.original(install.GENET_STATS) is None
    installer.install({install.DRIVER: b"5.1"})
    assert installer.original(install.DRIVER) == b"4.4"


def test_install_of_what_is_there_does_not_reboot(tmp_path, quick):
    device = FakeDevice({install.DRIVER: b"5.0"})
    placed = install.Installer(device, tmp_path).install({install.DRIVER: b"5.0"})
    assert not placed[0].changed and device.reboots == 0


def test_install_refuses_to_reboot_over_a_lost_write(tmp_path, quick):
    device = FakeDevice({install.DRIVER: b"4.4"})
    device.lose_writes = True
    with pytest.raises(HarnessError, match="did not read back before the reboot"):
        install.Installer(device, tmp_path).install({install.DRIVER: b"5.0"})
    assert device.reboots == 0


def test_restore_puts_back_what_the_machine_had(tmp_path, quick):
    device = FakeDevice({install.DRIVER: b"4.4"})
    installer = install.Installer(device, tmp_path)
    installer.install({install.DRIVER: b"5.0", install.GENET_STATS: b"stats"})
    installer.restore([install.DRIVER, install.GENET_STATS])
    assert device.files[install.DRIVER] == b"4.4" and device.reboots == 2
    assert install.GENET_STATS not in device.files


def test_a_failed_add_puts_the_machine_interface_back():
    lines = stack.Script("s").add("add", "RAM:gt/sana2/genet").text().splitlines()
    at = lines.index("C:AddNetInterface >>RAM:gt/s.log RAM:gt/sana2/genet TIMEOUT=30")
    assert lines[at + 1:at + 4] == ["Set addrc $RC", 'Echo >>RAM:gt/s.log "add rc $addrc"',
                                    "If $addrc GE 10 VAL"]
    assert not any("_" in word for line in lines for word in line.split() if word.startswith("$"))
    assert "  C:AddNetInterface >>RAM:gt/s.log genet TIMEOUT=30" in lines and "EndIf" in lines
    assert not stack.rescued("add rc 0\n") and stack.rescued("add rc 10\nrescue rc 0\n")


def test_sections_of_a_script_log():
    log = "s start\n== first\ntotal: 1 bytes, 1.0 Mb/s\nfirst rc 0\n== second\nnothing\n"
    assert stack.section(log, "first") == "total: 1 bytes, 1.0 Mb/s\nfirst rc 0\n"
    assert stack.section(log, "second") == "nothing\n"
    assert stack.section(log, "third") is None
    body = stack.Script("s").bench("one", "rx 1.2.3.4 1 3").program("q", "RAM:gt/GenetTest", "QUERY",
                                                                     "RAM:gt/out/q.txt").text()
    assert 'Echo >>RAM:gt/s.log "== one"' in body and "RAM:gt/sockbench >>RAM:gt/s.log rx 1.2.3.4 1 3" in body
    assert "RAM:gt/GenetTest >RAM:gt/out/q.txt QUERY" in body


class ScriptedDevice(FakeDevice):
    """A FakeDevice whose detached scripts and GenetTest runs answer from queues."""

    def __init__(self, files, logs, holders):
        super().__init__(files)
        self.logs, self.holders, self.scripts = list(logs), list(holders), []

    def exec(self, cmd, timeout=60):
        return 0, ""

    def script(self, name, body, marker, limit):
        self.scripts.append(name)
        return self.logs.pop(0)

    def run(self, command, args="", timeout=120):
        from harness.results import parse
        return parse(f"format=1\nholder={self.holders.pop(0)}\nPASS\n", 0)


def test_a_switch_runs_once_and_a_failed_one_is_not_tried_again():
    from harness.interface import Interface, SwitchFailed
    device = ScriptedDevice({install.INTERFACE: INTERFACE.encode()},
                            ["add rc 0\n", "add rc 10\nrescue rc 0\n"], ["sana2", "netdev"])
    iface = Interface(device)
    assert iface.machine_type == "AUTO" and iface.prefs is None
    assert device.files[iface.path("SANA2")].decode().endswith("TYPE=SANA2\n")
    iface.use("SANA2")
    iface.use("SANA2")
    assert device.scripts == ["use-sana2"]
    with pytest.raises(SwitchFailed, match="rc 10, the unit held by netdev.*put back"):
        iface.use("NETDEV")
    with pytest.raises(SwitchFailed):
        iface.use("NETDEV")
    assert device.scripts == ["use-sana2", "use-netdev"] and iface.current is None


def test_output_files_are_named_by_the_command_s_file_part(tmp_path):
    from harness.amiga import Device, amiga_file_part
    assert [amiga_file_part(c) for c in ("C:genet-stats", "RAM:gt/GenetTest", "Execute")] == \
        ["genet-stats", "GenetTest", "Execute"]
    device = Device(None, None, tmp_path)
    device.test_name = "test_genet_stats[netdev]"
    out = device._out_name("C:genet-stats")
    assert out == "RAM:gt/out/test_genet_stats_netdev_-1-genet-stats.txt" and out.count(":") == 1


def test_a_fresh_boot_installs_the_driver_or_reboots_and_puts_the_copies_back(monkeypatch):
    import harness.interface as interface_module
    installs, pushed = [], []
    monkeypatch.setattr(interface_module, "push", lambda dev, artefacts: pushed.append(artefacts))

    class FakeInstaller:
        def install(self, files):
            installs.append(files)
            return [install.Placed(r, files[r] != device.files.get(r), "d") for r in files]

    device = ScriptedDevice({install.INTERFACE: INTERFACE.encode(), install.DRIVER: b"5.0"}, ["add rc 0\n"],
                            ["netdev"])
    device.installer = FakeInstaller()
    iface = interface_module.Interface(device)
    iface.failed["SANA2"] = "earlier"
    iface.fresh_boot("ART", b"4.4")
    assert installs == [{install.DRIVER: b"4.4"}] and device.reboots == 0
    iface.fresh_boot("ART", b"5.0")
    assert device.reboots == 1 and pushed == ["ART", "ART"]
    assert iface.current == "machine" and not iface.failed
