# emu68-genet

> **Releases:** this component ships as part of the
> [emu68-driver-stack](https://github.com/rondoval/emu68-driver-stack) — the downloadable
> `.lha` and bundled documentation are published there. This repository is source-only
> and versioned via git tags.

**emu68-genet** is an AmigaOS driver for the Broadcom GENET v5 Ethernet controller on the
Raspberry Pi 4B and CM4, for use with the PiStorm32-lite and Emu68. The hardware-facing
GENET and PHY code is ported from [Das U-Boot](https://source.denx.de/u-boot/u-boot) and
Linux.

> **This is a netdev driver, not a SANA-II driver.** genet.device **4.x** speaks the
> zero-copy `netdev` ABI and works only with
> [**lwip-amiga**](https://github.com/rondoval/emu68-driver-stack)'s `bsdsocket.library`.
> It does **not** answer SANA-II, so Roadshow, AmiTCP and Miami cannot use it. Those
> stacks want the **3.x SANA-II variant**, which ships alongside 4.x in the same archive —
> pick it at install time.

## What you get

- **Gigabit Ethernet** on the Pi's onboard GENET v5, driven from a zero-copy datapath:
  packets move by reference between the stack's DMA memory and the hardware rings, with no
  per-frame copies and no per-protocol queues.
- **Hardware offload** — TX TCP/UDP checksums (GENET transmit status block), RX raw
  checksum (RXCHK), and interrupt coalescing negotiated with the stack and tunable at runtime.
- **Proper link handling** — PHY link events plus a periodic poll (GENET v5 misses the
  10 Mbps link-up interrupt), with optional advertisement narrowing / forced speed
  (`LINK_MODE`, `AUTONEG`) and negotiated symmetric flow control (`FLOW_CONTROL`).
- **Live diagnostics** through `netdev-stats`, including the complete UniMAC hardware MIB
  — the same counters Linux exposes via `ethtool -S`.
- **ROM-able** — the driver contains no writable data sections.

## Requirements

- AmigaOS 3.0 or newer (Kickstart V39 or newer).
- PiStorm32-lite with a Raspberry Pi 4B or CM4.
- An Emu68 build that exposes the `/scb` memory mapping for GENET (1.1 alpha.1 or newer)
  **and** the shared range data-cache opcode. On an Emu68 without that opcode, build the
  stack with `-DEMU68_FORCE_LVO_CACHE_OPS=ON`, which uses the 68040.library cache LVOs
  instead.
- `gic400.library` at runtime — interrupt delivery depends on it.
- A network stack: **lwip-amiga's `bsdsocket.library`** for this 4.x driver (the SANA-II
  stacks are served by the 3.x variant).

## Using it

Install `genet.device` to `DEVS:Networks/` (the installer does this). It carries no
personality of its own — the network stack opens it and configures the interface.

With lwip-amiga, point the stack at the driver in `ENVARC:netstack.prefs`:

```text
DEVICE = networks/genet.device
UNIT   = 0
MODE   = DHCP
```

With no prefs at all, lwip-amiga already defaults to `networks/genet.device` unit 0 on
DHCP. Check the interface with `netinfo`, and watch driver counters and link state live
with `netdev-stats` (`netdev-stats COUNTERS` for the full UniMAC MIB). Both ship with
lwip-amiga.

## Runtime configuration (`ENV:genet.prefs`)

The driver reads `ENV:genet.prefs` (plain text) the first time it is opened. Each line is
a case-insensitive `KEY = VALUE` pair; `#` and `;` begin a comment; unknown keys are
ignored and missing keys fall back to the defaults below. Changes take effect on the next
open — bring the stack down and up.

| Key | Default | Meaning |
|---|---|---|
| `UNIT_TASK_PRIORITY` | `10` | Exec priority of the driver's unit task. Keep it above dynamic-scheduler ranges (e.g. Executive's, ≤ 5) or a busy application can starve the driver. |
| `UNIT_STACK_SIZE` | `65536` | Unit-task stack in bytes (floor 4096). |
| `PERIODIC_TASK_MS` | `200` | Housekeeping-timer interval in ms (interrupt watchdog; paces the PHY poll). |
| `LINK_POLL_MS` | `1000` | How often the PHY is polled for link state, in ms. The poll — not the interrupt — is what converges a 10 Mbps link, which GENET v5 fails to signal. |
| `RX_COALESCE_USECS` | `500` | Target latency in µs before hardware raises an RX interrupt if the frame threshold is not met. |
| `RX_COALESCE_FRAMES` | `64` | Received frames that trigger an RX interrupt. |
| `TX_COALESCE_FRAMES` | `32` | Transmitted frames that trigger a TX interrupt. |
| `RX_POOL_BUFS` | `0` (auto) | Total RX buffers: the first fill the hardware ring, the rest cover frames the stack holds in socket receive queues. `0` autonegotiates at attach from the stack's declared hold budget; an explicit value is an operator override, clamped to 512–4096. Each buffer costs 2 KB of DMA memory; too few shows up as the `netdev-stats` pool-dry counter under load. |
| `LINK_MODE` | `auto` | Narrows the advertisement to one mode — `auto`, `10hd`, `10fd`, `100hd`, `100fd`, `1000fd`. Autonegotiation stays on, so this is the standards-clean way to pin a link. |
| `AUTONEG` | `on` | `off` forces `LINK_MODE`'s speed/duplex outright, for a partner that will not negotiate. Requires a `LINK_MODE` speed, and is refused for `1000fd` (1000BASE-T settles master/slave through negotiation). |
| `FLOW_CONTROL` | `on` | Advertises and negotiates symmetric pause (802.3 Annex 31B); the MAC honours and emits PAUSE only where both ends agree. `off` withdraws the advertisement and makes the MAC ignore PAUSE. |

## Known limitations

- **Upload has more headroom than download** — the driver's per-frame submission path is
  the current TX ceiling, not the stack.
- **No jumbo frames** — MTU is fixed at 1500.
- **RX checksum is reported raw**, folded against the pseudo-header by the stack; the
  hardware's per-frame OK/FR verdict mode is deliberately unused (it does not cover
  fragments). See `docs/offloads.md` for the full offload picture.

