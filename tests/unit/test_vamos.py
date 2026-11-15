# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""The logic tier's harness (harness/vamos.py): vamos's command line, staleness from the compiler's
depfiles, and the step facts."""
import os

import pytest

from harness import results, vamos

pytestmark = pytest.mark.unit

STEPS = """format=1
command=mcast
step.0.range.0.lower=01:00:5e:00:00:01
step.0.range.0.uses=2
step.0.range.1.lower=01:00:5e:00:00:05
step.0.slot.1=01:00:5e:00:00:02
step.0.slot.0=01:00:5e:00:00:01
step.0.slots=2
step.1.slots=0
steps=2
checks=0
checks_failed=0
PASS
"""


def test_the_command_line_names_volumes_assigns_and_the_program(tmp_path):
    program = vamos.Program(tmp_path / "LogicPrefs")
    command = program.command(["a=1"], {"genetenv": tmp_path}, {"ENV": "genetenv:"})
    assert command == ["vamos", "-q", "-C", "68040", "-m", str(vamos.RAM_KIB), "-V", f"genetenv:{tmp_path.resolve()}", "-a", "ENV:genetenv:",
                       "LogicPrefs", "a=1"]


def test_the_steps_come_apart_into_lists_and_groups():
    steps = vamos.Run(results.parse(STEPS, 0)).steps()
    assert len(steps) == 2
    assert vamos.listed(steps[0], "slot") == ["01:00:5e:00:00:01", "01:00:5e:00:00:02"]
    assert vamos.grouped(steps[0], "range") == [{"lower": "01:00:5e:00:00:01", "uses": "2"},
                                                {"lower": "01:00:5e:00:00:05"}]
    assert vamos.listed(steps[1], "slot") == [] and steps[1]["slots"] == "0"


def test_a_step_the_program_refused_is_a_harness_error():
    text = "format=1\ncommand=mcast\nfail=step 0: frob is not a step\nchecks=0\nchecks_failed=0\nFAIL\n"
    with pytest.raises(results.HarnessError, match="frob is not a step"):
        vamos.Run(results.parse(text, 10)).steps()


def test_the_depfiles_name_the_stack_files_and_leave_the_toolchain_out(tmp_path, monkeypatch):
    monkeypatch.setattr(vamos, "STACK_DIR", tmp_path / "stack")
    depfile = tmp_path / "prefs.c.obj.d"
    depfile.write_text("obj: \\\n /work/components/x/prefs.c \\\n /opt/amiga/include/exec/types.h \\\n"
                       " /work/install/include/prefs.h\n")
    assert vamos.dependencies([depfile]) == [tmp_path / "stack/components/x/prefs.c",
                                             tmp_path / "stack/install/include/prefs.h"]


def program_with_source(tmp_path, monkeypatch) -> tuple[vamos.Program, object]:
    """A built program in tmp_path whose depfile names one source, older than the program."""
    monkeypatch.setattr(vamos, "STACK_DIR", tmp_path / "stack")
    source = tmp_path / "stack" / "components" / "prefs.c"
    source.parent.mkdir(parents=True)
    source.write_text("int x;\n")
    depdir = tmp_path / "logic" / "CMakeFiles" / "LogicPrefs.dir"
    depdir.mkdir(parents=True)
    (depdir / "prefs.c.obj.d").write_text("obj: /work/components/prefs.c\n")
    program = tmp_path / "logic" / "LogicPrefs"
    program.write_bytes(b"\0")
    os.utime(source, (1000, 1000))
    os.utime(program, (2000, 2000))
    return vamos.Program(program), source


def test_a_program_newer_than_its_sources_is_current(tmp_path, monkeypatch):
    program, _ = program_with_source(tmp_path, monkeypatch)
    assert program.stale() is None


def test_a_source_changed_after_the_build_makes_the_program_stale(tmp_path, monkeypatch):
    program, source = program_with_source(tmp_path, monkeypatch)
    os.utime(source, (3000, 3000))
    assert program.stale() == source


def test_a_source_gone_since_the_build_makes_the_program_stale(tmp_path, monkeypatch):
    program, source = program_with_source(tmp_path, monkeypatch)
    source.unlink()
    assert program.stale() == source
