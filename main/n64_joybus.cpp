#include "n64_joybus.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_cpu.h"
#include "gb_time.h"
#include "hal/gpio_ll.h"
#include "n64_accessory.h"
#include "n64_controller.h"

namespace {
constexpr uint32_t BIT_LOW_ONE_US = 1;
constexpr uint32_t BIT_RELEASE_ONE_US = 3;
constexpr uint32_t BIT_LOW_ZERO_US = 3;
constexpr uint32_t BIT_RELEASE_ZERO_US = 1;
constexpr size_t MAX_COMMAND_BYTES = 40;
// Budget between the end of a command and the first response bit, beyond which
// the console may treat the port as empty. Tracked in CPU cycles so measuring it
// adds no latency of its own (see note_response_latency).
constexpr uint32_t RESPONSE_WINDOW_US = 6;

// The Joy-Bus encodes each bit in a ~4 us cell: a '1' holds the line low for
// ~1 us, a '0' for ~3 us. Distinguishing them needs sub-microsecond timing, so
// the receiver measures pulse widths with the CPU cycle counter instead of the
// 1 us-resolution esp_timer. All thresholds below are expressed in CPU cycles.
constexpr uint32_t CPU_CYCLES_PER_US = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
// Low pulses shorter than ~2 us decode as '1', longer as '0'.
constexpr uint32_t BIT_LOW_THRESHOLD_CYCLES = 2 * CPU_CYCLES_PER_US;
// A low held past this is a stuck line (e.g. console powered off), not a bit.
constexpr uint32_t BIT_STUCK_LOW_CYCLES = 12 * CPU_CYCLES_PER_US;
// A high gap longer than this between bits marks the end of the command frame.
constexpr uint32_t COMMAND_IDLE_CYCLES = 80 * CPU_CYCLES_PER_US;
// Continuous-high duration that means a frame has ended (longer than the ~1-3 us
// inter-bit gaps). Used by drain_to_idle to coalesce one frame into one ISR.
constexpr uint32_t FRAME_END_IDLE_CYCLES = 4 * CPU_CYCLES_PER_US;
// Upper bound on how long drain_to_idle will wait, so a stuck line cannot hang
// the ISR.
constexpr uint32_t DRAIN_MAX_CYCLES = 400 * CPU_CYCLES_PER_US;

int data_gpio = -1;
N64JoybusDebug debug = {};

// Total byte count of a command frame for a given opcode, or 0 if the opcode is
// unknown. Length-driven framing lets the reader return the instant the frame is
// complete instead of waiting out the inter-bit idle timeout.
size_t command_length(uint8_t opcode) {
  switch (opcode) {
    case 0x00:  // status / identify
    case 0xFF:  // reset + status
    case 0x01:  // poll buttons
      return 1;
    case 0x02:  // accessory read: opcode + 2 address bytes
      return 3;
    case 0x03:  // accessory write: opcode + 2 address bytes + data block
      return 3 + TRANSFER_PAK_BLOCK_SIZE;
    default:
      return 0;
  }
}

void note_response_latency(uint32_t cmd_end_cycles) {
  if (esp_cpu_get_cycle_count() - cmd_end_cycles >
      RESPONSE_WINDOW_US * CPU_CYCLES_PER_US) {
    debug.response_failures++;
  }
}

// N64 controller-accessory data CRC over a data block (polynomial 0x85). The
// console appends/expects this byte on every Transfer Pak read and write and
// rejects the accessory if it does not match, so it must be computed exactly.
uint8_t accessory_data_crc(const uint8_t *data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i <= len; ++i) {
    for (int bit = 7; bit >= 0; --bit) {
      const uint8_t xor_tap = (crc & 0x80) ? 0x85 : 0x00;
      crc = static_cast<uint8_t>(crc << 1);
      if (i < len && (data[i] & (1u << bit)) != 0) crc |= 0x01;
      crc ^= xor_tap;
    }
  }
  return crc;
}

// The 16-bit accessory address carries a 5-bit CRC in its low bits; the real
// block address is the upper 11 bits (also 32-byte aligned).
inline uint16_t accessory_block_address(uint8_t hi, uint8_t lo) {
  return static_cast<uint16_t>(((static_cast<uint16_t>(hi) << 8) | lo) & 0xFFE0);
}

void IRAM_ATTR tx_bit(bool bit) {
  if (bit) {
    n64_joybus_drive_low();
    delayMicroseconds(BIT_LOW_ONE_US);
    n64_joybus_release();
    delayMicroseconds(BIT_RELEASE_ONE_US);
  } else {
    n64_joybus_drive_low();
    delayMicroseconds(BIT_LOW_ZERO_US);
    n64_joybus_release();
    delayMicroseconds(BIT_RELEASE_ZERO_US);
  }
}

void tx_stop(void) {
  n64_joybus_drive_low();
  delayMicroseconds(1);
  n64_joybus_release();
}

void tx_byte(uint8_t value) {
  for (int bit = 7; bit >= 0; --bit) tx_bit((value & (1u << bit)) != 0);
}

