---
id: overview
title: System Overview
sidebar_label: System Overview
sidebar_position: 1
description: The modules that make up espN64 and how they cooperate between the timing-critical Joy-Bus path and the relaxed runtime loop.
---

# System Overview

espN64 is structured around a hard split between a **timing-critical path** (the
Joy-Bus response, driven by RMT hardware + an ISR-context callback) and a **relaxed
runtime loop** (Wi-Fi, HTTP, and flash persistence). The two never block each other:
the response path touches only RAM caches and precomputed bytes, while flash writes
and networking are deferred to the loop.

```text
[N64 Controller Port]
        │  single-wire open-drain Joy-Bus (GPIO7)
        ▼
┌──────────────────────────── ESP32-C3 ────────────────────────────┐
│                                                                   │
│  RMT RX  ──► joybus_rmt ──► n64_controller   (status / poll)      │
│   (ISR)              └────► n64_accessory ──► transfer_pak        │
│                                              │                    │
│                                     mbc1_mapper (banking)         │
│                                              │                    │
│                                     gb_cartridge (ROM mmap +      │
│                                              save RAM cache)      │
│                                              │                    │
│  ── runtime loop ─────────────────────────────────────────────── │
│   n64_runtime_loop:                                              │
│     • save_store_service  ──► SPIFFS save.srm (debounced)        │
│     • web_portal service  ──► HTTP + WebSocket (SoftAP)          │
│     • power_monitor       ──► emergency save on power loss       │
└───────────────────────────────────────────────────────────────────┘
```

## Module map

| Module | Source | Responsibility |
| --- | --- | --- |
| Joy-Bus transport | `joybus/joybus_rmt.*`, `joybus/n64_joybus.*` | Receive commands, emit hardware-timed open-drain responses |
| Controller | `joybus/n64_controller.*` | Status (`0x00`/`0xFF`) and poll (`0x01`) replies, button/stick state |
| Accessory | `joybus/n64_accessory.*` | Routes accessory read (`0x02`) / write (`0x03`) to the Transfer Pak, CRC |
| Transfer Pak | `joybus/transfer_pak.*` | Power / access / bank registers, 32-byte block reads/writes |
| MBC mapper | `cartridge/mbc1_mapper.*` | MBC1 / MBC5 ROM & RAM bank arithmetic |
| GB cartridge | `cartridge/gb_cartridge.*` | Memory-maps the ROM partition, parses the header, holds the 32 KB save cache |
| Save store | `cartridge/save_store.*` | Debounced SPIFFS persistence + emergency power-loss flash slot |
| Power monitor | `cartridge/power_monitor.*` | Detects power loss, triggers the emergency save |
| Stadium compat | `cartridge/pokemon_stadium_compat.*` | GB Tower header/handshake quirks |
| Web portal | `web/web_portal.*` | SoftAP, HTTP, WebSocket, ROM/save endpoints, SPIFFS mount |
| Runtime | `n64_runtime.*`, `main.cpp` | Init order and the cooperative service loop |

## The critical-path rule

The Joy-Bus response is generated inside an RMT receive callback. To keep it
deterministic it only ever touches:

- the controller state struct and precomputed poll/status bytes,
- the cached Transfer Pak registers and MBC banking state,
- the **memory-mapped** ROM (read as fast as a rodata array),
- the 32 KB save RAM cache in internal SRAM,
- precomputed CRC values.

It never touches SPIFFS, the HTTP server, JSON, Wi-Fi events, the heap, or blocking
mutexes. Anything that must hit flash or the network is queued for the runtime loop.

:::tip Why memory-map the ROM?
Reading the cartridge byte-by-byte through the mapper was too slow to fit a 32-byte
Transfer Pak block inside the response window (~69 µs). The ROM partition is
`esp_partition_mmap`'d so a whole contiguous block is served with a single `memcpy`.
See [GB Cartridge & ROM](../cartridge/gb-cartridge-rom).
:::

## Two transports

The default transport is **RMT receive + immediate open-drain GPIO response**. A
**bit-bang fallback** exists for diagnosis only (`-DN64_JOYBUS_BITBANG` or the
`esp32-c3-bitbang` environment). The runtime selects between them at init via
`USE_RMT_JOYBUS`. See [Joy-Bus Transport](../joybus/transport).
