# emu68-genet

> **Releases:** this component ships as part of the
> [emu68-driver-stack](https://github.com/rondoval/emu68-driver-stack) — the downloadable
> `.lha` and bundled documentation are published there. This repository is source-only
> and versioned via git tags.

**emu68-genet** is an Amiga OS driver for the Broadcom GENET v5 Ethernet controller found on the Raspberry PI 4B and CM4, designed for use with the Pistorm32-lite and Emu68 project.
The hardware-facing GENET and PHY implementation is based primarily on [Das U-Boot](https://source.denx.de/u-boot/u-boot) and Linux GENET code. The Amiga SANA-II device scaffolding resembles both generic AmigaOS SANA-II drivers and, in some files, Michal Schulz's [WiFiPi.device](https://github.com/michalsc/WiFiPi.device) more specifically.

**Beware:**
- 3.x requires `gic400.library` at runtime; interrupt delivery depends on it.
- The driver requires Emu68 exposing the correct `/scb` memory mapping for the GENET controller, i.e. version 1.1 alpha.1 or newer.
- Runtime tuning is driven by `ENV:genet.prefs`; unknown keys are ignored and missing keys fall back to built-in defaults.

## Known bugs

- None currently known.

## Features

- SANA-II rev 3.1
- Device tree parsing
- GENET v5 support, with rgmii-rxid PHY
- Interrupt handling via GIC-400
- Extended global statistics (`S2_GETEXTENDEDGLOBALSTATS`)
- Special statistics backed by GENET hardware MIB counters (`S2_GETSPECIALSTATS`)
- Throughput sampling (`S2_SAMPLE_THROUGHPUT`)
- Promiscuous mode support
- Multicast address and range programming
- `genet-stats` monitoring tool
- The drivers is ROM-able

## Unimplemented / Planned Features

- PHY link state updates at runtime

## Requirements

- Kickstart 3.0 (V39) or newer
- Pistorm32-lite with Raspberry Pi 4B or CM4
- Emu68 build with correct `/scb` memory mappings for the GENET controller (1.1 alpha.1 or newer)
- `gic400.library` runtime dependency
- A network stack

Tested using:

- OS 3.2.3 + AmiKit 12.8.3 + Roadshow 1.15
- OS 3.2.3 + Miami DX
- OS 3.0 + AmiTCP 4.2 (16 Jun 2022)
on an A1200 with RPi4B.

## Sample Roadshow config file

```ini
device=genet.device
unit=0
configure=dhcp
debug=no
iprequests=512
writerequests=64
arprequests=8
requiresinitdelay=no
copymode=fast
```

## Building

Use Bebbo's GCC cross compiler and cmake.

This project uses CMake packages from these companion projects:

- `devicetree.resource`
- `emu68-common`
- `emu68-gic400-library`

The simplest setup is to install all of them into the same prefix and then point `CMAKE_PREFIX_PATH` at that one location.

```sh
mkdir build
cd build
cmake .. \
	-DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain.cmake \
	-DCMAKE_PREFIX_PATH=/path/to/emu68-driver-stack \
	-DCMAKE_INSTALL_PREFIX=/path/to/emu68-driver-stack
make
make install
```

If you keep dependencies in separate install trees instead, set `CMAKE_PREFIX_PATH` to all of them, separated with semicolons.

```sh
cmake .. \
	-DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain.cmake \
	-DCMAKE_PREFIX_PATH="/path/to/emu68-gic400-library/install;/path/to/emu68-common/install;/path/to/devicetree.resource/install"
```

## Runtime configuration (genet.prefs)

At startup the driver looks for `ENV:genet.prefs` (plain text). Each line is a `KEY=VALUE` pair. Unknown keys are ignored. Keys are case-insensitive. If the file or specific lines are missing, built‑in defaults are used.

Default values (current):

```text
UNIT_TASK_PRIORITY=15
UNIT_STACK_SIZE=65536
USE_DMA=0
USE_MIAMI_WORKAROUND=0
BUDGET=64
PERIODIC_TASK_MS=200
RX_COALESCE_USECS=500
RX_COALESCE_FRAMES=64
TX_COALESCE_FRAMES=32
```

Setting descriptions:

- `UNIT_TASK_PRIORITY`  Exec priority of the driver's unit task, the bottom half of the receive interrupt. Clamped to 6–19. Exec does not preempt among equals, so at the priority of the AmigaDOS handler processes (10) a busy handler keeps it off the CPU for milliseconds; keep it above them and above your stack's own task, and below `input.device` (20).
- `UNIT_STACK_SIZE`  Stack size in bytes for the unit task. Minimum enforced is 4096.
- `USE_DMA`  Leave at 0. Not supported: SANA-II does not guarantee the alignment Genet's DMA needs; enabling can result with instability or packets missing on TX. (DMA is still used internally, but the data is copied to/from internal, aligned buffers)
- `USE_MIAMI_WORKAROUND`  1 enables length round up quirk for Miami DX stack; 0 disables.
- `BUDGET`  Maximum number of work items the unit task and ISR handles per wake-up before rescheduling itself.
- `PERIODIC_TASK_MS`  Interval in milliseconds for the housekeeping timer (PHY polling, interrupt watchdog).
- `RX_COALESCE_USECS`  Target latency in microseconds before the hardware raises an RX interrupt if the frame threshold is not met.
- `RX_COALESCE_FRAMES`  Number of received frames that trigger an RX interrupt when reached.
- `TX_COALESCE_FRAMES`  Number of transmitted frames that trigger a TX interrupt when reached (not used in 3.x).

You can omit any line to keep its default.
In order for the changes to be applied, the device must be closed (e.g. shutdown your IP stack).

## Utility: genet-stats

`genet-stats` is a simple Intuition/ListBrowser-based statistics viewer. It opens `genet.device`, samples extended stats, special stats, and throughput once per second, and presents them in a live-updating list.

It is intended mainly as an example of how to use the driver's API. I'm not planning to fruther develop the tool - contributions are welcome.
