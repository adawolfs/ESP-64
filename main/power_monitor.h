#ifndef POWER_MONITOR_H
#define POWER_MONITOR_H

#include <stdbool.h>

// Watches the power-loss sense input (board::PIN_POWER_LOSS_SENSE). On a power-loss
// edge a high-priority task stops WiFi (to extend hold-up time) and writes the
// dirty cartridge save to the emergency flash slot during the supply ride-down.
// Safe because, at console power-off, the Joy-Bus is already idle.
//
// Returns false if the sense pin is disabled (-1) or setup failed. Call once at
// startup, after save_store has been initialised.
bool power_monitor_init(void);

// True once a power-loss event has been handled (latched until reboot).
bool power_monitor_triggered(void);

#endif  // POWER_MONITOR_H
