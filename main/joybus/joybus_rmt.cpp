#include "joybus_rmt.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_log.h"
#include "n64_accessory.h"
#include "n64_controller.h"

namespace {
constexpr const char *TAG = "joybus_rmt";

// 0.1 us per tick. A bit's low pulse < 2 us (20 ticks) decodes as 1, else 0.
constexpr uint32_t RMT_RESOLUTION_HZ = 10 * 1000 * 1000;
constexpr uint32_t TICKS_PER_US = RMT_RESOLUTION_HZ / 1000000;  // 10
constexpr uint32_t LOW_THRESHOLD_TICKS = 20;
constexpr size_t RX_MEM_SYMBOLS = 64;
// Accessory writes are 35 command bytes plus a stop bit (281 Joy-Bus symbols).
// Keep the RMT hardware block small, but give the driver a larger software
// receive buffer so those long frames are decoded instead of truncated.
constexpr size_t RX_BUFFER_SYMBOLS = 384;
constexpr size_t MAX_CMD_BYTES = 40;
constexpr uint32_t CPU_CYCLES_PER_US = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
constexpr uint32_t BIT_LOW_ONE_US = 1;
constexpr uint32_t BIT_RELEASE_ONE_US = 3;
constexpr uint32_t BIT_LOW_ZERO_US = 3;
constexpr uint32_t BIT_RELEASE_ZERO_US = 1;
constexpr uint32_t ACTIVE_LOG_SUPPRESS_MS = 1500;
constexpr uint32_t IDLE_LOG_INTERVAL_MS = 5000;
// Approximate RX-done floor from RMT's idle-high frame terminator. This lets the
// log report end-of-command-to-first-response latency, not only callback latency.
constexpr uint32_t RX_IDLE_GAP_CYCLES = 4 * CPU_CYCLES_PER_US;

int data_gpio = -1;
rmt_channel_handle_t rx_chan = nullptr;
rmt_symbol_word_t rx_symbols[RX_BUFFER_SYMBOLS];
JoybusRmtStats stats = {};

volatile uint32_t rx_frame_count = 0;
volatile uint32_t rx_decoded_count = 0;
volatile uint32_t rx_status_count = 0;
volatile uint32_t rx_poll_count = 0;
volatile uint32_t rx_read_count = 0;
volatile uint32_t rx_write_count = 0;
volatile uint32_t rx_other_count = 0;
volatile uint8_t last_len_seen = 0;
volatile uint8_t last_bytes_seen[4] = {0x00, 0x00, 0x00, 0x00};
volatile uint16_t last_symbols_seen = 0;
volatile uint8_t cached_status_bytes[3] = {0x05, 0x00, 0x01};
volatile uint8_t cached_poll_bytes[4] = {0x00, 0x00, 0x00, 0x00};
volatile uint32_t last_turnaround_us = 0;
volatile uint32_t tx_status_count = 0;
volatile uint32_t tx_poll_count = 0;
volatile uint32_t tx_read_count = 0;
volatile uint32_t tx_write_count = 0;
volatile uint32_t tx_failures_count = 0;
volatile uint32_t rx_rearm_failures_count = 0;
volatile uint16_t last_accessory_addr = 0;
volatile uint8_t last_accessory_value = 0;
volatile uint8_t last_accessory_crc = 0;
volatile bool last_accessory_is_write = false;
uint8_t crc_table_crc[256];
uint8_t crc_table_data[256];

// End the frame after this much idle-high — just above the ~3 us inter-bit high,
// to minimize the latency before RX-done (the turnaround floor).
const rmt_receive_config_t rx_config = {
    .signal_range_min_ns = 200,
    .signal_range_max_ns = 3500,
    .flags = {.en_partial_rx = 0},
};

uint8_t IRAM_ATTR decode_symbols(const rmt_symbol_word_t *syms, size_t n,
                                 uint8_t *out, size_t cap) {
  uint8_t current = 0, bit_count = 0, byte_count = 0;
  for (size_t i = 0; i < n && byte_count < cap; ++i) {
    const uint16_t low_ticks =
        (syms[i].level0 == 0) ? syms[i].duration0 : syms[i].duration1;
    if (low_ticks == 0) continue;
    current = static_cast<uint8_t>((current << 1) |
                                   (low_ticks < LOW_THRESHOLD_TICKS ? 1 : 0));
    if (++bit_count == 8) {
      out[byte_count++] = current;
      current = 0;
      bit_count = 0;
    }
  }
  return byte_count;
}

void IRAM_ATTR bump(volatile uint32_t &counter) { counter = counter + 1; }

uint8_t crc_step_byte(uint8_t crc, uint8_t value) {
  for (int bit = 7; bit >= 0; --bit) {
    const uint8_t xor_tap = (crc & 0x80) ? 0x85 : 0x00;
    crc = static_cast<uint8_t>((crc << 1) | ((value >> bit) & 1u));
    crc ^= xor_tap;
  }
  return crc;
}

void build_crc_tables(void) {
  for (int i = 0; i < 256; ++i) {
    crc_table_crc[i] = crc_step_byte(static_cast<uint8_t>(i), 0x00);
    crc_table_data[i] = crc_step_byte(0x00, static_cast<uint8_t>(i));
  }
}

uint8_t IRAM_ATTR accessory_data_crc(const uint8_t *data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    crc = static_cast<uint8_t>(crc_table_crc[crc] ^ crc_table_data[data[i]]);
  }
  return crc_table_crc[crc];
}

