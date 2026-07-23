# emu68-genet-driver — Agent Notes

## Build

- CMake packages required: `devicetree.resource`, `emu68-common`, `emu68-gic400-library`, and `Netdev` (the netdev ABI header, exported by lwip-amiga).
- Build through the superbuild's container wrapper from the stack root — never host `cmake`:
  `./scripts/docker-build.sh --target emu68-genet-driver`
- Debug backend (`pistorm` default | `serial` | `off`): `EMU68_CONFIGURE_ARGS="-DEMU68_DEBUG_BACKEND=serial"`. Selected stack-wide via `emu68-common`; `serial` links `debug.lib` and is not ROM-able.

## Invariants

- Speaks the **netdev ABI** (`<devices/netdev.h>`) only; the SANA-II personality lives on the `main` branch. Packets move zero-copy between the stack's DMA memory and the hardware rings — no openers, no packet copies, no per-protocol queues.
- The datapath is lock-free: every object crossing the unit task and the caller context is a single-producer/single-consumer ring. Be conservative with anything that touches that contract.
- Keep hardware-facing code in `genet.device/` separate from user-configurable defaults in `runtime-config/`.
- `gic400.library` is a hard runtime dependency for interrupt handling.
- A reset guard quiesces GENET DMA before the Amiga resets (Ctrl-Amiga-Amiga and `ColdReboot()` / `C:Reboot`); be careful with interrupt-enable, teardown, and reset-guard changes.

## Runtime config

- Optional configuration loads from `ENV:genet.prefs`. Keys are case-insensitive, missing keys fall back to defaults, and unknown keys are ignored — preserve that.

## Licensing (mixed — preserve every file's SPDX header)

- Dual-licensed `MPL-2.0 OR GPL-2.0+` device scaffolding — do not casually relicense: `device.c`, `device_end.c`, `unit.c`, `unit_task.c`, `netdev_api.c`, `bcmgenet-link.c`, `bcmgenet-mib.c`, `include/device.h`.
- Original repository files, `GPL-2.0+`: `devtree_parse.c`, `include/genet/bcmgenet.h`, `runtime-config/*`.
- Linux/U-Boot-derived GPL-family: the remaining `bcmgenet*`, `phy*`, and imported `include/genet/*` (mostly `GPL-2.0+`, some `GPL-2.0-only`). Note `bcmgenet-link.c` and `bcmgenet-mib.c/.h` are the dual-licensed exceptions to the `bcmgenet*` glob.

## Validation

- Code or CMake changes: build with `./scripts/docker-build.sh --target emu68-genet-driver`. Runtime-config parsing changes: also exercise `runtime-config/`. Doc-only changes: a Problems check.
