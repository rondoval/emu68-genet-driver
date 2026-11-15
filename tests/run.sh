#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
# genet.device test suite, one entry point per tier (README.md).
#
#   run.sh host [pytest options]                 genet's harness modules, the logic tier (the driver's
#                                                sources under vamos) and the code gates; no Amiga
#   run.sh hw --tier install [--server amiga-pi4] [--driver FILE]
#                                                the build installed and checked; nothing tested
#   run.sh hw --tier smoke --peer <PC address> [--server amiga-pi4]
#                                                install, both personalities up, a little data
#   run.sh hw --tier full  --peer <PC address>   smoke, then every claim rule, refusal, filter,
#                                                prefs key, cycle and bsdsocktest (~16 min)
#   run.sh hw --tier perf  --peer <PC address>   the verdict on speed: sockbench's matrix and CPU share,
#                                                no worse than baselines.json; a boot per personality
#                                                [--seeding: measure only, for run.sh seed]
#   run.sh hw --tier compare --peer <PC address> --reference-netdev A.device [--reference-sana2 B.device]
#                                                the build vs another 5.x build on alternating boots;
#                                                a second --reference-* bisects, --compare-tests NAME,..
#   run.sh hw --tier profile --peer <PC address> --driver <profile build>
#                                                per-call costs off the serial line vs baselines.json
#   run.sh restore [--server amiga-pi4]          the machine's own files back (tests/.machine/)
#   run.sh seed RUNDIR... [--machine amiga-pi4]  baselines.json from the seeding runs (harness/seed.py)
#
# The hardware tiers run only after the machine's owner agreed to the round.
# The suite builds on emu68-common's test harness, which the stack installs to
# <stack>/install/share/emu68-testing ($EMU68_TESTING names another place): build the stack first.
# The first run creates tests/.venv from the harness's requirements.txt.
set -e
here=$(cd "$(dirname "$0")" && pwd)
harness=${EMU68_TESTING:-$here/../../../install/share/emu68-testing}
if [ ! -f "$harness/emu68test/plugin.py" ]; then
    echo "run.sh: no test harness in $harness: build the stack (scripts/docker-build.sh --target emu68-common)" >&2
    exit 10
fi
venv="$here/.venv"
if [ ! -x "$venv/bin/python" ]; then
    python3 -m venv "$venv"
    "$venv/bin/pip" install -q -r "$harness/requirements.txt"
fi
export PYTHONPATH="$harness${PYTHONPATH:+:$PYTHONPATH}"
usage() {
    echo "usage: run.sh host [pytest options] | run.sh hw --tier <install|smoke|full|perf|compare|profile> [options] | run.sh restore [options] | run.sh seed RUNDIR..." >&2
    exit 20
}
tier=${1:-host}
[ $# -gt 0 ] && shift
cd "$here"
case "$tier" in
    host)
        exec "$venv/bin/python" -m pytest unit logic gates "$@"
        ;;
    hw)
        [ "$1" = "--tier" ] && [ $# -ge 2 ] || usage
        case "$2" in
            install|smoke|perf|compare|profile) marks=$2 ;;
            full) marks="smoke or full" ;;
            *) usage ;;
        esac
        shift 2
        export GENET_HW=1
        exec "$venv/bin/python" -m pytest hw -m "$marks" "$@"
        ;;
    restore)
        export GENET_HW=1
        exec "$venv/bin/python" -m pytest hw -m install --restore "$@"
        ;;
    seed)
        [ $# -ge 1 ] || usage
        exec "$venv/bin/python" -m harness.seed "$@"
        ;;
    *)
        usage
        ;;
esac
