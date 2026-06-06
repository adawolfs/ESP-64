#include "save_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "esp_log.h"
#include "gb_cartridge.h"

namespace {
constexpr const char *TAG = "save_store";
// Persist only after writes have been quiet for this long, so an active transfer
// is flushed once at the end rather than on every block (flash wear / stalls).
constexpr uint32_t PERSIST_DEBOUNCE_MS = 3000;

bool persisted = false;
bool seq_initialized = false;
uint32_t last_seq = 0;
uint32_t last_change_ms = 0;

const char *save_path(void) {
  static char path[160];
  snprintf(path, sizeof(path), "%s/save.srm", board::SPIFFS_MOUNT);
  return path;
}

bool persist_now(void) {
  const size_t size = gb_cartridge_save_size();
  FILE *f = fopen(save_path(), "wb");
  if (!f) {
    ESP_LOGW(TAG, "save persist open failed");
    return false;
  }
  const size_t n = fwrite(gb_cartridge_save_data(), 1, size, f);
  fclose(f);
  if (n != size) {
    ESP_LOGW(TAG, "save persist short write %u/%u", (unsigned)n, (unsigned)size);
    return false;
  }
  gb_cartridge_mark_save_persisted();
  persisted = true;
  ESP_LOGI(TAG, "persisted save (%u bytes)", (unsigned)size);
  return true;
}
}  // namespace

bool save_store_load(void) {
  const size_t size = gb_cartridge_save_size();
  FILE *f = fopen(save_path(), "rb");
  if (!f) return false;
  uint8_t *buf = static_cast<uint8_t *>(malloc(size));
  if (!buf) {
    fclose(f);
    return false;
  }
  const size_t n = fread(buf, 1, size, f);
  fclose(f);
  const bool ok = (n == size) && gb_cartridge_load_save(buf, size);
  free(buf);
  if (ok) {
    persisted = true;
    ESP_LOGI(TAG, "loaded persisted save (%u bytes)", (unsigned)size);
  }
  return ok;
}

void save_store_service(uint32_t now_ms) {
  const uint32_t seq = gb_cartridge_save_write_seq();
  if (!seq_initialized || seq != last_seq) {
    seq_initialized = true;
    last_seq = seq;
    last_change_ms = now_ms;  // fresh write activity: restart the quiet timer
    return;
  }
  if (!gb_cartridge_save_dirty()) return;
  if (now_ms - last_change_ms < PERSIST_DEBOUNCE_MS) return;
  persist_now();
}

bool save_store_reset(void) {
  persisted = false;
  return remove(save_path()) == 0;
}

bool save_store_persisted(void) { return persisted; }
