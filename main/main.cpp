#include "emulator_runtime.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gb_time.h"

extern "C" void app_main(void) {
  static const char *TAG = "gb";
  delay(150);

  const bool emulator_ready = emulator_init();
  if (emulator_ready) {
    ESP_LOGI(TAG, "GB ready");
  }

  while (true) {
    if (!emulator_ready) {
      delay(1000);
      continue;
    }
    emulator_run_frame();
  }
}
