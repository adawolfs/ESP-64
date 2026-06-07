#ifndef JOYBUS_RMT_H
#define JOYBUS_RMT_H

#include <stdbool.h>
#include <stdint.h>

// RMT-based Joy-Bus transport (change: gb-tower-live-play). RMT receives
// commands with hardware timing; the receive callback immediately drives
// open-drain GPIO responses for controller and Transfer Pak traffic.

struct JoybusRmtStats {
  uint32_t frames;        // RMT receptions completed
  uint32_t decoded;       // frames that decoded to >= 1 byte
  uint32_t status_cmds;   // opcode 0x00/0xFF
  uint32_t poll_cmds;     // opcode 0x01
  uint32_t read_cmds;     // opcode 0x02
  uint32_t write_cmds;    // opcode 0x03
  uint32_t other_cmds;    // unrecognized opcode
  uint32_t tx_status;     // status responses emitted
  uint32_t tx_poll;       // poll responses emitted
  uint32_t tx_read;       // accessory read responses emitted
  uint32_t tx_write;      // accessory write ACKs emitted
  uint32_t tx_failures;   // responses that could not be emitted
  uint32_t rx_rearm_failures;  // failed receive re-arms after TX
  uint32_t last_turnaround_us; // approximate cmd-end to first response bit
  uint16_t last_accessory_addr; // last Transfer Pak block address
  uint8_t last_accessory_value; // first data/response byte for that access
  uint8_t last_accessory_crc;   // CRC byte returned for that access
  bool last_accessory_is_write;
  uint8_t current_poll_reply[4];  // bytes currently returned to poll commands
  uint8_t last_len;       // bytes in the last decoded frame
  uint8_t last_bytes[4];  // first bytes of the last decoded frame
  uint16_t last_symbols;  // RMT symbols in the last frame
};

bool joybus_rmt_init(int gpio_num);
bool joybus_rmt_loop(uint32_t now_ms);
const JoybusRmtStats &joybus_rmt_stats(void);

// Quiesce/restart RX around flash writes (ROM/save upload), so the response ISR
// is not needed while the cache is disabled. Returns false on the bitbang
// transport (no RMT channel).
bool joybus_rmt_pause(void);
bool joybus_rmt_resume(void);

#endif  // JOYBUS_RMT_H
