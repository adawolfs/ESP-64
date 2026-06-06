#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gb_time.h"
#include "n64_runtime.h"

extern "C" void app_main(void) {
  static const char *TAG = "n64";
  delay(150);

  const bool runtime_ready = n64_runtime_init();
  if (runtime_ready) {
    ESP_LOGI(TAG, "N64 controller/Transfer Pak ready");
  }

  while (true) {
    if (!runtime_ready) {
      delay(1000);
      continue;
    }
    n64_runtime_loop();
  }
}
