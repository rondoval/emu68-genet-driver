# emu68-genet

> **Releases:** this component ships as part of the
> [emu68-driver-stack](https://github.com/rondoval/emu68-driver-stack) — the downloadable
> `.lha` and bundled documentation are published there. This repository is source-only
> and versioned via git tags.

**emu68-genet** is an AmigaOS driver for the Broadcom GENET v5 Ethernet controller on the
Raspberry Pi 4B and CM4, for use with the PiStorm32-lite and Emu68. The hardware-facing
GENET and PHY code is ported from [Das U-Boot](https://source.denx.de/u-boot/u-boot) and
Linux. The SANA-II side resembles generic AmigaOS SANA-II drivers and, in places, Michal
Schulz's [WiFiPi.device](https://github.com/michalsc/WiFiPi.device).

**One driver for every network stack.** genet.device speaks two interfaces:

- the zero-copy **netdev** interface of
  [**lwip-amiga**](https://github.com/rondoval/emu68-driver-stack)'s `bsdsocket.library`, the
  fastest way to use the port;
- **SANA-II** (rev 3.1), for Roadshow, AmiTCP, Miami and any other SANA-II stack.

One stack uses the port at a time: the first to bring its interface up has it until it
closes it. Monitoring tools (`genet-stats`, `netdev-stats`) work beside either.

## What you get

- **Gigabit Ethernet** on the Pi's onboard GENET v5. Under lwip-amiga packets move by
  reference between the stack's memory and the hardware rings, with no per-frame copies.
  Under SANA-II they are copied through the driver's own buffers, since SANA-II buffers
  can sit anywhere, Chip RAM included, where the controller cannot reach.
- **Hardware offload** under lwip-amiga: TX TCP/UDP checksums, RX raw checksum, and
  interrupt coalescing negotiated with the stack.
- **The SANA-II feature set:** extended global statistics, special statistics backed by
  the hardware MIB counters, throughput sampling, promiscuous mode, multicast addresses
  and ranges, and the online, offline, connect and disconnect events. `S2_ONLINE` waits
  for the link for up to 6 seconds and also succeeds with no cable plugged in.
- **Multicast:** up to 15 groups are filtered in hardware; past that the controller passes
  everything and the driver keeps what a stack joined.
- **Proper link handling:** PHY link events plus a periodic poll (GENET v5 misses the
  10 Mbps link-up interrupt), with optional advertisement narrowing or forced speed
  (`LINK_MODE`, `AUTONEG`) and negotiated symmetric flow control (`FLOW_CONTROL`).
- **Live diagnostics:** `genet-stats` (SANA-II statistics in a window) and lwip-amiga's
  `netdev-stats`, which includes the complete UniMAC hardware MIB.
- **ROM-able:** the driver contains no writable data sections.

## Requirements

- AmigaOS 3.0 or newer (Kickstart V39 or newer).
- PiStorm32-lite with a Raspberry Pi 4B or CM4.
- Emu68 1.1 alpha.1 or newer. The driver from the stack's standard archive runs on any
  of them; the one from the `-rangeops` archive, faster, needs an Emu68 with the dcache
  extensions (newer than 1.1.0-beta.1, or a custom build) and refuses to load without
  them.
- `gic400.library` at runtime: interrupt delivery depends on it.
- A network stack: lwip-amiga, or any SANA-II stack. lwip-amiga 1.9 (`bsdsocket.library`
  4.109) or newer uses the netdev interface by itself. 1.4 to 1.8 take the SANA-II
  interface, which works but is slower, unless the interface file says `TYPE=NETDEV`;
  1.0 to 1.3 speak only netdev.

## Using it

Install `genet.device` to `DEVS:Networks/` (the installer does this). The network stack
opens it and configures the interface.

**lwip-amiga:** an interface file in `DEVS:NetInterfaces/` names the driver; the default
`TYPE=AUTO` picks netdev.

```text
DEVICE=genet.device
UNIT=0
ADDRESS=DHCP
```

Check the interface with `netinfo`, and watch driver counters and link state with
`netdev-stats` (`netdev-stats COUNTERS` for the full UniMAC MIB).

**Roadshow** (`DEVS:NetInterfaces/genet`):

```ini
device=genet.device
unit=0
configure=dhcp
iprequests=512
writerequests=64
arprequests=8
requiresinitdelay=no
copymode=fast
```

**Miami** needs `USE_MIAMI_WORKAROUND = on` in `ENV:genet.prefs`.

## Runtime configuration (`ENV:genet.prefs`)

The driver reads `ENV:genet.prefs` (plain text) whenever it is opened with no other
opener. Each line is a case-insensitive `KEY = VALUE` pair; `#` and `;` begin a comment;
unknown keys are ignored and missing keys fall back to the defaults below. Changes take
effect once every program has closed the device: bring the stack down and up.

| Key | Default | Meaning |
|---|---|---|
| `UNIT_TASK_PRIORITY` | `15` | Exec priority of the driver's unit task, the bottom half of the receive interrupt. Exec does not preempt among equals, so at the priority of the AmigaDOS handler processes (10) a busy handler keeps it off the CPU for milliseconds; keep it above them, below `input.device` (20), and far above dynamic-scheduler ranges (e.g. Executive's, ≤ 5). |
| `RX_POOL_BUFS` | `0` (auto) | lwip-amiga only. Total RX buffers: the first fill the hardware ring, the rest cover frames the stack holds in socket receive queues. `0` sizes the pool at attach from the stack's declared hold budget; an explicit value is an operator override, clamped to 512–4096. Each buffer costs 2 KB of DMA memory; too few shows up as the `netdev-stats` pool-dry counter under load. |
| `LINK_MODE` | `auto` | Narrows the advertisement to one mode — `auto`, `10hd`, `10fd`, `100hd`, `100fd`, `1000fd`. Autonegotiation stays on, so this is the standards-clean way to pin a link. |
| `AUTONEG` | `on` | `off` forces `LINK_MODE`'s speed/duplex outright, for a partner that will not negotiate. Requires a `LINK_MODE` speed, and is refused for `1000fd` (1000BASE-T settles master/slave through negotiation). |
| `FLOW_CONTROL` | `on` | Advertises and negotiates symmetric pause (802.3 Annex 31B); the MAC honours and emits PAUSE only where both ends agree. `off` withdraws the advertisement and makes the MAC ignore PAUSE. |
| `USE_MIAMI_WORKAROUND` | `off` | SANA-II only. `on` rounds every copy to and from the stack up to a multiple of 4 bytes, as Miami DX expects. |

The 3.x driver's `BUDGET`, `PERIODIC_TASK_MS`, `RX_COALESCE_USECS`, `RX_COALESCE_FRAMES`,
`TX_COALESCE_FRAMES`, `UNIT_STACK_SIZE` and `USE_DMA` are accepted and ignored, so an old
file still works. Interrupt coalescing is set at runtime instead, with either stack:
`netdev-stats RXUSECS <n> RXFRAMES <n> TXFRAMES <n>`.

## genet-stats

`genet-stats` (in `C:`) is a small Intuition/ListBrowser statistics viewer. It opens
`genet.device`, samples the extended statistics, the special statistics and the throughput
once per second, and shows them in a live-updating list. It works beside either stack. It
is mainly an example of how to use the driver's SANA-II statistics.

## Known limitations

- **One stack at a time.** While lwip-amiga holds the port, a SANA-II stack's
  `S2_ONLINE` fails (`S2ERR_OUTOFSERVICE`), and the other way round lwip-amiga's attach
  fails.
- **No jumbo frames:** MTU is fixed at 1500.
- **No checksum offload under SANA-II:** the interface has no way to carry it. Under
  lwip-amiga the RX checksum is reported raw and folded by the stack; see
  `docs/offloads.md`.

---

## For developers

### Layout

- `genet.device/`: the driver.
  - The hardware layer (`bcmgenet*.c`, `phy*.c`) started from U-Boot and Linux. The PHY
    code, the register definitions, the MIB table and the bring-up and UMAC programming
    (`bcmgenet.c`, `bcmgenet-mac.c`) stay close to upstream; the datapath
    (`bcmgenet-rx.c`, `-tx.c`, `-irq.c`, `-link.c`, `-rxpool.c` and most of `-dma.c`) is
    the driver's own. Hardware discovery (base address, IRQs, MAC, PHY) runs through
    `devicetree.resource` in `devtree_parse.c`.
  - `netdev_api.c`: the netdev personality. Its datapath is lock-free (SPSC recycle and
    TX-done rings).
  - `sana2_*.c`: the SANA-II personality — openers and commands (`sana2_api.c`), reads
    and delivery (`sana2_io.c`), writes (`sana2_tx.c`), multicast (`sana2_mcast.c`),
    statistics (`sana2_stats.c`).
  - `unit_task.c`: the unit task both personalities share — command dispatch, the
    interrupt bottom half, housekeeping.
- `runtime-config/`: `ENV:genet.prefs` parsing and defaults, kept apart from the
  hardware-facing code.
- `tools/genet-stats.c`: the statistics viewer.
- `tests/s2check.c`: SANA-II checks the stack's bench rounds run on the Amiga (built,
  not installed).
- `docs/offloads.md`: how the GENET offload machinery works and the platform constraints
  that govern it.

### Building

Built through the **emu68-driver-stack** superproject, which supplies the Bebbo
cross-toolchain and the companion CMake packages (`emu68-common`, `emu68-gic400-library`,
and `Netdev` from lwip-amiga). From a superproject checkout:

```sh
./scripts/docker-build.sh --target emu68-genet-driver-netdev   # no local toolchain needed
```

The superproject orders `lwip-amiga` before `genet.device` (which consumes the exported
`Netdev` package) and shares the stack-wide debug backend / `EMU68_TIER` options. The
default build is the `-rangeops` driver; `EMU68_CONFIGURE_ARGS="-DEMU68_FORCE_LVO_CACHE_OPS=ON"`
builds the standard one, whose cache maintenance goes through exec's
`CachePreDMA`/`CachePostDMA`.

## License

Mixed, per file — see the SPDX headers. The Amiga device scaffolding and most of the
SANA-II personality are dual-licensed (`MPL-2.0 OR GPL-2.0+`); the hardware-facing GENET/PHY
code, the SANA-II transmit path included, is Linux/U-Boot-derived GPL-family material; the
remaining original files are `GPL-2.0+`. See [LICENSE](LICENSE).
