#include "transfer_pak.h"

#include <string.h>

#include "esp_attr.h"
#include "gb_cartridge.h"

namespace {
TransferPakStatus state = {};

// Transfer Pak status register (0xB000) bits, matching the documented flags the
// console checks during the power-on / access-enable handshake.
constexpr uint8_t TPAK_STATUS_READY = 0x01;         // cartridge accessible
constexpr uint8_t TPAK_STATUS_IS_RESETTING = 0x04;  // bit 2: currently resetting
constexpr uint8_t TPAK_STATUS_WAS_RESET = 0x08;     // bit 3: was reset recently
constexpr uint8_t TPAK_STATUS_REMOVED = 0x40;       // 1 = no cartridge present
constexpr uint8_t TPAK_STATUS_POWERED = 0x80;       // pak powered on

// The GB cartridge resets whenever the pak is powered on or access is enabled.
// is_resetting (bit 2) is read-to-clear; was_reset (bit 3) persists until the
// next power-off. The console power-cycles the pak and polls 0xB000 for these
// bits to confirm the cartridge actually reset before it loads the GB game.
bool is_resetting = false;
bool was_reset = false;

void mark_cartridge_reset(void) {
  is_resetting = true;
  was_reset = true;
}

uint16_t IRAM_ATTR cartridge_window_to_gb_address(uint16_t address) {
  const uint16_t offset = address - 0xC000u;
  return static_cast<uint16_t>(static_cast<uint16_t>(state.bank) * 0x4000u +
                               offset);
}

bool IRAM_ATTR cartridge_enabled(void) {
  return state.powered && state.access_enabled;
}

// Rebuilds the status byte from the current pak state. The cartridge is
// permanently fitted, so REMOVED stays clear; READY is reported only once the
// console has both powered the pak and enabled access.
void recompute_status(void) {
  uint8_t value = 0;
  if (state.powered) {
    value |= TPAK_STATUS_POWERED;
    if (state.access_enabled) value |= TPAK_STATUS_READY;
    // Reset bits are visible regardless of access mode, so the console sees the
    // cartridge reset after a power-cycle (access disabled) as well.
    if (is_resetting) value |= TPAK_STATUS_IS_RESETTING;
    if (was_reset) value |= TPAK_STATUS_WAS_RESET;
  }
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
  mbc1_mapper_init(&state.mapper, cart.cartridge_type, cart.rom_bank_count,
                   cart.ram_bank_count);
  state.powered = false;
  state.access_enabled = false;
  state.bank = 0;
  state.status = 0;
  is_resetting = false;
  was_reset = false;
}

const TransferPakStatus &transfer_pak_status(void) { return state; }

uint8_t IRAM_ATTR transfer_pak_read_byte(uint16_t address) {
  state.reads++;
  // While the pak is disabled, every address reads back 0x00. The console relies
  // on this to confirm the pak responds to the 0xFE disable probe.
  if (!state.powered) return 0x00;
  if ((address & 0xF000u) == 0x8000u) return 0x84;
  if ((address & 0xF000u) == 0xA000u) return state.bank;
  if ((address & 0xF000u) == 0xB000u) {
    // Reading the status clears the "is resetting" bit (read-to-clear); the
    // "was reset" bit persists until power-off. This is the version that
    // reliably loads the game / reads the save (status 0x8C -> 0x88 -> 0x89).
    const uint8_t current = state.status;
    if (is_resetting) {
      is_resetting = false;
      recompute_status();
    }
    return current;
  }
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
    // 0x84 enables (powers) the pak; anything else (e.g. 0xFE) disables it.
    // Powering on resets the GB cartridge.
    const bool was_powered = state.powered;
    state.powered = (value == 0x84);
    if (state.powered && !was_powered) {
      mark_cartridge_reset();
    } else if (!state.powered) {
      state.access_enabled = false;
      is_resetting = false;
      was_reset = false;
    }
    recompute_status();
    return;
  }
  if ((address & 0xF000u) == 0xA000u) {
    state.bank = value;
    return;
  }
  if ((address & 0xF000u) == 0xB000u) {
    // Bit 0 of the status register is the cartridge access-mode enable. Enabling
    // access boots/resets the GB cartridge, which the console detects via the
    // reset bits on the next status read.
    const bool was_enabled = state.access_enabled;
    state.access_enabled = (value & 0x01) != 0;
    if (state.access_enabled && !was_enabled) mark_cartridge_reset();
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
      gb_cartridge_write_mapped_ram(&state.mapper, gb_address, value);
      return;
    }
    return;
  }
  mark_invalid();
}

void IRAM_ATTR transfer_pak_read_block(uint16_t address, uint8_t *out,
                                      size_t len) {
  if (!out) return;
  // Fast path: a block lying entirely within one contiguous cartridge ROM region
  // (the common header/data read) is served by a single bulk copy instead of
  // `len` deep per-byte call chains, which blew the Joy-Bus response deadline.
  if (cartridge_enabled() && address >= 0xC000u) {
    const uint16_t gb_address = cartridge_window_to_gb_address(address);
    if ((gb_address < 0x4000u && gb_address + len <= 0x4000u) ||
        (gb_address >= 0x4000u && gb_address + len <= 0x8000u)) {
      state.reads += len;
      gb_cartridge_read_rom_block(
          mbc1_mapper_rom_offset(&state.mapper, gb_address), out, len);
      return;
    }
  }
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
