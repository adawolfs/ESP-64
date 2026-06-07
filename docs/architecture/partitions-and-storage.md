---
id: partitions-and-storage
title: Partitions & Storage
sidebar_label: Partitions & Storage
sidebar_position: 3
description: The internal-flash partition map — firmware, ROM, SPIFFS storage, and the emergency save slot — and what lives in each.
---

# Partitions & Storage

Everything lives in the ESP32-C3's internal 4 MB flash. The layout is defined in
[`partitions.csv`](https://github.com/).

## Partition map

| Name | Type | SubType | Offset | Size | Purpose |
| --- | --- | --- | --- | --- | --- |
| `nvs` | data | nvs | `0x9000` | `0x6000` | Configuration |
| `phy_init` | data | phy | `0xF000` | `0x1000` | RF calibration |
| `factory` | app | factory | `0x10000` | `0x180000` | Firmware (1.5 MB) |
| `rom` | data | `0x41` | `0x190000` | `0x100000` | Active Game Boy ROM (1 MB), memory-mapped |
| `storage` | data | spiffs | `0x290000` | `0x167000` | SPIFFS: web UI bundle + `save.srm` |
| `emgsave` | data | `0x40` | `0x3F7000` | `0x9000` | Emergency power-loss save slot |

:::warning Offsets are authoritative in `partitions.csv`
Tooling and docs reference `0x190000` (ROM) and `0x290000` (storage). If you edit the
partition table, update every flash command and re-flash once with a full erase.
:::

## `rom` partition

A raw 1 MB region holding one active Game Boy ROM. It is `esp_partition_mmap`'d at
boot so the Transfer Pak read path reads it as fast as a rodata array. Replaced via
the web portal (`POST /api/rom`) or flashed from `roms/active.gb` by the PlatformIO
post-script. See [GB Cartridge & ROM](../cartridge/gb-cartridge-rom).

## `storage` SPIFFS partition

A SPIFFS filesystem mounted at `/spiffs`. It carries two kinds of content:

- the **web UI bundle** (built from `data/` by Vite),
- the **active/default save** `save.srm` (32 KB).

Because it is a real filesystem, it holds the *mutable* persisted user save. The
emulator's debounced persistence rewrites `/spiffs/save.srm` here. The bundled default
save is shipped inside this same image — see [Default Save](../cartridge/default-save).

:::note Self-healing mount
SPIFFS is mounted with `format_if_mount_failed = true`, so a corrupt or never-created
FS reformats to an empty (but valid) filesystem. The web portal also embeds a fallback
UI, so an empty FS still serves the portal — but an empty FS has no `save.srm`.
:::

## `emgsave` emergency slot

A small raw partition (36 KB = 9 sectors: 8 for a 32 KB save body + 1 for a header)
that is kept **pre-erased ("armed")**. On power loss the dirty save is written
**program-only** (no erase, no filesystem) during the supply ride-down, then merged
into `storage/save.srm` on the next boot. See
[Save System → Power-loss safety](../cartridge/save-system#power-loss-safety).

## Save RAM cache

The active 32 KB save lives in internal SRAM (`save_stub` in `gb_cartridge`). Console
writes update it directly during Joy-Bus transactions; persistence to flash is always
deferred to the runtime loop, never performed in the response path.
