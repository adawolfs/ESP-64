#include "gb_cartridge.h"

#include <string.h>

#include "esp_attr.h"
#include "gbrom.h"
#include "gbsave.h"

namespace {
constexpr size_t kHeaderTitleOffset = 0x134;
constexpr size_t kHeaderTypeOffset = 0x147;
constexpr size_t kHeaderRomSizeOffset = 0x148;
constexpr size_t kHeaderRamSizeOffset = 0x149;
constexpr size_t kHeaderChecksumOffset = 0x14D;
constexpr size_t kSaveRamSize = 32u * 1024u;

GbCartridgeStatus status = {};
uint8_t save_stub[kSaveRamSize] = {};
bool save_dirty = false;
uint32_t save_write_seq = 0;  // bumped on each actual save byte change

uint16_t rom_banks_from_code(uint8_t code) {
  switch (code) {
    case 0x00: return 2;
    case 0x01: return 4;
    case 0x02: return 8;
    case 0x03: return 16;
    case 0x04: return 32;
    case 0x05: return 64;
    case 0x06: return 128;
    case 0x07: return 256;
    case 0x08: return 512;
    case 0x52: return 72;
    case 0x53: return 80;
    case 0x54: return 96;
    default: return 2;
  }
}

uint8_t ram_banks_from_code(uint8_t code) {
  switch (code) {
    case 0x01:
    case 0x02:
      return 1;
    case 0x03:
      return 4;
    case 0x04:
      return 16;
    case 0x05:
      return 8;
    default:
      return 0;
  }
}
}  // namespace

void gb_cartridge_init(void) {
  memset(&status, 0, sizeof(status));
  status.rom_loaded = gb_rom_size > 0x150;
  if (status.rom_loaded) {
    memcpy(status.title, &gb_rom[kHeaderTitleOffset], 16);
    status.title[16] = '\0';
    status.cartridge_type = gb_rom[kHeaderTypeOffset];
    status.rom_bank_count = rom_banks_from_code(gb_rom[kHeaderRomSizeOffset]);
    status.ram_bank_count = ram_banks_from_code(gb_rom[kHeaderRamSizeOffset]);
    status.header_checksum = gb_rom[kHeaderChecksumOffset];
  }

  // Seed cartridge save RAM from the bundled .srm. On a size mismatch fall back
  // to a 0xFF stub so reads stay well-defined. A persisted save (if any) is
  // applied later via gb_cartridge_load_save() once SPIFFS is mounted.
  if (static_cast<size_t>(gb_save_size) == sizeof(save_stub)) {
    memcpy(save_stub, gb_save, sizeof(save_stub));
    status.save_loaded = true;
    status.save_stubbed = false;
  } else {
    memset(save_stub, 0xFF, sizeof(save_stub));
    status.save_loaded = false;
    status.save_stubbed = true;
  }
}

const GbCartridgeStatus &gb_cartridge_status(void) { return status; }

uint8_t IRAM_ATTR gb_cartridge_read_rom(size_t offset) {
  if (!status.rom_loaded || offset >= static_cast<size_t>(gb_rom_size)) {
    status.bounds_fault = true;
    return 0xFF;
  }
  return gb_rom[offset];
}

void IRAM_ATTR gb_cartridge_read_rom_block(size_t offset, uint8_t *out,
                                           size_t len) {
  if (!out) return;
  // Bulk copy a contiguous ROM run in one shot. Reading the cartridge byte by
  // byte through the mapper was far too slow inside the Joy-Bus response window
  // (~69 us for 32 bytes); a single memcpy keeps it well under the deadline.
  if (!status.rom_loaded ||
      offset + len > static_cast<size_t>(gb_rom_size)) {
    status.bounds_fault = true;
    memset(out, 0xFF, len);
    return;
  }
  memcpy(out, &gb_rom[offset], len);
}

uint8_t IRAM_ATTR gb_cartridge_read_mapped_rom(const Mbc1Mapper *mapper,
                                               uint16_t gb_address) {
  if (gb_address >= 0x8000) {
    status.bounds_fault = true;
    return 0xFF;
  }
  return gb_cartridge_read_rom(mbc1_mapper_rom_offset(mapper, gb_address));
}

uint8_t IRAM_ATTR gb_cartridge_read_mapped_ram(const Mbc1Mapper *mapper,
                                               uint16_t gb_address) {
  bool enabled = false;
  const size_t offset = mbc1_mapper_ram_offset(mapper, gb_address, &enabled);
  if (!enabled) return 0xFF;
  if (offset >= sizeof(save_stub)) {
    status.bounds_fault = true;
    return 0xFF;
  }
  return save_stub[offset];
}

void IRAM_ATTR gb_cartridge_write_mapped_ram(const Mbc1Mapper *mapper,
                                             uint16_t gb_address,
                                             uint8_t value) {
  bool enabled = false;
  const size_t offset = mbc1_mapper_ram_offset(mapper, gb_address, &enabled);
  if (!enabled || offset >= sizeof(save_stub)) return;
  if (save_stub[offset] != value) {
    save_stub[offset] = value;
    save_dirty = true;  // persisted later from the runtime loop, not the ISR
    save_write_seq++;
  }
}

uint32_t gb_cartridge_save_write_seq(void) { return save_write_seq; }

const uint8_t *gb_cartridge_save_data(void) { return save_stub; }

size_t gb_cartridge_save_size(void) { return sizeof(save_stub); }

bool gb_cartridge_save_dirty(void) { return save_dirty; }

void gb_cartridge_mark_save_persisted(void) { save_dirty = false; }

bool gb_cartridge_load_save(const uint8_t *data, size_t len) {
  if (!data || len != sizeof(save_stub)) return false;
  memcpy(save_stub, data, sizeof(save_stub));
  status.save_loaded = true;
  status.save_stubbed = false;
  save_dirty = false;
  return true;
}

bool gb_cartridge_header_self_test(void) {
  if (!status.rom_loaded) return false;
  return gb_cartridge_read_rom(0x134) == 'P' &&
         gb_cartridge_read_rom(0x135) == 'O' &&
         gb_cartridge_read_rom(0x136) == 'K' &&
         gb_cartridge_read_rom(0x137) == 'E';
}
