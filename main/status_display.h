#ifndef STATUS_DISPLAY_H
#define STATUS_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

enum class StatusDisplayMessageLevel : uint8_t {
  Info = 0,
  Warning = 1,
  Error = 2,
};

enum class StatusDisplayUploadKind : uint8_t {
  None = 0,
  Rom,
  Save,
};

enum class StatusDisplayUploadPhase : uint8_t {
  Idle = 0,
  Receiving,
  Success,
  Rejected,
  Failed,
};

struct StatusDisplayWifi {
  bool ap_active;
  bool sta_connected;
  const char *ap_ip;
  const char *sta_ip;
};

struct StatusDisplayGame {
  const char *title;
  bool rom_loaded;
  bool header_ok;
};

struct StatusDisplaySave {
  const char *load_result;
  const char *flush_result;
  bool persisted;
  bool pending;
  bool recovered_power_loss;
};

struct StatusDisplayRuntime {
  bool accessory_present;
  bool transfer_pak_powered;
  bool link_active;
};

struct StatusDisplayUpload {
  StatusDisplayUploadKind kind;
  StatusDisplayUploadPhase phase;
  size_t bytes_done;
  size_t bytes_total;
  const char *detail;
};

void status_display_init(void);
void status_display_service(uint32_t now_ms);
bool status_display_enabled(void);

void status_display_set_wifi(const StatusDisplayWifi &wifi);
void status_display_set_game(const StatusDisplayGame &game);
void status_display_set_save(const StatusDisplaySave &save);
void status_display_set_runtime(const StatusDisplayRuntime &runtime);
void status_display_set_upload(const StatusDisplayUpload &upload);
void status_display_set_message(StatusDisplayMessageLevel level,
                                const char *message);

#endif
