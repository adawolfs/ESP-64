#include "status_display.h"

#include "board_config.h"
#include "esp_log.h"

#if OLED_STATUS_ENABLED
#include <string.h>

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#endif

namespace {
constexpr const char *TAG = "status_display";

#if OLED_STATUS_ENABLED
constexpr i2c_port_t OLED_I2C_PORT = I2C_NUM_0;
constexpr uint32_t I2C_TIMEOUT_MS = 50;
constexpr uint32_t REFRESH_INTERVAL_MS = 150;
constexpr size_t OLED_BUFFER_SIZE =
    board::OLED_BUFFER_WIDTH * board::OLED_BUFFER_HEIGHT / 8;

bool initialized = false;
bool dirty = false;
bool snapshot_dirty = false;
uint32_t last_refresh_ms = 0;
uint32_t message_until_ms = 0;
bool message_hold_pending = false;
uint8_t framebuffer[OLED_BUFFER_SIZE] = {};

struct StoredWifi {
  bool ap_active;
  bool sta_connected;
  char ap_ip[16];
  char sta_ip[16];
};

struct StoredGame {
  char title[24];
  bool rom_loaded;
  bool header_ok;
};

struct StoredSave {
  char load_result[16];
  char flush_result[16];
  bool persisted;
  bool pending;
  bool recovered_power_loss;
};

struct StoredRuntime {
  bool accessory_present;
  bool transfer_pak_powered;
  bool link_active;
};

struct StoredUpload {
  StatusDisplayUploadKind kind;
  StatusDisplayUploadPhase phase;
  size_t bytes_done;
  size_t bytes_total;
  char detail[18];
};

struct StoredMessage {
  StatusDisplayMessageLevel level;
  char text[24];
};

StoredWifi wifi = {};
StoredGame game = {"BOOT", false, false};
StoredSave save = {};
StoredRuntime runtime = {};
StoredUpload upload = {};
StoredMessage message = {};

void copy_text(char *dst, size_t dst_len, const char *src) {
  if (dst_len == 0) return;
  if (!src) src = "";
  size_t i = 0;
  for (; i + 1 < dst_len && src[i] != '\0'; ++i) {
    const char c = src[i];
    dst[i] = (c >= 0x20 && c <= 0x7E) ? c : ' ';
  }
  dst[i] = '\0';
}

bool write_bytes(const uint8_t *data, size_t len) {
  return i2c_master_write_to_device(
             OLED_I2C_PORT, board::OLED_ADDRESS, data, len,
             pdMS_TO_TICKS(I2C_TIMEOUT_MS)) == ESP_OK;
}

bool write_command(uint8_t command) {
  const uint8_t packet[2] = {0x00, command};
  return write_bytes(packet, sizeof(packet));
}

bool write_command_list(const uint8_t *commands, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (!write_command(commands[i])) {
      return false;
    }
  }
  return true;
}

bool write_data(const uint8_t *data, size_t len) {
  uint8_t packet[17];
  packet[0] = 0x40;
  size_t offset = 0;
  while (offset < len) {
    const size_t chunk = (len - offset > sizeof(packet) - 1)
                             ? sizeof(packet) - 1
                             : len - offset;
    memcpy(&packet[1], &data[offset], chunk);
    if (!write_bytes(packet, chunk + 1)) {
      return false;
    }
    offset += chunk;
  }
  return true;
}

