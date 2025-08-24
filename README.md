# emu68-genet

**emu68-genet** is an Amiga OS driver for the Broadcom GENET v5 Ethernet controller found on the Raspberry PI 4B, designed for use with the Pistorm32-lite and Emu68 project.
The driver is based on [Das U-Boot](https://source.denx.de/u-boot/u-boot) bcmgenet driver. It also derives heavily from the [wifipi driver](https://github.com/michalsc/Emu68-tools/tree/master/network/wifipi.device) from Michal Schulz.

## Features

- SANA-II rev 3.1
- Device tree parsing
- GENET v5 support, with rgmii-rxid PHY

## Unimplemented / Planned Features

- Promiscuous mode (implemented, not tested)
- Multicast support (implemented, not tested)
- PHY link state updates at runtime
- Hardware sourced statistics
- Packet type statistics

## Requirements

- Kickstart 3.0 (V39) or newer
- Pistorm32-lite with Raspberry Pi 4B
- Emu68, version 1.0.5.1 or newer
- A network stack

Tested using:

- OS 3.2.3 + Roadshow 1.15
- OS 3.2.3 + Miami DX
- OS 3.0 + AmiTCP 4.2 (16 Jun 2022)

## Sample Roadshow config file

```ini
device=genet.device
unit=0
configure=dhcp
debug=no
iprequests=512
writerequests=512
requiresinitdelay=no
copymode=fast
```

## Building

Use Bebbo's GCC cross compiler. I'm building under Windows, so not really sure if the Makefile is good for Linux.

```sh
make all
```

## Runtime configuration (genet.prefs)

At startup the driver looks for `ENV:genet.prefs` (plain text). Each line is a `KEY=VALUE` pair. Unknown keys are ignored. Keys are case-insensitive. If the file is missing, built‑in defaults are used.

Default values (current):

```text
UNIT_TASK_PRIORITY=0
UNIT_STACK_SIZE=65536
USE_DMA=0
USE_MIAMI_WORKAROUND=0
TX_PENDING_FAST_TICKS=0
TX_RECLAIM_SOFT_US=500
RX_POLL_BURST=0
RX_POLL_BURST_IDLE_BREAK=16
POLL_DELAY_US=1000,2000,2000,4000,8000
```

Setting descriptions (brief):

- `UNIT_TASK_PRIORITY`  Exec task priority of the driver unit task (higher = runs sooner). 0 is neutral.
- `UNIT_STACK_SIZE`  Stack size in bytes for the unit task. Minimum enforced is 4096.
- `USE_DMA`  Leave at 0. Not supported: SANA-II does not guarantee the alignment GENET DMA needs; enabling can result with instability or packets missing on TX.
- `USE_MIAMI_WORKAROUND`  1 enables length round up quirk for Miami DX stack; 0 disables.
- `TX_PENDING_FAST_TICKS`  After any TX reclaim while descriptors still pending, force this many fast poll cycles to reduce latency.
- `TX_RECLAIM_SOFT_US`  Upper bound (microseconds) a poll sleep may extend to while TX descriptors outstanding (soft cap on backoff).
- `RX_POLL_BURST`  Additional immediate RX poll iterations after activity is first seen. 0 disables burst.
- `RX_POLL_BURST_IDLE_BREAK`  Early break threshold during a burst when consecutive empty polls reach this count.
- `POLL_DELAY_US`  Comma list of successive poll interval delays (microseconds) used as a backoff ladder when idle; index resets on activity.

You can omit any line to keep its default. `POLL_DELAY_US` is a comma-separated ladder (microseconds) used for adaptive polling backoff. Duplicate values are allowed. The driver enforces an internal maximum length (currently 32 entries); excess entries are ignored.

Changes require reloading the device (or reboot) to take effect.
