#include "display.h"

#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {
constexpr const char *TAG = "display";

esp_lcd_panel_io_handle_t io_handle = nullptr;
esp_lcd_panel_handle_t panel_handle = nullptr;
SemaphoreHandle_t trans_done_sem = nullptr;
bool overlay_dirty = false;

// Render the GB framebuffer in 24-row strips (160 × 24 = 3840 px = 7.5 KB)
// instead of staging the whole 46 KB frame. This cuts SPI transaction count by
// a third versus 16-row chunks while keeping C3 RAM usage predictable.
constexpr int CHUNK_ROWS = 24;
uint16_t scanout_chunk[board::GAMEBOY_WIDTH * CHUNK_ROWS];

// Small reusable row buffer for solid fills outside the GB area (status
// banner, decoration). Multiple consecutive draw_bitmap calls share the same
// data so reusing the row_buf across them is safe.
uint16_t row_buf[board::SCREEN_WIDTH];

constexpr uint16_t COLOR_BLACK = 0x0000;
constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t COLOR_DARKGREY = 0x4208;
constexpr uint16_t COLOR_SLATE = 0x2104;
constexpr uint16_t COLOR_GREEN = 0x07E0;
constexpr uint16_t COLOR_BLUE = 0x02BF;
constexpr uint16_t COLOR_ORANGE = 0xFD20;
constexpr uint16_t COLOR_RED = 0xF800;
constexpr uint16_t COLOR_YELLOW = 0xFFE0;
constexpr uint16_t COLOR_CYAN = 0x07FF;

inline uint16_t bswap16(uint16_t v) {
  return static_cast<uint16_t>((v << 8) | (v >> 8));
}

bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t /*io*/,
                                   esp_lcd_panel_io_event_data_t * /*edata*/,
                                   void * /*user_ctx*/) {
  BaseType_t hp_woken = pdFALSE;
  xSemaphoreGiveFromISR(trans_done_sem, &hp_woken);
  return hp_woken == pdTRUE;
}

// Wait for the previous DMA transfer to drain before reusing scanout_chunk.
void wait_trans_done() {
  if (!trans_done_sem) return;
  xSemaphoreTake(trans_done_sem, portMAX_DELAY);
}

void fill_rect(int x, int y, int w, int h, uint16_t color_be) {
  for (int i = 0; i < board::SCREEN_WIDTH; ++i) row_buf[i] = color_be;
  for (int row = y; row < y + h; ++row) {
    wait_trans_done();
    esp_lcd_panel_draw_bitmap(panel_handle, x, row, x + w, row + 1, row_buf);
  }
}

void fill_screen(uint16_t color_be) {
  fill_rect(0, 0, board::SCREEN_WIDTH, board::SCREEN_HEIGHT, color_be);
}

void draw_rect_outline(int x, int y, int w, int h, uint16_t color_be,
                       int thickness = 2) {
  if (w <= 0 || h <= 0 || thickness <= 0) return;
  fill_rect(x, y, w, thickness, color_be);
  fill_rect(x, y + h - thickness, w, thickness, color_be);
  fill_rect(x, y, thickness, h, color_be);
  fill_rect(x + w - thickness, y, thickness, h, color_be);
}

void draw_pixel_block(int x, int y, int scale, uint16_t color_be) {
  fill_rect(x, y, scale, scale, color_be);
}

const uint8_t *glyph_for(char c) {
  static constexpr uint8_t kGlyphA[7] = {0x0E, 0x11, 0x11, 0x1F,
                                         0x11, 0x11, 0x11};
  static constexpr uint8_t kGlyphB[7] = {0x1E, 0x11, 0x11, 0x1E,
                                         0x11, 0x11, 0x1E};
  static constexpr uint8_t kGlyphE[7] = {0x1F, 0x10, 0x10, 0x1E,
                                         0x10, 0x10, 0x1F};
  static constexpr uint8_t kGlyphL[7] = {0x10, 0x10, 0x10, 0x10,
                                         0x10, 0x10, 0x1F};
  static constexpr uint8_t kGlyphS[7] = {0x0F, 0x10, 0x10, 0x0E,
                                         0x01, 0x01, 0x1E};
  static constexpr uint8_t kGlyphT[7] = {0x1F, 0x04, 0x04, 0x04,
                                         0x04, 0x04, 0x04};

  switch (c) {
    case 'A':
      return kGlyphA;
    case 'B':
      return kGlyphB;
    case 'E':
      return kGlyphE;
    case 'L':
      return kGlyphL;
    case 'S':
      return kGlyphS;
    case 'T':
      return kGlyphT;
    default:
      return nullptr;
  }
}

