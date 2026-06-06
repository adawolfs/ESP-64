#include "n64_controller.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

namespace {
portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
N64ControllerState current_state = {};
// Reflects whether the Transfer Pak is attached; mirrored into the status byte.
bool accessory_present = true;

uint16_t control_to_button(const char *control) {
  if (!control) return 0;
  if (!strcasecmp(control, "A")) return N64_BUTTON_A;
  if (!strcasecmp(control, "B")) return N64_BUTTON_B;
  if (!strcasecmp(control, "Z")) return N64_BUTTON_Z;
  if (!strcasecmp(control, "START")) return N64_BUTTON_START;
  if (!strcasecmp(control, "UP")) return N64_BUTTON_DPAD_UP;
  if (!strcasecmp(control, "DOWN")) return N64_BUTTON_DPAD_DOWN;
  if (!strcasecmp(control, "LEFT")) return N64_BUTTON_DPAD_LEFT;
  if (!strcasecmp(control, "RIGHT")) return N64_BUTTON_DPAD_RIGHT;
  if (!strcasecmp(control, "L")) return N64_BUTTON_L;
  if (!strcasecmp(control, "R")) return N64_BUTTON_R;
  if (!strcasecmp(control, "C_UP") || !strcasecmp(control, "CUP")) {
    return N64_BUTTON_C_UP;
  }
  if (!strcasecmp(control, "C_DOWN") || !strcasecmp(control, "CDOWN")) {
    return N64_BUTTON_C_DOWN;
  }
  if (!strcasecmp(control, "C_LEFT") || !strcasecmp(control, "CLEFT")) {
    return N64_BUTTON_C_LEFT;
  }
  if (!strcasecmp(control, "C_RIGHT") || !strcasecmp(control, "CRIGHT")) {
    return N64_BUTTON_C_RIGHT;
  }
  return 0;
}
}  // namespace

void n64_controller_init(void) {
  N64ControllerState neutral = {};
  n64_controller_set_state(neutral);
}

void n64_controller_set_state(const N64ControllerState &state) {
  portENTER_CRITICAL(&state_mux);
  current_state = state;
  portEXIT_CRITICAL(&state_mux);
}

void n64_controller_set_button(uint16_t button, bool pressed) {
  portENTER_CRITICAL(&state_mux);
  if (pressed) {
    current_state.buttons |= button;
  } else {
    current_state.buttons &= ~button;
  }
  portEXIT_CRITICAL(&state_mux);
}

bool n64_controller_set_control(const char *control, bool pressed) {
  const uint16_t button = control_to_button(control);
  if (!button) return false;
  n64_controller_set_button(button, pressed);
  return true;
}

void n64_controller_set_stick(int8_t x, int8_t y) {
  portENTER_CRITICAL(&state_mux);
  current_state.stick_x = x;
  current_state.stick_y = y;
  portEXIT_CRITICAL(&state_mux);
}

void n64_controller_set_accessory_present(bool present) {
  portENTER_CRITICAL(&state_mux);
  accessory_present = present;
  portEXIT_CRITICAL(&state_mux);
}

N64ControllerState n64_controller_get_state(void) {
  portENTER_CRITICAL(&state_mux);
  const N64ControllerState snapshot = current_state;
  portEXIT_CRITICAL(&state_mux);
  return snapshot;
}

N64ControllerStatusResponse n64_controller_status_response(void) {
  // Standard N64 controller identifier (0x0500). The third byte reports the
  // accessory-present bit (0x01) based on the actual Transfer Pak state.
  portENTER_CRITICAL(&state_mux);
  const bool present = accessory_present;
  portEXIT_CRITICAL(&state_mux);
  return {0x05, 0x00, static_cast<uint8_t>(present ? 0x01 : 0x00)};
}

N64ControllerPollResponse n64_controller_poll_response(void) {
  const N64ControllerState snapshot = n64_controller_get_state();
  N64ControllerPollResponse response = {};
  response.bytes[0] = static_cast<uint8_t>(snapshot.buttons >> 8);
  response.bytes[1] = static_cast<uint8_t>(snapshot.buttons & 0xFF);
  response.bytes[2] = static_cast<uint8_t>(snapshot.stick_x);
  response.bytes[3] = static_cast<uint8_t>(snapshot.stick_y);
  return response;
}

bool n64_controller_self_test(void) {
  n64_controller_init();
  if (n64_controller_get_state().buttons != 0) return false;
  n64_controller_set_button(N64_BUTTON_A, true);
  if ((n64_controller_get_state().buttons & N64_BUTTON_A) == 0) return false;
  n64_controller_set_stick(12, -9);
  N64ControllerPollResponse poll = n64_controller_poll_response();
  if (poll.bytes[0] != 0x80 || poll.bytes[1] != 0x00) return false;
  if (static_cast<int8_t>(poll.bytes[2]) != 12) return false;
  if (static_cast<int8_t>(poll.bytes[3]) != -9) return false;
  n64_controller_init();
  return true;
}