void tx_bytes(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; ++i) tx_byte(data[i]);
  tx_stop();
}

// Reads one Joy-Bus bit by timing its low pulse with the CPU cycle counter.
// Returns 1/0 for a decoded bit, -1 when the line stays idle past the frame
// gap (no more bits coming), or -2 when the line is stuck low (fault / console
// off). *last_edge holds the cycle count of the previous rising edge.
int IRAM_ATTR read_bit(gpio_num_t pin, uint32_t *last_edge) {
  while (gpio_get_level(pin) != 0) {
    if (esp_cpu_get_cycle_count() - *last_edge > COMMAND_IDLE_CYCLES) return -1;
  }
  const uint32_t low_start = esp_cpu_get_cycle_count();
  while (gpio_get_level(pin) == 0) {
    if (esp_cpu_get_cycle_count() - low_start > BIT_STUCK_LOW_CYCLES) return -2;
  }
  const uint32_t low_cycles = esp_cpu_get_cycle_count() - low_start;
  *last_edge = esp_cpu_get_cycle_count();
  return low_cycles < BIT_LOW_THRESHOLD_CYCLES ? 1 : 0;
}

// Spins until the line has stayed high continuously for longer than any inter-bit
// gap, i.e. the frame is over. Called before clearing the interrupt so the many
// falling edges within one frame (and our own response edges) are consumed by a
// single ISR invocation instead of each re-triggering it bit-by-bit.
void IRAM_ATTR drain_to_idle(gpio_num_t pin) {
  const uint32_t start = esp_cpu_get_cycle_count();
  uint32_t last_low = start;
  for (;;) {
    const uint32_t now = esp_cpu_get_cycle_count();
    if (gpio_get_level(pin) == 0) {
      last_low = now;
    } else if (now - last_low > FRAME_END_IDLE_CYCLES) {
      return;  // line idle long enough: frame complete
    }
    if (now - start > DRAIN_MAX_CYCLES) return;  // safety cap against a stuck bus
  }
}

// Reads a command frame that the falling-edge ISR caught at the start of bit 0
// (the opcode MSB). Interrupt latency makes timing that first bit unreliable, but
// every command this device answers (0x00, 0x01, 0x02, 0x03) has opcode MSB = 0
// (a ~3 us low), so bit 0 is assumed 0: we wait out its low pulse to resync, then
// spin-poll every remaining bit with the cycle counter — free of latency error.
bool read_command_frame(uint8_t *bytes, size_t *byte_count) {
  if (!bytes || !byte_count || data_gpio < 0) return false;
  *byte_count = 0;

  const gpio_num_t pin = static_cast<gpio_num_t>(data_gpio);

  // Resync: wait for bit 0's low pulse (we entered partway through it) to end.
  const uint32_t bit0_start = esp_cpu_get_cycle_count();
  while (gpio_get_level(pin) == 0) {
    if (esp_cpu_get_cycle_count() - bit0_start > BIT_STUCK_LOW_CYCLES) {
      return false;  // line stuck low: not a real frame
    }
  }

  uint8_t current = 0;    // bit 0 (opcode MSB) assumed 0
  uint8_t bit_count = 1;  // bit 0 already accounted for
  size_t expected = MAX_COMMAND_BYTES;  // refined once the opcode is decoded
  uint32_t last_edge = esp_cpu_get_cycle_count();

  while (*byte_count < expected && *byte_count < MAX_COMMAND_BYTES) {
    const int bit = read_bit(pin, &last_edge);
    if (bit == -2) {  // stuck low: electrical/timing fault
      debug.timing_errors++;
      return false;
    }
    if (bit == -1) {  // line went idle before the frame completed
      debug.malformed_frames++;
      return false;
    }
    current = static_cast<uint8_t>((current << 1) | bit);
    if (++bit_count == 8) {
      bytes[(*byte_count)++] = current;
      current = 0;
      bit_count = 0;
      if (*byte_count == 1) {
        expected = command_length(bytes[0]);
        if (expected == 0) {  // unknown opcode: abort, do not respond
          debug.malformed_frames++;
          return false;
        }
      }
    }
  }

  // Frame is complete; consume the console stop bit (if present) so the line is
  // released before we drive the response. A timeout here is harmless.
  read_bit(pin, &last_edge);
  return *byte_count == expected;
}