template <typename PixelFn>
void draw_text_centered_generic(int x, int y, int w, int h, const char *text,
                                uint16_t color_be, int scale,
                                PixelFn draw_pixel) {
  if (!text || !*text) return;

  constexpr int kGlyphW = 5;
  constexpr int kGlyphH = 7;
  constexpr int kSpacing = 1;

  const int length = static_cast<int>(strlen(text));
  const int text_w = (length * kGlyphW + (length - 1) * kSpacing) * scale;
  const int text_h = kGlyphH * scale;
  int cursor_x = x + (w - text_w) / 2;
  const int start_y = y + (h - text_h) / 2;

  for (const char *p = text; *p; ++p) {
    const uint8_t *glyph = glyph_for(*p);
    if (!glyph) {
      cursor_x += (kGlyphW + kSpacing) * scale;
      continue;
    }
    for (int row = 0; row < kGlyphH; ++row) {
      for (int col = 0; col < kGlyphW; ++col) {
        if ((glyph[row] >> (kGlyphW - 1 - col)) & 0x01) {
          for (int sy = 0; sy < scale; ++sy) {
            for (int sx = 0; sx < scale; ++sx) {
              draw_pixel(cursor_x + col * scale + sx,
                         start_y + row * scale + sy, color_be);
            }
          }
        }
      }
    }
    cursor_x += (kGlyphW + kSpacing) * scale;
  }
}

void draw_text_centered(int x, int y, int w, int h, const char *text,
                        uint16_t color_be) {
  draw_text_centered_generic(
      x, y, w, h, text, color_be, 1,
      [](int px, int py, uint16_t c) { draw_pixel_block(px, py, 1, c); });
}

enum class ArrowDirection { Up, Down, Left, Right };

void draw_arrow_icon(int x, int y, int w, int h, ArrowDirection direction,
                     uint16_t color_be) {
  const int cx = x + w / 2;
  const int cy = y + h / 2;
  const int triangle_height = h > w ? w / 3 : h / 3;
  const int triangle_width = w > h ? h / 2 : w / 2;
  const int shaft_w = triangle_width / 3 < 4 ? 4 : triangle_width / 3;
  const int shaft_h = triangle_height < 8 ? 8 : triangle_height;
  (void)shaft_h;

  switch (direction) {
    case ArrowDirection::Up:
      for (int row = 0; row < triangle_height; ++row) {
        const int half = (triangle_width * row) / triangle_height;
        fill_rect(cx - half, y + 6 + row, half * 2 + 1, 1, color_be);
      }
      fill_rect(cx - shaft_w / 2, y + triangle_height + 6, shaft_w,
                h - triangle_height - 12, color_be);
      break;
    case ArrowDirection::Down:
      for (int row = 0; row < triangle_height; ++row) {
        const int half = (triangle_width * row) / triangle_height;
        fill_rect(cx - half, y + h - 7 - row, half * 2 + 1, 1, color_be);
      }
      fill_rect(cx - shaft_w / 2, y + 6, shaft_w, h - triangle_height - 12,
                color_be);
      break;
    case ArrowDirection::Left:
      for (int col = 0; col < triangle_height; ++col) {
        const int half = (triangle_width * col) / triangle_height;
        fill_rect(x + 6 + col, cy - half, 1, half * 2 + 1, color_be);
      }
      fill_rect(x + triangle_height + 6, cy - shaft_w / 2,
                w - triangle_height - 12, shaft_w, color_be);
      break;
    case ArrowDirection::Right:
      for (int col = 0; col < triangle_height; ++col) {
        const int half = (triangle_width * col) / triangle_height;
        fill_rect(x + w - 7 - col, cy - half, 1, half * 2 + 1, color_be);
      }
      fill_rect(x + 6, cy - shaft_w / 2, w - triangle_height - 12, shaft_w,
                color_be);
      break;
  }
}

void draw_button_base(int x, int y, int w, int h, uint16_t fill_be,
                      uint16_t border_be) {
  fill_rect(x, y, w, h, fill_be);
  draw_rect_outline(x, y, w, h, border_be, 2);
}

// On-frame translucent button overlay clipped to the active row chunk.
struct ChunkButton {
  int local_x;
  int local_y;
  int w;
  int h;
  uint16_t fill_be;
  uint16_t border_be;
  const char *label;
};