---

## For developers

### Layout

- `genet.device/` — the driver. The hardware layer (`bcmgenet*.c`, `phy*.c`) is ported
  from U-Boot/Linux and kept close to upstream. `netdev_api.c` is the netdev ABI personality; the datapath is lock-free (SPSC recycle and TX-done rings). Hardware discovery (base address, IRQs, MAC, PHY) runs through `devicetree.resource` in `devtree_parse.c`.
- `runtime-config/` — `ENV:genet.prefs` parsing and defaults, kept separate from the
  hardware-facing code.
- `docs/offloads.md` — how the GENET offload machinery works and the platform constraints that govern it.

### Building

Built through the **emu68-driver-stack** superproject, which supplies the Bebbo
cross-toolchain and the companion CMake packages (`devicetree.resource`, `emu68-common`,
`emu68-gic400-library`, and `Netdev` from lwip-amiga). From a superproject checkout:

```sh
./scripts/docker-build.sh --target emu68-genet-driver   # no local toolchain needed
```

The superproject orders `lwip-amiga` before `genet.device` (which consumes the exported
`Netdev` package) and shares the stack-wide debug backend / `EMU68_TIER` options —
`emu68-genet-driver` is a valid component name for `EMU68_PROFILE`, `EMU68_DEBUG` and
`EMU68_TRACE`.

## License

Mixed, per file — see the SPDX headers. The Amiga device scaffolding is dual-licensed
(`MPL-2.0 OR GPL-2.0+`); the hardware-facing GENET/PHY code is Linux/U-Boot-derived
GPL-family material; the remaining original files are `GPL-2.0+`. See [LICENSE](LICENSE).
