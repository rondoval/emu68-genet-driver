# emu68-genet

**emu68-genet** is an Amiga OS driver for the Broadcom GENET v5 Ethernet controller found on the Raspberry PI 4B, designed for use with the Pistorm32-lite and Emu68 project.
The driver is based on [Das U-Boot](https://source.denx.de/u-boot/u-boot) bcmgenet driver. It also derives heavily from the [wifipi driver](https://github.com/michalsc/Emu68-tools/tree/master/network/wifipi.device) from Michal Schulz.

**Beware:**
- The upcoming changes in Emu68 1.1 are likely not compatible with the interrupts implementation in this driver, as the new Arm side handler reads IAR and writes EORI registers.
- v2.0 **requires** gic400.library and Emu68 build with PR#306 - see below for details

## Known bugs

- Amiga will get stuck at boot if you soft reboot while the driver is online. This is likely due to the interrupts remaining enabled.

## What's new
2.1:
- Fix for issue #14: Driver crashes when gic400.library is not present. Now it doesn't.

2.0:
- No more pooling: interrupts used via the GIC-400 controller
- Confg reload does not require flush of the driver - just bring the interface down and up
- Bugfixes

## Features

- SANA-II rev 3.1
- Device tree parsing
- GENET v5 support, with rgmii-rxid PHY
- Interrupt handling via GIC-400

## Unimplemented / Planned Features

- Promiscuous mode (implemented, not tested)
- Multicast support (implemented, not tested)
- PHY link state updates at runtime
- Hardware sourced statistics
- Packet type statistics

## Requirements

- Kickstart 3.0 (V39) or newer
- Pistorm32-lite with Raspberry Pi 4B
- Emu68, version 1.0.6+PR#306 (https://github.com/rondoval/Emu68/releases/tag/v1.0.6-pcie)
- gic400.library (https://github.com/rondoval/emu68-gic400-library/releases/tag/v1.0)
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
writerequests=2048
arprequests=8
requiresinitdelay=no
copymode=fast
```

## Building

Use Bebbo's GCC cross compiler and cmake.

```sh
mkdir build install
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain.cmake
make
make install
```

## Runtime configuration (genet.prefs)

At startup the driver looks for `ENV:genet.prefs` (plain text). Each line is a `KEY=VALUE` pair. Unknown keys are ignored. Keys are case-insensitive. If the file is missing, built‑in defaults are used.

Default values (current):

```text
UNIT_TASK_PRIORITY=5
UNIT_STACK_SIZE=65536
USE_DMA=0
USE_MIAMI_WORKAROUND=0
BUDGET=32
PERIODIC_TASK_MS=200
RX_COALESCE_USECS=500
RX_COALESCE_FRAMES=10
TX_COALESCE_FRAMES=10
```

Setting descriptions:

- `UNIT_TASK_PRIORITY`  Exec task priority of the driver unit task (higher = runs sooner). 0 is neutral.
- `UNIT_STACK_SIZE`  Stack size in bytes for the unit task. Minimum enforced is 4096.
- `USE_DMA`  Leave at 0. Not supported: SANA-II does not guarantee the alignment Genet's DMA needs; enabling can result with instability or packets missing on TX. (DMA is still used internally, but the data is copied to/from internal, aligned buffers)
- `USE_MIAMI_WORKAROUND`  1 enables length round up quirk for Miami DX stack; 0 disables.
- `BUDGET`  Maximum number of work items the unit task and ISR handles per wake-up before rescheduling itself.
- `PERIODIC_TASK_MS`  Interval in milliseconds for the housekeeping timer (PHY polling, interrupt watchdog).
- `RX_COALESCE_USECS`  Target latency in microseconds before the hardware raises an RX interrupt if the frame threshold is not met.
- `RX_COALESCE_FRAMES`  Number of received frames that trigger an RX interrupt when reached.
- `TX_COALESCE_FRAMES`  Number of transmitted frames that trigger a TX interrupt when reached.

You can omit any line to keep its default.
In order for the changes to be applied, the device must be closed (e.g. shutdown your IP stack).
