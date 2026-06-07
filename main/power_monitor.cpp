#include "power_monitor.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "save_store.h"

namespace {
constexpr const char *TAG = "power_monitor";
// Confirm the sense line really stayed asserted before acting, to reject brief
// dips/glitches. Kept tiny so it does not eat into the hold-up budget.
constexpr int CONFIRM_SAMPLES = 8;
constexpr uint32_t CONFIRM_SAMPLE_US = 100;

SemaphoreHandle_t trigger_sem = nullptr;
volatile bool handled = false;

void IRAM_ATTR power_loss_isr(void *) {
  BaseType_t hp_task_woken = pdFALSE;
  xSemaphoreGiveFromISR(trigger_sem, &hp_task_woken);
  if (hp_task_woken) portYIELD_FROM_ISR();
}

bool sense_asserted(void) {
  return gpio_get_level(static_cast<gpio_num_t>(board::PIN_POWER_LOSS_SENSE)) ==
         board::POWER_LOSS_ACTIVE_LEVEL;
}

void shutdown_task(void *) {
  for (;;) {
    if (xSemaphoreTake(trigger_sem, portMAX_DELAY) != pdTRUE) continue;
    if (handled) continue;

    // Debounce: require the line to stay asserted across the confirm window.
    bool real = true;
    for (int i = 0; i < CONFIRM_SAMPLES; ++i) {
      if (!sense_asserted()) {
        real = false;
        break;
      }
      esp_rom_delay_us(CONFIRM_SAMPLE_US);
    }
    if (!real) continue;  // glitch / brief dip — ignore

    handled = true;
    ESP_LOGW(TAG, "power loss detected; committing save");
    // Drop the radio first: the SoftAP dominates current draw, so stopping it
    // stretches the hold-up time available for the flash write.
    esp_wifi_stop();
    save_store_flush_on_power_loss();
    // Nothing more to do; park until power is actually gone.
  }
}
}  // namespace

bool power_monitor_init(void) {
  if (board::PIN_POWER_LOSS_SENSE < 0) {
    ESP_LOGI(TAG, "power-loss monitor disabled (no sense pin)");
    return false;
  }

  trigger_sem = xSemaphoreCreateBinary();
  if (!trigger_sem) return false;

  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << board::PIN_POWER_LOSS_SENSE;
  io.mode = GPIO_MODE_INPUT;
  // Idle-high pull so an unconnected/healthy line never false-triggers; the
  // comparator pulls it low on power loss (active-low -> falling edge).
  io.pull_up_en = board::POWER_LOSS_ACTIVE_LEVEL == 0 ? GPIO_PULLUP_ENABLE
                                                      : GPIO_PULLUP_DISABLE;
  io.pull_down_en = board::POWER_LOSS_ACTIVE_LEVEL == 0 ? GPIO_PULLDOWN_DISABLE
                                                        : GPIO_PULLDOWN_ENABLE;
  io.intr_type = board::POWER_LOSS_ACTIVE_LEVEL == 0 ? GPIO_INTR_NEGEDGE
                                                     : GPIO_INTR_POSEDGE;
  if (gpio_config(&io) != ESP_OK) return false;

  // High priority so the flush pre-empts everything once power is dropping.
  if (xTaskCreate(shutdown_task, "pwrloss", 4096, nullptr,
                  configMAX_PRIORITIES - 1, nullptr) != pdPASS) {
    return false;
  }

  // ISR service may already be installed elsewhere; ignore "already installed".
  esp_err_t isr_err = gpio_install_isr_service(0);
  if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "gpio_install_isr_service failed: %d", isr_err);
    return false;
  }
  if (gpio_isr_handler_add(static_cast<gpio_num_t>(board::PIN_POWER_LOSS_SENSE),
                           power_loss_isr, nullptr) != ESP_OK) {
    return false;
  }

  ESP_LOGI(TAG, "power-loss monitor armed on GPIO%d (active-%s)",
           board::PIN_POWER_LOSS_SENSE,
           board::POWER_LOSS_ACTIVE_LEVEL == 0 ? "low" : "high");
  return true;
}

bool power_monitor_triggered(void) { return handled; }
