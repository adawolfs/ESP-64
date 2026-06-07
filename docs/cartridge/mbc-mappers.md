---
id: mbc-mappers
title: MBC Mappers
sidebar_label: MBC Mappers
sidebar_position: 2
description: The MBC1 / MBC5 banking model that translates Game Boy addresses into ROM and save-RAM offsets.
---

# MBC Mappers

[`cartridge/mbc1_mapper.*`](https://github.com/) implements the Game Boy memory bank
controller arithmetic that turns a 16-bit GB address into a flat ROM or save-RAM
offset. Despite the name, it covers both **MBC1** and **MBC5** families (selected by
the raw cartridge-type byte).

## State

```c
struct Mbc1Mapper {
  uint8_t  cartridge_type;   // GB header 0x147; selects MBC1 vs MBC5
  // MBC1
  uint8_t  rom_low5, bank_high2, mode;
  // MBC5
  uint16_t mbc5_rom_bank;    // 9-bit ROM bank
  uint8_t  mbc5_ram_bank;    // 4-bit RAM bank
  bool     ram_enabled;
  uint16_t rom_bank_count;
  uint8_t  ram_bank_count;
};
```

## Banking registers

`mbc1_mapper_write(mapper, address, value)` interprets writes to the MBC register
regions exactly as a real cartridge would:

- **RAM enable** — gates save-RAM access (`ram_enabled`).
- **ROM bank** — low bits (MBC1: 5-bit `rom_low5`; MBC5: 8-bit + a 9th bit).
- **RAM bank / upper ROM bank** — MBC1's 2 high bits (`bank_high2`) or MBC5's 4-bit
  RAM bank.
- **Mode select** — MBC1's banking mode (`mode`).

## Resolving offsets

```c
size_t  mbc1_mapper_rom_offset(const Mbc1Mapper *m, uint16_t gb_address);
size_t  mbc1_mapper_ram_offset(const Mbc1Mapper *m, uint16_t gb_address, bool *enabled);
uint8_t mbc1_mapper_rom_bank(const Mbc1Mapper *m);
uint8_t mbc1_mapper_ram_bank(const Mbc1Mapper *m);
```

- `rom_offset` maps `0x0000`–`0x7FFF` to a flat ROM offset (bank 0 fixed, the high
  bank windowed by the current bank registers).
- `ram_offset` maps `0xA000`–`0xBFFF` to a save-RAM offset and reports whether RAM is
  currently enabled via `*enabled`.

The mapper is owned by the [Transfer Pak](../joybus/accessory-transfer-pak) status and
fed addresses derived from the console's bank-select register. The resolved offsets are
read from / written to the [GB cartridge](./gb-cartridge-rom) ROM mmap and save cache.

:::note Scope
The first target is Pokémon Gen 1 (MBC1, 32 KB save). MBC3 RTC behavior is out of scope
for now; the mapper accepts MBC3 ROM/RAM access but does not emulate the real-time
clock.
:::
