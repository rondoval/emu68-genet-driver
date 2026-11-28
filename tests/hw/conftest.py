# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
hw/conftest.py - the hardware tiers' fixtures: the Amiga (amiga), the build
installed on it (installer, installed), then with lwip-amiga running and the
tools in RAM:gt (device), the PC's peers (peer), genet's
interface in the personality a test's marker names (interface), and the order
the tests run in: the install check first, then the smoke tests, then the rest;
within each, grouped by personality, so that each switch happens once.

    @pytest.mark.personality(NETDEV)              genet up as TYPE=NETDEV first
    @pytest.mark.personality(SANA2, last=True)    ... and after the others of its group
                                                  (a test that ends in another personality)
    @pytest.mark.personality(NETDEV, boot=True)   ... on a boot of its own, before the group's first
                                                  test (the perf tier)
"""
from __future__ import annotations

import os
from pathlib import Path

import pytest

from emu68test import plugin
from emu68test.baselines import Baselines
from emu68test.install import Installer
from emu68test.machine import Device

from harness import stack, tools
from harness.interface import Interface
from harness.peers import BENCH_PORT, HELPER_PORT, Peer, Peers
from harness.suite import BSDSOCKET, DRIVER, GENET_STATS, RAM, TESTS

EXIT_NOT_READY = 4      # pytest.exit()'s code when the build, the stack or the peer is not what the tiers need
RANK = {stack.AUTO: 1, stack.NETDEV: 2, stack.SANA2: 3}
NEEDED = ("C:AddNetInterface", "C:RemoveNetInterface", "C:netinfo")   # beside what the harness itself needs


def pytest_collection_modifyitems(config, items):
    """The install check, then smoke, then the rest; by personality within each (a stable sort)."""
    def order(item):
        marker = item.get_closest_marker("personality")
        kind = marker.args[0] if marker else None
        tier = 0 if item.get_closest_marker("install") else 1 if item.get_closest_marker("smoke") else 2
        return tier, RANK.get(kind, 0), bool(marker and marker.kwargs.get("last"))
    items.sort(key=order)


def _files(request, artefacts) -> dict[str, bytes]:
    """What the session installs: remote path -> content."""
    missing = artefacts.missing(artefacts.driver, artefacts.genet_stats, artefacts.genettest, artefacts.sockbench,
                                artefacts.netdev_stats)
    if missing:
        pytest.exit("not built: " + ", ".join(missing) + " (build the stack first)", returncode=EXIT_NOT_READY)
    files = {DRIVER: artefacts.driver.read_bytes(), GENET_STATS: artefacts.genet_stats.read_bytes()}
    if request.config.getoption("--bsdsocket"):
        files[BSDSOCKET] = Path(request.config.getoption("--bsdsocket")).read_bytes()
    return files


def _placed(request, installer: Installer, files: dict[str, bytes]) -> list[str]:
    """The files in place (or checked to be, with --no-install): the report's header lines."""
    if request.config.getoption("--no-install"):
        differ = installer.differing(files)
        if differ:
            pytest.exit("installed files differ from the build (run without --no-install): " + ", ".join(differ),
                        returncode=plugin.EXIT_ENVIRONMENT)
        return ["Installed files checked against the build: " + ", ".join(files)]
    placed = installer.install(files)
    lines = [f"- {p.remote}: {'replaced' if p.changed else 'already in place'}, {p.digest}, "
             f"{' '.join(p.version.split()) or '?'}" for p in placed]
    return ["Installed on the Amiga (the machine's own files kept in tests/.machine/):"] + lines


@pytest.fixture(scope="session")
def amiga(request) -> Device:
    """The Amiga, reachable and checked. Only for run.sh hw, after the machine's owner agreed to the round."""
    if not os.environ.get("GENET_HW"):
        pytest.skip("a hardware tier: run.sh hw --tier <tier>, after the machine's owner agreed to the round")
    found = plugin.connect(request)
    rc, text = found.exec("List >NIL: " + " ".join(NEEDED), timeout=15)
    if rc != 0:
        pytest.exit(f"C: is not as expected (Assign C: EXISTS): {text.strip()}", returncode=plugin.EXIT_ENVIRONMENT)
    yield found
    found.exec(f"Delete >NIL: {RAM} ALL QUIET", timeout=60)


