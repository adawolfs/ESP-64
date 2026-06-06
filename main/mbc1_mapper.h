#ifndef MBC1_MAPPER_H
#define MBC1_MAPPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct Mbc1Mapper {
  uint8_t rom_low5;
  uint8_t bank_high2;
  uint8_t mode;
  bool ram_enabled;
  uint16_t rom_bank_count;
  uint8_t ram_bank_count;
};

void mbc1_mapper_init(Mbc1Mapper *mapper, uint16_t rom_bank_count,
                      uint8_t ram_bank_count);
void mbc1_mapper_write(Mbc1Mapper *mapper, uint16_t address, uint8_t value);
size_t mbc1_mapper_rom_offset(const Mbc1Mapper *mapper, uint16_t gb_address);
size_t mbc1_mapper_ram_offset(const Mbc1Mapper *mapper, uint16_t gb_address,
                              bool *enabled);
uint8_t mbc1_mapper_rom_bank(const Mbc1Mapper *mapper);
uint8_t mbc1_mapper_ram_bank(const Mbc1Mapper *mapper);

#endif
