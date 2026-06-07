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
- Joy-Bus data GPIO: GPIO4 by default (`N64_JOYBUS_DATA_GPIO` can override it).
- The Joy-Bus data line is single-wire open-drain. Firmware must only drive the
  line low or release it; it must never actively drive the line high.
- The ESP32-C3 and N64 controller port must share ground.

:::danger Open-drain only
Driving the data line high can damage the console's port. All firmware paths pull low
or release — never source current onto the line. Use a series resistor (220 Ω–1 kΩ)
and ESD protection on the data line.
:::

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
