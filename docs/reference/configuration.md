---
id: configuration
title: Configuration
sidebar_label: Configuration
sidebar_position: 1
description: Compile-time tunables in board_config.h and the build flags that control pins, timing, Wi-Fi, and audio.
---

# Configuration

Most behavior is set at compile time in
[`main/board_config.h`](https://github.com/) (the `board::` namespace) and a few build
flags.

## Pins

| Symbol | Default | Purpose |
| --- | --- | --- |
| `N64_JOYBUS_DATA_GPIO` | `4` | Joy-Bus data line (override with `-DN64_JOYBUS_DATA_GPIO=<n>`) |
| `POWER_LOSS_SENSE_GPIO` | `-1` | Power-loss sense input; `-1` disables the monitor |
| `POWER_LOSS_ACTIVE_LEVEL` | `0` | Logic level indicating power loss (active-low) |

:::note Power-loss monitor is opt-in
The emergency-save power monitor is disabled by default (`-1`). Enable it by defining
`POWER_LOSS_SENSE_GPIO` for a board that wires the console-side 3V3 sense comparator.
See [Save System → Power monitor](../cartridge/save-system#power-monitor).
:::

## Wi-Fi / portal

| Symbol | Default |
| --- | --- |
| `WEB_AP_SSID` | `GameBoy-Link` |
| `WEB_AP_PASSWORD` | `gameboy123` |
| `WEB_HTTP_PORT` / `WEB_SOCKET_PORT` | `80` |
| `WEB_STREAM_INTERVAL_MS` | `100` (≈ stream pacing to avoid TX saturation) |

## Input timing

| Symbol | Default | Purpose |
| --- | --- | --- |
| `WEB_MIN_PRESS_MS` | `180` | Minimum hold for a web press so the ROM registers it |
| `WEB_INPUT_TIMEOUT_MS` | `1500` | Auto-release a web press after inactivity |

## Save timing

| Symbol | Default | Purpose |
| --- | --- | --- |
| `SAVE_FLUSH_DEBOUNCE_MS` | `1000` | Quiet window before a flush is allowed |

The save store additionally uses `PERSIST_DEBOUNCE_MS` (900 ms) internally and is gated
on a fully-idle accessory bus. See [Save System](../cartridge/save-system).

## Build flags

| Flag | Default | Effect |
| --- | --- | --- |
| `GB_ENABLE_AUDIO` | `1` | Compile the APU / WebSocket audio path |
| `N64_JOYBUS_BITBANG` | unset | Use the bit-bang transport (diagnosis) |
| `N64_JOYBUS_DATA_GPIO` | `4` | Override the Joy-Bus data pin |
| `POWER_LOSS_SENSE_GPIO` | `-1` | Enable the power-loss monitor on a pin |

## Dimensions & palette

`board_config.h` also retains display/touch geometry and a DMG palette from the
project's earlier display-based lineage; these are not used by the active N64
controller/Transfer Pak runtime but remain for legacy references.
