# genet.device test suite

`run.sh` is the entry point; its header lists the tiers. The host tier needs nothing but this
checkout. The hardware tiers run on an Amiga through amimcp's Python client, and only after the
machine's owner has agreed to the round.

```sh
tests/run.sh host                                # the harness's own tests, the logic tier, the gates; no Amiga
tests/run.sh hw --tier install                   # the build installed and checked, nothing tested
tests/run.sh hw --tier smoke --peer <PC address> # both personalities up, a little data (~8 min)
tests/run.sh hw --tier full  --peer <PC address> # smoke, then everything below (~16 min)
tests/run.sh hw --tier perf  --peer <PC address> # the verdict on speed: no worse than baselines.json
tests/run.sh hw --tier compare --peer <PC address> \
    --reference-netdev <another 5.x genet.device>  # an A/B of two builds on alternating boots
tests/run.sh hw --tier profile --peer <PC address> --driver <a profile build>
tests/run.sh restore                             # the machine's own files back
tests/run.sh seed runs/<a> runs/<b> ...          # baselines.json from the seeding runs
```

| Tier | What it checks |
|---|---|
| `host` | the harness's own parsers and judgements (`unit/`), and the logic tier (`logic/`, below): the driver's own sources under vamos. `ENV:genet.prefs` (every key, the grammar, the clamps, the 3.x keys, LINK_MODE and AUTONEG); SANA-II multicast ranges and the MDF filter they become; interrupt moderation (`NETDEV_CMD_SET_COALESCE`, the RX profiles, the burst ladder, the bulk level); SANA-II reads and delivery (types, 802.3, orphans, RAW, cast flags, the packet filter, the copy functions, the Miami rounding, promiscuous openers), the openers' lifecycle and foreign closes, aborts, `S2_OFFLINE`, `CMD_FLUSH`, `S2_DEVICEQUERY`, `S2_ONEVENT`, the claim's refusals; `NSCMD_DEVICEQUERY`'s list against the handlers; SANA-II writes (the frame on the ring, the refusals, the backlog of a full ring, a failed copy's events); netdev's reception filter and station address. Then the code gates and traceability (below). About 15 s |
| `install` | the driver and genet-stats in place, read back after the reboot |
| `smoke` | TYPE=AUTO comes up as netdev; the queries any opener may make; the SANA-II commands that would take the unit refused; TCP both ways over a floor; netdev-stats counting; then the same as TYPE=SANA2, held by SANA-II |
| `full` | nobody holds the unit with the interface removed, each personality takes it once the other let go, every refusal; the opens taken and refused, foreign closes; three remove/add cycles each; LINK_MODE, AUTONEG and FLOW_CONTROL on the wire, the TX backlog at 100 Mb/s; the multicast filter against the PC's sender, coalescing beside SANA-II, S2_OFFLINE/S2_ONLINE, every kept request aborted, the Miami workaround; genet-stats; bsdsocktest's suite in each personality |
| `perf` | the verdict on speed: sockbench's matrix (`harness/sockbench.py`) in each personality on a boot of its own, `--reps` runs of each at priority 6 with Executive's `Stat` and `Ps` around each run. Each judged figure's median (throughput, request-response rate and latency, CPU share) must reach its baseline's bound, the worst of the seeding boots: no worse than the driver was then. A test with a miss is measured once more on a fresh boot first. Context switches, the unit task's and the stack's shares (`--ps`) and AmiSpeedTest are recorded; a Stat window stretched by a wait around the run is dropped. `values.json` and `stat.json` in the test's directory; `--seeding` measures without judging |
| `compare` | an A/B when a change needs one: the build against one or more other genet.device builds on alternating boots per personality, each round every reference in turn, then the build (`--compare-rounds`, 2); a second `--reference-*` makes a bisect. Every judged figure of the build must reach each reference's worst run, and a test with a miss is measured once more on a boot of each before it counts. `--compare-tests` takes part of the matrix. The build stays installed; `compare.md`, `values.json` and `stat.json` in the test's directory |
| `profile` | a profile build only (genet in `EMU68_PROFILE`, backend `pistorm`): one run of each profiled test, the perf slots' time per call off the serial line (`--serial`, default `/dev/ttyUSB0`), each within 20 % of its baseline's median: a diagnosis, one sample per boot |

`--driver FILE` tests another genet.device (a profile build, another build) in place of the
install tree's; the tools still come from the install and build trees.

The first run creates `tests/.venv` from `requirements.txt`.

## What a hardware round needs

