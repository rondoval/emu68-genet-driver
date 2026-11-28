# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
build.py - what the suite tests: where the artefacts of the stack's build of
this component are (its flavour is emu68test's BuildInfo).

The stack builds genet into <stack>/build/emu68-genet-driver-netdev/ and
installs the shipped files into <stack>/install/. The suite takes the driver,
genet-stats and lwip-amiga's netdev-stats from the install tree, GenetTest and
the logic programs from the component's build dir, lwip-amiga's sockbench from
its build dir (a developer tool, never installed); bsdsocktest comes from its
own checkout.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .suite import COMPONENT

PERF_FORMAT = b": n=%lu sum=%luus"  # perf.h's report line: in profile (and trace) builds only


def profiled(driver: bytes) -> bool:
    """Whether a genet.device binary reports its perf slots (built with genet in PROFILE, or traced)."""
    return PERF_FORMAT in driver


@dataclass
class Artefacts:
    """Where each file the suite pushes comes from."""

    install_dir: Path
    build_dir: Path
    bsdsocktest: Path
    driver_file: Path | None = None  # another genet.device to test (--driver), e.g. a reference build

    @property
    def component_build(self) -> Path:
        """This component's own build dir inside the stack build."""
        return self.build_dir / COMPONENT

    @property
    def driver(self) -> Path:
        """genet.device under test: --driver's, else the one the stack installs."""
        return self.driver_file or self.install_dir / "DEVS" / "Networks" / "genet.device"

    @property
    def genet_stats(self) -> Path:
        """genet-stats as the stack installs it."""
        return self.install_dir / "C" / "genet-stats"

    @property
    def genettest(self) -> Path:
        """The rig, built and never installed."""
        return self.component_build / "tests" / "GenetTest"

    @property
    def sockbench(self) -> Path:
        """lwip-amiga's sockbench, a developer benchmark the stack builds and never installs."""
        return self.build_dir / "lwip-amiga" / "src" / "sockbench"

    @property
    def netdev_stats(self) -> Path:
        """lwip-amiga's netdev-stats, as the stack installs it."""
        return self.install_dir / "C" / "netdev-stats"

    def missing(self, *paths: Path) -> list[str]:
        """The @paths that are not there, as strings."""
        return [str(p) for p in paths if not Path(p).is_file()]