uint16_t IRAM_ATTR accessory_block_address(uint8_t hi, uint8_t lo) {
  return static_cast<uint16_t>(((static_cast<uint16_t>(hi) << 8) | lo) & 0xFFE0);
}

void IRAM_ATTR wait_us(uint32_t us) {
  const uint32_t start = esp_cpu_get_cycle_count();
  const uint32_t cycles = us * CPU_CYCLES_PER_US;
  while (esp_cpu_get_cycle_count() - start < cycles) {
  }
}

void IRAM_ATTR drive_low(void) {
  if (data_gpio >= 0) gpio_set_level(static_cast<gpio_num_t>(data_gpio), 0);
}

void IRAM_ATTR release_line(void) {
  if (data_gpio >= 0) gpio_set_level(static_cast<gpio_num_t>(data_gpio), 1);
}

void IRAM_ATTR tx_bit(bool bit) {
  if (bit) {
    drive_low();
    wait_us(BIT_LOW_ONE_US);
    release_line();
    wait_us(BIT_RELEASE_ONE_US);
  } else {
    drive_low();
    wait_us(BIT_LOW_ZERO_US);
    release_line();
    wait_us(BIT_RELEASE_ZERO_US);
  }
}

void IRAM_ATTR tx_stop(void) {
  drive_low();
  wait_us(1);
  release_line();
}

bool IRAM_ATTR tx_response(const uint8_t *bytes, size_t len,
                           uint32_t rx_done_cycles) {
  if (!bytes || len == 0 || data_gpio < 0) return false;
  const uint32_t tx_start = esp_cpu_get_cycle_count();
  const uint32_t cmd_end =
      rx_done_cycles > RX_IDLE_GAP_CYCLES ? rx_done_cycles - RX_IDLE_GAP_CYCLES
                                          : rx_done_cycles;
  last_turnaround_us = (tx_start - cmd_end) / CPU_CYCLES_PER_US;
  for (size_t i = 0; i < len; ++i) {
    for (int bit = 7; bit >= 0; --bit) {
      tx_bit((bytes[i] & (1u << bit)) != 0);
    }
  }
  tx_stop();
  return true;
}

void refresh_response_cache(void) {
  const N64ControllerStatusResponse status = n64_controller_status_response();
  cached_status_bytes[0] = status.device_high;
  cached_status_bytes[1] = status.device_low;
  cached_status_bytes[2] = status.status;

  const N64ControllerPollResponse poll = n64_controller_poll_response();
  cached_poll_bytes[0] = poll.bytes[0];
  cached_poll_bytes[1] = poll.bytes[1];
  cached_poll_bytes[2] = poll.bytes[2];
  cached_poll_bytes[3] = poll.bytes[3];
}

void IRAM_ATTR rearm_rx_from_isr(rmt_channel_handle_t chan) {
  if (rmt_receive(chan, rx_symbols, sizeof(rx_symbols), &rx_config) != ESP_OK) {
    bump(rx_rearm_failures_count);
  }
}

