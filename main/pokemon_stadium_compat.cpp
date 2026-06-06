#include "pokemon_stadium_compat.h"

#include "gb_cartridge.h"
#include "n64_controller.h"

PokemonStadiumCompatStatus pokemon_stadium_compat_status(void) {
  const GbCartridgeStatus &cart = gb_cartridge_status();
  const N64ControllerStatusResponse controller = n64_controller_status_response();
  PokemonStadiumCompatStatus status = {};
  status.accessory_present = (controller.status & 0x01) != 0;
  status.rom_header_ok = gb_cartridge_header_self_test();
  status.save_read_only_or_stubbed = cart.save_loaded || cart.save_stubbed;
  return status;
}