- **The build:** the stack built and installed (`./scripts/docker-build.sh` from the stack root).
  The suite takes `genet.device`, `genet-stats` and lwip-amiga's `netdev-stats` from the stack's
  `install/`, sockbench from lwip-amiga's build dir, and GenetTest from this component's build dir.
  `--install-dir` and `--build-dir` point elsewhere.
- **bsdsocktest:** its binary, by default from its checkout beside the stack's (`--bsdsocktest`).
- **The machine:** its amiga MCP server registered in `~/.claude.json` (`--server`, default
  `amiga-pi4`); amimcp's checkout beside the stack's, or `--amimcp`.
- **The PC:** its LAN address (`--peer` or `GENET_PEER`). The suite starts lwip-amiga's
  `tcp-bench-peer.py` and bsdsocktest's helper on Windows when their ports do not listen, and stops
  at the end what it started. They run on Windows because WSL's NAT keeps the Amiga and LAN
  multicast out.

## What it changes on the Amiga

- **Replaced, the machine's own copies kept first:** `DEVS:Networks/genet.device` and
  `C:genet-stats`, plus `LIBS:bsdsocket.library` when `--bsdsocket FILE` is given. The first run
  against a machine copies each file it would replace into `tests/.machine/<server>/` and never
  overwrites those copies. A build that passed stays installed; `--restore` puts the machine's own
  files back (and deletes what it never had).
- **Changed during a test and put back at its end:** `ENV:genet.prefs` (the link and Miami tests),
  and which personality the genet interface runs in. The personalities come from copies of the
  machine's `DEVS:NetInterfaces/genet` in `RAM:gt/<type>/genet` with their `TYPE=` set (an
  interface is named after its file), so the machine's own file is never written. The session ends
  with the machine's own interface and prefs back in use.
- **Temporary:** everything else goes to `RAM:gt`, which a reboot empties.
- **Reboots:** after an install that changed a file, with a 20 s wait first (PFS3 delays its
  writes) and a read-back on both sides of it.

Changing the personality or the prefs takes the interface down, and the agent talks over that
interface: those steps run as detached scripts that log to `RAM:gt/<name>.log` and end in a marker,
and the host polls the log until the agent answers again. A script whose add fails puts the
machine's own interface back up, so the Amiga stays reachable. A test's marker names the
personality it needs (`@pytest.mark.personality(SANA2)`); the tests run grouped by it, so that
each switch happens once.

## What comes out

`tests/runs/<timestamp>/`: `report.md` (the build, the files replaced, one row per test, the
baseline warnings, the values measured), `run.json`, `measured.json`, and each program's output.

## Baselines

`baselines.json` holds, per machine (`<server>`, or `<server>-roadshow` with the Roadshow card in),
what the driver measured on the seeding boots, under keys such as `netdev.tcp_tx.mbps`,
`sana2.rr_1.p50_us`, `netdev.tcp_tx.cpu_pct` and `netdev.udp_tx_1472.tx_submit.avg_us`: each
figure's median, and for a judged one its bound, the worst of the seeding boots' own medians. A
later build must do no worse: its median must reach the bound. Which way is better is said where
a figure is checked (`harness/matrix.py`).

Seeding: run the perf tier with `--seeding` on several sessions (a boot per personality each, so
every figure gets three boots or more), then `run.sh seed` the run directories; it refuses runs of
different driver builds, names the figures with fewer than three boots, and the diff is read before
it is committed. The 5.0 seed (2026-11-14) is the bar for 5.x; 4.4 and 3.17 were compared with it
once and are not measured again.

## The logic tier

The programs in `logic/` are the driver's own sources built by the stack as m68k CLI programs
(never installed), and run on the host under amitools' vamos (`harness/vamos.py`). Each includes the
source under test, so its static functions are in reach, and links the sources beside it that the
behaviour needs: `LogicPrefs` (`runtime_config.c`), `LogicMcast` (`sana2_mcast.c` and
`bcmgenet-mac.c`), `LogicNetdev` (`bcmgenet-dma.c` and `netdev_api.c`), `LogicSana2`
(`sana2_io.c`, `sana2_api.c`, `sana2_mcast.c`, `sana2_tx.c`, `bcmgenet-link.c`), `LogicUnitTask`
(`unit_task.c`).

- **Steps:** a program's arguments are steps, `name[=operands]`; it prints the state after each
  as `step.N.*` facts in the grammar below, and the test judges them. Each program's header lists
  its steps. Errors, events and commands print by their NDK names.
- **exec and utility.library:** the driver reaches them through `unit->sysBase` and the device's
  `utilityBase`, so the programs hand it fake bases (`fakelib.c`, `fakeexec.c`): memory and list
  calls go on to vamos's exec; `Signal()`, `PutMsg()` and `ReplyMsg()` are recorded (and what was
  posted waits for a `task` step); `Forbid()` and `Disable()` count their nesting, which every
  test wants back at 0; `CallHookPkt()` and the tag calls are done here. A call to anything else
  ends the run with `fail=` naming the library and the LVO.
