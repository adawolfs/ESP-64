#ifndef N64_JOYBUS_H
#define N64_JOYBUS_H

#include <stddef.h>
#include <stdint.h>

struct N64JoybusDebug {
  uint32_t status_commands;
  uint32_t poll_commands;
  uint32_t accessory_reads;
  uint32_t accessory_writes;
  uint32_t malformed_frames;
  uint32_t timing_errors;
  uint32_t response_failures;
  uint32_t dropped_starts;
  uint16_t last_accessory_addr;   // masked address of the most recent 0x02/0x03
  uint8_t last_accessory_value;   // first data byte (writes) for handshake tracing
  bool last_accessory_is_write;   // true if the last access was a write (0x03)
  uint16_t last_accessory_latency_us;  // command-end to first response bit
};

bool n64_joybus_init(int gpio_num);
bool n64_joybus_service(void);
const N64JoybusDebug &n64_joybus_debug(void);
// Total accessory accesses recorded, and a formatter for the recent trace
// ("R8000=00 W8000=84 RC100=00 ..."). For Transfer Pak diagnosis.
uint32_t n64_joybus_access_count(void);
void n64_joybus_format_access_trace(char *out, size_t cap);
void n64_joybus_drive_low(void);
void n64_joybus_release(void);
int n64_joybus_read_level(void);

#endif
