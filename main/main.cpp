#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gb_time.h"
#include "n64_runtime.h"
#include "status_display.h"

extern "C" void app_main(void) {
  static const char *TAG = "n64";
  delay(150);
  status_display_init();
  status_display_set_message(StatusDisplayMessageLevel::Info, "BOOT");

  const bool runtime_ready = n64_runtime_init();
  if (runtime_ready) {
    ESP_LOGI(TAG, "N64 controller/Transfer Pak ready");
    status_display_set_message(StatusDisplayMessageLevel::Info, "READY");
  } else {
    status_display_set_message(StatusDisplayMessageLevel::Error, "RUNTIME FAIL");
  }

  while (true) {
    status_display_service(millis());
    if (!runtime_ready) {
      delay(1000);
      continue;
    }
    n64_runtime_loop();
  }
}
