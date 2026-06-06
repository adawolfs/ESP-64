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

void gb_cartridge_init(void);
const GbCartridgeStatus &gb_cartridge_status(void);
uint8_t gb_cartridge_read_rom(size_t offset);
uint8_t gb_cartridge_read_mapped_rom(const Mbc1Mapper *mapper,
                                     uint16_t gb_address);
uint8_t gb_cartridge_read_mapped_ram(const Mbc1Mapper *mapper,
                                     uint16_t gb_address);
void gb_cartridge_note_volatile_save_write(uint16_t gb_address, uint8_t value);
bool gb_cartridge_header_self_test(void);

#endif
