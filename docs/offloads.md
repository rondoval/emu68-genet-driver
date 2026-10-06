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
  in 8.192 µs hardware units). Seeded from `GENET_COAL_RX_USECS` (500) and
  `GENET_COAL_RX_FRAMES` (64) in `device.h`, and set at runtime by the netdev
  `SET_COALESCE` op (`NDCF_COALESCE`) — which `netdev-stats RXUSECS <n>
  RXFRAMES <n> TXFRAMES <n>` issues by hand.
- **RX profile** (`NDCF_RX_PROFILE`, `NETDEV_CMD_SET_RX_PROFILE`): the timeout
  counts from the first pending frame and is not postponed by frames that keep
  arriving, so a lone small frame — the reply a task is blocked on — always
  waits all of it, while a short static timeout costs a third of the bulk
  receive rate. Whether anybody waits is known to the stack alone, so it states
  intent and the driver maps it.

  Two sources can set the RX coalescer, and **the pin decides which is in
  force**. An explicit `SET_COALESCE` — one naming a non-zero `RXUSECS` or
  `RXFRAMES` — pins the operator's numbers and the stack's intent is ignored
  until an all-default RX setting releases them. Unpinned, the driver decides
  from the profile, and uses its own `GENET_COAL_*` constants rather than the
  operator's fields:

  | pinned | profile | programmed |
  |---|---|---|
  | yes | any | the operator's `RXFRAMES` + `RXUSECS` |
  | no | not stated | `GENET_COAL_RX_FRAMES` + `GENET_COAL_RX_USECS` (64 / 500) |
  | no | throughput | `GENET_COAL_RX_FRAMES` + `GENET_COAL_RX_BULK_USECS` (1000) |
  | no | latency | the burst ladder below |

  The profile keeps being recorded while pinned, so `drv_rx_profile` still
  shows what the stack asked for and the last intent takes effect the moment
  the pin is released. Two edges worth knowing: the pin covers RX only —
  `TXFRAMES` is always applied, there being no TX profile to outrank — and
  because the ABI reads a zero field as "use the driver default", a
  `SET_COALESCE` that names only `TXFRAMES` resets RX to the defaults and
  releases the pin with it.

  - *throughput*: the driver's RX frame threshold with the longer
    `GENET_COAL_RX_BULK_USECS` timeout (1000) — nobody waits for a single
    frame, and at line rate the frame threshold fires first.
  - *latency*: a burst ladder. When an RX pass is about to re-arm its interrupt
    it looks at the ring once more: quiet → one frame, so whatever comes next
    interrupts at once; frames already waiting → one level up (64 frames with
    100 → 200 → 400 µs → `GENET_COAL_RX_USECS`; the top is that 500 µs,
    because the end of a large reply waits for it and somebody waits for that
    end). Registers are written only on a level change; going down to one frame
    is followed by another look at the ring and a poll if a frame slipped in.
    One thing overrules a quiet ring: a consumer that is behind. If the stack
    still holds two poll batches or more of the driver's buffers while the wire
    is idle, nobody is waiting for this traffic — a file copied window by
    window while the application writes the previous one out — and the ring is
    re-armed at the bulk level, so the next burst is batched
    from its first frame; a quiet ring that finds the buffers returned ends it.
    Counters: `drv_rx_profile` (0 nothing stated, 1 throughput, 2 latency),
    `drv_rx_level` (both gauges), `drv_rx_level_changes`, `drv_rx_bulk_arms`.
 - **TX**: the TX-done interrupt is masked in steady state. Completed cookies
  are harvested on every unit-task wakeup — receive interrupts, the submit
  path's low-water kick, the housekeeping tick — because nothing waits for a
  completion, while the hardware also interrupts whenever the ring drains:
  one interrupt plus one task wakeup for every lone frame, i.e. for every
  request and every ACK, at more than 100 µs apiece on this platform. The
  exception is backpressure: when `ndo_TxSubmit` returns short, the ABI tells
  the stack to retry after the next `nso_TxDone`, so the submitter wakes the
  unit task, which hands back what has completed or, if that is nothing, arms
  one TX-done interrupt (`DMA_MBUF_DONE_THRESH` = the unit's TX frame
  threshold, `GENET_COAL_TX_FRAMES` = 32 unless `SET_COALESCE` moved it).
  Counter: `drv_tx_irq_arms`. Without receive traffic the tail of a burst is
  completed by the tick, at most `GENET_PERIODIC_TASK_MS` later.

## Capability inventory

Used by this driver:

| Capability | HW mechanism | Our use |
|---|---|---|
| TX L4 checksum (TCP/UDP) | TSB `tx_csum_info` + `DMA_TX_DO_CSUM` | `NDCF_TX_L4CSUM`, per-frame `NDTF_L4CSUM/L4_UDP` |
| RX full-frame checksum | RXCHK raw mode → RSB | `NDCF_RX_CSUM_RAW`, `nrd_CsumRaw` |
| FCS generation | `DMA_TX_APPEND_CRC` | always (also gives hardware runt padding) |
| Interrupt coalescing | MBUF_DONE_THRESH + RDMA timeout | `NDCF_COALESCE` + `NDCF_RX_PROFILE` |
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
