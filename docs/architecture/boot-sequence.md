---
id: boot-sequence
title: Boot Sequence
sidebar_label: Boot Sequence
sidebar_position: 2
description: What happens between power-on and a ready Joy-Bus, including the save-load precedence and emergency-slot recovery.
---

# Boot Sequence

`app_main()` ([`main/main.cpp`](https://github.com/)) delays briefly, calls
`n64_runtime_init()`, then enters an endless loop calling `n64_runtime_loop()`.

## Initialization order

`n64_runtime_init()` performs, in order:

1. **`gb_cartridge_init()`** — memory-maps the `rom` partition, parses/validates the
   Game Boy header, and seeds a blank (`0xFF`) save cache.
2. **`n64_controller_init()`**, **`transfer_pak_init()`**, **`n64_accessory_init()`** —
   bring up controller state and the accessory/Transfer Pak registers; wire the
   accessory-present bit into the controller status.
3. **Self-tests** — `n64_controller_self_test()` and `gb_cartridge_header_self_test()`.
   A missing ROM is allowed (the portal can upload one); the cartridge test only
   fails if a present ROM reads back wrong.
4. **`web_portal_mount_storage()`** — mounts SPIFFS (`storage` partition) at
   `/spiffs`, formatting on mount failure so the FS self-heals after a chip erase.
5. **`save_store_load()`** — loads the persisted save with precedence over the blank
   default (see below).
6. **`save_store_recover_power_loss_slot()`** — adopts a save captured during a
   previous power-loss ride-down, if present, then re-arms the emergency slot.
7. **Joy-Bus init** — `joybus_rmt_init()` (default) or `n64_joybus_init()` (bit-bang
   fallback).
8. **`web_portal_begin()`** — starts the SoftAP + HTTP/WebSocket server.
9. **`power_monitor_init()`** — arms the power-loss monitor (no-op if the sense pin is
   disabled).

## Save-load precedence

At boot the active save is chosen in this order:

```text
1. Persisted user save   (/spiffs/save.srm, must be exactly 32 KB)
2. Emergency slot save    (valid CRC in the emgsave partition, from a power loss)
3. Bundled default save   (data/save.srm, shipped in the SPIFFS image)
4. Blank 0xFF stub        (no save available)
```

`save_store_load()` returns the load result, surfaced in logs and the status struct
as one of `persisted`, `missing`, `invalid_size`, `no_memory`, or `read_failed`. A
fresh device with the bundled default flashed reports the save as **loaded**, not
`missing`. See [Default Save](../cartridge/default-save) for how the bundled save is
delivered.

:::warning Fresh device with no SPIFFS image
If the `storage` partition was never flashed (e.g. a plain `upload` that skips the
filesystem image), `/spiffs/save.srm` is absent, the load result is `missing`, and
the console reads the blank `0xFF` save. This is the exact failure that the
[default-save delivery](../cartridge/default-save) fix addresses.
:::

## The runtime loop

`n64_runtime_loop()` (RMT path) on each iteration:

- calls `joybus_rmt_loop()` and tracks the last bus/accessory activity timestamps,
- services the web portal **only when the accessory bus is quiet**
  (`RMT_ACCESSORY_WEB_QUIET_MS`),
- calls `save_store_service()`, which only persists when the bus is **fully idle**
  (`RMT_ACCESSORY_SAVE_QUIET_MS`) — flushing mid-access would stall the response path,
- yields with `vTaskDelay(1)` so the idle task can feed the watchdog during GB Tower's
  long accessory bursts.
