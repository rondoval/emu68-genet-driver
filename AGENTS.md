# emu68-genet-driver Agent Notes

## Build

- Required companion packages: `devicetree.resource`, `emu68-common`, and `emu68-gic400-library`.
- The README examples use an out-of-tree `build/` directory with `cmake ..`, `make`, and `make install`.
- Equivalent configure/build flow is acceptable, but do not assume `build/` already exists.

## Runtime Notes

- The driver loads optional configuration from `ENV:genet.prefs`.
- Config keys are case-insensitive and missing keys fall back to defaults.
- If a task changes runtime config parsing, preserve the current behavior that unknown keys are ignored.

## Code Handling

- This is a SANA-II driver; be conservative with opener, queue, and packet-copying logic.
- Preserve the current split between hardware-facing code in `genet.device/` and user-configurable defaults in `runtime-config/`.
- `gic400.library` is a hard runtime dependency for interrupt handling in current versions.
- The README documents a soft-reboot hang when the driver is online; be cautious with interrupt enable and teardown changes.

## Validation

- Changes to runtime config parsing should be checked in `runtime-config/` and at least one driver build.
- Interface changes in shared headers or install outputs should be validated through `emu68-driver-stack`.

