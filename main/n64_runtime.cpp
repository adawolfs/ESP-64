#include "n64_runtime.h"

#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gb_cartridge.h"
#include "gb_time.h"
#include "n64_accessory.h"
#include "n64_controller.h"
#include "n64_joybus.h"
#include "pokemon_stadium_compat.h"
#include "transfer_pak.h"
#include "web_portal.h"

namespace {
constexpr const char *TAG = "n64_runtime";
constexpr uint32_t JOYBUS_QUIET_BEFORE_DELAY_MS = 250;
constexpr uint32_t IDLE_DELAY_INTERVAL_MS = 25;
// TEMP(fix-controller-detection): serial dump of Joy-Bus counters for hardware
// diagnosis. Watch with `pio device monitor` while plugged into the console.
// Remove once detection is confirmed on hardware.
constexpr uint32_t JOYBUS_DEBUG_LOG_INTERVAL_MS = 1000;
uint32_t last_web_service_ms = 0;
uint32_t last_joybus_activity_ms = 0;
uint32_t last_idle_delay_ms = 0;
uint32_t last_joybus_debug_ms = 0;

WebPortalConfig make_portal_config() {
  WebPortalConfig config;
  config.ap_ssid = board::WEB_AP_SSID;
  config.ap_password = board::WEB_AP_PASSWORD;
  config.http_port = board::WEB_HTTP_PORT;
  config.websocket_port = board::WEB_SOCKET_PORT;
  config.stream_interval_ms = board::WEB_STREAM_INTERVAL_MS;
  return config;
}

void service_web(uint32_t now_ms) {
  if (last_web_service_ms != 0 &&
      now_ms - last_web_service_ms < board::WEB_PORTAL_IDLE_SERVICE_INTERVAL_MS) {
    return;
  }
  web_portal_loop(now_ms);
  last_web_service_ms = now_ms;
}

// TEMP(fix-controller-detection): periodically log Joy-Bus traffic counters so
// detection problems are visible over serial without a connected web client.
// status>0 means the console is probing the port; poll>0 means it accepted the
// controller and is reading input; respFail>0 means replies are arriving late.
void log_joybus_debug(uint32_t now_ms) {
  if (last_joybus_debug_ms != 0 &&
      now_ms - last_joybus_debug_ms < JOYBUS_DEBUG_LOG_INTERVAL_MS) {
    return;
  }
  last_joybus_debug_ms = now_ms;
  const N64JoybusDebug &d = n64_joybus_debug();
  ESP_LOGI(TAG,
           "joybus status=%lu poll=%lu rd=%lu wr=%lu malformed=%lu timing=%lu "
           "respFail=%lu dropped=%lu",
           (unsigned long)d.status_commands, (unsigned long)d.poll_commands,
           (unsigned long)d.accessory_reads, (unsigned long)d.accessory_writes,
           (unsigned long)d.malformed_frames, (unsigned long)d.timing_errors,
           (unsigned long)d.response_failures, (unsigned long)d.dropped_starts);
  // TEMP(fix-controller-detection): Transfer Pak handshake visibility. powered/
  // enabled flip as the console probes; bank selects the GB window; invalid>0
  // means it addressed a register we don't map.
  const TransferPakStatus &p = transfer_pak_status();
  ESP_LOGI(TAG,
           "tpak powered=%d enabled=%d bank=%u status=0x%02X invalid=%lu "
           "lastAcc=%c@0x%04X=0x%02X",
           p.powered, p.access_enabled, p.bank, p.status,
           (unsigned long)p.invalid_accesses,
           d.last_accessory_is_write ? 'W' : 'R', d.last_accessory_addr,
           d.last_accessory_value);
}
}  // namespace

bool n64_runtime_init(void) {
  gb_cartridge_init();
  n64_controller_init();
  transfer_pak_init();
  n64_accessory_init();
  n64_controller_set_accessory_present(n64_accessory_present());

  if (!n64_controller_self_test() || !gb_cartridge_header_self_test()) {
    ESP_LOGE(TAG, "startup self-test failed");
    return false;
  }

  if (!n64_joybus_init(board::PIN_N64_JOYBUS_DATA)) {
    ESP_LOGE(TAG, "Joy-Bus GPIO init failed on GPIO%d",
             board::PIN_N64_JOYBUS_DATA);
    return false;
  }

  web_portal_mount_storage();
  if (!web_portal_begin(make_portal_config())) {
    ESP_LOGE(TAG, "web portal start failed");
    return false;
  }

  const PokemonStadiumCompatStatus compat = pokemon_stadium_compat_status();
  ESP_LOGI(TAG, "N64 runtime ready ip=%s title=%s accessory=%d header=%d",
           web_portal_ip(), gb_cartridge_status().title,
           compat.accessory_present, compat.rom_header_ok);
  return true;
}

void n64_runtime_loop(void) {
  const bool bus_activity = n64_joybus_service();
  const uint32_t now_ms = millis();
  if (bus_activity) last_joybus_activity_ms = now_ms;

  service_web(now_ms);
  log_joybus_debug(now_ms);

  const bool bus_quiet =
      now_ms - last_joybus_activity_ms >= JOYBUS_QUIET_BEFORE_DELAY_MS;
  const bool idle_delay_due =
      now_ms - last_idle_delay_ms >= IDLE_DELAY_INTERVAL_MS;
  if (bus_quiet && idle_delay_due) {
    last_idle_delay_ms = now_ms;
    vTaskDelay(1);
  } else {
    taskYIELD();
  }
}
