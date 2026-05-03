#include "emulator_runtime.h"

#include "board_config.h"
#include "cpu.h"
#include "display.h"
#include "gb_time.h"
#include "gbrom.h"
#include "lcd.h"
#include "mem.h"
#include "rom.h"
#include "sdl.h"
#include "timer.h"
#include "web_portal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if GB_ENABLE_AUDIO
#include "apu.h"
#endif

namespace {

uint32_t last_web_service_ms = 0;
uint32_t frames_since_idle_feed = 0;

WebPortalConfig make_portal_config() {
  WebPortalConfig portal_config;
  portal_config.ap_ssid = board::WEB_AP_SSID;
  portal_config.ap_password = board::WEB_AP_PASSWORD;
  portal_config.http_port = board::WEB_HTTP_PORT;
  portal_config.websocket_port = board::WEB_SOCKET_PORT;
  portal_config.stream_interval_ms = board::WEB_STREAM_INTERVAL_MS;
  return portal_config;
}

void service_web_portal(uint32_t now_ms, const uint8_t *framebuffer) {
  const bool has_clients = web_portal_client_count() > 0;
  if (last_web_service_ms != 0 && !has_clients &&
      now_ms - last_web_service_ms < board::WEB_PORTAL_IDLE_SERVICE_INTERVAL_MS) {
    return;
  }

  web_portal_loop(now_ms, framebuffer);
  last_web_service_ms = now_ms;
}

bool cooperative_frame_sleep(uint32_t remaining_us) {
  constexpr uint32_t tick_us = portTICK_PERIOD_MS * 1000u;
  bool delayed_for_tick = false;

  if (remaining_us >= tick_us) {
    const TickType_t ticks = remaining_us / tick_us;
    if (ticks > 0) {
      vTaskDelay(ticks);
      delayed_for_tick = true;
      remaining_us -= static_cast<uint32_t>(ticks) * tick_us;
    }
  }
  if (remaining_us > 0) {
    delayMicroseconds(remaining_us);
  }
  return delayed_for_tick;
}

}  // namespace

bool emulator_init(void) {
  sdl_init();

  // Mount SPIFFS up-front so cartridge SRAM saves load before WiFi/HTTP eat
  // into the heap. gameboy_mem_init() needs ~64 KB for the GB address space
  // plus optional cart RAM banks; allocating that *before* the WiFi stack
  // comes up keeps the heap fragmentation-free.
  display_show_status("Loading ROM", "");
  web_portal_mount_storage();

  if (!rom_init(gb_rom)) {
    display_show_status("ROM error", "Invalid header/checksum");
    return false;
  }

  if (!gameboy_mem_init()) {
    display_show_status("Memory error", "Allocation failed");
    return false;
  }
  cpu_init();

  display_show_status("Starting WiFi", board::WEB_AP_SSID);
  if (!web_portal_begin(make_portal_config())) {
    display_show_status("WiFi error", "AP start failed");
    return false;
  }

  display_show_status("GB ready", web_portal_ip());
  return true;
}

void emulator_run_frame(void) {
  const uint32_t frame_start_us = micros();
  uint32_t last_coop_us = frame_start_us;
  uint32_t last_web_audio_service_us = frame_start_us;
  uint32_t coop_yields_this_frame = 0;

  bool screen_updated = false;

  while (!screen_updated) {
    const unsigned int cycles = cpu_cycle();
    if (!cycles) {
      display_show_status("CPU stopped", "Unhandled opcode");
      delay(1000);
      return;
    }
    screen_updated = lcd_cycle(cycles);
    timer_cycle(cycles);
#if GB_ENABLE_AUDIO
    apu_cycle(cycles);
#endif

    const uint32_t now_us = micros();
    if (web_portal_client_count() > 0 &&
        now_us - last_web_audio_service_us >=
            board::EMULATOR_WEB_AUDIO_SERVICE_US) {
      // Keep WebAudio fed even when a frame takes longer than expected.
      web_portal_loop(millis(), nullptr);
      last_web_audio_service_us = micros();
    }
    if (now_us - last_coop_us >= board::EMULATOR_COOP_SLICE_US) {
      sdl_poll_input();
      // Do not sleep for a whole 10 ms tick on a late frame. A plain yield
      // lets higher-priority WiFi/lwIP/httpd tasks run without tanking FPS.
      if ((++coop_yields_this_frame % 4) == 0) {
        vTaskDelay(1);
      } else {
        taskYIELD();
      }
      last_coop_us = micros();
    }
  }

  sdl_update();
  const uint32_t now_ms = millis();
  service_web_portal(now_ms, sdl_get_framebuffer());
  mem_persist(now_ms);

  const uint32_t elapsed = micros() - frame_start_us;
  bool delayed_for_frame_pacing = false;
  if (elapsed < board::FRAME_US) {
    delayed_for_frame_pacing = cooperative_frame_sleep(board::FRAME_US - elapsed);
  } else {
    taskYIELD();
  }

  if (!delayed_for_frame_pacing) {
    frames_since_idle_feed++;
    if (frames_since_idle_feed >= board::EMULATOR_IDLE_FEED_FRAMES) {
      vTaskDelay(1);
      frames_since_idle_feed = 0;
    }
  } else {
    frames_since_idle_feed = 0;
  }
}
