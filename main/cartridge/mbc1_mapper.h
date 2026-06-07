#ifndef MBC1_MAPPER_H
#define MBC1_MAPPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct Mbc1Mapper {
  uint8_t cartridge_type;  // raw GB header byte 0x147; selects MBC1 vs MBC5
  // MBC1 banking state
  uint8_t rom_low5;
  uint8_t bank_high2;
  uint8_t mode;
  // MBC5 banking state
  uint16_t mbc5_rom_bank;  // 9-bit ROM bank
  uint8_t mbc5_ram_bank;   // 4-bit RAM bank
  bool ram_enabled;
  uint16_t rom_bank_count;
  uint8_t ram_bank_count;
};

void mbc1_mapper_init(Mbc1Mapper *mapper, uint8_t cartridge_type,
                      uint16_t rom_bank_count, uint8_t ram_bank_count);
void mbc1_mapper_write(Mbc1Mapper *mapper, uint16_t address, uint8_t value);
size_t mbc1_mapper_rom_offset(const Mbc1Mapper *mapper, uint16_t gb_address);
size_t mbc1_mapper_ram_offset(const Mbc1Mapper *mapper, uint16_t gb_address,
                              bool *enabled);
uint8_t mbc1_mapper_rom_bank(const Mbc1Mapper *mapper);
uint8_t mbc1_mapper_ram_bank(const Mbc1Mapper *mapper);

#endif
