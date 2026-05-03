#include "cst816d.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char *TAG = "cst816d";
// Match Arduino Wire.begin() default timing more closely. The previous 400kHz
// setting was materially more aggressive than the original implementation and
// makes CST816D reads much less tolerant of marginal pull-ups/wiring.
constexpr uint32_t I2C_FREQ_HZ = 100000;
constexpr int I2C_READ_RETRIES = 3;
constexpr i2c_port_num_t I2C_PORT = I2C_NUM_0;

bool driver_ready = false;
bool i2c_installed = false;
i2c_master_bus_handle_t i2c_bus = nullptr;
i2c_master_dev_handle_t touch_dev = nullptr;

esp_err_t reg_read(uint8_t reg, uint8_t *out, size_t len) {
  if (!touch_dev) return ESP_ERR_INVALID_STATE;
  return i2c_master_transmit_receive(touch_dev, &reg, 1, out, len, 50);
}

esp_err_t reg_write(uint8_t reg, uint8_t value) {
  if (!touch_dev) return ESP_ERR_INVALID_STATE;
  uint8_t payload[2] = {reg, value};
  return i2c_master_transmit(touch_dev, payload, sizeof(payload), 50);
}

esp_err_t reg_read_retry(uint8_t reg, uint8_t *out, size_t len) {
  for (int attempt = 0; attempt < I2C_READ_RETRIES; ++attempt) {
    const esp_err_t err = reg_read(reg, out, len);
    if (err == ESP_OK) return err;
    if (attempt + 1 == I2C_READ_RETRIES) return err;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return ESP_FAIL;
}
}  // namespace

bool cst816d_begin(int sda_pin, int scl_pin, int rst_pin, int int_pin) {
  driver_ready = false;

  if (int_pin >= 0) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << int_pin;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    gpio_set_level(static_cast<gpio_num_t>(int_pin), 1);
    esp_rom_delay_us(1000);
    gpio_set_level(static_cast<gpio_num_t>(int_pin), 0);
    esp_rom_delay_us(1000);

    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&cfg);
  }

  if (rst_pin >= 0) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << rst_pin;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    gpio_set_level(static_cast<gpio_num_t>(rst_pin), 0);
    esp_rom_delay_us(10000);
    gpio_set_level(static_cast<gpio_num_t>(rst_pin), 1);
    vTaskDelay(pdMS_TO_TICKS(300));
  }

  if (i2c_installed) {
    if (touch_dev) {
      i2c_master_bus_rm_device(touch_dev);
      touch_dev = nullptr;
    }
    if (i2c_bus) {
      i2c_del_master_bus(i2c_bus);
      i2c_bus = nullptr;
    }
    i2c_installed = false;
  }

  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port = I2C_PORT;
  bus_cfg.sda_io_num = static_cast<gpio_num_t>(sda_pin);
  bus_cfg.scl_io_num = static_cast<gpio_num_t>(scl_pin);
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = true;

  esp_err_t err = i2c_new_master_bus(&bus_cfg, &i2c_bus);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
    return false;
  }

  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address = I2C_ADDR_CST816D;
  dev_cfg.scl_speed_hz = I2C_FREQ_HZ;
  dev_cfg.scl_wait_us = 10000;

  err = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &touch_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s",
             esp_err_to_name(err));
    i2c_del_master_bus(i2c_bus);
    i2c_bus = nullptr;
    return false;
  }
  i2c_installed = true;

  // Disable automatic low-power mode to keep touch reads predictable.
  err = reg_write(0xFE, 0xFF);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "failed to configure CST816D low-power mode: %s",
             esp_err_to_name(err));
    return false;
  }

  uint8_t finger = 0;
  err = reg_read_retry(0x02, &finger, 1);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "touch probe failed: %s", esp_err_to_name(err));
    return false;
  }

  driver_ready = true;
  ESP_LOGI(TAG, "CST816D ready at %lu Hz", static_cast<unsigned long>(I2C_FREQ_HZ));
  return true;
}

bool cst816d_get_touch(uint16_t *x, uint16_t *y, uint8_t *gesture) {
  if (!driver_ready || !i2c_installed || !touch_dev) return false;

  uint8_t finger = 0;
  esp_err_t err = reg_read_retry(0x02, &finger, 1);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "finger read failed: %s", esp_err_to_name(err));
    return false;
  }
  const bool finger_detected = finger != 0;

  uint8_t gesture_raw = 0;
  err = reg_read_retry(0x01, &gesture_raw, 1);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "gesture read failed: %s", esp_err_to_name(err));
    gesture_raw = CST816D_GESTURE_NONE;
  }
  if (gesture_raw != CST816D_GESTURE_SLIDE_UP &&
      gesture_raw != CST816D_GESTURE_SLIDE_DOWN) {
    gesture_raw = CST816D_GESTURE_NONE;
  }
  if (gesture) *gesture = gesture_raw;

  uint8_t data[4] = {};
  err = reg_read_retry(0x03, data, sizeof(data));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "coordinate read failed: %s", esp_err_to_name(err));
    return false;
  }
  if (x) *x = ((data[0] & 0x0F) << 8) | data[1];
  if (y) *y = ((data[2] & 0x0F) << 8) | data[3];

  return finger_detected;
}

bool cst816d_ready(void) { return driver_ready; }
