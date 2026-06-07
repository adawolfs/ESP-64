---
id: intro
title: espN64 Overview
sidebar_label: Overview
sidebar_position: 1
slug: /
description: An ESP32-C3-only emulator of an N64 controller and Transfer Pak, with a Wi-Fi web portal for input, ROM, and save management.
---

# espN64

**espN64** turns a single **ESP32-C3** into a Nintendo 64 controller *and* an
experimental **Transfer Pak**, with no external storage, no co-processor, and no
Game Boy cartridge reader. The console sees a standard controller with an accessory
in its expansion slot; that accessory exposes a Game Boy ROM and 32 KB of battery
save RAM stored entirely in the ESP32-C3's internal flash.

A built-in Wi-Fi SoftAP serves a web portal for live input, ROM replacement, and
save upload/download.

## What it does

- **Emulates an N64 controller** over the single-wire open-drain Joy-Bus, with
  hardware-timed responses driven from an RMT receive callback.
- **Emulates a Transfer Pak** so Pokémon Stadium (GB Tower) can detect the
  accessory, read the Game Boy ROM header, load the save, play, and write the save
  back.
- **Stores one active ROM + one active save** in internal flash partitions, both
  replaceable from the web portal.
- **Persists in-game saves** safely — debounced away from the timing-critical
  Joy-Bus path, with an emergency power-loss flush path.
- **Serves a web UI** for input, status, and file management over a Wi-Fi SoftAP.

## Hardware target

| Item | Value |
| --- | --- |
| MCU | ESP32-C3 (RISC-V single-core) |
| Flash | 4 MB |
| Joy-Bus data | GPIO4 (`N64_JOYBUS_DATA_GPIO` overridable) |
| Data line | Single-wire, open-drain, 3.3 V, shared ground |

:::note ESP32-C3-only constraint
The project deliberately uses **only** the ESP32-C3 — internal flash, internal
SRAM, and passive data-line protection. No microSD, external flash/PSRAM, RTC, or
second MCU. See the original [requirements](https://github.com/) captured in
`SPECS/MAIN.MD`.
:::

## How the documentation is organized

- **Architecture** — system overview, boot sequence, flash partition map.
- **Joy-Bus** — the controller/accessory transport and protocol, plus hardware notes.
- **Cartridge** — ROM mapping, MBC mappers, and the save system.
- **Web Portal** — the SoftAP, HTTP/WebSocket API, and file endpoints.
- **Build & Flash** — building the firmware and provisioning a device (including the
  bundled default save).
- **Reference** — tunables in `board_config.h`.

:::info Docusaurus-ready
These pages use Docusaurus frontmatter and `_category_.json` files. They are written
to drop into a Docusaurus `docs/` folder later without edits; no Docusaurus tooling
is bundled here.
:::
