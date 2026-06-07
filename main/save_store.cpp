#include "save_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "board_config.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "gb_cartridge.h"

namespace {
constexpr const char *TAG = "save_store";
// Persist only after writes have been quiet for this long, so an active transfer
// is flushed once at the end rather than on every block (flash wear / stalls).
constexpr uint32_t PERSIST_DEBOUNCE_MS = 900;

SaveStoreStatus status = {};
bool seq_initialized = false;
uint32_t last_seq = 0;
bool dirty_logged = false;

// --- Emergency power-loss save slot (raw flash partition) ---------------------
// Layout: [sector 0] header, [sectors 1..] save body. Program-only on power loss
// (the slot is pre-erased), merged into save.srm on the next boot.
constexpr const char *EMG_PARTITION_LABEL = "emgsave";
constexpr esp_partition_subtype_t EMG_PARTITION_SUBTYPE =
    static_cast<esp_partition_subtype_t>(0x40);
constexpr uint32_t EMG_MAGIC = 0x53474D45u;  // 'E''M''G''S'
constexpr uint16_t EMG_VERSION = 1;
constexpr size_t EMG_BODY_OFFSET = 0x1000;  // body starts at sector 1

struct EmgSlotHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t write_seq;
  uint32_t length;
  uint32_t crc32;
};

bool emg_slot_armed = false;

const esp_partition_t *emg_partition(void) {
  static const esp_partition_t *part = nullptr;
  static bool searched = false;
  if (!searched) {
    searched = true;
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    EMG_PARTITION_SUBTYPE, EMG_PARTITION_LABEL);
    if (!part) ESP_LOGW(TAG, "emergency save partition '%s' not found",
                        EMG_PARTITION_LABEL);
  }
  return part;
}

const char *save_path(void) {
  static char path[160];
  snprintf(path, sizeof(path), "%s/save.srm", board::SPIFFS_MOUNT);
  return path;
}

const char *tmp_path(void) {
  static char path[160];
  snprintf(path, sizeof(path), "%s/save.tmp", board::SPIFFS_MOUNT);
  return path;
}

bool file_size(const char *path, size_t *size_out) {
  struct stat st = {};
  if (stat(path, &st) != 0) return false;
  if (size_out) *size_out = static_cast<size_t>(st.st_size);
  return true;
}

bool valid_save_file(const char *path, size_t expected, size_t *actual_out) {
  size_t actual = 0;
  if (!file_size(path, &actual)) {
    if (actual_out) *actual_out = 0;
    return false;
  }
  if (actual_out) *actual_out = actual;
  return actual == expected;
}

bool close_checked(FILE *f) {
  if (!f) return false;
  if (fflush(f) != 0) {
    fclose(f);
    return false;
  }
  return fclose(f) == 0;
}

// Sets the flush-active flag for the lifetime of the flash write so the RMT ISR
// returns 0xFF for ROM reads (ROM is in flash, unreadable with the cache off)
// instead of faulting, then clears it on every exit path.
struct FlushGuard {
  FlushGuard() { gb_cartridge_set_flush_active(true); }
  ~FlushGuard() { gb_cartridge_set_flush_active(false); }
};

