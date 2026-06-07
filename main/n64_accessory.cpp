#include "n64_accessory.h"

#include <string.h>

#include "esp_attr.h"

namespace {
N64AccessoryDebug debug = {};
bool accessory_present = false;

bool IRAM_ATTR valid_block_size(size_t len) {
  return len > 0 && len <= TRANSFER_PAK_BLOCK_SIZE;
}

void IRAM_ATTR malformed(uint8_t *status) {
  debug.malformed++;
  if (status) *status = 0x40;
}
}  // namespace

void n64_accessory_init(void) {
  memset(&debug, 0, sizeof(debug));
  // The Transfer Pak is hardwired into this device, so it is present whenever
  // the accessory subsystem has been brought up.
  accessory_present = true;
}

bool n64_accessory_present(void) { return accessory_present; }

bool IRAM_ATTR n64_accessory_read(uint16_t address, uint8_t *out, size_t len,
                                  uint8_t *status) {
  if (!out || !valid_block_size(len)) {
    malformed(status);
    return false;
  }
  debug.reads++;
  transfer_pak_read_block(address, out, len);
  if (status) *status = transfer_pak_status().status;
  return true;
}

bool IRAM_ATTR n64_accessory_write(uint16_t address, const uint8_t *data,
                                   size_t len, uint8_t *status) {
  if (!data || !valid_block_size(len)) {
    malformed(status);
    return false;
  }
  debug.writes++;
  transfer_pak_write_block(address, data, len);
  if (status) *status = transfer_pak_status().status;
  return true;
}

const N64AccessoryDebug &n64_accessory_debug(void) { return debug; }
