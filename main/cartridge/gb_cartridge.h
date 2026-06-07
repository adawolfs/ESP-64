#ifndef GB_CARTRIDGE_H
#define GB_CARTRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mbc1_mapper.h"

struct GbCartridgeStatus {
  bool rom_loaded;
  bool save_loaded;
  bool save_stubbed;
  bool bounds_fault;
  char title[17];
  uint8_t cartridge_type;
  uint16_t rom_bank_count;
  uint8_t ram_bank_count;
  uint8_t header_checksum;
};

struct GbCartridgeSaveDebug {
  bool dirty;
  uint32_t write_seq;
  uint32_t changed_bytes;
  uint32_t last_offset;
  uint16_t last_gb_address;
  uint8_t last_value;
};

void gb_cartridge_init(void);
const GbCartridgeStatus &gb_cartridge_status(void);
GbCartridgeSaveDebug gb_cartridge_save_debug(void);
uint8_t gb_cartridge_read_rom(size_t offset);
void gb_cartridge_read_rom_block(size_t offset, uint8_t *out, size_t len);
uint8_t gb_cartridge_read_mapped_rom(const Mbc1Mapper *mapper,
                                     uint16_t gb_address);
uint8_t gb_cartridge_read_mapped_ram(const Mbc1Mapper *mapper,
                                     uint16_t gb_address);
void gb_cartridge_write_mapped_ram(const Mbc1Mapper *mapper, uint16_t gb_address,
                                   uint8_t value);

// Cartridge save RAM access for persistence/export.
const uint8_t *gb_cartridge_save_data(void);
size_t gb_cartridge_save_size(void);
bool gb_cartridge_save_dirty(void);
uint32_t gb_cartridge_save_write_seq(void);
void gb_cartridge_mark_save_persisted(void);
// Replace the save RAM from a persisted copy (size must match the RAM size).
bool gb_cartridge_load_save(const uint8_t *data, size_t len);
void gb_cartridge_save_tracking_reset(void);

bool gb_cartridge_header_self_test(void);

#endif