bool persist_now(uint32_t now_ms, SaveStoreFlushReason reason) {
  const size_t size = gb_cartridge_save_size();
  FlushGuard flush_guard;
  remove(tmp_path());
  FILE *f = fopen(tmp_path(), "wb");
  if (!f) {
    status.flush_result = SAVE_STORE_FLUSH_OPEN_FAILED;
    status.last_flush_ok = false;
    status.failed_flush_count++;
    ESP_LOGW(TAG, "save persist temp open failed");
    return false;
  }
  const size_t n = fwrite(gb_cartridge_save_data(), 1, size, f);
  if (n != size) {
    fclose(f);
    remove(tmp_path());
    status.flush_result = SAVE_STORE_FLUSH_SHORT_WRITE;
    status.last_flush_ok = false;
    status.failed_flush_count++;
    ESP_LOGW(TAG, "save persist short write %u/%u", (unsigned)n,
             (unsigned)size);
    return false;
  }
  if (!close_checked(f)) {
    remove(tmp_path());
    status.flush_result = SAVE_STORE_FLUSH_CLOSE_FAILED;
    status.last_flush_ok = false;
    status.failed_flush_count++;
    ESP_LOGW(TAG, "save persist temp close failed");
    return false;
  }

  // POSIX rename replaces the destination atomically. If the SPIFFS VFS rejects
  // replacement, fall back to remove+rename after the temp file is complete.
  if (rename(tmp_path(), save_path()) != 0) {
    const int rename_errno = errno;
    remove(save_path());
    if (rename(tmp_path(), save_path()) != 0) {
      remove(tmp_path());
      status.flush_result = SAVE_STORE_FLUSH_RENAME_FAILED;
      status.last_flush_ok = false;
      status.failed_flush_count++;
      ESP_LOGW(TAG, "save persist rename failed errno=%d/%d", rename_errno,
               errno);
      return false;
    }
  }

  gb_cartridge_mark_save_persisted();
  status.persisted = true;
  status.loaded_persisted = true;
  status.pending = false;
  status.last_flush_ok = true;
  status.flush_result = SAVE_STORE_FLUSH_OK;
  status.flush_reason = reason;
  status.flush_count++;
  status.last_flush_ms = now_ms;
  status.last_persisted_seq = gb_cartridge_save_write_seq();
  dirty_logged = false;
  ESP_LOGI(TAG, "persisted save (%u bytes seq=%lu changes=%lu)",
           (unsigned)size, (unsigned long)status.last_persisted_seq,
           (unsigned long)gb_cartridge_save_debug().changed_bytes);
  return true;
}
}  // namespace

bool save_store_load(void) {
  const size_t size = gb_cartridge_save_size();
  remove(tmp_path());
  status.load_result = SAVE_STORE_LOAD_NOT_ATTEMPTED;
  status.loaded_persisted = false;
  status.persisted = false;
  status.pending = false;
  status.last_load_size = 0;

  size_t actual = 0;
  if (!file_size(save_path(), &actual)) {
    status.load_result = SAVE_STORE_LOAD_MISSING;
    ESP_LOGI(TAG, "no persisted save; using bundled default");
    gb_cartridge_save_tracking_reset();
    seq_initialized = true;
    last_seq = gb_cartridge_save_write_seq();
    status.observed_write_seq = last_seq;
    status.last_persisted_seq = last_seq;
    return false;
  }
  status.last_load_size = actual;
  if (actual != size) {
    status.load_result = SAVE_STORE_LOAD_INVALID_SIZE;
    ESP_LOGW(TAG, "persisted save invalid size %u/%u; using bundled default",
             (unsigned)actual, (unsigned)size);
    gb_cartridge_save_tracking_reset();
    seq_initialized = true;
    last_seq = gb_cartridge_save_write_seq();
    status.observed_write_seq = last_seq;
    status.last_persisted_seq = last_seq;
    return false;
  }

  FILE *f = fopen(save_path(), "rb");
  if (!f) {
    status.load_result = SAVE_STORE_LOAD_READ_FAILED;
    return false;
  }
  uint8_t *buf = static_cast<uint8_t *>(malloc(size));
  if (!buf) {
    fclose(f);
    status.load_result = SAVE_STORE_LOAD_NO_MEMORY;
    return false;
  }
  const size_t n = fread(buf, 1, size, f);
  fclose(f);
  const bool ok = (n == size) && gb_cartridge_load_save(buf, size);
  free(buf);
  if (ok) {
    status.persisted = true;
    status.loaded_persisted = true;
    status.load_result = SAVE_STORE_LOAD_PERSISTED;
    seq_initialized = true;
    last_seq = gb_cartridge_save_write_seq();
    status.observed_write_seq = last_seq;
    status.last_persisted_seq = last_seq;
    status.pending = false;
    dirty_logged = false;
    ESP_LOGI(TAG, "loaded persisted save (%u bytes)", (unsigned)size);
  } else {
    status.load_result = SAVE_STORE_LOAD_READ_FAILED;
    ESP_LOGW(TAG, "persisted save read failed (%u/%u); using bundled default",
             (unsigned)n, (unsigned)size);
  }
  return ok;
}

