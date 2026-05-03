#ifndef CST816D_H
#define CST816D_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

constexpr uint8_t I2C_ADDR_CST816D = 0x15;

enum CST816DGesture {
  CST816D_GESTURE_NONE = 0x00,
  CST816D_GESTURE_SLIDE_DOWN = 0x01,
  CST816D_GESTURE_SLIDE_UP = 0x02,
  CST816D_GESTURE_SLIDE_LEFT = 0x03,
  CST816D_GESTURE_SLIDE_RIGHT = 0x04,
  CST816D_GESTURE_SINGLE_TAP = 0x05,
  CST816D_GESTURE_DOUBLE_TAP = 0x0B,
  CST816D_GESTURE_LONG_PRESS = 0x0C,
};

bool cst816d_begin(int sda_pin, int scl_pin, int rst_pin, int int_pin);
bool cst816d_get_touch(uint16_t *x, uint16_t *y, uint8_t *gesture);
bool cst816d_ready(void);

#ifdef __cplusplus
}
#endif

#endif
