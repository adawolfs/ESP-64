#include "transfer_pak.h"

#include <string.h>

#include "gb_cartridge.h"

namespace {
TransferPakStatus state = {};

// Transfer Pak status register (0xB000) bits, matching the documented flags the
// console checks during the power-on / access-enable handshake.
constexpr uint8_t TPAK_STATUS_READY = 0x01;    // cartridge accessible
constexpr uint8_t TPAK_STATUS_REMOVED = 0x40;  // 1 = no cartridge present
constexpr uint8_t TPAK_STATUS_POWERED = 0x80;  // pak powered on

uint16_t cartridge_window_to_gb_address(uint16_t address) {
  const uint16_t offset = address - 0xC000u;
  return static_cast<uint16_t>(static_cast<uint16_t>(state.bank) * 0x4000u +
                               offset);
}

bool cartridge_enabled(void) { return state.powered && state.access_enabled; }

// Rebuilds the status byte from the current pak state. The cartridge is
// permanently fitted, so REMOVED stays clear; READY is reported only once the
// console has both powered the pak and enabled access.
void recompute_status(void) {
  uint8_t value = 0;
  if (state.powered) value |= TPAK_STATUS_POWERED;
  if (state.powered && state.access_enabled) value |= TPAK_STATUS_READY;
  state.status = value;
}

void mark_invalid(void) {
  state.invalid_accesses++;
  state.status = TPAK_STATUS_REMOVED;
}
}  // namespace

void transfer_pak_init(void) {
  memset(&state, 0, sizeof(state));
  const GbCartridgeStatus &cart = gb_cartridge_status();
  mbc1_mapper_init(&state.mapper, cart.rom_bank_count, cart.ram_bank_count);
  state.powered = false;
  state.access_enabled = false;
  state.bank = 0;
  state.status = 0;
}

const TransferPakStatus &transfer_pak_status(void) { return state; }

uint8_t transfer_pak_read_byte(uint16_t address) {
  state.reads++;
  if ((address & 0xF000u) == 0x8000u) return state.powered ? 0x84 : 0x00;
  if ((address & 0xF000u) == 0xA000u) return state.bank;
  if ((address & 0xF000u) == 0xB000u) return state.status;
  if (address >= 0xC000u) {
    if (!cartridge_enabled()) return 0xFF;
    const uint16_t gb_address = cartridge_window_to_gb_address(address);
    if (gb_address < 0x8000u) {
      return gb_cartridge_read_mapped_rom(&state.mapper, gb_address);
    }
    if (gb_address >= 0xA000u && gb_address < 0xC000u) {
      return gb_cartridge_read_mapped_ram(&state.mapper, gb_address);
    }
    return 0xFF;
  }
  mark_invalid();
  return 0xFF;
}

void transfer_pak_write_byte(uint16_t address, uint8_t value) {
  state.writes++;
  if ((address & 0xF000u) == 0x8000u) {
    state.powered = (value & 0x01) != 0 || (value & 0x84) != 0;
    recompute_status();
    return;
  }
  if ((address & 0xF000u) == 0xA000u) {
    state.bank = value;
    return;
  }
  if ((address & 0xF000u) == 0xB000u) {
    state.access_enabled = (value & 0x01) != 0 || (value & 0x84) != 0;
    recompute_status();
    return;
  }
  if (address >= 0xC000u) {
    if (!cartridge_enabled()) return;
    const uint16_t gb_address = cartridge_window_to_gb_address(address);
    if (gb_address < 0x8000u) {
      mbc1_mapper_write(&state.mapper, gb_address, value);
      return;
    }
    if (gb_address >= 0xA000u && gb_address < 0xC000u) {
      gb_cartridge_note_volatile_save_write(gb_address, value);
      return;
    }
    return;
  }
  mark_invalid();
}

void transfer_pak_read_block(uint16_t address, uint8_t *out, size_t len) {
  if (!out) return;
  for (size_t i = 0; i < len; ++i) {
    out[i] = transfer_pak_read_byte(static_cast<uint16_t>(address + i));
  }
}

void transfer_pak_write_block(uint16_t address, const uint8_t *data,
                              size_t len) {
  if (!data) return;
  for (size_t i = 0; i < len; ++i) {
    transfer_pak_write_byte(static_cast<uint16_t>(address + i), data[i]);
  }
}
