---
id: accessory-transfer-pak
title: Accessory & Transfer Pak
sidebar_label: Accessory & Transfer Pak
sidebar_position: 3
description: How accessory read/write commands map to the Transfer Pak registers and the Game Boy cartridge through the MBC mapper.
---

# Accessory & Transfer Pak

Accessory commands (`0x02` read, `0x03` write) carry a 16-bit block address and a
32-byte payload. [`joybus/n64_accessory.*`](https://github.com/) validates the frame,
computes/checks the accessory CRC, and routes the access to the Transfer Pak.

## Address space and registers

The Transfer Pak ([`joybus/transfer_pak.*`](https://github.com/)) decodes the N64
accessory address space into control registers and a banked window onto the Game Boy
cartridge:

```c
struct TransferPakStatus {
  bool      powered;         // power register
  bool      access_enabled;  // access-mode register
  uint8_t   bank;            // bank-select register
  uint8_t   status;          // status byte returned to the console
  uint32_t  reads, writes, invalid_accesses;
  Mbc1Mapper mapper;         // current GB banking state
};
```

The console's typical sequence is: enable **power** → enable **access** → select a
**bank** → read/write 32-byte blocks of the mapped cartridge region. Invalid accesses
(out of range / disabled) are counted and return safe `0xFF`.

## Block reads and writes

```c
uint8_t transfer_pak_read_byte(uint16_t address);
void    transfer_pak_write_byte(uint16_t address, uint8_t value);
void    transfer_pak_read_block(uint16_t address, uint8_t *out, size_t len);
void    transfer_pak_write_block(uint16_t address, const uint8_t *data, size_t len);
```

`TRANSFER_PAK_BLOCK_SIZE` is 32 bytes. Reads of the cartridge ROM window are served by
a single `memcpy` from the memory-mapped ROM (fast enough for the response deadline);
reads of the save window return the SRAM cache for the active RAM bank; writes to the
save window update the cache and mark it dirty.

## Mapping through the cartridge

The Transfer Pak's banked window is translated into ROM/RAM offsets by the
[MBC mapper](../cartridge/mbc-mappers) and resolved against the
[GB cartridge](../cartridge/gb-cartridge-rom):

```text
N64 accessory addr ──► transfer_pak (bank/access) ──► mbc1_mapper (bank arithmetic)
                                                  ──► gb_cartridge (ROM mmap / save cache)
```

## GB Tower (Pokémon Stadium) quirks

`cartridge/pokemon_stadium_compat.*` captures handshake/header quirks GB Tower expects.
Hardware verification confirmed the full path on a real console: Transfer Pak
detection, Pokémon Yellow launching in GB Tower, save loading, gameplay, and in-game
saving. See [Hardware Notes](./hardware).