@pytest.fixture(scope="session")
def installer(request, amiga) -> Installer:
    """The install on this machine, its own files kept in tests/.machine/<server>/."""
    return Installer(amiga, TESTS / ".machine" / request.config.getoption("--server"))


@pytest.fixture(scope="session")
def installed(request, amiga, installer, artefacts) -> dict[str, bytes]:
    """The build under test in place (a reboot when a file changed): remote path -> content. With --restore
    the machine's own files go back instead, and the run ends."""
    report = plugin.session(request.config).report
    if request.config.getoption("--restore"):
        placed = installer.restore([DRIVER, GENET_STATS, BSDSOCKET])
        report.header += [f"- {p.remote}: the machine's own back, {p.version or '?'}" for p in placed]
        pytest.exit("the machine's own files are back: " + ", ".join(p.remote for p in placed), returncode=0)
    files = _files(request, artefacts)
    report.header += _placed(request, installer, files)
    return files


@pytest.fixture(scope="session")
def device(request, amiga, installed, artefacts) -> Device:
    """The Amiga with the build installed, lwip-amiga running and the tools in RAM:gt."""
    report = plugin.session(request.config).report
    version = amiga.exec("Version bsdsocket.library", timeout=20)[1].strip()
    report.header.append(f"Stack: {version}")
    if stack.stack_of(version) != stack.LWIP:
        pytest.exit(f"these tiers need lwip-amiga, the machine runs {version!r} (the roadshow tier is for Roadshow)",
                    returncode=EXIT_NOT_READY)
    tools.push(amiga, artefacts)
    return amiga


@pytest.fixture(scope="session")
def peer(request, device) -> str:
    """The PC's LAN address, with tcp-bench-peer.py and bsdsocktest's helper listening there."""
    opt = request.config.getoption
    address = opt("--peer")
    if not address:
        pytest.exit("the hardware tiers need the PC's LAN address: --peer or GENET_PEER", returncode=EXIT_NOT_READY)
    peers = Peers([Peer("bench-peer", Path(opt("--peer-script")), BENCH_PORT),
                   Peer("bst-helper", Path(opt("--bst-helper")), HELPER_PORT)])
    plugin.session(request.config).report.header.append("PC peers: " + "; ".join(peers.ensure()))
    yield address
    peers.stop()


@pytest.fixture(scope="session")
def interface(request, device, installer) -> Interface:
    """genet's interface; the machine's own file and prefs back in use at the end."""
    iface = Interface(device, installer)
    report = plugin.session(request.config).report
    report.header.append(f"Interface: the machine's DEVS:NetInterfaces/genet has TYPE={iface.machine_type}, "
                         f"ENV:genet.prefs {'present' if iface.prefs is not None else 'absent'}")
    yield iface
    report.header.append(iface.restore())


@pytest.fixture(scope="session")
def baselines(request) -> Baselines:
    """This machine's baselines (none judged with --seeding); what the run measured goes to measured.json at
    the session's end, every run's values to each test's values.json (run.sh seed)."""
    opt = request.config.getoption
    if opt("--seeding"):
        found = Baselines(opt("--server"), seeding=True)    # baselines.json is what the seeding will replace
    else:
        found = Baselines.load(TESTS / "baselines.json", opt("--server"))
    plugin.session(request.config).baselines = found
    return found


_booted: set[str] = set()


@pytest.fixture(autouse=True)
def personality(request, test_dir) -> str | None:
    """genet up in the personality the test's marker names (none: as it is); with boot=True, on a boot of
    its own, once per personality in the session."""
    marker = request.node.get_closest_marker("personality")
    if marker is None:
        return None
    kind = marker.args[0]
    iface = request.getfixturevalue("interface")
    if marker.kwargs.get("boot") and kind not in _booted:
        iface.fresh_boot(request.getfixturevalue("artefacts"))
        _booted.add(kind)
    iface.use(kind)
    return kind
