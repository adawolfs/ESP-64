#ifndef TRANSFER_PAK_H
#define TRANSFER_PAK_H

#include <stddef.h>
#include <stdint.h>

#include "mbc1_mapper.h"

constexpr size_t TRANSFER_PAK_BLOCK_SIZE = 32;

struct TransferPakStatus {
  bool powered;
  bool access_enabled;
  uint8_t bank;
  uint8_t status;
  uint32_t reads;
  uint32_t writes;
  uint32_t invalid_accesses;
  Mbc1Mapper mapper;
};

void transfer_pak_init(void);
const TransferPakStatus &transfer_pak_status(void);
uint8_t transfer_pak_read_byte(uint16_t address);
void transfer_pak_write_byte(uint16_t address, uint8_t value);
void transfer_pak_read_block(uint16_t address, uint8_t *out, size_t len);
void transfer_pak_write_block(uint16_t address, const uint8_t *data,
                              size_t len);

#endif
