---
id: input-protocol
title: Input Protocol (WebSocket)
sidebar_label: Input Protocol
sidebar_position: 3
description: The WebSocket message contract between any client (web UI, R36S) and the ESP32 for driving the emulated N64 controller.
---

# Input Protocol (WebSocket)

Clients drive the emulated N64 controller over the same WebSocket the web UI uses:
**`ws://<device>/ws`** (port 80). Messages are JSON text frames. The ESP32 applies
them to the controller state used in Joy-Bus poll replies.

## Client → server

### Digital button

```json
{ "type": "input", "control": "A", "pressed": true }
```

- `control` (case-insensitive): `A`, `B`, `Z`, `START`, `UP`, `DOWN`, `LEFT`, `RIGHT`,
  `L`, `R`, `C_UP`, `C_DOWN`, `C_LEFT`, `C_RIGHT`.
- `pressed`: `true` (down) or `false` (up). Send the release too.

### Analog stick

```json
{ "type": "stick", "x": -40, "y": 12 }
```

- `x`, `y`: signed, clamped to the N64 int8 range **−128..127**. `0,0` = centered.

### Request a state push

```json
{ "type": "state" }
```

Asks the server to emit a `state` frame immediately.

## Server → client

- **Text** `{"type":"state", … }` — periodic status (network, controller, Transfer Pak,
  cartridge, save, debug). See [Portal & API](../web-portal/portal-and-api).
- **Binary `GBF` / `GBA`** frames — video/audio (unused by the R36S client).

## Compatibility

The `stick` message was added for analog gamepads (the R36S). The digital `input`
message is unchanged and remains the contract for the existing web UI.

## Client conventions (R36S)

The [R36S client](./r36s-client) sends a digital `input` message on each button
edge and a `stick` message whenever the scaled left-stick value changes. It maps the
right stick to the four `C_*` controls via a threshold. It sends only **changes**, not
a fixed-rate stream, to keep the link light.
