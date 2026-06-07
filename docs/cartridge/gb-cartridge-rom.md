---
id: gb-cartridge-rom
title: GB Cartridge & ROM
sidebar_label: GB Cartridge & ROM
sidebar_position: 1
description: How the ROM partition is memory-mapped and parsed, and how the cartridge serves ROM and save-RAM reads to the Transfer Pak.
---

# GB Cartridge & ROM

[`cartridge/gb_cartridge.*`](https://github.com/) owns the active Game Boy cartridge:
it memory-maps the ROM partition, parses the header, and holds the 32 KB save RAM
cache.

## Memory-mapped ROM

At init, `gb_cartridge_init()` finds the `rom` partition (subtype `0x41`) and
`esp_partition_mmap`'s it so the Transfer Pak read path reads it like a rodata array.
`gb_cartridge_read_rom_block()` serves a contiguous run with a single `memcpy` — the
key to fitting a 32-byte block inside the Joy-Bus response window.

```c
uint8_t gb_cartridge_read_rom(size_t offset);
void    gb_cartridge_read_rom_block(size_t offset, uint8_t *out, size_t len);
```

Out-of-range reads set a `bounds_fault` flag and return `0xFF` rather than reading past
the mapping.

## Header parsing

`gb_cartridge_parse_header()` validates and extracts the standard GB header at
`0x134`–`0x14F`:

- **Title** (`0x134`), **cartridge type** (`0x147`), **ROM size code** (`0x148`),
  **RAM size code** (`0x149`), **header checksum** (`0x14D`).
- The header checksum is verified (`x = x − byte − 1` over `0x134`–`0x14C`).
- Only known mapper families are accepted: ROM-only/MBC1 (`≤ 0x03`), MBC3
  (`0x0F`–`0x13`), MBC5 (`0x19`–`0x1E`).

ROM and RAM bank counts are derived from the size codes. The parsed status is exposed
via `gb_cartridge_status()`:

```c
struct GbCartridgeStatus {
  bool rom_loaded, save_loaded, save_stubbed, bounds_fault;
  char title[17];
  uint8_t cartridge_type, ram_bank_count, header_checksum;
  uint16_t rom_bank_count;
};
```

A device may boot with **no valid ROM** (the portal can upload one); `rom_loaded` stays
false and reads return `0xFF`.

## Save RAM access

The 32 KB save cache (`save_stub`) is served per the active RAM bank, respecting the
MBC RAM-enable and banking state:

```c
uint8_t gb_cartridge_read_mapped_ram(const Mbc1Mapper *m, uint16_t gb_address);
void    gb_cartridge_write_mapped_ram(const Mbc1Mapper *m, uint16_t gb_address, uint8_t value);
```

Writes that change a byte mark the save **dirty**, bump a write sequence counter, and
record debug info (last offset / GB address / value). Persistence happens later from
the runtime loop — never in the write path. See [Save System](./save-system).

## Replacing the ROM at runtime

The portal rewrites the ROM partition in place:

```c
bool gb_cartridge_rom_write_begin(size_t total_len);   // erase
bool gb_cartridge_rom_write_chunk(size_t off, const uint8_t *d, size_t n);
bool gb_cartridge_rom_write_finish(void);              // re-map + re-parse
```

The ROM is unmapped between `begin` and a successful `finish`, so the Joy-Bus transport
must be paused around these calls (the portal does this). Replacing the ROM also clears
the save RAM so a different game's SRAM isn't presented to the console.
