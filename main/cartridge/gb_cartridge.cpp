#include "gb_cartridge.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_partition.h"

namespace {
constexpr const char *TAG = "gb_cartridge";
constexpr size_t kHeaderTitleOffset = 0x134;
constexpr size_t kHeaderTypeOffset = 0x147;
constexpr size_t kHeaderRomSizeOffset = 0x148;
constexpr size_t kHeaderRamSizeOffset = 0x149;
constexpr size_t kHeaderChecksumOffset = 0x14D;
constexpr size_t kHeaderEnd = 0x150;
constexpr size_t kSaveRamSize = 32u * 1024u;

constexpr const char *ROM_PARTITION_LABEL = "rom";
constexpr esp_partition_subtype_t ROM_PARTITION_SUBTYPE =
    static_cast<esp_partition_subtype_t>(0x41);

GbCartridgeStatus status = {};
uint8_t save_stub[kSaveRamSize] = {};
bool save_dirty = false;
uint32_t save_write_seq = 0;  // bumped on each actual save byte change
uint32_t save_changed_bytes = 0;
uint32_t last_save_offset = 0xFFFFFFFFu;
uint16_t last_save_gb_address = 0;
uint8_t last_save_value = 0;

// The active ROM lives in the `rom` flash partition, memory-mapped so the
// Transfer Pak read path reads it as fast as a rodata array.
const esp_partition_t *rom_part = nullptr;
esp_partition_mmap_handle_t rom_mmap = 0;
const uint8_t *rom_ptr = nullptr;  // mapped ROM base (in flash)
size_t rom_size = 0;               // usable ROM size from the header

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

// A Gen-1 mapper we emulate (MBC1 / MBC3 / MBC5 families, incl. ROM/RAM/battery).
bool known_cartridge_type(uint8_t type) {
  return type <= 0x03 ||                  // ROM only / MBC1
         (type >= 0x0F && type <= 0x13) ||  // MBC3
         (type >= 0x19 && type <= 0x1E);    // MBC5
}

// Standard GB header checksum over 0x134..0x14C (x = x - byte - 1).
bool header_checksum_ok(const uint8_t *rom) {
  uint8_t x = 0;
  for (size_t i = kHeaderTitleOffset; i < kHeaderChecksumOffset; ++i) {
    x = static_cast<uint8_t>(x - rom[i] - 1);
  }
  return x == rom[kHeaderChecksumOffset];
}

bool gb_cartridge_parse_header(const uint8_t *rom, size_t available,
                               GbCartridgeStatus *out,
                               size_t *rom_size_out) {
  if (!rom || !out || available < kHeaderEnd || !header_checksum_ok(rom) ||
      !known_cartridge_type(rom[kHeaderTypeOffset])) {
    return false;
  }

  memcpy(out->title, &rom[kHeaderTitleOffset], 16);
  out->title[16] = '\0';
  out->cartridge_type = rom[kHeaderTypeOffset];
  out->rom_bank_count = rom_banks_from_code(rom[kHeaderRomSizeOffset]);
  out->ram_bank_count = ram_banks_from_code(rom[kHeaderRamSizeOffset]);
  out->header_checksum = rom[kHeaderChecksumOffset];

  size_t parsed_rom_size = static_cast<size_t>(out->rom_bank_count) * 0x4000u;
  if (parsed_rom_size == 0 || parsed_rom_size > available) {
    parsed_rom_size = available;
  }
  if (rom_size_out) *rom_size_out = parsed_rom_size;
  return true;
}

void seed_blank_save(void) {
  memset(save_stub, 0xFF, sizeof(save_stub));
  status.save_loaded = false;
  status.save_stubbed = true;
}

void unmap_rom(void) {
  if (rom_mmap) {
    esp_partition_munmap(rom_mmap);
    rom_mmap = 0;
  }
  rom_ptr = nullptr;
  rom_size = 0;
}

// Maps the ROM partition and parses/validates the header into `status`.
void map_and_parse_rom(void) {
  unmap_rom();
  status.rom_loaded = false;
  status.title[0] = '\0';
  status.cartridge_type = 0;
  status.rom_bank_count = 0;
  status.ram_bank_count = 0;
  status.header_checksum = 0;
  status.bounds_fault = false;

  if (!rom_part) {
    rom_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                        ROM_PARTITION_SUBTYPE, ROM_PARTITION_LABEL);
    if (!rom_part) {
      ESP_LOGW(TAG, "rom partition '%s' not found", ROM_PARTITION_LABEL);
      return;
    }
  }

  const void *mapped = nullptr;
  if (esp_partition_mmap(rom_part, 0, rom_part->size, ESP_PARTITION_MMAP_DATA,
                         &mapped, &rom_mmap) != ESP_OK) {
    ESP_LOGW(TAG, "rom mmap failed");
    rom_mmap = 0;
    return;
  }
  rom_ptr = static_cast<const uint8_t *>(mapped);

  if (!gb_cartridge_parse_header(rom_ptr, rom_part->size, &status, &rom_size)) {
    ESP_LOGW(TAG, "no valid ROM in partition (upload one via the web portal)");
    return;  // mapped but invalid -> rom_loaded stays false
  }