bool IRAM_ATTR on_rx_done(rmt_channel_handle_t chan,
                          const rmt_rx_done_event_data_t *edata, void *ctx) {
  const uint32_t now = esp_cpu_get_cycle_count();
  uint8_t bytes[MAX_CMD_BYTES];
  const uint8_t len = decode_symbols(edata->received_symbols,
                                     edata->num_symbols, bytes, sizeof(bytes));

  bump(rx_frame_count);
  last_symbols_seen = static_cast<uint16_t>(edata->num_symbols);
  last_len_seen = len;
  for (uint8_t i = 0; i < sizeof(last_bytes_seen); ++i) {
    last_bytes_seen[i] = i < len ? bytes[i] : 0x00;
  }

  const uint8_t op = len > 0 ? bytes[0] : 0xEE;
  if (len > 0) {
    bump(rx_decoded_count);
    switch (op) {
      case 0x00:
      case 0xFF: bump(rx_status_count); break;
      case 0x01: bump(rx_poll_count); break;
      case 0x02: bump(rx_read_count); break;
      case 0x03: bump(rx_write_count); break;
      default: bump(rx_other_count); break;
    }
  }

  if (op == 0x00 || op == 0xFF) {
    const uint8_t reply[3] = {cached_status_bytes[0], cached_status_bytes[1],
                              cached_status_bytes[2]};
    if (tx_response(reply, sizeof(reply), now)) {
      bump(tx_status_count);
    } else {
      bump(tx_failures_count);
    }
  } else if (op == 0x01) {
    const uint8_t reply[4] = {cached_poll_bytes[0], cached_poll_bytes[1],
                              cached_poll_bytes[2], cached_poll_bytes[3]};
    if (tx_response(reply, sizeof(reply), now)) {
      bump(tx_poll_count);
    } else {
      bump(tx_failures_count);
    }
  } else if (op == 0x02 && len >= 3) {
    uint8_t reply[TRANSFER_PAK_BLOCK_SIZE + 1] = {};
    const uint16_t addr = accessory_block_address(bytes[1], bytes[2]);
    uint8_t status = 0;
    n64_accessory_read(addr, reply, TRANSFER_PAK_BLOCK_SIZE, &status);
    reply[TRANSFER_PAK_BLOCK_SIZE] =
        accessory_data_crc(reply, TRANSFER_PAK_BLOCK_SIZE);
    last_accessory_addr = addr;
    last_accessory_value = reply[0];
    last_accessory_crc = reply[TRANSFER_PAK_BLOCK_SIZE];
    last_accessory_is_write = false;
    if (tx_response(reply, sizeof(reply), now)) {
      bump(tx_read_count);
    } else {
      bump(tx_failures_count);
    }
  } else if (op == 0x03 && len >= 3 + TRANSFER_PAK_BLOCK_SIZE) {
    const uint16_t addr = accessory_block_address(bytes[1], bytes[2]);
    const uint8_t crc = accessory_data_crc(bytes + 3, TRANSFER_PAK_BLOCK_SIZE);
    last_accessory_addr = addr;
    last_accessory_value = bytes[3];
    last_accessory_crc = crc;
    last_accessory_is_write = true;
    if (tx_response(&crc, 1, now)) {
      bump(tx_write_count);
      uint8_t status = 0;
      n64_accessory_write(addr, bytes + 3, TRANSFER_PAK_BLOCK_SIZE, &status);
    } else {
      bump(tx_failures_count);
    }
  }

  rearm_rx_from_isr(chan);
  return false;
}
}  // namespace

bool joybus_rmt_init(int gpio_num) {
  data_gpio = gpio_num;
  memset(&stats, 0, sizeof(stats));
  rx_frame_count = 0;
  rx_decoded_count = 0;
  rx_status_count = 0;
  rx_poll_count = 0;
  rx_read_count = 0;
  rx_write_count = 0;
  rx_other_count = 0;
  last_len_seen = 0;
  last_symbols_seen = 0;
  for (uint8_t i = 0; i < sizeof(last_bytes_seen); ++i) last_bytes_seen[i] = 0;
  tx_status_count = 0;
  tx_poll_count = 0;
  tx_read_count = 0;
  tx_write_count = 0;
  tx_failures_count = 0;
  rx_rearm_failures_count = 0;
  last_turnaround_us = 0;
  last_accessory_addr = 0;
  last_accessory_value = 0;
  last_accessory_crc = 0;
  last_accessory_is_write = false;
  build_crc_tables();
  refresh_response_cache();

  gpio_config_t gpio_cfg = {};
  gpio_cfg.pin_bit_mask = 1ULL << gpio_num;
  gpio_cfg.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  gpio_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_cfg.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&gpio_cfg) != ESP_OK) return false;
  release_line();

  rmt_rx_channel_config_t rx_cfg = {};
  rx_cfg.gpio_num = static_cast<gpio_num_t>(gpio_num);
  rx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  rx_cfg.resolution_hz = RMT_RESOLUTION_HZ;
  rx_cfg.mem_block_symbols = RX_MEM_SYMBOLS;
  if (rmt_new_rx_channel(&rx_cfg, &rx_chan) != ESP_OK) {
    ESP_LOGE(TAG, "rmt_new_rx_channel failed");
    return false;
  }

  // Pull-up so the open-drain line idles high (the console drives it low).
  gpio_set_direction(static_cast<gpio_num_t>(gpio_num), GPIO_MODE_INPUT_OUTPUT_OD);
  gpio_set_pull_mode(static_cast<gpio_num_t>(gpio_num), GPIO_PULLUP_ONLY);
  release_line();

  rmt_rx_event_callbacks_t rx_cbs = {};
  rx_cbs.on_recv_done = on_rx_done;
  rmt_rx_register_event_callbacks(rx_chan, &rx_cbs, nullptr);

  if (rmt_enable(rx_chan) != ESP_OK) {
    ESP_LOGE(TAG, "rmt_enable failed");
    return false;
  }

  if (rmt_receive(rx_chan, rx_symbols, sizeof(rx_symbols), &rx_config) !=
      ESP_OK) {
    ESP_LOGE(TAG, "rmt_receive failed");
    return false;
  }
  ESP_LOGI(TAG, "RMT RX + immediate OD GPIO TX armed on GPIO%d (%.1f MHz)",
           gpio_num, RMT_RESOLUTION_HZ / 1e6);
  return true;
}

