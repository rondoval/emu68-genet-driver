# emu68-genet-driver

genet.device — AmigaOS 68k driver for the Broadcom GENET v5 Ethernet MAC on the
Raspberry Pi 4 / CM4, behind PiStorm32 + Emu68.

## Build

Required packages: `devicetree.resource`, `emu68-common`, `emu68-gic400-library`, and
`Netdev` (the netdev ABI header, exported by lwip-amiga).

Build through the superbuild's container wrapper from the stack root — never host `cmake`:

```sh
./scripts/docker-build.sh --target emu68-genet-driver
```

- Debug backend (`pistorm` default | `serial` | `off`): `EMU68_CONFIGURE_ARGS="-DEMU68_DEBUG_BACKEND=serial"`. Selected stack-wide via `emu68-common`; `serial` links `debug.lib` and is not ROM-able.
- The top-level `CMakeLists.txt` adds `runtime-config` (static lib) and `genet.device` (the driver), and `find_package`s `GIC400`, `Emu68Common`, and `Netdev`. `devicetree.resource` is a runtime resource, not a build-time package. `VERSTRING` is defined at the top level from project version plus date; `genet.device/CMakeLists.txt` only consumes it.

## Architecture

- Speaks the **netdev ABI** (`<devices/netdev.h>`) only. Packets move zero-copy between the stack's DMA memory and the hardware rings — no openers, no packet copies, no per-protocol queues.
- The datapath is lock-free: every object crossing the unit task and the caller context is a single-producer/single-consumer ring. Be conservative with anything that touches that contract.
- The hardware layer (`bcmgenet*.c`, `phy*.c`) is ported from U-Boot/Linux — keep it close to upstream.
- Hardware discovery (GENET base, IRQs, MAC, PHY) goes through `devicetree.resource` in `devtree_parse.c`.
- `bcmgenet-mib.c` is the counter surface behind `NETDEV_CMD_GET_COUNTERS`: the whole UniMAC MIB block plus every driver counter and ring gauge, read on demand. Its hardware table mirrors upstream `bcmgenet_gstrings_stats[]`, names included — keep the two diffable. Nothing sweeps these counters periodically (do not reintroduce that); the list is deliberately complete, so figures that `NETDEV_CMD_GET_STATS` also reports appear here too and the two are not de-duplicated; `NetDevStats` is the portable core only, so a new figure naming genet's own machinery belongs here, not there.
- `gic400.library` is a hard runtime dependency for interrupt handling.
- A reset guard quiesces GENET DMA before the Amiga resets (Ctrl-Amiga-Amiga and `ColdReboot()` / `C:Reboot`); be careful with interrupt-enable, teardown, and reset-guard changes.
- Branch topology: `main` is the 3.x SANA-II line, `netdev_release` the 4.x netdev release line, `netdev` the integration branch. Keep this out of shipped source comments — it lives here.

## Runtime config

- Optional configuration loads from `ENV:genet.prefs`. Keys are case-insensitive, missing keys fall back to defaults, and unknown keys are ignored — preserve that.

## Licensing (mixed — preserve every file's SPDX header)

- Dual-licensed `MPL-2.0 OR GPL-2.0+` device scaffolding — do not casually relicense: `device.c`, `device_end.c`, `unit.c`, `unit_task.c`, `netdev_api.c`, `bcmgenet-link.c`, `bcmgenet-mib.c`, `include/device.h`.
- Original repository files, `GPL-2.0+`: `devtree_parse.c`, `include/genet/bcmgenet.h`, `runtime-config/*`.
- Linux/U-Boot-derived GPL-family: the remaining `bcmgenet*`, `phy*`, and imported `include/genet/*` (mostly `GPL-2.0+`, some `GPL-2.0-only`). `bcmgenet-link.c` and `bcmgenet-mib.c/.h` are the dual-licensed exceptions to the `bcmgenet*` glob.

## Validation

- Code or CMake changes: build with `./scripts/docker-build.sh --target emu68-genet-driver`. Runtime-config parsing changes: also exercise `runtime-config/`. Interface changes in shared headers or install outputs: validate through the full `emu68-driver-stack` build. Doc-only changes: a Problems check.