void save_store_service(uint32_t now_ms, bool allow_flash_write,
                        SaveStoreFlushReason reason) {
  const uint32_t seq = gb_cartridge_save_write_seq();
  if (!seq_initialized || seq != last_seq) {
    seq_initialized = true;
    last_seq = seq;
    status.observed_write_seq = seq;
    status.last_change_ms = now_ms;  // fresh write activity: restart the quiet timer
    status.pending = gb_cartridge_save_dirty();
    if (status.pending && !dirty_logged) {
      const GbCartridgeSaveDebug debug = gb_cartridge_save_debug();
      ESP_LOGI(TAG,
               "save dirty seq=%lu changes=%lu lastOff=0x%04lX gb=0x%04X "
               "value=0x%02X",
               (unsigned long)debug.write_seq,
               (unsigned long)debug.changed_bytes,
               (unsigned long)debug.last_offset, debug.last_gb_address,
               debug.last_value);
      dirty_logged = true;
    }
    return;
  }
  status.pending = gb_cartridge_save_dirty();
  if (!status.pending) return;
  if (!allow_flash_write) return;
  if (now_ms - status.last_change_ms < PERSIST_DEBOUNCE_MS) return;
  persist_now(now_ms, reason);
}

bool save_store_flush_on_power_loss(void) {
  if (!gb_cartridge_save_dirty()) return false;
  const esp_partition_t *p = emg_partition();
  if (!p) return false;
  const size_t size = gb_cartridge_save_size();
  if (EMG_BODY_OFFSET + size > p->size) {
    ESP_LOGW(TAG, "emergency slot too small for save");
    return false;
  }
  const uint8_t *data = gb_cartridge_save_data();
  // Program-only writes into the pre-erased slot. Body first, then the header, so
  // a torn/incomplete write leaves the header invalid and is rejected on boot.
  if (esp_partition_write(p, EMG_BODY_OFFSET, data, size) != ESP_OK) {
    status.failed_flush_count++;
    return false;
  }
  EmgSlotHeader h = {};
  h.magic = EMG_MAGIC;
  h.version = EMG_VERSION;
  h.write_seq = gb_cartridge_save_write_seq();
  h.length = static_cast<uint32_t>(size);
  h.crc32 = esp_rom_crc32_le(0, data, size);
  if (esp_partition_write(p, 0, &h, sizeof(h)) != ESP_OK) {
    status.failed_flush_count++;
    return false;
  }
  emg_slot_armed = false;
  status.last_flush_ok = true;
  status.flush_result = SAVE_STORE_FLUSH_OK;
  status.flush_reason = SAVE_STORE_FLUSH_REASON_POWER_LOSS;
  status.flush_count++;
  ESP_LOGW(TAG, "power-loss save written to emergency slot (%u bytes seq=%lu)",
           (unsigned)size, (unsigned long)h.write_seq);
  return true;
}

void save_store_arm_power_loss_slot(void) {
  const esp_partition_t *p = emg_partition();
  if (!p) return;
  if (esp_partition_erase_range(p, 0, p->size) == ESP_OK) {
    emg_slot_armed = true;
  } else {
    emg_slot_armed = false;
    ESP_LOGW(TAG, "emergency slot erase (arm) failed");
  }
}

bool save_store_power_loss_slot_armed(void) { return emg_slot_armed; }

