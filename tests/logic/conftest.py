# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
conftest.py - the logic tier's fixtures: its programs, from the stack build,
run under vamos (harness/vamos.py).
"""
from __future__ import annotations

from pathlib import Path
from typing import Callable

import pytest

from harness import vamos


@pytest.fixture(scope="session")
def logic_program(request) -> Callable[[str], vamos.Program]:
    """A logic program by name: skipped without vamos or a build, failed when older than its sources."""
    why = vamos.unavailable()
    if why is not None:
        pytest.skip(why)
    where = vamos.programs_dir(Path(request.config.getoption("--build-dir")))
    checked: dict[str, vamos.Program] = {}

    def program(name: str) -> vamos.Program:
        if name in checked:
            return checked[name]
        found = vamos.Program(where / name)
        if not found.path.is_file():
            pytest.skip(f"{name} is not built in {where} (scripts/docker-build.sh from the stack root)")
        changed = found.stale()
        if changed is not None:
            pytest.fail(f"{name} is older than {changed}: rebuild (scripts/docker-build.sh from the stack root)")
        checked[name] = found
        return found

    return program
