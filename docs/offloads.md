# GENET offload guide — genet.device on BCM2711 (GENET v5)

How the hardware offload machinery works, what the silicon can do, what this
driver uses, and the platform constraints that govern them. The
authoritative reference implementation is the Linux driver
(`drivers/net/ethernet/broadcom/genet/`;
line references below are to that driver's current mainline.

## The status-block mechanism (TSB / RSB)

GENET's per-frame offload metadata does not live in DMA descriptors — it rides
in a **64-byte status block** prepended to every frame's DMA stream, in both
directions:

- **TSB** (transmit): enabled by `TBUF_64B_EN` in `TBUF_CTRL`. The 64 bytes
  precede the outgoing frame in the TX DMA stream; the TBUF strips them before
  the wire. Only one word is consumed by hardware: `tx_csum_info` at offset 48.
- **RSB** (receive): enabled by `RBUF_64B_EN` in `RBUF_CTRL`. Every received
  frame arrives with the 64 bytes prepended; the payload starts at a fixed +64.
  `rx_csum` at offset 8 carries the RXCHK result.

Both are enabled unconditionally (`bcmgenet_umac_reset`), so *every* frame
carries them whether or not checksum offload is requested — an idle TSB is
simply all-zeros. The struct layout is `struct genet_status_64`
(`bcmgenet-regs.h`), byte-identical to Linux's `struct status_64`.

Layout note: Linux additionally sets `RBUF_ALIGN_2B` (2-byte pad so the IP
header lands 4-byte aligned). We deliberately leave it OFF: Emu68 reads
unaligned 32-bit words for free, and a fixed +64 payload offset is simpler
than +66.

### Endianness — read this before touching either block

Three different conventions coexist; mixing them up produces silent garbage:

| What                         | Where it lives          | Access rule |
|------------------------------|-------------------------|-------------|
| Registers                    | MMIO                    | `mmio_read32`/`mmio_write32` (swap built in) |
| DMA descriptors              | on-chip, behind MMIO    | `dmadesc_set` → mmio accessors |
| TSB/RSB words                | ordinary DMA RAM        | explicit `le32()` at every access |
| RSB `rx_csum` low halfword   | inside the LE word      | **no further swap** — see below |

The status-block words are little-endian in memory (the hardware's view); the
m68k is big-endian, and nothing swaps for you. The `rx_csum` halfword is the
one empirically-calibrated exception: after `le32()` of the word, the low 16
bits are already the sum in native/network order; a further `le16()` would
byte-swap them incorrectly.

## TX L4 checksum offload

Model: CHECKSUM_PARTIAL, exactly Linux's contract (`bcmgenet_add_tsb`,
bcmgenet.c ~2049). Three parties must each do their part:

1. **The stack** (netdev ABI, `ndif_l4_offsets` in lwip-amiga's `netdev_tx.c`)
   seeds the L4 checksum field with the folded **pseudo-header sum** and hands
   the driver frame-relative offsets: `ntd_CsumStart` (L4 header start, e.g. 34
   for IPv4/no-options) and `ntd_CsumOffset` (checksum field, start+6 for UDP,
   +16 for TCP), plus `NDTF_L4CSUM` and `NDTF_L4_UDP`.
2. **The TSB** says *where*: `tx_csum_info = STATUS_TX_CSUM_LV |
   (start << 16) | offset`, plus `STATUS_TX_CSUM_PROTO_UDP` for UDP (makes the
   engine rewrite a 0x0000 result to 0xFFFF, per RFC 768). Offsets are relative
   to the **frame** start, not the DMA stream — the TSB's own 64 bytes don't
   count. Written with `le32()`.
3. **The SOP descriptor** says *do it*: `DMA_TX_DO_CSUM` in `len_stat`
   (Linux bcmgenet.c ~2203). Without this bit the engine ignores a perfectly
   valid TSB and the frame leaves with the pseudo-seed as its "checksum" —
   an invalid non-zero value that every receiver silently drops: the TSB alone
   is *not* enough.

The engine then sums from `csum_start` to frame end (the seed makes the
pseudo-header part come out right), complements, and writes the result at
`csum_offset` on the fly.

Descriptor layout difference from Linux: Linux pushes the TSB into the same
buffer as the frame (`skb_push`), so its SOP descriptor covers TSB+headers
together. We submit the TSB as its **own SOP descriptor** (one 64-byte slab
slot per ring entry, `unit->ndTxTsb`), then the frame segments, EOP on the
last. Equivalent; costs +1 descriptor per frame. `DMA_SOP`,
`DMA_TX_DO_CSUM` go on the TSB descriptor, `DMA_EOP` on the last data segment,
`DMA_TX_APPEND_CRC` on all.

Not offloaded on TX: the IPv4 **header** checksum (software in lwIP, cheap —
20 bytes) — GENET has no IP-header insert. No TSO/GSO either: the silicon has
no segmentation engine (Linux doesn't advertise one; don't go looking).

## RX checksum offload (RXCHK)

The RXCHK block has two personalities, selected in `RBUF_CHK_CTRL`:

- **L3-parse mode** (parser on): hardware recognizes IPv4/TCP/UDP, verifies the
  L4 checksum itself, and reports a per-frame verdict in the RSB
  (`STATUS_RX_CSUM_OK` / `STATUS_RX_CSUM_FR`). We do **not** use this mode.
- **Full-frame raw mode** (`RBUF_RXCHK_EN | RBUF_L3_PARSE_DIS`) — what we use,
  same as modern Linux: the block computes the plain 1's-complement sum over
  the **entire frame past the Ethernet header** and delivers it in the RSB.
  Protocol-agnostic (works for anything IP), fragments included. Zero means
  "no result" → the frame is passed up unvalidated (wire integrity was already
  covered by the Ethernet FCS). The OK/FR bits never set in this mode, which
  is why the driver advertises `NDCF_RX_CSUM_RAW` only, not `NDCF_RX_CSUM_VALID`.

`RBUF_SKIP_FCS` is only needed when the UMAC forwards the CRC
(`CMD_CRC_FWD`) — we strip it, so the bit stays clear.

Consumption (netdev ABI): the driver reports the sum as `nrd_CsumRaw` with
`NDRF_CSUM_RAW`; the stack glue (`ndif_rx_csum_ok`) folds
`raw + pseudo-header` and expects `0xFFFF`. A valid IP header folds to −0, so
one fold validates both the header and the L4 checksum. UDP-with-zero-checksum
and fragments pass through by design. lwIP's software `CHECK_TCP/UDP` is
switched off per-netif — hardware does that work at line rate.

Belt and suspenders: if the hardware raw sum disagrees, the stack recomputes
the sum in software before dropping and logs
`[netdevif] RX csum: hw 0x… sw 0x… len …`. This line is a **canary** — it must
stay silent. If it ever fires: sw==swap(hw) means an endianness regression;
sw confirming the drop means genuine wire corruption reaching the MAC.

## Interrupt coalescing

- **RX**: `DMA_MBUF_DONE_THRESH` (frames) + the RDMA timeout register (usecs,
  in 8.192 µs hardware units). Runtime-tunable via `ENV:genet.prefs`
  (`RX_COALESCE_USECS`, default 500; `RX_COALESCE_FRAMES`, default 64) and the
  netdev `SET_COALESCE` op (`NDCF_COALESCE`).
- **TX**: `DMA_MBUF_DONE_THRESH` on the TX ring (`TX_COALESCE_FRAMES`, default
  32); the hardware additionally interrupts when the ring drains, so
  completions are never starved.

## Capability inventory

Used by this driver:

| Capability | HW mechanism | Our use |
|---|---|---|
| TX L4 checksum (TCP/UDP) | TSB `tx_csum_info` + `DMA_TX_DO_CSUM` | `NDCF_TX_L4CSUM`, per-frame `NDTF_L4CSUM/L4_UDP` |
| RX full-frame checksum | RXCHK raw mode → RSB | `NDCF_RX_CSUM_RAW`, `nrd_CsumRaw` |
| FCS generation | `DMA_TX_APPEND_CRC` | always (also gives hardware runt padding) |
| Interrupt coalescing | MBUF_DONE_THRESH + RDMA timeout | `NDCF_COALESCE` + prefs |
| Link events | PHY IRQ → `UMAC_IRQ_LINK_UP/DOWN` | `NDCF_LINK_EVENTS` → `nso_LinkChange` |
| MAC address filter | MDF (17 exact-match slots) | broadcast + own MAC + up to 15 joined multicast groups (`NDCF_MCAST_FILTER`); promiscuous only for `NDFF_PROMISC`/`NDFF_ALLMULTI` or a list that overruns the slots — the UniMAC has no multicast-only accept bit, so `CMD_PROMISC` is our all-multi |

Present in silicon, deliberately unused:

| Capability | Why not |
|---|---|
| L3-parse RX verdicts (`STATUS_RX_CSUM_OK/FR`) | raw mode is protocol-agnostic and matches the ABI's fold contract; verdict mode covers less (no fragments) for no gain |
| HFB (hardware filter block) | packet classification/steering into rings — single-consumer stack, one RX ring; init stays commented out |
| Priority queues (16 TX + 16 RX + default Q16) | one ring each direction suffices; the netdev ABI has no QoS concept |
| Wake-on-LAN (MPD block, `UMAC_IRQ_MPD_R`) | no Amiga-side suspend/resume story to wire it to |
| EEE | the PHY's AutogrEEEn is switched off in `bcm54xx_config_init()`, and the RBUF/TBUF energy-control registers are cleared at every UMAC reset — RBUF EEE/PM breaks the GENET receive path, per Broadcom's own driver |
| Auto power-down (APD) | not implemented, and neither is the `EXP08` 10BaseT DAC-wake pair that exists to make a 10 Mbps link recover from it. The Pi 4 B and CM4 device trees do request APD (`brcm,powerdown-enable`). **The two must be added together** — in `bcmgenet_link_poll()` (`bcmgenet-link.c`), keyed on `speed == SPEED_10` — or 10 Mbps link stability regresses |
| Jumbo frames | `UMAC_MAX_FRAME_LEN` set to standard 1536; MTU 1500 fixed in caps; lwIP config sized for TCP_MSS 1460 |

### Flow control (implemented)

Symmetric pause is advertised and negotiated. `PHY_GBIT_FEATURES` carries
`SUPPORTED_Pause | SUPPORTED_Asym_Pause`, `genphy_config_advert()` puts them into
`MII_ADVERTISE`, and `genphy_read_pause()` resolves the outcome from
`MII_ADVERTISE & MII_LPA` per 802.3 Annex 31B. `bcmgenet_mac_config()` programs that
outcome into `CMD_RX_PAUSE_IGNORE` / `CMD_TX_PAUSE_IGNORE`, so the MAC honours and
emits PAUSE only on a link where both ends agreed to it.

The resolution lives in `genphy_read_pause()` rather than inside
`genphy_parse_link()` because the latter returns as soon as it resolves a gigabit
link, before it reads `MII_LPA` at all — pause folded in there would never resolve at
1000 Mbps.

Half duplex never uses PAUSE: collision detection is the backpressure mechanism, so
both IGNORE bits are set regardless of what was negotiated.

`FLOW_CONTROL = off` in `ENV:genet.prefs` withdraws the advertisement *and* sets both
IGNORE bits, so the two halves stay consistent either way.