bool save_store_recover_power_loss_slot(void) {
  const esp_partition_t *p = emg_partition();
  if (!p) return false;

  EmgSlotHeader h = {};
  if (esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK) return false;

  // Blank (all 0xFF) header => already armed, nothing to recover.
  bool blank = true;
  const uint8_t *hb = reinterpret_cast<const uint8_t *>(&h);
  for (size_t i = 0; i < sizeof(h); ++i) {
    if (hb[i] != 0xFF) { blank = false; break; }
  }
  if (blank) {
    emg_slot_armed = true;
    return false;
  }

  const size_t size = gb_cartridge_save_size();
  bool valid = h.magic == EMG_MAGIC && h.version == EMG_VERSION &&
               h.length == size && EMG_BODY_OFFSET + size <= p->size;
  uint8_t *body = valid ? static_cast<uint8_t *>(malloc(size)) : nullptr;
  if (valid && body && esp_partition_read(p, EMG_BODY_OFFSET, body, size) ==
                            ESP_OK &&
      esp_rom_crc32_le(0, body, size) == h.crc32 &&
      gb_cartridge_load_save(body, size)) {
    free(body);
    // Slot wins: it captured an in-game save that never reached save.srm.
    persist_now(0, SAVE_STORE_FLUSH_REASON_BUS_QUIET);
    seq_initialized = true;
    last_seq = gb_cartridge_save_write_seq();
    status.observed_write_seq = last_seq;
    save_store_arm_power_loss_slot();
    ESP_LOGI(TAG, "recovered power-loss save from emergency slot (seq=%lu)",
             (unsigned long)h.write_seq);
    return true;
  }
  if (body) free(body);
  // Non-blank but invalid/unreadable: re-arm so it is ready next time.
  ESP_LOGW(TAG, "emergency slot invalid; re-arming");
  save_store_arm_power_loss_slot();
  return false;
}

bool save_store_reset(void) {
  const bool removed_save = remove(save_path()) == 0 || errno == ENOENT;
  const bool removed_tmp = remove(tmp_path()) == 0 || errno == ENOENT;
  status.persisted = false;
  status.loaded_persisted = false;
  status.pending = gb_cartridge_save_dirty();
  status.load_result = SAVE_STORE_LOAD_MISSING;
  status.flush_result = SAVE_STORE_FLUSH_NONE;
  status.last_flush_ok = false;
  return removed_save && removed_tmp;
}

bool save_store_persisted(void) { return status.persisted; }

const SaveStoreStatus &save_store_status(void) { return status; }

const char *save_store_load_result_name(SaveStoreLoadResult result) {
  switch (result) {
    case SAVE_STORE_LOAD_NOT_ATTEMPTED: return "not_attempted";
    case SAVE_STORE_LOAD_MISSING: return "missing";
    case SAVE_STORE_LOAD_PERSISTED: return "persisted";
    case SAVE_STORE_LOAD_INVALID_SIZE: return "invalid_size";
    case SAVE_STORE_LOAD_NO_MEMORY: return "no_memory";
    case SAVE_STORE_LOAD_READ_FAILED: return "read_failed";
  }
  return "unknown";
}

const char *save_store_flush_result_name(SaveStoreFlushResult result) {
  switch (result) {
    case SAVE_STORE_FLUSH_NONE: return "none";
    case SAVE_STORE_FLUSH_OK: return "ok";
    case SAVE_STORE_FLUSH_OPEN_FAILED: return "open_failed";
    case SAVE_STORE_FLUSH_SHORT_WRITE: return "short_write";
    case SAVE_STORE_FLUSH_CLOSE_FAILED: return "close_failed";
    case SAVE_STORE_FLUSH_RENAME_FAILED: return "rename_failed";
  }
  return "unknown";
}

const char *save_store_flush_reason_name(SaveStoreFlushReason reason) {
  switch (reason) {
    case SAVE_STORE_FLUSH_REASON_NONE: return "none";
    case SAVE_STORE_FLUSH_REASON_WRITE_BURST_SETTLE: return "write_burst_settle";
    case SAVE_STORE_FLUSH_REASON_BUS_QUIET: return "bus_quiet";
    case SAVE_STORE_FLUSH_REASON_POWER_LOSS: return "power_loss";
  }
  return "unknown";
}
