#ifndef N64_CONTROLLER_H
#define N64_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

enum N64Button : uint16_t {
  N64_BUTTON_A = 1u << 15,
  N64_BUTTON_B = 1u << 14,
  N64_BUTTON_Z = 1u << 13,
  N64_BUTTON_START = 1u << 12,
  N64_BUTTON_DPAD_UP = 1u << 11,
  N64_BUTTON_DPAD_DOWN = 1u << 10,
  N64_BUTTON_DPAD_LEFT = 1u << 9,
  N64_BUTTON_DPAD_RIGHT = 1u << 8,
  N64_BUTTON_L = 1u << 5,
  N64_BUTTON_R = 1u << 4,
  N64_BUTTON_C_UP = 1u << 3,
  N64_BUTTON_C_DOWN = 1u << 2,
  N64_BUTTON_C_LEFT = 1u << 1,
  N64_BUTTON_C_RIGHT = 1u << 0,
};

struct N64ControllerState {
  uint16_t buttons;
  int8_t stick_x;
  int8_t stick_y;
};

struct N64ControllerStatusResponse {
  uint8_t device_high;
  uint8_t device_low;
  uint8_t status;
};

struct N64ControllerPollResponse {
  uint8_t bytes[4];
};

void n64_controller_init(void);
void n64_controller_set_state(const N64ControllerState &state);
void n64_controller_set_button(uint16_t button, bool pressed);
bool n64_controller_set_control(const char *control, bool pressed);
void n64_controller_set_stick(int8_t x, int8_t y);
void n64_controller_set_accessory_present(bool present);
N64ControllerState n64_controller_get_state(void);
N64ControllerStatusResponse n64_controller_status_response(void);
N64ControllerPollResponse n64_controller_poll_response(void);
bool n64_controller_self_test(void);

#endif
