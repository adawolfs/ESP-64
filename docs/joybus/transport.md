---
id: transport
title: Joy-Bus Transport
sidebar_label: Transport
sidebar_position: 1
description: The RMT receive + open-drain response transport, the bit-bang diagnostic fallback, and how commands are decoded.
---

# Joy-Bus Transport

The N64 Joy-Bus is a single-wire, open-drain, ~1 Mbit serial bus. Bits are encoded by
the **low-pulse width**: a short low is a `1`, a long low is a `0`. Devices may only
pull the line low or release it — never drive it high.

## Default: RMT RX + open-drain TX

The production transport ([`joybus/joybus_rmt.*`](https://github.com/)) uses the
ESP32-C3 **RMT** peripheral to receive command symbols with hardware timing. The RMT
receive callback decodes the opcode and **immediately** drives an open-drain GPIO
response — controller status/poll replies and Transfer Pak read/write traffic all come
out of this callback.

| Parameter | Value |
| --- | --- |
| RMT receive resolution | 10 MHz |
| Low-pulse `1` threshold | `< 2 µs` low decodes as `1`, longer as `0` |
| Frame-end idle threshold | `3.5 µs` |
| RX software buffer | 384 symbols (≥ 35-byte accessory write + stop bit) |
| Measured status/poll turnaround | ~5 µs command-end → first response bit |

The transport tracks rich stats (`JoybusRmtStats`): frame/decoded counts, per-opcode
counts, TX counts and failures, RX re-arm failures, last turnaround, and the last
accessory address/value/CRC. These surface in the periodic runtime log.

:::tip Pause around flash writes
`joybus_rmt_pause()` / `joybus_rmt_resume()` quiesce RX while a ROM or save upload
disables the flash cache, so the response ISR isn't invoked while the mapped ROM is
temporarily invalid.
:::

## Fallback: bit-bang (diagnosis only)

A pure GPIO bit-bang transport ([`joybus/n64_joybus.*`](https://github.com/)) exists
for diagnosis. Enable it with `-DN64_JOYBUS_BITBANG` or the `esp32-c3-bitbang`
PlatformIO environment. The default RMT path is the hardware-verified GB Tower path;
prefer it unless you are debugging timing.

The runtime picks the transport at init via the `USE_RMT_JOYBUS` flag and logs which
one is active.

## Command opcodes

| Opcode | Meaning | Handler |
| --- | --- | --- |
| `0x00` | Status / identify | [Controller](./controller) |
| `0xFF` | Reset + status | [Controller](./controller) |
| `0x01` | Poll controller state | [Controller](./controller) |
| `0x02` | Accessory read (32-byte block) | [Accessory / Transfer Pak](./accessory-transfer-pak) |
| `0x03` | Accessory write (32-byte block) | [Accessory / Transfer Pak](./accessory-transfer-pak) |

See [Hardware Notes](./hardware) for wiring and measured on-console behavior.
