#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include <stdint.h>

#ifndef GB_ENABLE_AUDIO
#define GB_ENABLE_AUDIO 1
#endif

namespace board {

// N64 controller port
#ifndef N64_JOYBUS_DATA_GPIO
#define N64_JOYBUS_DATA_GPIO 4
#endif

static constexpr int PIN_N64_JOYBUS_DATA = N64_JOYBUS_DATA_GPIO;

// Display (GC9A01 on SPI2) - retained only for legacy references while the
// active build targets the N64 controller/Transfer Pak runtime.
static constexpr int PIN_BACKLIGHT = 3;
static constexpr int PIN_TFT_DC = 2;
static constexpr int PIN_TFT_SCLK = 6;
static constexpr int PIN_TFT_MOSI = 7;
static constexpr int PIN_TFT_CS = 10;

// Touch (CST816D on I2C0)
static constexpr int PIN_TOUCH_SDA = 4;
static constexpr int PIN_TOUCH_SCL = 5;
static constexpr int PIN_TOUCH_RST = 1;
static constexpr int PIN_TOUCH_INT = 0;

// Dimensions
static constexpr int GAMEBOY_WIDTH = 160;
static constexpr int GAMEBOY_HEIGHT = 144;
static constexpr int SCREEN_WIDTH = 240;
static constexpr int SCREEN_HEIGHT = 240;
static constexpr int SCREEN_X_OFFSET = (SCREEN_WIDTH - GAMEBOY_WIDTH) / 2;
static constexpr int SCREEN_Y_OFFSET = (SCREEN_HEIGHT - GAMEBOY_HEIGHT) / 2;
static constexpr bool DISPLAY_MIRROR_X = true;
static constexpr bool DISPLAY_MIRROR_Y = false;
static constexpr bool TOUCH_SWAP_XY = false;
static constexpr bool TOUCH_MIRROR_X = false;
static constexpr bool TOUCH_MIRROR_Y = false;
static constexpr int TOUCH_TOP_ARROW_X = 0;
static constexpr int TOUCH_TOP_ARROW_Y = 0;
static constexpr int TOUCH_TOP_ARROW_W = SCREEN_WIDTH;
static constexpr int TOUCH_TOP_ARROW_H = SCREEN_Y_OFFSET;
static constexpr int TOUCH_BOTTOM_ARROW_X = 0;
static constexpr int TOUCH_BOTTOM_ARROW_Y = SCREEN_Y_OFFSET + GAMEBOY_HEIGHT;
static constexpr int TOUCH_BOTTOM_ARROW_W = SCREEN_WIDTH;
static constexpr int TOUCH_BOTTOM_ARROW_H =
    SCREEN_HEIGHT - TOUCH_BOTTOM_ARROW_Y;
static constexpr int TOUCH_LEFT_ARROW_X = 0;
static constexpr int TOUCH_LEFT_ARROW_Y = SCREEN_Y_OFFSET;
static constexpr int TOUCH_LEFT_ARROW_W = SCREEN_X_OFFSET;
static constexpr int TOUCH_LEFT_ARROW_H = GAMEBOY_HEIGHT;
static constexpr int TOUCH_RIGHT_ARROW_X = SCREEN_X_OFFSET + GAMEBOY_WIDTH;
static constexpr int TOUCH_RIGHT_ARROW_Y = SCREEN_Y_OFFSET;
static constexpr int TOUCH_RIGHT_ARROW_W =
    SCREEN_WIDTH - TOUCH_RIGHT_ARROW_X;
static constexpr int TOUCH_RIGHT_ARROW_H = GAMEBOY_HEIGHT;
static constexpr int TOUCH_B_X = SCREEN_X_OFFSET + 6;
static constexpr int TOUCH_B_Y = SCREEN_Y_OFFSET + 8;
static constexpr int TOUCH_B_W = 34;
static constexpr int TOUCH_B_H = 30;
static constexpr int TOUCH_SELECT_X = SCREEN_X_OFFSET + 6;
static constexpr int TOUCH_SELECT_Y =
    SCREEN_Y_OFFSET + GAMEBOY_HEIGHT - 8 - 30;
static constexpr int TOUCH_SELECT_W = 34;
static constexpr int TOUCH_SELECT_H = 30;
static constexpr int TOUCH_A_X = SCREEN_X_OFFSET + GAMEBOY_WIDTH - 6 - 34;
static constexpr int TOUCH_A_Y = SCREEN_Y_OFFSET + 8;
static constexpr int TOUCH_A_W = 34;
static constexpr int TOUCH_A_H = 30;
static constexpr int TOUCH_START_X =
    SCREEN_X_OFFSET + GAMEBOY_WIDTH - 6 - 34;
static constexpr int TOUCH_START_Y =
    SCREEN_Y_OFFSET + GAMEBOY_HEIGHT - 8 - 30;
static constexpr int TOUCH_START_W = 34;
static constexpr int TOUCH_START_H = 30;

// Timing
static constexpr uint32_t TFT_WRITE_HZ = 40000000;
static constexpr uint32_t TARGET_FPS = 60;
static constexpr uint32_t FRAME_US = 1000000u / TARGET_FPS;
static constexpr uint32_t EMULATOR_COOP_SLICE_US = 50000;
static constexpr uint32_t EMULATOR_WEB_AUDIO_SERVICE_US = 10000;
static constexpr uint32_t EMULATOR_IDLE_FEED_FRAMES = 16;
static constexpr uint32_t FRAME_SLEEP_GRANULARITY_US = 1000;
static constexpr uint32_t WEB_PORTAL_IDLE_SERVICE_INTERVAL_MS = 50;
static constexpr uint32_t WEB_INPUT_TIMEOUT_MS = 1500;
static constexpr uint32_t WEB_MIN_PRESS_MS = 180;
static constexpr uint32_t SAVE_FLUSH_DEBOUNCE_MS = 1000;

// WiFi soft AP and HTTP/WS
static constexpr const char *WEB_AP_SSID = "GameBoy-Link";
static constexpr const char *WEB_AP_PASSWORD = "gameboy123";
static constexpr bool WEB_AP_OPEN = true;
static constexpr uint16_t WEB_HTTP_PORT = 80;
static constexpr uint16_t WEB_SOCKET_PORT = 80;  // unified with HTTP under esp_http_server
// 50 ms ≈ 20 FPS. The C3 + a 5.7 KB packed frame can sustain ~15-20 FPS over
// the AP without saturating LWIP/wifi TX buffers (which manifests as
// `httpd_sock_err: error in send : 119` and the WS framing getting corrupted).
static constexpr uint16_t WEB_STREAM_INTERVAL_MS = 100;

static constexpr bool AUDIO_ENABLED = GB_ENABLE_AUDIO != 0;
static constexpr bool WEB_AUDIO_ENABLED = AUDIO_ENABLED;
static constexpr bool DEBUG_TOUCH_INPUT = false;
static constexpr bool DEBUG_WEB_INPUT = false;
static constexpr bool DEBUG_INTERRUPTS = false;

// DMG palette (RGB565)
static constexpr uint16_t DMG_PALETTE[4] = {
    0xFFFF,
    static_cast<uint16_t>((16 << 11) | (32 << 5) | 16),
    static_cast<uint16_t>((8 << 11) | (16 << 5) | 8),
    0x0000,
};

// SPIFFS mount point used for cartridge SRAM and the web UI bundle.
static constexpr const char *SPIFFS_MOUNT = "/spiffs";

}  // namespace board

#endif
