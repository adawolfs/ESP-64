#include "mbc1_mapper.h"

#include "esp_attr.h"

namespace {
uint8_t IRAM_ATTR normalize_rom_bank(uint8_t bank) {
  const uint8_t low = bank & 0x1F;
  if (low == 0) bank++;
  return bank;
}

// GB header byte 0x147 values 0x19-0x1E are MBC5 variants.
bool IRAM_ATTR is_mbc5(uint8_t cartridge_type) {
  return cartridge_type >= 0x19 && cartridge_type <= 0x1E;
}

// 0x0F-0x13 are MBC3 variants (Pokemon Red/Blue are MBC3+RAM+battery, 0x13).
bool IRAM_ATTR is_mbc3(uint8_t cartridge_type) {
  return cartridge_type >= 0x0F && cartridge_type <= 0x13;
}
}  // namespace

void mbc1_mapper_init(Mbc1Mapper *mapper, uint8_t cartridge_type,
                      uint16_t rom_bank_count, uint8_t ram_bank_count) {
  if (!mapper) return;
  mapper->cartridge_type = cartridge_type;
  mapper->rom_low5 = 1;
  mapper->bank_high2 = 0;
  mapper->mode = 0;
  mapper->mbc5_rom_bank = 1;  // MBC5 powers up with ROM bank 1 selected
  mapper->mbc5_ram_bank = 0;
  mapper->ram_enabled = false;
  mapper->rom_bank_count = rom_bank_count ? rom_bank_count : 2;
  mapper->ram_bank_count = ram_bank_count;
}

void IRAM_ATTR mbc1_mapper_write(Mbc1Mapper *mapper, uint16_t address,
                                uint8_t value) {
  if (!mapper) return;
  if (is_mbc3(mapper->cartridge_type)) {
    if (address < 0x2000) {
      mapper->ram_enabled = (value & 0x0F) == 0x0A;
    } else if (address < 0x4000) {  // 7-bit ROM bank (0 -> 1)
      uint8_t bank = value & 0x7F;
      if (bank == 0) bank = 1;
      mapper->mbc5_rom_bank = bank;
    } else if (address < 0x6000) {  // RAM bank 0-3, or RTC register select 0x08-0x0C
      mapper->mbc5_ram_bank = value;  // values > 0x03 = RTC (no RAM), handled in ram_offset
    }
    // 0x6000-0x7FFF latch clock data: ignored (Gen-1 R/B have no RTC).
    return;
  }
  if (is_mbc5(mapper->cartridge_type)) {
    if (address < 0x2000) {
      mapper->ram_enabled = (value & 0x0F) == 0x0A;
    } else if (address < 0x3000) {  // ROM bank low 8 bits
      mapper->mbc5_rom_bank =
          static_cast<uint16_t>((mapper->mbc5_rom_bank & 0x100) | value);
    } else if (address < 0x4000) {  // ROM bank bit 8
      mapper->mbc5_rom_bank = static_cast<uint16_t>(
          (mapper->mbc5_rom_bank & 0x0FF) | ((value & 0x01) << 8));
    } else if (address < 0x6000) {  // RAM bank (4 bits)
      mapper->mbc5_ram_bank = value & 0x0F;
    }
    // MBC5 has no mode register; 0x6000-0x7FFF is ignored.
    return;
  }
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

uint8_t IRAM_ATTR mbc1_mapper_rom_bank(const Mbc1Mapper *mapper) {
  if (!mapper) return 1;
  uint8_t bank = mapper->rom_low5 & 0x1F;
  if (mapper->mode == 0) bank |= mapper->bank_high2 << 5;
  bank = normalize_rom_bank(bank);
  if (mapper->rom_bank_count) bank %= mapper->rom_bank_count;
  if (bank == 0) bank = 1;
  return bank;
}

uint8_t IRAM_ATTR mbc1_mapper_ram_bank(const Mbc1Mapper *mapper) {
  if (!mapper || mapper->mode == 0) return 0;
  uint8_t bank = mapper->bank_high2 & 0x03;
  if (mapper->ram_bank_count) bank %= mapper->ram_bank_count;
  return bank;
}

size_t IRAM_ATTR mbc1_mapper_rom_offset(const Mbc1Mapper *mapper,
                                        uint16_t gb_address) {
  if (!mapper) return gb_address;
  if (is_mbc5(mapper->cartridge_type) || is_mbc3(mapper->cartridge_type)) {
    // 0x0000-0x3FFF is always ROM bank 0 (never remapped, unlike MBC1 mode 1).
    if (gb_address < 0x4000) return gb_address;
    if (gb_address < 0x8000) {
      uint16_t bank = mapper->mbc5_rom_bank;
      if (mapper->rom_bank_count) bank %= mapper->rom_bank_count;
      return static_cast<size_t>(bank) * 0x4000u + (gb_address - 0x4000u);
    }
    return 0;
  }
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

size_t IRAM_ATTR mbc1_mapper_ram_offset(const Mbc1Mapper *mapper,
                                        uint16_t gb_address, bool *enabled) {
  const bool is_enabled = mapper && mapper->ram_enabled &&
                          gb_address >= 0xA000 && gb_address < 0xC000;
  uint8_t bank = 0;
  bool resolved_enabled = is_enabled;
  if (is_mbc3(mapper->cartridge_type)) {
    // RAM bank 0-3; a register select >= 0x08 means RTC, which has no save RAM.
    if (mapper->mbc5_ram_bank > 0x03) {
      resolved_enabled = false;
    } else {
      bank = mapper->mbc5_ram_bank;
      if (mapper->ram_bank_count) bank %= mapper->ram_bank_count;
    }
  } else if (is_mbc5(mapper->cartridge_type)) {
    bank = mapper->mbc5_ram_bank;
    if (mapper->ram_bank_count) bank %= mapper->ram_bank_count;
  } else {
    bank = mbc1_mapper_ram_bank(mapper);
  }
  if (enabled) *enabled = resolved_enabled;
  if (!resolved_enabled) return 0;
  return static_cast<size_t>(bank) * 0x2000u + (gb_address - 0xA000u);
}
