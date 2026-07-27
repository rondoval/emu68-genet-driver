# Release notes — genet.device 4.1

Changes since v4.0.

A portability and internal-cleanup release.

---

## Breaking changes

None.

---

## Reliability

### Firmware gate for rangeops builds

Builds using the inline Emu68 range cache opcodes (`EMU68_FORCE_LVO_CACHE_OPS`
off — the `-rangeops` stack archives) now check the `/emu68` device-tree
node's `dcache-range-ops` capability at init and refuse to load on firmware
that would Line-F trap on those opcodes, instead of crashing. Standard (LVO)
builds are unaffected.

---

## Build & tooling

### GCC 16.1 build portability

`genet.device` now builds cleanly under GCC 16.1. No behavior change:

- `-ffreestanding` moved from link options to compile options, where it
  actually affects code generation — as a link-only flag it was silently
  inert.

### `UnitTask` adopts the shared driver-task helpers

Message-port setup, the periodic housekeeping timer, and the task-exit
sequence now use `emu68-common`'s `drv_unit_msgport_init()` / `drv_timer` /
`drv_task_exit()` instead of hand-rolled `AllocSignal` / `CreateMsgPort` /
`CreateIORequest` / `OpenDevice` calls. Same periodic-tick semantics, same
exit signalling. No functional change.

### Dependencies

Building now requires **`emu68-common` 1.9.0** or later (`drv_timer.h`,
`driver_task.h`).

---

# Release notes — genet.device 4.0