void handle_command(const uint8_t *cmd, size_t len, uint32_t cmd_end_cycles) {
  if (!cmd || len == 0) return;

  switch (cmd[0]) {
    case 0x00:
    case 0xFF: {
      debug.status_commands++;
      // Hot path: prepare the 3 identify bytes, then transmit immediately.
      const N64ControllerStatusResponse status =
          n64_controller_status_response();
      const uint8_t reply[3] = {status.device_high, status.device_low,
                                status.status};
      note_response_latency(cmd_end_cycles);
      tx_bytes(reply, sizeof(reply));
      break;
    }
    case 0x01: {
      debug.poll_commands++;
      // Hot path: snapshot controller state and clock it straight out.
      const N64ControllerPollResponse poll = n64_controller_poll_response();
      note_response_latency(cmd_end_cycles);
      tx_bytes(poll.bytes, sizeof(poll.bytes));
      break;
    }
    case 0x02: {
      uint8_t response[TRANSFER_PAK_BLOCK_SIZE + 1] = {};
      if (len < 3) {
        debug.malformed_frames++;
        return;
      }
      debug.accessory_reads++;
      const uint16_t address = accessory_block_address(cmd[1], cmd[2]);
      debug.last_accessory_addr = address;
      debug.last_accessory_is_write = false;
      uint8_t status = 0;
      n64_accessory_read(address, response, TRANSFER_PAK_BLOCK_SIZE, &status);
      // The trailing byte is the data CRC of the block, not the pak status.
      response[TRANSFER_PAK_BLOCK_SIZE] =
          accessory_data_crc(response, TRANSFER_PAK_BLOCK_SIZE);
      tx_bytes(response, TRANSFER_PAK_BLOCK_SIZE + 1);
      break;
    }
    case 0x03: {
      if (len < 3 + TRANSFER_PAK_BLOCK_SIZE) {
        debug.malformed_frames++;
        return;
      }
      debug.accessory_writes++;
      const uint16_t address = accessory_block_address(cmd[1], cmd[2]);
      debug.last_accessory_addr = address;
      debug.last_accessory_value = cmd[3];
      debug.last_accessory_is_write = true;
      uint8_t status = 0;
      n64_accessory_write(address, cmd + 3, TRANSFER_PAK_BLOCK_SIZE, &status);
      // The response to a write is the data CRC of the written block.
      const uint8_t crc = accessory_data_crc(cmd + 3, TRANSFER_PAK_BLOCK_SIZE);
      tx_bytes(&crc, 1);
      break;
    }
    default:
      debug.malformed_frames++;
      break;
  }
}

// Falling-edge ISR: fires when the console pulls the line low to start a command.
// Captures the whole frame and drives the response inline, with interrupts off.
void IRAM_ATTR joybus_isr(void *arg) {
  (void)arg;
  if (data_gpio < 0) return;
  const gpio_num_t pin = static_cast<gpio_num_t>(data_gpio);

  // A real frame opens with a ~3 us low (opcode MSB = 0). If the line is already
  // high, either this is a spurious re-trigger from our own response edges or we
  // dispatched late and landed mid-frame; drain the rest of the frame so its
  // remaining edges don't re-trigger us, then drop it.
  if (gpio_get_level(pin) != 0) {
    debug.dropped_starts++;
    drain_to_idle(pin);
    gpio_ll_clear_intr_status(&GPIO, 1u << data_gpio);
    return;
  }

  uint8_t cmd[MAX_COMMAND_BYTES];
  size_t len = 0;
  const bool ok = read_command_frame(cmd, &len);
  const uint32_t cmd_end_cycles = esp_cpu_get_cycle_count();
  if (ok) handle_command(cmd, len, cmd_end_cycles);

  // Let the frame (and the edges from our own response) finish, then clear the
  // latched interrupt status so a single frame triggers this ISR exactly once.
  drain_to_idle(pin);
  gpio_ll_clear_intr_status(&GPIO, 1u << data_gpio);
}
}  // namespace

bool n64_joybus_init(int gpio_num) {
  data_gpio = gpio_num;
  memset(&debug, 0, sizeof(debug));

  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << gpio_num;
  cfg.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&cfg) != ESP_OK) return false;
  n64_joybus_release();

  // Capture command frames on the falling edge instead of polling the line from
  // the cooperative loop, which was too slow to catch the start of each frame.
  // Run above the WiFi interrupt (LEVEL3) so dispatch latency cannot push us past
  // bit 0, and so WiFi cannot preempt us mid-frame.
  const gpio_num_t pin = static_cast<gpio_num_t>(gpio_num);
  const esp_err_t isr_err = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3);
  if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) return false;
  if (gpio_isr_handler_add(pin, joybus_isr, nullptr) != ESP_OK) return false;
  gpio_set_intr_type(pin, GPIO_INTR_NEGEDGE);
  if (gpio_intr_enable(pin) != ESP_OK) return false;
  return true;
}

void n64_joybus_drive_low(void) {
  if (data_gpio >= 0) gpio_set_level(static_cast<gpio_num_t>(data_gpio), 0);
}

void n64_joybus_release(void) {
  if (data_gpio >= 0) gpio_set_level(static_cast<gpio_num_t>(data_gpio), 1);
}

int n64_joybus_read_level(void) {
  if (data_gpio < 0) return 1;
  return gpio_get_level(static_cast<gpio_num_t>(data_gpio));
}

bool n64_joybus_service(void) {
  // Joy-Bus frames are now captured by the falling-edge ISR (joybus_isr); the
  // cooperative loop no longer polls the line. Always report "no activity" so the
  // runtime loop yields the core to WiFi/web between interrupts.
  return false;
}

const N64JoybusDebug &n64_joybus_debug(void) { return debug; }