  status.rom_loaded = true;
  ESP_LOGI(TAG, "ROM loaded: %s type=0x%02X banks=%u/%u", status.title,
           status.cartridge_type, status.rom_bank_count, status.ram_bank_count);
}
}  // namespace

void gb_cartridge_init(void) {
  memset(&status, 0, sizeof(status));
  map_and_parse_rom();
  // Save RAM starts blank; a persisted or uploaded save is applied later via
  // gb_cartridge_load_save(). (No game-specific save is bundled — upload yours.)
  seed_blank_save();
  gb_cartridge_save_tracking_reset();
}

bool gb_cartridge_reload_from_partition(void) {
  map_and_parse_rom();
  return status.rom_loaded;
}

const GbCartridgeStatus &gb_cartridge_status(void) { return status; }

GbCartridgeSaveDebug gb_cartridge_save_debug(void) {
  GbCartridgeSaveDebug debug = {};
  debug.dirty = save_dirty;
  debug.write_seq = save_write_seq;
  debug.changed_bytes = save_changed_bytes;
  debug.last_offset = last_save_offset;
  debug.last_gb_address = last_save_gb_address;
  debug.last_value = last_save_value;
  return debug;
}

uint8_t IRAM_ATTR gb_cartridge_read_rom(size_t offset) {
  if (!status.rom_loaded || !rom_ptr || offset >= rom_size) {
    status.bounds_fault = true;
    return 0xFF;
  }
  return rom_ptr[offset];
}

void IRAM_ATTR gb_cartridge_read_rom_block(size_t offset, uint8_t *out,
                                           size_t len) {
  if (!out) return;
  // Bulk copy a contiguous ROM run in one shot. Reading the cartridge byte by
  // byte through the mapper was far too slow inside the Joy-Bus response window
  // (~69 us for 32 bytes); a single memcpy keeps it well under the deadline.
  if (!status.rom_loaded || !rom_ptr || offset + len > rom_size) {
    status.bounds_fault = true;
    memset(out, 0xFF, len);
    return;
  }
  memcpy(out, &rom_ptr[offset], len);
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
    save_changed_bytes++;
    last_save_offset = static_cast<uint32_t>(offset);
    last_save_gb_address = gb_address;
    last_save_value = value;
  }
}

uint32_t gb_cartridge_save_write_seq(void) { return save_write_seq; }

const uint8_t *gb_cartridge_save_data(void) { return save_stub; }

size_t gb_cartridge_save_size(void) { return sizeof(save_stub); }

bool gb_cartridge_save_dirty(void) { return save_dirty; }

void gb_cartridge_mark_save_persisted(void) { save_dirty = false; }

void gb_cartridge_clear_save(void) {
  seed_blank_save();
  gb_cartridge_save_tracking_reset();
  save_dirty = true;  // ensure the blank save is written over any persisted copy
}

bool gb_cartridge_load_save(const uint8_t *data, size_t len) {
  if (!data || len != sizeof(save_stub)) return false;
  memcpy(save_stub, data, sizeof(save_stub));
  status.save_loaded = true;
  status.save_stubbed = false;
  gb_cartridge_save_tracking_reset();
  return true;
}

void gb_cartridge_save_tracking_reset(void) {
  save_dirty = false;
  save_write_seq = 0;
  save_changed_bytes = 0;
  last_save_offset = 0xFFFFFFFFu;
  last_save_gb_address = 0;
  last_save_value = 0;
}

// --- ROM partition write (web upload) ---------------------------------------
// The ROM is unmapped during the write (the mapping is invalid mid-erase), then
// re-mapped and re-parsed by gb_cartridge_reload_from_partition() on finish.

bool gb_cartridge_rom_write_begin(size_t total_len) {
  if (!rom_part) {
    rom_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                        ROM_PARTITION_SUBTYPE, ROM_PARTITION_LABEL);
  }
  if (!rom_part) return false;
  if (total_len == 0 || total_len > rom_part->size) return false;
  unmap_rom();
  status.rom_loaded = false;
  // Erase only the sectors we will write (round up to 4 KB).
  size_t erase_len = (total_len + 0xFFF) & ~static_cast<size_t>(0xFFF);
  if (erase_len > rom_part->size) erase_len = rom_part->size;
  return esp_partition_erase_range(rom_part, 0, erase_len) == ESP_OK;
}

bool gb_cartridge_rom_write_chunk(size_t offset, const uint8_t *data,
                                  size_t len) {
  if (!rom_part || !data) return false;
  if (offset + len > rom_part->size) return false;
  return esp_partition_write(rom_part, offset, data, len) == ESP_OK;
}

bool gb_cartridge_rom_write_finish(void) {
  return gb_cartridge_reload_from_partition();
}

bool gb_cartridge_header_self_test(void) {
  // No ROM at boot is allowed now (upload one via the web portal); only fail the
  // self-test if a ROM is present but reads back wrong.
  if (!status.rom_loaded) return true;
  return gb_cartridge_read_rom(0x134) == 'P' &&
         gb_cartridge_read_rom(0x135) == 'O' &&
         gb_cartridge_read_rom(0x136) == 'K' &&
         gb_cartridge_read_rom(0x137) == 'E';
}