genet.device 4.0 is a rewrite built for **lwip-amiga**, a new, much faster TCP/IP stack
for AmigaOS. Packets now move between the driver and the stack with no copying and no
locking on the busy path — together with lwip-amiga's own send-path work, TCP upload
reaches 906 Mb/s (see lwip-amiga's own release notes for the full numbers).

## Breaking change: this is not a SANA-II driver

genet.device 4.0 only works with **lwip-amiga**. It does not speak SANA-II, so Roadshow,
AmiTCP, and Miami cannot use it. If you use one of those, install the **3.x** version
instead — it ships in the same download, and the installer lets you pick.

### Coming from 3.x

Old SANA-II-based tools and settings (Roadshow's `device=genet.device` config, the
`genet-stats` utility, and so on) don't apply to 4.0. Configure the network through
lwip-amiga's own `netstack.prefs` instead, and use `netdev-stats` in place of
`genet-stats` for live driver statistics — see the [README](README.md).

## What's faster

- Data moves directly between the driver and the network stack instead of being copied
  back and forth, and the hardware calculates checksums instead of the CPU.
- The busy send/receive path never has to wait on a lock, so throughput holds up better
  under load.
- **Transmit doorbell batched.** The driver starts the hardware transmitting once per
  burst of packets the stack hands it, instead of once per packet — one fewer slow
  register write per packet on the busy send path.
- RX-path cache maintenance was trimmed, and the periodic link/PHY check now returns early
  when nothing has changed.

## Hardware checksum offload

The GENET chip now calculates TCP/UDP checksums for outgoing packets and a checksum for
incoming ones, instead of the CPU doing that work. See `docs/offloads.md` for the
technical detail, and the known-limitations note there on what isn't offloaded.

## Link handling

The driver now tracks the network link more reliably — both from link-change interrupts
and a periodic check, since the GENET chip doesn't always signal a 10 Mbps link coming up
on its own. Three new `ENV:genet.prefs` settings give you more control:

- `LINK_MODE` restricts the link to one speed/duplex (`10hd`…`1000fd`) instead of letting
  it negotiate freely — useful for a flaky cable that keeps renegotiating down.
- `AUTONEG=off` forces that speed/duplex outright, for equipment that won't negotiate
  (gigabit can't be forced this way — it requires negotiation to work at all).
- `FLOW_CONTROL` (on by default) lets the Amiga and the switch/router ask each other to
  pause traffic briefly instead of dropping packets under load.

## Diagnostics

`netdev-stats` (from lwip-amiga) shows live link state and packet counters, and
`netdev-stats COUNTERS` shows the full set of hardware statistics the chip keeps — handy
for troubleshooting. It can be run at any time without restarting the network.

## Requirements

- lwip-amiga 1.1 or newer (SANA-II users want the 3.x variant instead).
- `gic400.library` at runtime.
- A reasonably current Emu68 build; see the [README](README.md) for the exact version
  and a fallback option for older builds.
- PiStorm32-lite with a Raspberry Pi 4B or CM4.

---

# Release notes — genet.device 3.11

Changes since v3.10.

A maintenance release: an interrupt-handling cleanup, adoption of the stack-wide
debug backend, and portability fixes for building against newer/older NDKs and at
`-O3`.  No functional change for the typical user.

---

## Breaking changes

None.

---

## Improvements / Reliability

### Interrupt handler reports handled / not-handled; RX re-arm simplified

`bcmgenet_isr0()` now returns `ULONG` following the AmigaOS interrupt-server
convention — `1` when the interrupt was ours, `0` otherwise — and returns `0`
early when no GENET status bits are pending, so it no longer claims interrupts it
did not service.  `bcmgenet_irq0_disable()` became `static inline` (it is internal
to the IRQ module and is dropped from the public header).

The unit task's RX "caught up" path now simply unmasks `UMAC_IRQ_RXDMA_DONE`
instead of clearing `INTRL2_CPU_CLEAR` before unmasking: the ISR has already
ACKed, and any RX writeback that latched the status during the drain window
re-fires the interrupt on unmask, so nothing is lost and the clear-before-unmask
race is gone.  The "just in case we got stuck" blind re-arm after the periodic
timer was removed.  The now-unused `phy_reset()` declaration was deleted.

### Debug output now follows the stack-wide backend

`genet.device` and the `runtime-config` static library no longer hard-code
`-DDEBUG`.  Debug output is selected through emu68-common's stack-wide
`EMU68_DEBUG_BACKEND` (`pistorm` | `serial` | `off`): the builds now call
`emu68_debug_backend_definitions()` for the compile defines and
`emu68_debug_backend_finalize(genet.device ROMABLE)` in place of the direct
`emu68_rom_check` (for the `serial` backend this links `debug.lib` plus the weak
`__divsi3` glue and skips the ROM check).  `DumpGenetRuntimeConfig()` is now
compiled out entirely when `DEBUG` is unset, with the call site retained as a
`((void)0)` no-op.

---

## Build / portability

### NDK 3.9 (and older) compatibility

The driver now builds cleanly against newer and older NDKs as well as the target
NDK 3.2:

- `internal_stats.last_start` is typed `struct timeval` rather than the
  NDK-3.2-only `TimeVal_Type` (same layout, available on any NDK).
- `<minlist.h>` is included where the type-safe MinList wrappers are used
  (`unit_commands.c`, `unit_commands_mcast.c`, `unit_io.c`), picking up
  emu68-common's fallback macros on pre-3.2 NDKs.
- `bcmgenet-tx.c` includes `<exec/execbase.h>` explicitly for `DMA_ReadFromRAM`,
  and the `CachePreDMA()` / `CachePostDMA()` length arguments are `ULONG` so their
  addresses match the API prototype on any NDK.

### Built at `-O3`

`mem_zero()` calls were replaced with the standard `memset()` (emu68-common
dropped its `mem_zero` family), and the `runtime-config` library was brought from
`-Os` to `-O3` to match the rest of the driver.

### Versioning workflow

A `.github/workflows/versioning.yml` that gates every PR on a version bump + a
`RELEASE-NOTES.md` update + a clean build inside the stack, and auto-tags
`v<version>` on merge to `main`, via the stack's reusable
`component-versioning.yml` so all components stay in lock-step.

### `$VER` strings report `major.minor`; `genet-stats` carries a version tag

The driver's embedded `$VER:` string now uses
`PROJECT_VERSION_MAJOR.PROJECT_VERSION_MINOR` so it matches the reported library
version.  The `genet-stats` tool now embeds its own `$VER:` tag (via a `VERSTAG`
compile definition) so `version genet-stats` and `C:Version` report it.


# Release notes — genet.device 3.10

Changes since v3.8.

This is a small, mostly reliability-focused release. The headline change is a
reset guard that quiesces the GENET DMA engine before the machine resets,
together with a memory-management rework that separates DMA-reachable buffers
from CPU-only metadata.

---

## Breaking changes

None.

---

## New features

### Reset guard for DMA quiesce

The driver now installs a reset guard at device init that runs a "prepare for
reset" hook before the Amiga resets, covering both reset flavours that exist on
PiStorm/Emu68: the Ctrl-Amiga-Amiga keyboard reset-warning protocol and
`ColdReboot()` (`C:Reboot`, Installer, and similar callers).  When the unit is
online, the hook stops all GENET bus-master activity — RX/TX DMA, the MAC, and
interrupts — without releasing any resources, so the controller stops writing
into RAM that the next OS session will reuse.

The quiesce step is factored into a dedicated `bcmgenet_reset_quiesce()` path
that the normal stop path also uses.  On expunge the guard is removed only if
its `ColdReboot` vector has not been re-patched by another driver; otherwise the
device stays resident, as required for safe vector chaining.

---

## Bug fixes / Reliability

### Soft reboot while online no longer hangs the Amiga

Previously, soft-rebooting while the driver was online could leave the Amiga
stuck at boot, because the GENET DMA engine kept writing received frames into
RAM that the next OS session reused.  The new reset guard quiesces the
controller before the reset completes, fixing this hang.  The "Better shutdown /
reset handling" item has been removed from the planned-features list and the
corresponding known-bug entry is cleared.

### RX buffer cache coherency via `CachePreDMA()`

`bcmgenet_gmac_eth_start()` now issues `CachePreDMA()` over the RX buffer before
the controller is brought up, ensuring the buffer is cache-coherent before the
GENET DMA engine begins writing received frames into it.

### DMA-reachability check is now explicit

The TX path no longer decides whether an opener-supplied DMA buffer is usable by
comparing its address against a hardcoded Chip-RAM boundary (`> 0x1FFFFF`).  It
now calls `dma_addr_reachable()` against the unit's DMA context, which correctly
rejects Chip RAM and any Zorro/accelerator Fast RAM that the GENET DMA engine
cannot reach, falling back to a copy in those cases.

---

## Improvements

### Separate DMA and metadata memory pools

`GenetUnit` previously served every allocation from a single Exec pool.  It now
keeps two:

- A **region-restricted DMA pool** backed by a `dma_mem` context, so DMA buffers
  (the RX buffer, ring control structures' staging, and the TX buffer slab
  cache) are allocated from Emu68 (Pi-DRAM) RAM that the GENET DMA engine can
  actually reach.  With no device tree there is no reachable region and the unit
  refuses to open.
- An ordinary **Exec metadata pool** for CPU-only data (RX/TX control blocks,
  the PHY device, multicast range nodes, async control messages).

Allocation call sites were migrated accordingly, and `UnitOpen` now has a single
`fail:` cleanup path that tears down both pools on any error.

### Delay functions switched to milliseconds

Busy-wait delays in the GENET and PHY code now use `delay_ms()` instead of
`delay_us()` with microsecond arithmetic (for example `delay_us(50 * 1000)`
becomes `delay_ms(50)`).  This is a readability change with no behavioural
effect.

### ROM-ability check enforced at build time

The build now runs `emu68_rom_check()` against `genet.device` so that no
writable `.data`/`.bss` can sneak in, keeping the driver ROM-able.


# emu68-genet 2.2 to 3.8 Release Notes

This document summarizes the user-visible and maintenance-relevant changes between the repository tag `v2.2` and the current `v3.8`.

## Summary

- `3.x` keeps the interrupt-driven GENET design introduced in `2.0`, but substantially reworks request flow, statistics exposure, and memory management.
- The biggest user-visible additions after `2.2` are hardware-backed statistics, throughput sampling, and the `genet-stats` utility.
- The remaining changes are mostly reliability and maintainability work in the TX/RX, opener, and command-processing paths.
- See also the updated README.md

## Release Delta

- Updated the driver to the current `emu68-common` support library.
- Removed older repo coupling and aligned the build/package layout with the current driver-stack structure.
- Improved type safety and consistency across the driver code.
- Switched interrupt discovery to `DT_GetInterrupt()` and improved IRQ number decoding.
- Added GENET hardware MIB register support.
- Added `S2_GETSPECIALSTATS` support with GENET-specific counters.
- Added `S2_GETEXTENDEDGLOBALSTATS` support for 64-bit-capable extended device statistics.
- Exported counters for RX/TX error buckets, IRQ activity, MAC/MIB packet counters, and related diagnostics.
- Expanded internal statistics tracking.
- Enabled richer extended global stats reporting based on the new internal counters.
- Added `S2_SAMPLE_THROUGHPUT` support.
- Added the `genet-stats` tool, a simple live viewer for extended stats, special stats, and throughput samples.
- `UnitClose` now uses synchronous control submission for opener removal.
- Added async event reporting support.
- Restricted `CMD_WRITE` handling to user context.
- Moved TX reclaim work into the bottom half.
- Implemented an SPSC ring for `CMD_READ` handling.
- Reworked TX allocation to use the common slab allocator.
- Switched RX ring buffer allocation to `dma_zalloc()`.
- Disabled TX IRQ use in favour of the updated reclaim flow.
- Replaced a semaphore-based path with `Forbid()` / `Permit()` in the current head.

## User-Visible Effects

- Better observability: you can now inspect extended global stats, special stats, and throughput directly through the device interface or the `genet-stats` tool.
- Better robustness under load.
- Runtime config keys from the `2.2` README remain valid on the current branch.
- The driver is ROM-able.
- Updated genet.prefs defaults.

## Still Known

- Soft reboot while the driver is online may still hang the Amiga.

# Release notes — genet.device 2.1

## What's Changed
* Fix crash when gic400.library cannot be opened @rondoval in https://github.com/rondoval/emu68-genet-driver/pull/16
Driver will no longer crash. It will not load, still, as the library is essential.

**Full Changelog**: https://github.com/rondoval/emu68-genet-driver/compare/v2.0...v2.1


# Release notes — genet.device 2.0

## What's Changed
* transition to cmake by @rondoval in https://github.com/rondoval/emu68-genet-driver/pull/12
* interrupt support by @rondoval in https://github.com/rondoval/emu68-genet-driver/pull/13


**Full Changelog**: https://github.com/rondoval/emu68-genet-driver/compare/v1.3...v2.0


# Release notes — genet.device 1.3

## What's Changed

- Fixes for https://github.com/rondoval/emu68-genet-driver/issues/4, https://github.com/rondoval/emu68-genet-driver/issues/8, https://github.com/rondoval/emu68-genet-driver/issues/9.
- Compatibility improvements for Miami DX and AmiTCP.
- Compatibility with KS 3.0&up.
- ENV:genet.prefs configuration file (see readme for details. Miami DX requires special flag to be set in this file).
- Zero-copy TX DMA disabled by default as it requires 64 byte alignment for stable operation.
- Internal queues and polling backoff algorithm reworked.
- A few updates in internal stats - these are also dumped to serial port every 15s (for now).

**Full Changelog**: https://github.com/rondoval/emu68-genet-driver/compare/v1.2...v1.3


# Release notes — genet.device 1.2

## What's Changed
* TX ring rewritten - DMACopyFromBuff used to put TX data directly on TX ring
* Quite some cleanup and refactoring
* RX coalescing initialization - currently useless, as we don't have interrupts exposed to M68k
* Hardware MDF filter used if there are less than 15 multicast addresses in use
* Promiscuous mode support
* "MTU bug" fixed

Now, caveats:
- Neither promiscuous mode nor multicasts were tested - simply because I did not have time to find out which software is using these. So if you know - let me know :)
- DMACopyFromBuff & Roadshow - it seems that only about 25-30% of the buffers exposed by Roadshow are DMA capable... so 70% of packets still require copying to internal buffers. No idea if this is just how Roadshow is, or can this be fixed in config files. As a side note, I am working on enabling use of DMACopyToBuff - though this seems unlikely without lots of trickery. It appears that SANA-II arch just does not fit to how modern NICs work...
- There's some risk this is not stable, thus marking it as a pre-release. Although I'm afraid what I see is an unrelated hardware issue with my Amiga.

**Full Changelog**: https://github.com/rondoval/emu68-genet-driver/compare/v1.1...v1.2


# Release notes — genet.device 1.1

- license info
- bugfixes
- multicast filtering
- readme added

**Full Changelog**: https://github.com/rondoval/emu68-genet-driver/compare/v1.0.0...v1.1


# Release notes — genet.device 1.0.0

genet.device is an Amiga OS driver for the Gigabit Ethernet controller on the Raspberry Pi 4B.

This is a very early, untested version of the driver.
Currently multicast support is disabled, promiscuous mode is not being configured. No packet statistics.
The way the hardware is configured is quite primitive... will improve in the future.

**Note:** it requires a patch to Emu68 in order to initialize correctly.
