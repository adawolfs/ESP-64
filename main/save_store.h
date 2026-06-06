#ifndef SAVE_STORE_H
#define SAVE_STORE_H

#include <stdbool.h>
#include <stdint.h>

// Loads a persisted save from SPIFFS into the cartridge, taking precedence over
// the embedded default. Returns true if a valid persisted save was applied.
// Requires SPIFFS to be mounted first.
bool save_store_load(void);

// Persists the cartridge save to SPIFFS once writes have gone quiet. Call from
// the runtime loop with a millisecond timestamp; never from an ISR.
void save_store_service(uint32_t now_ms);

// Deletes the persisted save so the bundled default is restored on next boot.
bool save_store_reset(void);

// True if a persisted save currently exists on storage.
bool save_store_persisted(void);

#endif  // SAVE_STORE_H
