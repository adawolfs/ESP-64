#include "n64_runtime.h"

#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gb_cartridge.h"
#include "gb_time.h"
#include "n64_accessory.h"
#include "n64_controller.h"
#include "joybus_rmt.h"
#include "n64_joybus.h"
#include "pokemon_stadium_compat.h"
#include "power_monitor.h"
#include "save_store.h"
#include "transfer_pak.h"
#include "web_portal.h"

namespace {
constexpr const char *TAG = "n64_runtime";
constexpr uint32_t JOYBUS_QUIET_BEFORE_DELAY_MS = 250;
constexpr uint32_t IDLE_DELAY_INTERVAL_MS = 25;
constexpr uint32_t JOYBUS_DEBUG_LOG_INTERVAL_MS = 1000;
constexpr uint32_t RMT_ACCESSORY_WEB_QUIET_MS = 500;
constexpr uint32_t RMT_ACCESSORY_SAVE_QUIET_MS = 750;
uint32_t last_web_service_ms = 0;
uint32_t last_joybus_activity_ms = 0;
uint32_t last_idle_delay_ms = 0;
uint32_t last_joybus_debug_ms = 0;
uint32_t last_rmt_accessory_activity_ms = 0;

#ifdef N64_JOYBUS_BITBANG
constexpr bool USE_RMT_JOYBUS = false;
#else
constexpr bool USE_RMT_JOYBUS = true;
#endif

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
  const TransferPakStatus &p = transfer_pak_status();
  ESP_LOGI(TAG,
           "tpak powered=%d enabled=%d bank=%u status=0x%02X invalid=%lu "
           "lastAcc=%c@0x%04X=0x%02X lat=%uus save[L%d D%d P%d]",
           p.powered, p.access_enabled, p.bank, p.status,
           (unsigned long)p.invalid_accesses,
           d.last_accessory_is_write ? 'W' : 'R', d.last_accessory_addr,
           d.last_accessory_value, d.last_accessory_latency_us,
           gb_cartridge_status().save_loaded, gb_cartridge_save_dirty(),
           save_store_persisted());
  static uint32_t last_access_count = 0;
  const uint32_t acc_count = n64_joybus_access_count();
  if (acc_count != last_access_count) {
    last_access_count = acc_count;
    char trace[320];
    n64_joybus_format_access_trace(trace, sizeof(trace));
    ESP_LOGI(TAG, "acc[%lu] %s", (unsigned long)acc_count, trace);
  }
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

  web_portal_mount_storage();
  if (save_store_load()) {
    ESP_LOGI(TAG, "using persisted cartridge save (%s)",
             save_store_load_result_name(save_store_status().load_result));
  } else {
    ESP_LOGI(TAG, "using embedded cartridge save (%s)",
             save_store_load_result_name(save_store_status().load_result));
  }
  // Adopt a save captured during a previous power-loss (if any), then ensure the
  // emergency slot is armed for the next ride-down.
  if (save_store_recover_power_loss_slot()) {
    ESP_LOGI(TAG, "recovered save from power-loss emergency slot");
  }

  if (USE_RMT_JOYBUS) {
    if (!joybus_rmt_init(board::PIN_N64_JOYBUS_DATA)) {
      ESP_LOGE(TAG, "RMT Joy-Bus init failed on GPIO%d",
               board::PIN_N64_JOYBUS_DATA);
      return false;
    }
    ESP_LOGI(TAG, "Joy-Bus transport: RMT RX + immediate open-drain TX");
  } else {
    if (!n64_joybus_init(board::PIN_N64_JOYBUS_DATA)) {
      ESP_LOGE(TAG, "Joy-Bus GPIO init failed on GPIO%d",
               board::PIN_N64_JOYBUS_DATA);
      return false;
    }
    ESP_LOGW(TAG, "Joy-Bus transport: bit-bang fallback");
  }

  if (!web_portal_begin(make_portal_config())) {
    ESP_LOGE(TAG, "web portal start failed");
    return false;
  }

  // Arm the power-loss monitor so an abrupt console power-off commits the save.
  power_monitor_init();

  const PokemonStadiumCompatStatus compat = pokemon_stadium_compat_status();
  ESP_LOGI(TAG, "N64 runtime ready ip=%s title=%s accessory=%d header=%d",
           web_portal_ip(), gb_cartridge_status().title,
           compat.accessory_present, compat.rom_header_ok);
  return true;
}

void n64_runtime_loop(void) {
  const uint32_t now_ms = millis();
  if (USE_RMT_JOYBUS) {
    static uint32_t last_rmt_tx_read = 0;
    static uint32_t last_rmt_tx_write = 0;
    const bool bus_activity = joybus_rmt_loop(now_ms);
    if (bus_activity) last_joybus_activity_ms = now_ms;

    const JoybusRmtStats &rmt = joybus_rmt_stats();
    if (rmt.tx_read != last_rmt_tx_read || rmt.tx_write != last_rmt_tx_write) {
      last_rmt_tx_read = rmt.tx_read;
      last_rmt_tx_write = rmt.tx_write;
      last_rmt_accessory_activity_ms = now_ms;
    }

    const bool accessory_web_quiet =
        last_rmt_accessory_activity_ms == 0 ||
        now_ms - last_rmt_accessory_activity_ms >= RMT_ACCESSORY_WEB_QUIET_MS;
    // Only flush when the bus is fully idle. Flushing while the console is still
    // accessing the cartridge (reads OR writes) stalls the bit-bang response path
    // during the SPIFFS write and breaks the Transfer Pak connector check; an
    // all-quiet window cannot overlap an active console access sequence.
    const bool accessory_save_quiet =
        last_rmt_accessory_activity_ms == 0 ||
        now_ms - last_rmt_accessory_activity_ms >= RMT_ACCESSORY_SAVE_QUIET_MS;

    if (accessory_web_quiet) service_web(now_ms);
    save_store_service(now_ms, accessory_save_quiet);

    // RMT callbacks own the Joy-Bus response timing. The main task must still
    // block briefly so the idle task can feed the watchdog during GB Tower's
    // long accessory bursts; taskYIELD() can immediately reschedule main again.
    vTaskDelay(1);
    return;
  }

  const bool bus_activity = n64_joybus_service();
  if (bus_activity) last_joybus_activity_ms = now_ms;

  service_web(now_ms);
  save_store_service(now_ms);
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
