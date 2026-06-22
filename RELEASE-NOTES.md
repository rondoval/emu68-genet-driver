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