const JoybusRmtStats &joybus_rmt_stats(void) { return stats; }

bool joybus_rmt_pause(void) {
  if (!rx_chan) return false;
  // Quiesce RX so the console is not answered while a flash write (ROM/save
  // upload) has the cache disabled and the response ISR cannot run.
  return rmt_disable(rx_chan) == ESP_OK;
}

bool joybus_rmt_resume(void) {
  if (!rx_chan) return false;
  if (rmt_enable(rx_chan) != ESP_OK) return false;
  return rmt_receive(rx_chan, rx_symbols, sizeof(rx_symbols), &rx_config) ==
         ESP_OK;
}

bool joybus_rmt_loop(uint32_t now_ms) {
  refresh_response_cache();

  const uint32_t prev_frames = stats.frames;
  stats.frames = rx_frame_count;
  stats.decoded = rx_decoded_count;
  stats.status_cmds = rx_status_count;
  stats.poll_cmds = rx_poll_count;
  stats.read_cmds = rx_read_count;
  stats.write_cmds = rx_write_count;
  stats.other_cmds = rx_other_count;
  stats.tx_status = tx_status_count;
  stats.tx_poll = tx_poll_count;
  stats.tx_read = tx_read_count;
  stats.tx_write = tx_write_count;
  stats.tx_failures = tx_failures_count;
  stats.rx_rearm_failures = rx_rearm_failures_count;
  stats.last_turnaround_us = last_turnaround_us;
  stats.last_accessory_addr = last_accessory_addr;
  stats.last_accessory_value = last_accessory_value;
  stats.last_accessory_crc = last_accessory_crc;
  stats.last_accessory_is_write = last_accessory_is_write;
  stats.current_poll_reply[0] = cached_poll_bytes[0];
  stats.current_poll_reply[1] = cached_poll_bytes[1];
  stats.current_poll_reply[2] = cached_poll_bytes[2];
  stats.current_poll_reply[3] = cached_poll_bytes[3];
  stats.last_len = last_len_seen;
  stats.last_symbols = last_symbols_seen;
  for (uint8_t i = 0; i < sizeof(stats.last_bytes); ++i) {
    stats.last_bytes[i] = last_bytes_seen[i];
  }

  const bool active = stats.frames != prev_frames;

  static uint32_t last_log_ms = 0;
  static uint32_t last_activity_ms = 0;
  if (active) last_activity_ms = now_ms;
  if (last_activity_ms != 0 &&
      now_ms - last_activity_ms < ACTIVE_LOG_SUPPRESS_MS) {
    return active;
  }
  if (last_log_ms != 0 && now_ms - last_log_ms < IDLE_LOG_INTERVAL_MS) {
    return active;
  }
  last_log_ms = now_ms;
  ESP_LOGI(TAG,
           "rmt frames=%lu dec=%lu status=%lu poll=%lu rd=%lu wr=%lu other=%lu "
           "txS=%lu txP=%lu txR=%lu txW=%lu txFail=%lu rearmFail=%lu "
           "turn=%luus | reply [%02X %02X %02X %02X] | acc %c%04X=%02X/%02X | "
           "last len=%u sym=%u [%02X %02X %02X]",
           (unsigned long)stats.frames, (unsigned long)stats.decoded,
           (unsigned long)stats.status_cmds, (unsigned long)stats.poll_cmds,
           (unsigned long)stats.read_cmds, (unsigned long)stats.write_cmds,
           (unsigned long)stats.other_cmds, (unsigned long)stats.tx_status,
           (unsigned long)stats.tx_poll, (unsigned long)stats.tx_read,
           (unsigned long)stats.tx_write, (unsigned long)stats.tx_failures,
           (unsigned long)stats.rx_rearm_failures,
           (unsigned long)stats.last_turnaround_us,
           stats.current_poll_reply[0], stats.current_poll_reply[1],
           stats.current_poll_reply[2], stats.current_poll_reply[3],
           stats.last_accessory_is_write ? 'W' : 'R',
           stats.last_accessory_addr, stats.last_accessory_value,
           stats.last_accessory_crc,
           stats.last_len, stats.last_symbols, stats.last_bytes[0],
           stats.last_bytes[1], stats.last_bytes[2]);
  return active;
}
