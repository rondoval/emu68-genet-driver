# emu68-genet-driver Agent Notes

## Build

- Required companion packages: `devicetree.resource`, `emu68-common`, and `emu68-gic400-library`.
- The README examples use an out-of-tree `build/` directory with `cmake ..`, `make`, and `make install`.
- Equivalent configure/build flow is acceptable, but do not assume `build/` already exists.
- The repo-local `compile` task expects an existing `build/` directory. If it is missing, configure first with `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake -DCMAKE_PREFIX_PATH=/path/to/prefix -DCMAKE_INSTALL_PREFIX=/path/to/prefix`.
- Debug backend: pass `-DEMU68_DEBUG_BACKEND=serial` (default `pistorm` | `serial` | `off`); selected stack-wide via `emu68-common`, `serial` links `debug.lib` and is not ROM-able.
- Version string handling now follows the same pattern as `emu68-xhci-driver`: the top-level `CMakeLists.txt` defines `VERSTRING` from project version plus date, and `genet.device/CMakeLists.txt` only consumes it.

## Runtime Notes

- The driver loads optional configuration from `ENV:genet.prefs`.
- Config keys are case-insensitive and missing keys fall back to defaults.
- If a task changes runtime config parsing, preserve the current behavior that unknown keys are ignored.

## Code Handling

- This is a SANA-II driver; be conservative with opener, queue, and packet-copying logic.
- Preserve the current split between hardware-facing code in `genet.device/` and user-configurable defaults in `runtime-config/`.
- `gic400.library` is a hard runtime dependency for interrupt handling in current versions.
- This repository is mixed-license; preserve existing SPDX headers and do not relabel imported AmigaOS SDK headers without explicit provenance work.
- Treat clearly original repository files as `GPL-2.0+` unless a task establishes a different provenance requirement.
- Treat `device*.c`, `unit*.c`, `unit_commands*.c`, `unit_io.c`, `unit_task.c`, and `genet.device/include/device.h` as the cautious dual-licensed SANA-II scaffolding track: they resemble generic SANA-II drivers and also WiFiPi more specifically, so do not casually relicense them.
- Treat `genet.device/src/devtree_parse.c`, `genet.device/include/genet/bcmgenet.h`, and `runtime-config/*` as original repository files currently licensed `GPL-2.0+`.
- Treat the hardware-facing `bcmgenet*`, `phy*`, and imported `include/genet/*` headers as Linux/U-Boot-derived GPL-family material and preserve their file-level SPDX identifiers.
- A reset guard quiesces GENET DMA before the Amiga resets (both Ctrl-Amiga-Amiga and `ColdReboot()` / `C:Reboot`); be cautious with interrupt enable, teardown, and reset-guard changes.

## Validation

- Changes to runtime config parsing should be checked in `runtime-config/` and at least one driver build.
- Interface changes in shared headers or install outputs should be validated through `emu68-driver-stack`.
- Pure documentation or AGENTS updates usually only need a Problems check; code or CMake changes should be followed by at least `cmake --build build` once configured.

