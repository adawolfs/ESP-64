#ifndef SAVE_STORE_H
#define SAVE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum SaveStoreLoadResult : uint8_t {
  SAVE_STORE_LOAD_NOT_ATTEMPTED = 0,
  SAVE_STORE_LOAD_MISSING,
  SAVE_STORE_LOAD_PERSISTED,
  SAVE_STORE_LOAD_INVALID_SIZE,
  SAVE_STORE_LOAD_NO_MEMORY,
  SAVE_STORE_LOAD_READ_FAILED,
};

enum SaveStoreFlushResult : uint8_t {
  SAVE_STORE_FLUSH_NONE = 0,
  SAVE_STORE_FLUSH_OK,
  SAVE_STORE_FLUSH_OPEN_FAILED,
  SAVE_STORE_FLUSH_SHORT_WRITE,
  SAVE_STORE_FLUSH_CLOSE_FAILED,
  SAVE_STORE_FLUSH_RENAME_FAILED,
};

// Why the most recent flush was triggered.
enum SaveStoreFlushReason : uint8_t {
  SAVE_STORE_FLUSH_REASON_NONE = 0,
  // A console save-write burst settled (in-game save), flushed while powered.
  SAVE_STORE_FLUSH_REASON_WRITE_BURST_SETTLE,
  // Fallback: all accessory traffic went quiet (end of session / shutdown).
  SAVE_STORE_FLUSH_REASON_BUS_QUIET,
  // Power loss detected: bounded program-only write to the emergency slot during
  // the supply ride-down (console already off, bus idle).
  SAVE_STORE_FLUSH_REASON_POWER_LOSS,
};

struct SaveStoreStatus {
  bool persisted;
  bool loaded_persisted;
  bool pending;
  bool last_flush_ok;
  SaveStoreLoadResult load_result;
  SaveStoreFlushResult flush_result;
  SaveStoreFlushReason flush_reason;
  uint32_t observed_write_seq;
  uint32_t last_persisted_seq;
  uint32_t flush_count;
  uint32_t failed_flush_count;
  uint32_t last_change_ms;
  uint32_t last_flush_ms;
  size_t last_load_size;
};

// Loads a persisted save from SPIFFS into the cartridge, taking precedence over
// the embedded default. Returns true if a valid persisted save was applied.
// Requires SPIFFS to be mounted first.
bool save_store_load(void);

// Persists the cartridge save to SPIFFS once writes have gone quiet. Call from
// the runtime loop with a millisecond timestamp; never from an ISR. The reason
// records why the flush was permitted (recorded on the next successful flush).
void save_store_service(
    uint32_t now_ms, bool allow_flash_write = true,
    SaveStoreFlushReason reason = SAVE_STORE_FLUSH_REASON_BUS_QUIET);

// Commits the dirty save to the pre-erased emergency flash slot with a bounded,
// program-only write (no erase, no filesystem). Safe to call from a high-priority
// task during the power-loss ride-down once the console has stopped driving the
// bus. No-op if the save is clean or the emergency partition is unavailable.
// Returns true if a write was performed and succeeded.
bool save_store_flush_on_power_loss(void);

// Re-erases ("arms") the emergency slot so a future power-loss write is a pure
// program. Called after a normal/boot commit. No-op if the partition is missing.
void save_store_arm_power_loss_slot(void);

// True if the emergency slot is currently armed (erased and ready).
bool save_store_power_loss_slot_armed(void);

// Boot-time merge: if the emergency slot holds a valid save (written during a
// previous power-loss), adopt it as authoritative, rewrite save.srm from it, and
// re-arm the slot. Otherwise ensure the slot is armed. Call once at startup after
// save_store_load() and SPIFFS mount. Returns true if a slot was recovered.
bool save_store_recover_power_loss_slot(void);

// Deletes the persisted save so the bundled default is restored on next boot.
bool save_store_reset(void);

// True if a persisted save currently exists on storage.
bool save_store_persisted(void);
const SaveStoreStatus &save_store_status(void);
const char *save_store_load_result_name(SaveStoreLoadResult result);
const char *save_store_flush_result_name(SaveStoreFlushResult result);
const char *save_store_flush_reason_name(SaveStoreFlushReason reason);

#endif  // SAVE_STORE_H