void overlay_button_into_chunk(const ChunkButton &btn, int chunk_y_start,
                               int chunk_rows) {
  const int btn_y_end = btn.local_y + btn.h;
  if (btn_y_end <= chunk_y_start) return;
  if (btn.local_y >= chunk_y_start + chunk_rows) return;

  const int local_x_start = btn.local_x < 0 ? 0 : btn.local_x;
  const int local_x_end = btn.local_x + btn.w > board::GAMEBOY_WIDTH
                              ? board::GAMEBOY_WIDTH
                              : btn.local_x + btn.w;

  // Pattern fill (every-other pixel) — gives the button a ghosted look so the
  // game pixels behind still bleed through.
  for (int row = 0; row < chunk_rows; ++row) {
    const int frame_row = chunk_y_start + row;
    if (frame_row < btn.local_y || frame_row >= btn_y_end) continue;
    if ((frame_row & 1) != 0) continue;
    uint16_t *line = scanout_chunk + row * board::GAMEBOY_WIDTH;
    for (int col = local_x_start; col < local_x_end; col += 2) {
      line[col] = btn.fill_be;
    }
  }

  // Border (2 px thick, top/bottom horizontal + left/right vertical).
  for (int t = 0; t < 2; ++t) {
    const int top_row = btn.local_y + t;
    if (top_row >= chunk_y_start && top_row < chunk_y_start + chunk_rows) {
      uint16_t *line = scanout_chunk + (top_row - chunk_y_start) *
                                            board::GAMEBOY_WIDTH;
      for (int col = local_x_start; col < local_x_end; ++col) {
        line[col] = btn.border_be;
      }
    }
    const int bot_row = btn_y_end - 1 - t;
    if (bot_row >= chunk_y_start && bot_row < chunk_y_start + chunk_rows) {
      uint16_t *line = scanout_chunk + (bot_row - chunk_y_start) *
                                            board::GAMEBOY_WIDTH;
      for (int col = local_x_start; col < local_x_end; ++col) {
        line[col] = btn.border_be;
      }
    }
  }
  for (int t = 0; t < 2; ++t) {
    const int left_col = btn.local_x + t;
    const int right_col = btn.local_x + btn.w - 1 - t;
    for (int row = 0; row < chunk_rows; ++row) {
      const int frame_row = chunk_y_start + row;
      if (frame_row < btn.local_y || frame_row >= btn_y_end) continue;
      uint16_t *line = scanout_chunk + row * board::GAMEBOY_WIDTH;
      if (left_col >= 0 && left_col < board::GAMEBOY_WIDTH) {
        line[left_col] = btn.border_be;
      }
      if (right_col >= 0 && right_col < board::GAMEBOY_WIDTH) {
        line[right_col] = btn.border_be;
      }
    }
  }

  // Centred label, clipped to chunk by the lambda.
  auto chunk_pixel_writer = [chunk_y_start, chunk_rows](int px, int py,
                                                        uint16_t c) {
    if (py < chunk_y_start || py >= chunk_y_start + chunk_rows) return;
    if (px < 0 || px >= board::GAMEBOY_WIDTH) return;
    scanout_chunk[(py - chunk_y_start) * board::GAMEBOY_WIDTH + px] = c;
  };
  draw_text_centered_generic(btn.local_x, btn.local_y, btn.w, btn.h, btn.label,
                             btn.border_be, 2, chunk_pixel_writer);
}

void apply_button_overlay_to_chunk(int chunk_y_start, int chunk_rows) {
  static const ChunkButton buttons[] = {
      {board::TOUCH_B_X - board::SCREEN_X_OFFSET,
       board::TOUCH_B_Y - board::SCREEN_Y_OFFSET, board::TOUCH_B_W,
       board::TOUCH_B_H, bswap16(COLOR_BLUE), bswap16(COLOR_WHITE), "B"},
      {board::TOUCH_A_X - board::SCREEN_X_OFFSET,
       board::TOUCH_A_Y - board::SCREEN_Y_OFFSET, board::TOUCH_A_W,
       board::TOUCH_A_H, bswap16(COLOR_GREEN), bswap16(COLOR_WHITE), "A"},
      {board::TOUCH_SELECT_X - board::SCREEN_X_OFFSET,
       board::TOUCH_SELECT_Y - board::SCREEN_Y_OFFSET, board::TOUCH_SELECT_W,
       board::TOUCH_SELECT_H, bswap16(COLOR_CYAN), bswap16(COLOR_WHITE), "SEL"},
      {board::TOUCH_START_X - board::SCREEN_X_OFFSET,
       board::TOUCH_START_Y - board::SCREEN_Y_OFFSET, board::TOUCH_START_W,
       board::TOUCH_START_H, bswap16(COLOR_ORANGE), bswap16(COLOR_WHITE), "ST"},
  };

  for (const auto &btn : buttons) {
    overlay_button_into_chunk(btn, chunk_y_start, chunk_rows);
  }
}

