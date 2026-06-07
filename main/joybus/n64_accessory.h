#ifndef N64_ACCESSORY_H
#define N64_ACCESSORY_H

#include <stddef.h>
#include <stdint.h>

#include "transfer_pak.h"

struct N64AccessoryDebug {
  uint32_t reads;
  uint32_t writes;
  uint32_t malformed;
};

void n64_accessory_init(void);
// True when an accessory (the Transfer Pak) is attached to the controller's
// expansion slot. Drives the accessory-present bit in the controller status.
bool n64_accessory_present(void);
bool n64_accessory_read(uint16_t address, uint8_t *out, size_t len,
                        uint8_t *status);
bool n64_accessory_write(uint16_t address, const uint8_t *data, size_t len,
                         uint8_t *status);
const N64AccessoryDebug &n64_accessory_debug(void);

#endif
