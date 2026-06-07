#ifndef POKEMON_STADIUM_COMPAT_H
#define POKEMON_STADIUM_COMPAT_H

#include <stdint.h>

struct PokemonStadiumCompatStatus {
  bool accessory_present;
  bool rom_header_ok;
  bool save_read_only_or_stubbed;
};

PokemonStadiumCompatStatus pokemon_stadium_compat_status(void);

#endif