uint16_t status_color_for(const char *line) {
  if (!line) return COLOR_BLACK;
  if (strstr(line, "ready") || strstr(line, "Ready")) return COLOR_GREEN;
  if (strstr(line, "error") || strstr(line, "Error") ||
      strstr(line, "failed") || strstr(line, "stopped"))
    return COLOR_RED;
  if (strstr(line, "Loading") || strstr(line, "WiFi") ||
      strstr(line, "Starting"))
    return COLOR_CYAN;
  if (strstr(line, "Memory") || strstr(line, "ROM")) return COLOR_YELLOW;
  return COLOR_DARKGREY;
}

void draw_touch_overlay() {
  fill_screen(bswap16(COLOR_BLACK));

  draw_rect_outline(board::SCREEN_X_OFFSET - 3, board::SCREEN_Y_OFFSET - 3,
                    board::GAMEBOY_WIDTH + 6, board::GAMEBOY_HEIGHT + 6,
                    bswap16(COLOR_SLATE), 3);
  draw_rect_outline(board::SCREEN_X_OFFSET - 1, board::SCREEN_Y_OFFSET - 1,
                    board::GAMEBOY_WIDTH + 2, board::GAMEBOY_HEIGHT + 2,
                    bswap16(COLOR_WHITE), 1);

  draw_button_base(board::TOUCH_TOP_ARROW_X, board::TOUCH_TOP_ARROW_Y,
                   board::TOUCH_TOP_ARROW_W, board::TOUCH_TOP_ARROW_H,
                   bswap16(COLOR_DARKGREY), bswap16(COLOR_WHITE));
  draw_arrow_icon(board::TOUCH_TOP_ARROW_X, board::TOUCH_TOP_ARROW_Y,
                  board::TOUCH_TOP_ARROW_W, board::TOUCH_TOP_ARROW_H,
                  ArrowDirection::Up, bswap16(COLOR_WHITE));

  draw_button_base(board::TOUCH_BOTTOM_ARROW_X, board::TOUCH_BOTTOM_ARROW_Y,
                   board::TOUCH_BOTTOM_ARROW_W, board::TOUCH_BOTTOM_ARROW_H,
                   bswap16(COLOR_DARKGREY), bswap16(COLOR_WHITE));
  draw_arrow_icon(board::TOUCH_BOTTOM_ARROW_X, board::TOUCH_BOTTOM_ARROW_Y,
                  board::TOUCH_BOTTOM_ARROW_W, board::TOUCH_BOTTOM_ARROW_H,
                  ArrowDirection::Down, bswap16(COLOR_WHITE));

  draw_button_base(board::TOUCH_LEFT_ARROW_X, board::TOUCH_LEFT_ARROW_Y,
                   board::TOUCH_LEFT_ARROW_W, board::TOUCH_LEFT_ARROW_H,
                   bswap16(COLOR_DARKGREY), bswap16(COLOR_WHITE));
  draw_arrow_icon(board::TOUCH_LEFT_ARROW_X, board::TOUCH_LEFT_ARROW_Y,
                  board::TOUCH_LEFT_ARROW_W, board::TOUCH_LEFT_ARROW_H,
                  ArrowDirection::Left, bswap16(COLOR_WHITE));

  draw_button_base(board::TOUCH_RIGHT_ARROW_X, board::TOUCH_RIGHT_ARROW_Y,
                   board::TOUCH_RIGHT_ARROW_W, board::TOUCH_RIGHT_ARROW_H,
                   bswap16(COLOR_DARKGREY), bswap16(COLOR_WHITE));
  draw_arrow_icon(board::TOUCH_RIGHT_ARROW_X, board::TOUCH_RIGHT_ARROW_Y,
                  board::TOUCH_RIGHT_ARROW_W, board::TOUCH_RIGHT_ARROW_H,
                  ArrowDirection::Right, bswap16(COLOR_WHITE));
}

}  // namespace

