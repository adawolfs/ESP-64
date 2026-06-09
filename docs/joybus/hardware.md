---
id: hardware
title: Hardware Notes
sidebar_label: Hardware Notes
sidebar_position: 4
description: Wiring, RMT transport parameters, and measured on-console behavior for the Joy-Bus data line.
---

# Joy-Bus Hardware Notes

## Wiring

- MCU target: ESP32-C3.
- Joy-Bus data GPIO: GPIO7 by default (`N64_JOYBUS_DATA_GPIO` can override it).
- Optional onboard OLED status display: ESP32-C3 Super Mini 0.42-inch OLED board
  uses GPIO5 as I2C SDA and GPIO6 as I2C SCL when `OLED_STATUS_ENABLED=1`.
- The Joy-Bus data line is single-wire open-drain. Firmware must only drive the
  line low or release it; it must never actively drive the line high.
- The ESP32-C3 and N64 controller port must share ground.

:::danger Open-drain only
Driving the data line high can damage the console's port. All firmware paths pull low
or release — never source current onto the line. Use a series resistor (220 Ω–1 kΩ)
and ESD protection on the data line.
:::

## Optional OLED status display

The ESP32-C3 0.42-inch OLED Super Mini variant can show firmware status on its
onboard SSD1306-compatible display. OLED support is disabled by default; enabling
it reserves GPIO5/GPIO6 for I2C and initializes the display at boot.

| Setting | Default |
| --- | --- |
| Enable flag | `OLED_STATUS_ENABLED=1` |
| SDA | GPIO5 |
| SCL | GPIO6 |
| I2C address | `0x3C` |
| I2C clock | 400 kHz |
| Controller buffer | 128x64 |
| Visible window | 72x40 |
| Draw offset | `(30,24)` |

The OLED boot page draws a border and `OLED OK` test pattern inside the logical
72x40 canvas. On hardware, confirm that the pattern is centered in the visible
panel area before relying on status readouts. If the board variant has a different
OLED mapping, override `OLED_X_OFFSET`, `OLED_Y_OFFSET`, `OLED_VISIBLE_WIDTH`, or
`OLED_VISIBLE_HEIGHT` at build time.

OLED refreshes are serviced from normal firmware loop context, not from Joy-Bus
receive callbacks or other timing-critical response paths.

## Transport

- Default transport: RMT receive with immediate open-drain GPIO responses.
- Diagnostic fallback: build with `-DN64_JOYBUS_BITBANG` or use the
  `esp32-c3-bitbang` PlatformIO environment.
- RMT receive resolution: 10 MHz.
- RMT low-pulse threshold: less than 2 µs decodes as Joy-Bus `1`; longer low
  pulses decode as `0`.
- RMT frame-end idle threshold: 3.5 µs.
- RMT software receive buffer: 384 symbols, enough for 35-byte accessory writes
  plus stop bit.

## Measured behavior

- Controller status/poll replies are accepted by the console at about 5 µs
  command-end-to-first-response turnaround.
- Transfer Pak accessory reads and writes have been observed with `txFail=0` and
  `rearmFail=0` during sustained GB Tower traffic.
- Hardware verification on the target console confirmed:
  - controller detection,
  - live web input in controller poll replies,
  - Transfer Pak detection,
  - Pokémon Yellow launching in GB Tower,
  - save loading,
  - gameplay,
  - in-game saving.

## Save persistence

- Save RAM writes are reflected in cartridge RAM during Joy-Bus transactions.
- Flash persistence is deferred to the runtime loop by `save_store_service()` and
  is debounced so flash is not written in the timing-critical Joy-Bus path. See
  [Save System](../cartridge/save-system).
