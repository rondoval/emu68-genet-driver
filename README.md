# emu68-genet

**emu68-genet** is an Amiga OS driver for the Broadcom GENET v5 Ethernet controller found on the Raspberry PI 4B, designed for use with the Pistorm32-lite and Emu68 project.
The driver is based on [Das U-Boot](https://source.denx.de/u-boot/u-boot) bcmgenet driver. It also derives heavily from the [wifipi driver](https://github.com/michalsc/Emu68-tools/tree/master/network/wifipi.device) from Michal Schulz.

## Features

- SANA-II rev 3.1
- Device tree parsing
- GENET v5 support, with rgmii-rxid PHY

## Unimplemented / Planned Features

- Hardware Filter Block use
- Promiscuous mode
- Multicast support (not tested)
- PHY link state updates at runtime
- Hardware sourced statistics
- Packet type statistics

## Requirements

- Kickstart 3.2 (V47) or newer (tested with 47.115)
- Pistorm32-lite with Raspberry Pi 4B
- Emu68, version newer than 1.0.4 (with [this PR](https://github.com/michalsc/Emu68/pull/295)
- A network stack (tested with Roadshow 1.15)

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