void display_init(void) {
  // Backlight pin
  gpio_config_t bl_cfg = {};
  bl_cfg.pin_bit_mask = 1ULL << board::PIN_BACKLIGHT;
  bl_cfg.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&bl_cfg));
  gpio_set_level(static_cast<gpio_num_t>(board::PIN_BACKLIGHT), 1);

  // SPI bus — sized for the chunked transfer (up to 32 rows × 240 px × 2 bytes).
  spi_bus_config_t bus_cfg = {};
  bus_cfg.sclk_io_num = board::PIN_TFT_SCLK;
  bus_cfg.mosi_io_num = board::PIN_TFT_MOSI;
  bus_cfg.miso_io_num = -1;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.max_transfer_sz = board::SCREEN_WIDTH * 32 * sizeof(uint16_t);
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

  // Panel IO. A queue depth of 4 lets a few transfers pipeline while still
  // keeping the code simple — we explicitly serialize with the trans-done
  // semaphore between each chunk so the single scanout buffer is safe.
  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = static_cast<gpio_num_t>(board::PIN_TFT_CS);
  io_config.dc_gpio_num = static_cast<gpio_num_t>(board::PIN_TFT_DC);
  io_config.spi_mode = 0;
  io_config.pclk_hz = board::TFT_WRITE_HZ;
  io_config.trans_queue_depth = 4;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
      static_cast<esp_lcd_spi_bus_handle_t>(static_cast<int>(SPI2_HOST)),
      &io_config, &io_handle));

  // Trans-done semaphore — start in "given" state so the first wait succeeds
  // immediately. The on_color_trans_done callback signals it from ISR.
  trans_done_sem = xSemaphoreCreateBinary();
  configASSERT(trans_done_sem);
  xSemaphoreGive(trans_done_sem);

  esp_lcd_panel_io_callbacks_t cbs = {};
  cbs.on_color_trans_done = on_color_trans_done;
  ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs,
                                                            nullptr));

  // Panel
  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = GPIO_NUM_NC;
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
  panel_cfg.bits_per_pixel = 16;
  ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_cfg, &panel_handle));

  esp_lcd_panel_reset(panel_handle);
  esp_lcd_panel_init(panel_handle);
  esp_lcd_panel_invert_color(panel_handle, true);
  esp_lcd_panel_mirror(panel_handle, board::DISPLAY_MIRROR_X,
                       board::DISPLAY_MIRROR_Y);
  esp_lcd_panel_disp_on_off(panel_handle, true);

  draw_touch_overlay();
  overlay_dirty = false;

  ESP_LOGI(TAG, "GC9A01 ready (chunked rendering, %d rows/chunk)", CHUNK_ROWS);
}

void display_present_indexed_frame(const uint8_t *framebuffer) {
  if (overlay_dirty) {
    draw_touch_overlay();
    overlay_dirty = false;
  }

  const uint16_t *palette = board::DMG_PALETTE;
  // Pre-byte-swap the palette into a stack-local table so each pixel is a
  // single byte→uint16 lookup with no shift/swap inside the inner loop.
  uint16_t pal_be[4] = {bswap16(palette[0]), bswap16(palette[1]),
                        bswap16(palette[2]), bswap16(palette[3])};

  for (int chunk_y = 0; chunk_y < board::GAMEBOY_HEIGHT; chunk_y += CHUNK_ROWS) {
    int rows = CHUNK_ROWS;
    if (chunk_y + rows > board::GAMEBOY_HEIGHT) {
      rows = board::GAMEBOY_HEIGHT - chunk_y;
    }

    // Wait for the previous chunk's DMA to drain before stamping new pixels
    // into the shared scanout_chunk buffer.
    wait_trans_done();

    for (int row = 0; row < rows; ++row) {
      const uint8_t *src =
          framebuffer + (chunk_y + row) * board::GAMEBOY_WIDTH;
      uint16_t *dst = scanout_chunk + row * board::GAMEBOY_WIDTH;
      for (int x = 0; x < board::GAMEBOY_WIDTH; ++x) {
        dst[x] = pal_be[src[x] & 0x03];
      }
    }

    apply_button_overlay_to_chunk(chunk_y, rows);

    esp_lcd_panel_draw_bitmap(
        panel_handle, board::SCREEN_X_OFFSET,
        board::SCREEN_Y_OFFSET + chunk_y,
        board::SCREEN_X_OFFSET + board::GAMEBOY_WIDTH,
        board::SCREEN_Y_OFFSET + chunk_y + rows, scanout_chunk);
  }
}

void display_show_status(const char *line1, const char *line2) {
  if (!panel_handle) {
    ESP_LOGI(TAG, "[boot] %s | %s", line1 ? line1 : "", line2 ? line2 : "");
    return;
  }

  draw_touch_overlay();
  draw_rect_outline(board::SCREEN_X_OFFSET - 4, board::SCREEN_Y_OFFSET - 4,
                    board::GAMEBOY_WIDTH + 8, board::GAMEBOY_HEIGHT + 8,
                    bswap16(status_color_for(line1)), 4);
  overlay_dirty = true;

  ESP_LOGI(TAG, "[status] %s | %s", line1 ? line1 : "", line2 ? line2 : "");
}
