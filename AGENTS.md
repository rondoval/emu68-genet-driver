# emu68-genet-driver

genet.device — AmigaOS 68k driver for the Broadcom GENET v5 Ethernet MAC on the
Raspberry Pi 4 / CM4, behind PiStorm32 + Emu68.

## Build

Required packages: `devicetree.resource`, `emu68-common`, `emu68-gic400-library`, and
`Netdev` (the netdev ABI header, exported by lwip-amiga).

Build through the superbuild's container wrapper from the stack root — never host `cmake`:

```sh
./scripts/docker-build.sh --target emu68-genet-driver-netdev
```

- Debug backend (`pistorm` default | `serial` | `off`): `EMU68_CONFIGURE_ARGS="-DEMU68_DEBUG_BACKEND=serial"`. Selected stack-wide via `emu68-common`; `serial` links `debug.lib` and is not ROM-able.
- The top-level `CMakeLists.txt` adds `runtime-config` (static lib), `genet.device` (the driver), `tools` (`genet-stats`, installed to `C/`) and `tests` (`GenetTest`, the suite's rig, built, not installed), and `find_package`s `GIC400`, `Emu68Common`, and `Netdev`. `devicetree.resource` is a runtime resource, not a build-time package. `VERSTRING` is defined at the top level from project version plus date; `genet.device/CMakeLists.txt` only consumes it, and `device.c` refuses to build without it.

## Architecture

- Two personalities on one core: **netdev** (`<devices/netdev.h>`, `netdev_api.c`: zero-copy, packets move by reference between the stack's DMA memory and the rings) and **SANA-II** (`sana2_*.c`: openers, read queues, copies through the driver's buffers).
- **The claim:** netdev holds the unit iff `ndStackOps != NULL` (ATTACH to DETACH); SANA-II holds it iff `s2->claimers != 0` (an opener's `S2_CONFIGINTERFACE` or `S2_ONLINE`, until it closes). Opening holds nothing. ATTACH while SANA-II holds it answers `NDERR_BUSY`; SANA-II configuration, multicast and datapath while netdev holds it answer `S2ERR_OUTOFSERVICE` — never `S2ERR_BAD_STATE`, which stacks read as "already configured / online". Queries (`S2_DEVICEQUERY`, the S2 statistics, netdev `GET_*`, `SET_COALESCE`) answer in any mode. The claim rule is the one `netdev.h` defines.
- **netdev's hot path must not pay for SANA-II.** The unit task's datapath gate tests `ndStarted` first (one load and branch, as 4.x); SANA-II per-wake work sits only in its branch. SANA-II state lives in `struct GenetS2` behind `unit->s2`, so `GenetUnit`'s hot field order stays 4.x's. A datapath change is judged on hardware: the perf tier against `tests/baselines.json`, and the compare tier against the previous build when a verdict needs an A/B.
- The netdev datapath is lock-free: every object crossing the unit task and the caller context is a single-producer/single-consumer ring. Be conservative with anything that touches that contract. SANA-II writes go out in the caller under one `Forbid()`; a full ring queues them for the unit task.
- An `IOSana2Req`-sized open gets an Opener, left in `ios2_BufferManagement` (the open rule is `netdev.h`'s). `closeLib` takes back only an Opener on `s2->issued`, the list `openLib`/`closeLib` keep under exec's `Forbid()`: never trust that field from a closing request otherwise.
- SANA-II commands and opener list changes run on the unit task only. `openLib`/`closeLib` reach it through `GENET_CMD_OPENER_CONTROL` (ADD and REMOVE one-way, the opener freed on REMOVE; ABORT synchronous). `closeLib` always queues REMOVE.
- Interrupt-mask writes stay on the unit task; SANA-II writers only signal it.
- `RBUF_ALIGN_2B` follows the personality, set at START: SANA-II frames start at +66 (IP header longword-aligned for the copy), netdev's at +64.
- Upstream proximity is not a blanket rule — it applies where diffing against U-Boot/Linux still pays, and nowhere else:
  - **Keep diffable.** The PHY layer (`phy.c` from U-Boot phylib, `phy_mdio.c` and `phy_bcm54xx.c` from Linux, plus the imported `mii.h`/`ethtool.h`/`phy*.h`), the register and descriptor definitions in `bcmgenet-regs.h`, the MIB table in `bcmgenet-mib.c`, and the bring-up/teardown and UMAC-programming sequences in `bcmgenet.c` / `bcmgenet-mac.c`. These encode hardware behaviour, so upstream is the reference for what the silicon wants and a clean diff is worth preserving.
  - **Ours, deliberately.** The datapath — `bcmgenet-rx.c`, `bcmgenet-tx.c`, `bcmgenet-irq.c`, `bcmgenet-link.c` and the hot paths of `bcmgenet-dma.c`. Zero-copy netdev RX, the lock-free TX cookie FIFO, polled link state, RX interrupt profiles and TX-done masking have no upstream counterpart; the file split is ours too (upstream is one `bcmgenet.c`). Judge changes here on their own merits, not on resemblance to Linux.
- Hardware discovery (GENET base, IRQs, MAC, PHY) goes through `devicetree.resource` in `devtree_parse.c`.
- `bcmgenet-mib.c` is the counter surface behind `NETDEV_CMD_GET_COUNTERS`: the whole UniMAC MIB block plus every driver counter and ring gauge, read on demand. Its hardware table mirrors upstream `bcmgenet_gstrings_stats[]`, names included — keep the two diffable. Nothing sweeps these counters periodically (do not reintroduce that); the list is deliberately complete, so figures that `NETDEV_CMD_GET_STATS` also reports appear here too and the two are not de-duplicated; `NetDevStats` is the portable core only, so a new figure naming genet's own machinery belongs here, not there.
- Keep hardware-facing code in `genet.device/` separate from user-configurable defaults in `runtime-config/`.
- `gic400.library` is a hard runtime dependency for interrupt handling.
- A reset guard quiesces GENET DMA before the Amiga resets (Ctrl-Amiga-Amiga and `ColdReboot()` / `C:Reboot`); be careful with interrupt-enable, teardown, and reset-guard changes.
- Branch topology: `main` is the 3.x SANA-II line, `netdev_release` the 4.x netdev release line, `netdev` the integration branch. Keep this out of shipped source comments — it lives here.

## Runtime config

- Optional configuration loads from `ENV:genet.prefs`. Keys are case-insensitive, missing keys fall back to defaults, and unknown keys are ignored — preserve that.

## Licensing (mixed — preserve every file's SPDX header)

- Dual-licensed `MPL-2.0 OR GPL-2.0+` device scaffolding — do not casually relicense: `device.c`, `unit.c`, `unit_task.c`, `netdev_api.c`, `sana2_api.c`, `sana2_io.c`, `sana2_mcast.c`, `sana2_stats.c`, `bcmgenet-link.c`, `bcmgenet-mib.c`, `bcmgenet-rxpool.c`, `include/device.h`, `include/sana2_unit.h`, `include/genet/genet_specialstats.h`, `tools/genet-stats.c`, and the test suite's own files under `tests/` (the rig, its harness and tests, `run.sh`). The SANA-II scaffolding resembles generic SANA-II drivers and WiFiPi.device more specifically.
- Original repository files, `GPL-2.0+`: `devtree_parse.c`, `include/genet/bcmgenet.h`, `runtime-config/*`.
- Linux/U-Boot-derived GPL-family: the remaining `bcmgenet*`, `phy*`, and imported `include/genet/*` (mostly `GPL-2.0+`, some `GPL-2.0-only`). `bcmgenet-link.c`, `bcmgenet-mib.c/.h` and `bcmgenet-rxpool.c` are the dual-licensed exceptions to the `bcmgenet*` glob. `sana2_tx.c` and `include/genet/bcmgenet-tx.h` hold Linux-derived TX status-block handling and stay `GPL-2.0-only`: Linux-derived code goes there, not into a dual-licensed file.
- The SANA-II headers (`devices/sana2*.h`) are the NDK 3.2R4 ones the build image installs: never vendor a copy into this repository.

## Validation

- Code or CMake changes: build with `./scripts/docker-build.sh --target emu68-genet-driver-netdev`, then `tests/run.sh host`: the suite's own modules and the logic tier, the driver's own sources under vamos (`tests/README.md`). The suite builds on emu68-common's test harness (`emu68test`, taken from the stack's install tree): shared test code belongs there, not in `tests/harness/`. The logic programs include driver sources and stand in for the rest with weak stubs in `tests/logic/stubs.c`, so a changed signature or a new cross-file call shows up there first. The host tier also gates the component's own C: kernel-doc on every function, cyclomatic complexity at most 10 (`tests/complexity-allow.txt` holds the exceptions at a ceiling that only goes down), and every command, SANA-II event, prefs key, claim rule and refusal code covered by a test (`tests/TRACEABILITY.md`, rewritten each run). Runtime-config parsing changes: also exercise `runtime-config/`. Interface changes in shared headers or install outputs: validate through the full `emu68-driver-stack` build. Doc-only changes: a Problems check.
- Hardware, after the machine's owner agreed to the round: `tests/run.sh hw --tier smoke` for a quick check, `--tier full` before a release (every claim rule, refusal, prefs key, cycle and bsdsocktest in both personalities; `tests/README.md`). Speed: `--tier perf` is the verdict, every judged figure no worse than `tests/baselines.json` (the 5.0 seeding boots; re-seed only with `--seeding` runs and `run.sh seed`, the diff read first); `--tier compare --reference-netdev <another build>` is an A/B of two builds on alternating boots when a change needs one, since one run after the other reads as a regression of the first (a second `--reference-*` bisects, `--compare-tests` narrows it). `--tier profile --driver <profile build>` for the per-call costs.
