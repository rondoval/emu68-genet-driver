# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
"""
The compare tier, an A/B of two 5.x builds when a change needs one (the
verdict on speed is the perf tier against the baselines): the build against
one or more other genet.device builds in one session, on alternating boots,
in each personality one is given for (--reference-netdev, --reference-sana2).
Each round boots every reference in turn and then the build (--compare-rounds,
2: reference, build, reference, build); more than one reference makes a bisect.
Every judged figure of the build must reach each reference's worst run
(compare.py); the tests with a miss are measured once more, a boot of each
driver, before the miss counts. The build is installed last, so it stays.
compare.md in the test's directory has every figure, values.json the build's
runs per boot (run.sh seed can read them), stat.json every Stat sample.
"""
from __future__ import annotations

import json
import statistics
from pathlib import Path

import pytest

from harness import compare, sockbench, values
from harness.compare import BUILD
from harness.matrix import Tally, measure
from harness.stack import NETDEV, SANA2
from harness.suite import DRIVER

pytestmark = [pytest.mark.hw, pytest.mark.compare]

TIMEOUT_S = 4 * 3600


def matrix_part(request, kind: str) -> list[sockbench.Test]:
    """The personality's matrix, or the part of it --compare-tests names."""
    tests = sockbench.matrix(kind.lower())
    wanted = {name.strip() for name in request.config.getoption("--compare-tests").split(",") if name.strip()}
    unknown = wanted - {t.name for t in tests}
    if unknown:
        raise pytest.UsageError(f"--compare-tests: no {', '.join(sorted(unknown))} in the {kind} matrix")
    return [t for t in tests if not wanted or t.name in wanted]


def boot_each(request, interface, artefacts, peer, kind: str, drivers: dict[str, bytes],
              tests: list[sockbench.Test], tally: Tally) -> None:
    """Every driver of @drivers on a boot of its own, in order, genet up as @kind, @tests measured into @tally."""
    opt = request.config.getoption
    for label, driver in drivers.items():
        interface.fresh_boot(artefacts, driver)
        interface.use(kind)
        for test in tests:
            block = measure(interface.device, test, peer, opt("--reps"), opt("--stat"), opt("--ps"))
            tally.add(label, kind, block, interface.boots)


def verdicts_written(references: list[str], tally: Tally, test_dir: Path, record_property) -> list[str]:
    """Each reference's verdicts into compare.md and the report, the Stat samples into stat.json: the misses."""
    misses: list[str] = []
    sections = []
    for reference in references:
        verdicts = compare.against(tally, reference)
        sections.append(compare.table(reference, verdicts, tally))
        failed = [f"{reference}: {v}" for v in verdicts if not v.ok]
        misses += failed
        median_ratio = statistics.median(compare.ratio(v) for v in verdicts)
        record_property(f"figures against {reference}", f"{len(verdicts) - len(failed)} of {len(verdicts)} reach it")
        record_property(f"median ratio against {reference}", f"{median_ratio:.3f}")
    (test_dir / "compare.md").write_text("\n".join(sections))
    (test_dir / "stat.json").write_text(json.dumps(tally.stats, indent=1))
    return misses


@pytest.mark.timeout(TIMEOUT_S)
@pytest.mark.parametrize("kind", [NETDEV, SANA2], ids=["netdev", "sana2"])
@pytest.mark.covers("datapath")
def test_the_build_keeps_up_with_the_reference(request, installed, interface, artefacts, peer, test_dir, kind,
                                               record_property):
    paths = request.config.getoption(f"--reference-{kind.lower()}")
    if not paths:
        pytest.skip(f"no reference driver for {kind} (--reference-{kind.lower()} FILE)")
    drivers = {label: Path(path).read_bytes() for label, path in zip(compare.labels(paths), paths)}
    drivers[BUILD] = installed[DRIVER]
    tests = matrix_part(request, kind)
    tally = Tally()
    for _ in range(request.config.getoption("--compare-rounds")):
        boot_each(request, interface, artefacts, peer, kind, drivers, tests, tally)
    references = [label for label in drivers if label != BUILD]
    missed = set().union(*(compare.tests_of(compare.against(tally, r)) for r in references))
    if missed:
        record_property("measured again", ", ".join(sorted(missed)))
        again = [t for t in tests if t.name in missed]
        boot_each(request, interface, artefacts, peer, kind, drivers, again, tally)
    values.write(test_dir, request.config.getoption("--server"), drivers[BUILD], tally.boots_of(BUILD))
    misses = verdicts_written(references, tally, test_dir, record_property)
    assert not misses, "; ".join(misses)
