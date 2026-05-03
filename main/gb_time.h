#ifndef GB_TIME_H
#define GB_TIME_H

#include <stdint.h>

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Arduino-flavoured timing helpers used by the ported emulator code.
// Implemented on top of esp_timer / FreeRTOS so the rest of the source
// tree can stay unchanged.

static inline uint32_t millis(void) {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000LL);
}

static inline uint32_t micros(void) {
  return static_cast<uint32_t>(esp_timer_get_time());
}

static inline void delay(uint32_t ms) {
  if (ms == 0) {
    taskYIELD();
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(ms));
}

static inline void delayMicroseconds(uint32_t us) {
  if (us == 0) return;
  esp_rom_delay_us(us);
}

#endif  // GB_TIME_H