- **The rest of the driver:** `stubs.c` holds weak stand-ins for every driver function a program
  does not build, so the real one wins wherever it is linked. The register block is a zeroed
  64 KB buffer the program reads back; the cache operations take the exec LVO
  (`EMU68_FORCE_LVO_CACHE_OPS`), as vamos runs no LINE-F opcode; there is no debug sink.
- **What it needs:** vamos (`pipx install amitools`) and the stack build. Without either the tier
  is skipped; a program older than a file it was compiled from (its depfiles in the build dir
  name them) fails, as it would judge code that is no longer there.
- **What vamos is not:** its dos.library is a reimplementation, so a test that rests on a
  dos.library edge case belongs on the Amiga (the full tier), not here.

## The gates

The host tier ends with the code gates over the component's own C: the device scaffolding, both
personalities, the datapath files, the prefs reader, genet-stats, GenetTest and the logic programs
(`harness/gates.py`). The upstream-diffable files (the PHY layer, `bcmgenet.c`, `bcmgenet-mac.c`, the
imported headers) are left out.

- **Kernel-doc:** a `/**` comment above every function: `name() - what it does`, an `@param:` line
  for each parameter, `Return:` unless it returns void.
- **Complexity** (lizard's cyclomatic complexity): at most 10. `complexity-allow.txt` lists the
  functions allowed more, each with its ceiling and the reason: at most 15 for a new entry; the
  5.0 code's (marked `5.0:`) at what they had. A ceiling above what its function needs is a finding
  until lowered, so the list only goes down.
- **Traceability** (`harness/trace.py`): the items the suite must cover, most read from the
  sources (the commands each dispatcher serves, the events SANA-II raises, the prefs keys), the
  claim's rules and the refusal codes AGENTS.md names; a test covers an item with
  `@pytest.mark.covers(...)`. Every host run rewrites `TRACEABILITY.md` and fails while an item has
  no test.

## The grammar

GenetTest and the logic programs print `format=1`, then `key=value` lines (prose indented two
spaces), then `PASS` or `FAIL`. Every check is a group entry: `check.N.name` (the same text every
run), `check.N.ok=yes|no` and `check.N.detail` (what was seen); `checks` and `checks_failed` come
last. Return codes: 0 passed, 5 a check failed, 10 the environment (a device or timer did not
open), 20 usage, printed before the format line. `harness/results.py` reads it.

## Layout

- `rig/`: GenetTest, the program the hardware tiers run on the Amiga. Its first argument names the
  command (`QUERY`, `HOLDER`, `NETDEV`, `SANA2`, `BAREOPEN`, `CLOSE`, `MCAST`, `ONLINECYCLE`,
  `CABLE`, `ABORT`, `MIAMI`); `rig/genettest.c` says what each checks and what it needs.
- `common/`: what the Amiga-side programs share: `kv.c`, the grammar.
- `logic/`: the logic tier: its programs, their support (`support.h`: the fake exec and utility,
  the unit, the step loop, the names, the weak stubs), `CMakeLists.txt`, and the tests, a file
  per area (`logic/conftest.py` finds the programs in the build dir).
- `gates/`: the code gates and traceability on every host run.
- `runs/` (not in git): every run's output, and
  in `runs/bench/` what the 5.0 rounds' scripts left: their results, the builds they compared and the
  PI4's own files from before the 5.0 work (`NOTE.md`).
- `complexity-allow.txt`, `TRACEABILITY.md` (written by the host tier), `baselines.json`.
- `harness/`: the host side, one module per concern: the machine (`amiga.py`), the install
  (`install.py`), the stack and its detached scripts (`stack.py`), the interface's personality
  (`interface.py`), the programs in `RAM:gt` (`tools.py`), the floors the short runs must reach
  (`limits.py`), a test measured for the perf and compare tiers (`matrix.py`), the serial capture
  (`uart.py`), the PC peers (`peers.py`), the grammar (`results.py`), the parsers of what
  sockbench, Executive's `Stat`, bsdsocktest, netdev-stats, AmiSpeedTest and the profile build
  print, baselines, the compare criterion, the report, the logic tier's programs (`vamos.py`), the
  code gates (`gates.py`) and traceability (`trace.py`).
- `hw/`: the hardware tiers' tests, one file per area, and their fixtures (`hw/conftest.py`).
- `unit/`: the harness's own tests, on outputs recorded from the hardware rounds (`samples/`).
