---
id: save-system
title: Save System
sidebar_label: Save System
sidebar_position: 3
description: Debounced SPIFFS persistence, atomic writes, and the emergency power-loss flash slot that protect battery saves from flash wear and corruption.
---

# Save System

[`cartridge/save_store.*`](https://github.com/) persists the 32 KB cartridge save to
internal flash **without ever writing flash in the Joy-Bus path**. It balances three
goals: don't lose in-game saves, don't wear out flash, and don't stall the
timing-critical response.

## Write path overview

```text
console write ──► gb_cartridge save cache (SRAM, marks dirty, bumps write_seq)
                          │
            runtime loop  ▼
        save_store_service(now_ms, allow_flash_write)
            • only when the accessory bus is fully idle
            • only after writes have been quiet PERSIST_DEBOUNCE_MS (900 ms)
                          │
                          ▼
        atomic write: save.tmp ──rename──► save.srm   (SPIFFS)
```

## Debounced, idle-gated persistence

`save_store_service()` watches the cartridge's `write_seq`. When it changes it restarts
a quiet timer; it only flushes once writes have been quiet for `PERSIST_DEBOUNCE_MS`
(900 ms) **and** the runtime says the bus is idle (`allow_flash_write`). This means an
active save burst is written **once** at the end, not on every block — protecting flash
and avoiding mid-access stalls.

## Atomic write

`persist_now()` writes to `save.tmp`, fsyncs/closes with error checks, then `rename()`s
over `save.srm` (POSIX atomic replace, with a remove+rename fallback if the SPIFFS VFS
rejects replacement). A torn write never corrupts the live save.

## Loading and precedence

`save_store_load()` reads `/spiffs/save.srm`, requires it to be exactly 32 KB, and loads
it into the cartridge with precedence over the blank default. The result is reported as
one of: `persisted`, `missing`, `invalid_size`, `no_memory`, `read_failed`. See the
[boot sequence](../architecture/boot-sequence#save-load-precedence) for the full
precedence order. The bundled default is covered in [Default Save](./default-save).

## Power-loss safety

A dedicated `emgsave` raw partition is kept **pre-erased ("armed")**. On a detected
power loss the [power monitor](#power-monitor) calls
`save_store_flush_on_power_loss()`, which writes the dirty save **program-only** (no
erase, no filesystem) during the supply ride-down:

1. Write the 32 KB body, then the header (magic, version, `write_seq`, length, CRC32).
   Body-first so a torn write leaves an invalid header that is rejected on boot.
2. On the next boot, `save_store_recover_power_loss_slot()` validates magic/version/
   length/CRC, adopts the slot's save as authoritative, rewrites `save.srm` from it,
   and re-arms the slot.

```c
bool save_store_flush_on_power_loss(void);     // ride-down program-only write
void save_store_arm_power_loss_slot(void);     // pre-erase the slot
bool save_store_recover_power_loss_slot(void); // boot-time merge
```

## Power monitor

[`cartridge/power_monitor.*`](https://github.com/) watches `PIN_POWER_LOSS_SENSE`. On a
falling edge (power dropping) a high-priority task stops Wi-Fi (to extend hold-up time)
and triggers the emergency flush — safe because at console power-off the Joy-Bus is
already idle. Set the sense GPIO to `-1` to disable the monitor (default
`POWER_LOSS_SENSE_GPIO = -1`).

## Web uploads and exclusivity

While a web ROM/save upload rewrites flash, `save_store_set_busy(true)` suppresses the
runtime save service so the two don't fight. `save_store_force_persist()` writes the
current save immediately (used after a web save upload, when the console is paused), and
`save_store_reset()` deletes the persisted save so the bundled default is restored next
boot.

## Observability

`save_store_status()` exposes load/flush results, dirty/pending/persisted flags, the
observed and last-persisted write sequences, flush counts, and timestamps. The periodic
runtime log prints `save[L# D# P#]` (loaded / dirty / persisted) alongside Transfer Pak
state.
