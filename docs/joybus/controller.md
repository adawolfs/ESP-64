---
id: controller
title: Controller Emulation
sidebar_label: Controller
sidebar_position: 2
description: How espN64 answers status and poll commands, the button/stick state model, and how web input feeds the poll reply.
---

# Controller Emulation

[`joybus/n64_controller.*`](https://github.com/) models a standard N64 controller and
produces the bytes returned for status (`0x00`/`0xFF`) and poll (`0x01`) commands.

## State model

```c
struct N64ControllerState {
  uint16_t buttons;   // bitmask, see N64Button
  int8_t   stick_x;
  int8_t   stick_y;
};
```

Button bits (`N64Button`) follow the standard Joy-Bus layout: A, B, Z, Start, D-pad
(up/down/left/right), L, R, and the four C buttons.

## Status response

The status response identifies the device and reports whether an accessory is present
in the expansion slot:

```c
struct N64ControllerStatusResponse {
  uint8_t device_high;
  uint8_t device_low;
  uint8_t status;     // includes the accessory-present bit
};
```

`n64_controller_set_accessory_present(bool)` wires the Transfer Pak's presence (from
`n64_accessory_present()`) into the status byte so the console knows to issue
accessory reads/writes.

## Poll response

The poll response is the 4-byte controller report (buttons + stick) the console reads
~60 times a second:

```c
struct N64ControllerPollResponse { uint8_t bytes[4]; };
```

## Setting input

Input can come from several sources, all funneling into the same state:

| Function | Use |
| --- | --- |
| `n64_controller_set_state(state)` | Replace the whole state |
| `n64_controller_set_button(button, pressed)` | Toggle one button |
| `n64_controller_set_control("a", pressed)` | Named control (used by the web API) |
| `n64_controller_set_stick(x, y)` | Analog stick |

The web portal's `POST /api/input` and the WebSocket `input` message call
`n64_controller_set_control()`. A web press has a configurable minimum hold
(`WEB_MIN_PRESS_MS`, 180 ms) so the ROM registers it, and auto-releases after
`WEB_INPUT_TIMEOUT_MS` (1.5 s) of inactivity. See [Configuration](../reference/configuration).

## Self-test

`n64_controller_self_test()` runs at boot and must pass for the runtime to come up; it
validates that the controller produces well-formed status/poll bytes.