bool init_i2c() {
  i2c_config_t config = {};
  config.mode = I2C_MODE_MASTER;
  config.sda_io_num = static_cast<gpio_num_t>(board::PIN_OLED_I2C_SDA);
  config.scl_io_num = static_cast<gpio_num_t>(board::PIN_OLED_I2C_SCL);
  config.sda_pullup_en = GPIO_PULLUP_ENABLE;
  config.scl_pullup_en = GPIO_PULLUP_ENABLE;
  config.master.clk_speed = board::OLED_BUS_HZ;
  config.clk_flags = 0;

  esp_err_t err = i2c_param_config(OLED_I2C_PORT, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2C config failed: %s", esp_err_to_name(err));
    return false;
  }
  err = i2c_driver_install(OLED_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool init_ssd1306() {
  const uint8_t init[] = {
      0xAE,        // display off
      0xD5, 0x80,  // clock divide
      0xA8, 0x3F,  // multiplex 1/64
      0xD3, 0x00,  // display offset
      0x40,        // start line
      0x8D, 0x14,  // charge pump
      0x20, 0x00,  // horizontal addressing
      0xA1,        // segment remap
      0xC8,        // COM scan direction
      0xDA, 0x12,  // COM pins
      0x81, 0xCF,  // contrast
      0xD9, 0xF1,  // pre-charge
      0xDB, 0x40,  // VCOM detect
      0xA4,        // resume RAM display
      0xA6,        // normal display
      0x2E,        // deactivate scroll
      0xAF,        // display on
  };
  return write_command_list(init, sizeof(init));
}

void mark_dirty() { dirty = true; }

void clear() {
  memset(framebuffer, 0, sizeof(framebuffer));
  mark_dirty();
}

void set_physical_pixel(int x, int y, bool on) {
  if (x < 0 || y < 0 || x >= board::OLED_BUFFER_WIDTH ||
      y >= board::OLED_BUFFER_HEIGHT) {
    return;
  }
  const size_t index = static_cast<size_t>(x) +
                       (static_cast<size_t>(y) / 8) * board::OLED_BUFFER_WIDTH;
  const uint8_t mask = 1u << (y & 7);
  if (on) {
    framebuffer[index] |= mask;
  } else {
    framebuffer[index] &= ~mask;
  }
}

void set_pixel(int x, int y, bool on = true) {
  if (x < 0 || y < 0 || x >= board::OLED_LOGICAL_WIDTH ||
      y >= board::OLED_LOGICAL_HEIGHT) {
    return;
  }
  set_physical_pixel(x + board::OLED_DRAW_X_OFFSET,
                     y + board::OLED_DRAW_Y_OFFSET, on);
  mark_dirty();
}

void hline(int x, int y, int w, bool on = true) {
  for (int i = 0; i < w; ++i) {
    set_pixel(x + i, y, on);
  }
}

void vline(int x, int y, int h, bool on = true) {
  for (int i = 0; i < h; ++i) {
    set_pixel(x, y + i, on);
  }
}

void rect(int x, int y, int w, int h, bool on = true) {
  if (w <= 0 || h <= 0) return;
  hline(x, y, w, on);
  hline(x, y + h - 1, w, on);
  vline(x, y, h, on);
  vline(x + w - 1, y, h, on);
}

const uint8_t *glyph(char c) {
  static constexpr uint8_t UNKNOWN[5] = {0x7E, 0x09, 0x09, 0x00, 0x06};
  static constexpr uint8_t SPACE[5] = {0, 0, 0, 0, 0};
  static constexpr uint8_t FONT[][5] = {
      {0x3E, 0x51, 0x49, 0x45, 0x3E},  // 0
      {0x00, 0x42, 0x7F, 0x40, 0x00},  // 1
      {0x42, 0x61, 0x51, 0x49, 0x46},  // 2
      {0x21, 0x41, 0x45, 0x4B, 0x31},  // 3
      {0x18, 0x14, 0x12, 0x7F, 0x10},  // 4
      {0x27, 0x45, 0x45, 0x45, 0x39},  // 5
      {0x3C, 0x4A, 0x49, 0x49, 0x30},  // 6
      {0x01, 0x71, 0x09, 0x05, 0x03},  // 7
      {0x36, 0x49, 0x49, 0x49, 0x36},  // 8
      {0x06, 0x49, 0x49, 0x29, 0x1E},  // 9
      {0x7E, 0x11, 0x11, 0x11, 0x7E},  // A
      {0x7F, 0x49, 0x49, 0x49, 0x36},  // B
      {0x3E, 0x41, 0x41, 0x41, 0x22},  // C
      {0x7F, 0x41, 0x41, 0x22, 0x1C},  // D
      {0x7F, 0x49, 0x49, 0x49, 0x41},  // E
      {0x7F, 0x09, 0x09, 0x09, 0x01},  // F
      {0x3E, 0x41, 0x49, 0x49, 0x7A},  // G
      {0x7F, 0x08, 0x08, 0x08, 0x7F},  // H
      {0x00, 0x41, 0x7F, 0x41, 0x00},  // I
      {0x20, 0x40, 0x41, 0x3F, 0x01},  // J
      {0x7F, 0x08, 0x14, 0x22, 0x41},  // K
      {0x7F, 0x40, 0x40, 0x40, 0x40},  // L
      {0x7F, 0x02, 0x0C, 0x02, 0x7F},  // M
      {0x7F, 0x04, 0x08, 0x10, 0x7F},  // N
      {0x3E, 0x41, 0x41, 0x41, 0x3E},  // O
      {0x7F, 0x09, 0x09, 0x09, 0x06},  // P
      {0x3E, 0x41, 0x51, 0x21, 0x5E},  // Q
      {0x7F, 0x09, 0x19, 0x29, 0x46},  // R
      {0x46, 0x49, 0x49, 0x49, 0x31},  // S
      {0x01, 0x01, 0x7F, 0x01, 0x01},  // T
      {0x3F, 0x40, 0x40, 0x40, 0x3F},  // U
      {0x1F, 0x20, 0x40, 0x20, 0x1F},  // V
      {0x3F, 0x40, 0x38, 0x40, 0x3F},  // W
      {0x63, 0x14, 0x08, 0x14, 0x63},  // X
      {0x07, 0x08, 0x70, 0x08, 0x07},  // Y
      {0x61, 0x51, 0x49, 0x45, 0x43},  // Z
  };
  if (c == ' ') return SPACE;
  if (c >= '0' && c <= '9') return FONT[c - '0'];
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  if (c >= 'A' && c <= 'Z') return FONT[10 + c - 'A'];
  if (c == ':' || c == '.') {
    static constexpr uint8_t DOTS[5] = {0, 0x36, 0x36, 0, 0};
    return DOTS;
  }
  if (c == '-' || c == '_') {
    static constexpr uint8_t DASH[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    return DASH;
  }
  if (c == '/') {
    static constexpr uint8_t SLASH[5] = {0x40, 0x30, 0x08, 0x06, 0x01};
    return SLASH;
  }
  return UNKNOWN;
}

void draw_char(int x, int y, char c) {
  const uint8_t *g = glyph(c);
  for (int col = 0; col < 5; ++col) {
    uint8_t bits = g[col];
    for (int row = 0; row < 7; ++row) {
      if (bits & (1u << row)) {
        set_pixel(x + col, y + row);
      }
    }
  }
}

void draw_text(int x, int y, const char *text, int max_chars) {
  if (!text) return;
  for (int i = 0; text[i] != '\0' && i < max_chars; ++i) {
    draw_char(x + i * 6, y, text[i]);
  }
}

bool status_page_scrolling() {
  return game.title[0] && strlen(game.title) > 11 &&
         (upload.phase == StatusDisplayUploadPhase::Idle ||
          upload.phase == StatusDisplayUploadPhase::Success);
}

void draw_scrolling_text(int x, int y, const char *text, int max_chars,
                         uint32_t now_ms) {
  if (!text) return;
  const size_t len = strlen(text);
  if (len <= static_cast<size_t>(max_chars)) {
    draw_text(x, y, text, max_chars);
    return;
  }

  constexpr size_t GAP = 3;
  const size_t cycle = len + GAP;
  const size_t offset = (now_ms / 450) % cycle;
  char window[12];
  for (int i = 0; i < max_chars && i + 1 < static_cast<int>(sizeof(window)); ++i) {
    const size_t pos = (offset + static_cast<size_t>(i)) % cycle;
    window[i] = pos < len ? text[pos] : ' ';
    window[i + 1] = '\0';
  }
  draw_text(x, y, window, max_chars);
}

const char *upload_kind_name(StatusDisplayUploadKind kind) {
  switch (kind) {
    case StatusDisplayUploadKind::Rom:
      return "ROM";
    case StatusDisplayUploadKind::Save:
      return "SAVE";
    default:
      return "UP";
  }
}

const char *upload_phase_name(StatusDisplayUploadPhase phase) {
  switch (phase) {
    case StatusDisplayUploadPhase::Receiving:
      return "RECV";
    case StatusDisplayUploadPhase::Success:
      return "OK";
    case StatusDisplayUploadPhase::Rejected:
      return "REJECT";
    case StatusDisplayUploadPhase::Failed:
      return "FAIL";
    default:
      return "IDLE";
  }
}

const char *message_prefix(StatusDisplayMessageLevel level) {
  switch (level) {
    case StatusDisplayMessageLevel::Error:
      return "ERR";
    case StatusDisplayMessageLevel::Warning:
      return "WARN";
    default:
      return "INFO";
  }
}

void draw_label_value(int y, char label, const char *value) {
  draw_char(2, y, label);
  draw_char(8, y, ':');
  draw_text(15, y, value, 9);
}

void draw_footer() {
  char flags[12];
  snprintf(flags, sizeof(flags), "%s %s %s",
           wifi.sta_connected ? "STA" : (wifi.ap_active ? "AP" : "NET"),
           runtime.accessory_present ? "TP" : "--",
           runtime.link_active ? "N64" : "IDLE");
  hline(0, 31, board::OLED_LOGICAL_WIDTH);
  draw_text(2, 32, flags, 11);
}

void render_message_page() {
  clear();
  draw_text(2, 1, message_prefix(message.level), 5);
  draw_text(2, 13, message.text[0] ? message.text : "STATUS", 11);
  draw_footer();
}

void render_upload_page() {
  clear();
  draw_text(2, 1, upload_kind_name(upload.kind), 5);
  draw_text(32, 1, upload_phase_name(upload.phase), 6);
  if (upload.bytes_total > 0) {
    char progress[16];
    const unsigned done_k =
        static_cast<unsigned>((upload.bytes_done / 1024) > 9999
                                  ? 9999
                                  : (upload.bytes_done / 1024));
    const unsigned total_k =
        static_cast<unsigned>((upload.bytes_total / 1024) > 9999
                                  ? 9999
                                  : (upload.bytes_total / 1024));
    snprintf(progress, sizeof(progress), "%u/%uK", done_k, total_k);
    draw_text(2, 13, progress, 11);
  } else if (upload.detail[0]) {
    draw_text(2, 13, upload.detail, 11);
  }
  draw_footer();
}

void render_status_page(uint32_t now_ms) {
  clear();
  if (game.title[0]) {
    draw_scrolling_text(2, 1, game.title, 11, now_ms);
  } else {
    draw_text(2, 1, "espN64", 11);
  }

  const char *rom_status =
      game.rom_loaded ? (game.header_ok ? "ROMOK" : "ROMBAD") : "NOROM";
  draw_label_value(11, 'G', rom_status);

  const char *save_status = "DEF";
  if (save.pending) {
    save_status = "PEND";
  } else if (save.recovered_power_loss) {
    save_status = "RECOV";
  } else if (save.persisted) {
    save_status = "PERS";
  } else if (save.load_result[0]) {
    if (strcmp(save.load_result, "missing") == 0) {
      save_status = "MISS";
    } else if (strcmp(save.load_result, "default") == 0) {
      save_status = "DEF";
    } else if (strcmp(save.load_result, "persisted") == 0) {
      save_status = "PERS";
    } else if (strcmp(save.load_result, "read_failed") == 0) {
      save_status = "READFAIL";
    } else {
      save_status = save.load_result;
    }
  }
  draw_label_value(21, 'S', save_status);
  draw_footer();
}

void render_current(uint32_t now_ms) {
  if (message_hold_pending) {
    message_until_ms = now_ms + 2500;
    message_hold_pending = false;
  }
  if (message.text[0] && now_ms < message_until_ms) {
    render_message_page();
    return;
  }
  if (upload.phase != StatusDisplayUploadPhase::Idle &&
      upload.phase != StatusDisplayUploadPhase::Success) {
    render_upload_page();
    return;
  }
  if (upload.phase == StatusDisplayUploadPhase::Success &&
      now_ms < message_until_ms) {
    render_upload_page();
    return;
  }
  render_status_page(now_ms);
}

bool flush() {
  if (!initialized) return false;
  const uint8_t addr[] = {
      0x21, 0, static_cast<uint8_t>(board::OLED_BUFFER_WIDTH - 1),
      0x22, 0, static_cast<uint8_t>(board::OLED_BUFFER_HEIGHT / 8 - 1),
  };
  if (!write_command_list(addr, sizeof(addr))) {
    return false;
  }
  if (!write_data(framebuffer, sizeof(framebuffer))) {
    return false;
  }
  dirty = false;
  return true;
}

void draw_test_pattern() {
  clear();
  hline(0, 9, board::OLED_LOGICAL_WIDTH);
  draw_text(2, 1, "espN64", 10);
  draw_text(2, 13, "OLED OK", 10);
  draw_text(2, 25, "72X40 Y24", 10);
}
#endif
}  // namespace

void status_display_init(void) {
#if OLED_STATUS_ENABLED
  if (!board::OLED_STATUS_DISPLAY_ENABLED) {
    return;
  }
  if (!init_i2c() || !init_ssd1306()) {
    ESP_LOGE(TAG, "OLED status display init failed");
    initialized = false;
    return;
  }
  initialized = true;
  draw_test_pattern();
  flush();
  ESP_LOGI(TAG,
           "OLED status display ready SDA=%d SCL=%d addr=0x%02X visible=%dx%d "
           "offset=(%d,%d)",
           board::PIN_OLED_I2C_SDA, board::PIN_OLED_I2C_SCL,
           board::OLED_ADDRESS, board::OLED_LOGICAL_WIDTH,
           board::OLED_LOGICAL_HEIGHT, board::OLED_DRAW_X_OFFSET,
           board::OLED_DRAW_Y_OFFSET);
#endif
}

void status_display_service(uint32_t now_ms) {
#if OLED_STATUS_ENABLED
  if (!initialized) {
    return;
  }
  if (last_refresh_ms != 0 && now_ms - last_refresh_ms < REFRESH_INTERVAL_MS) {
    return;
  }
  if (snapshot_dirty || (message.text[0] && now_ms < message_until_ms) ||
      status_page_scrolling()) {
    render_current(now_ms);
    snapshot_dirty = false;
  }
  if (!dirty) {
    return;
  }
  last_refresh_ms = now_ms;
  if (!flush()) {
    ESP_LOGW(TAG, "OLED refresh failed");
  }
#else
  (void)now_ms;
#endif
}

bool status_display_enabled(void) {
#if OLED_STATUS_ENABLED
  return initialized;
#else
  return false;
#endif
}

void status_display_set_wifi(const StatusDisplayWifi &wifi) {
#if OLED_STATUS_ENABLED
  ::wifi.ap_active = wifi.ap_active;
  ::wifi.sta_connected = wifi.sta_connected;
  copy_text(::wifi.ap_ip, sizeof(::wifi.ap_ip), wifi.ap_ip);
  copy_text(::wifi.sta_ip, sizeof(::wifi.sta_ip), wifi.sta_ip);
  snapshot_dirty = true;
#else
  (void)wifi;
#endif
}

void status_display_set_game(const StatusDisplayGame &game) {
#if OLED_STATUS_ENABLED
  copy_text(::game.title, sizeof(::game.title), game.title);
  ::game.rom_loaded = game.rom_loaded;
  ::game.header_ok = game.header_ok;
  snapshot_dirty = true;
#else
  (void)game;
#endif
}

void status_display_set_save(const StatusDisplaySave &save) {
#if OLED_STATUS_ENABLED
  copy_text(::save.load_result, sizeof(::save.load_result), save.load_result);
  copy_text(::save.flush_result, sizeof(::save.flush_result), save.flush_result);
  ::save.persisted = save.persisted;
  ::save.pending = save.pending;
  ::save.recovered_power_loss = save.recovered_power_loss;
  snapshot_dirty = true;
#else
  (void)save;
#endif
}

void status_display_set_runtime(const StatusDisplayRuntime &runtime) {
#if OLED_STATUS_ENABLED
  ::runtime.accessory_present = runtime.accessory_present;
  ::runtime.transfer_pak_powered = runtime.transfer_pak_powered;
  ::runtime.link_active = runtime.link_active;
  snapshot_dirty = true;
#else
  (void)runtime;
#endif
}

void status_display_set_upload(const StatusDisplayUpload &upload) {
#if OLED_STATUS_ENABLED
  ::upload.kind = upload.kind;
  ::upload.phase = upload.phase;
  ::upload.bytes_done = upload.bytes_done;
  ::upload.bytes_total = upload.bytes_total;
  copy_text(::upload.detail, sizeof(::upload.detail), upload.detail);
  if (upload.phase == StatusDisplayUploadPhase::Success ||
      upload.phase == StatusDisplayUploadPhase::Rejected ||
      upload.phase == StatusDisplayUploadPhase::Failed) {
    message_hold_pending = true;
  }
  snapshot_dirty = true;
#else
  (void)upload;
#endif
}

void status_display_set_message(StatusDisplayMessageLevel level,
                                const char *message) {
#if OLED_STATUS_ENABLED
  ::message.level = level;
  copy_text(::message.text, sizeof(::message.text), message);
  message_hold_pending = true;
  snapshot_dirty = true;
#else
  (void)level;
  (void)message;
#endif
}
