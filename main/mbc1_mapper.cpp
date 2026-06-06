#include "mbc1_mapper.h"

namespace {
uint8_t normalize_rom_bank(uint8_t bank) {
  const uint8_t low = bank & 0x1F;
  if (low == 0) bank++;
  return bank;
}
}  // namespace

void mbc1_mapper_init(Mbc1Mapper *mapper, uint16_t rom_bank_count,
                      uint8_t ram_bank_count) {
  if (!mapper) return;
  mapper->rom_low5 = 1;
  mapper->bank_high2 = 0;
  mapper->mode = 0;
  mapper->ram_enabled = false;
  mapper->rom_bank_count = rom_bank_count ? rom_bank_count : 2;
  mapper->ram_bank_count = ram_bank_count;
}

void mbc1_mapper_write(Mbc1Mapper *mapper, uint16_t address, uint8_t value) {
  if (!mapper) return;
  if (address < 0x2000) {
    mapper->ram_enabled = (value & 0x0F) == 0x0A;
    return;
  }
  if (address < 0x4000) {
    mapper->rom_low5 = value & 0x1F;
    if (mapper->rom_low5 == 0) mapper->rom_low5 = 1;
    return;
  }
  if (address < 0x6000) {
    mapper->bank_high2 = value & 0x03;
    return;
  }
  if (address < 0x8000) {
    mapper->mode = value & 0x01;
  }
}

uint8_t mbc1_mapper_rom_bank(const Mbc1Mapper *mapper) {
  if (!mapper) return 1;
  uint8_t bank = mapper->rom_low5 & 0x1F;
  if (mapper->mode == 0) bank |= mapper->bank_high2 << 5;
  bank = normalize_rom_bank(bank);
  if (mapper->rom_bank_count) bank %= mapper->rom_bank_count;
  if (bank == 0) bank = 1;
  return bank;
}

uint8_t mbc1_mapper_ram_bank(const Mbc1Mapper *mapper) {
  if (!mapper || mapper->mode == 0) return 0;
  uint8_t bank = mapper->bank_high2 & 0x03;
  if (mapper->ram_bank_count) bank %= mapper->ram_bank_count;
  return bank;
}

size_t mbc1_mapper_rom_offset(const Mbc1Mapper *mapper, uint16_t gb_address) {
  if (!mapper) return gb_address;
  if (gb_address < 0x4000) {
    uint8_t bank = 0;
    if (mapper->mode != 0) bank = mapper->bank_high2 << 5;
    if (mapper->rom_bank_count) bank %= mapper->rom_bank_count;
    return static_cast<size_t>(bank) * 0x4000u + gb_address;
  }
  if (gb_address < 0x8000) {
    return static_cast<size_t>(mbc1_mapper_rom_bank(mapper)) * 0x4000u +
           (gb_address - 0x4000u);
  }
  return 0;
}

size_t mbc1_mapper_ram_offset(const Mbc1Mapper *mapper, uint16_t gb_address,
                              bool *enabled) {
  const bool is_enabled = mapper && mapper->ram_enabled &&
                          gb_address >= 0xA000 && gb_address < 0xC000;
  if (enabled) *enabled = is_enabled;
  if (!is_enabled) return 0;
  return static_cast<size_t>(mbc1_mapper_ram_bank(mapper)) * 0x2000u +
         (gb_address - 0xA000u);
}
