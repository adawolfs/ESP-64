---
id: default-save
title: Default Save Delivery
sidebar_label: Default Save
sidebar_position: 4
description: How the bundled default savegame is generated, shipped inside the SPIFFS image, and loaded on a fresh device — and why a plain upload skips it.
---

# Default Save Delivery

The **bundled default save** is the savegame a fresh device shows before the player has
saved anything of their own. It lives in `data/save.srm`, is embedded into the firmware
as a fallback, and is also shipped inside the SPIFFS `storage` image. At boot,
[`save_store_load()`](./save-system#loading-and-precedence) loads it into cartridge RAM
**only when no valid persisted user save exists** — a persisted save always wins.

## The delivery gap (why the default save can go missing)

The active ROM and the default save reach the device by **different** mechanisms:

| Artifact | Carried by | Flashed on a plain `pio run -t upload`? |
| --- | --- | --- |
| ROM (`roms/active.gb`) | PlatformIO post-script → `0x190000` | ✅ Yes |
| Default save (`data/save.srm`) | Firmware fallback + SPIFFS `storage` image | ✅ Yes, firmware fallback; SPIFFS copy only on `uploadfs` |

So a normal `upload` flashes the app **and the ROM** (the game appears) but leaves the
`storage` partition untouched. On a fresh device `/spiffs/save.srm` can be absent, but
`save_store_load()` now falls back to the firmware-embedded default save instead of
the blank `0xFF` save.

:::info Symptom
"The game loads but the default savegame doesn't." The firmware now closes this gap by
embedding `data/save.srm`; the SPIFFS copy is still useful for web access and as the
mutable persisted save file.
:::

## How the bundled save is generated

The default save is regenerated from the source `game/*.srm` so it can never drift from
— or be missing relative to — the source:

- [`scripts/copy_default_save.mjs`](https://github.com/) copies the source `.srm` to
  `data/save.srm` and asserts it is exactly **32768 bytes**. This is the single source
  of truth, also run by the webui `npm run build`.
- [`scripts/platformio_default_save.py`](https://github.com/) is a PlatformIO **`pre:`**
  extra script that runs the Node copy **before** the SPIFFS image is built. If Node is
  unavailable or the copy fails, it falls back to validating an already-present 32 KB
  `data/save.srm`, and aborts the build only when there is no valid default save at all.

```ini
# platformio.ini
extra_scripts =
	pre:scripts/platformio_default_save.py
	post:scripts/platformio_active_rom.py
```

`main/web_assets.S` embeds the refreshed `save.srm` into the firmware as a fallback.
`main/CMakeLists.txt` also builds the SPIFFS image from `data/` via
`spiffs_create_partition_image(storage "${WEB_UI_DIST_DIR}" FLASH_IN_PROJECT)`, so the
refreshed `save.srm` is included when `uploadfs` is used.

## Delivering it to a device

Because flashing the filesystem **overwrites the player's persisted save**, espN64
treats full-FS flashing as **provisioning**, not a routine step:

```bash
# Provision a fresh device (app + ROM + SPIFFS with the default save):
cd webui && npm run upload:all      # build → pio upload → pio uploadfs

# Flash only the SPIFFS image (default save + web UI):
pio run --target uploadfs
```

For day-to-day firmware iteration use `upload` / `app-flash`, which leave the player's
save in `storage` intact. See [Flashing & Provisioning](../build-and-flash/flashing-and-provisioning).

## Verifying on hardware

1. Erase the device (`pio run -t erase` / `idf.py erase-flash`).
2. `cd webui && npm run upload:all`, then capture the boot log.
3. Confirm `save_store_load()` reports `default` or `persisted` (not `missing`).
4. Confirm the console — or `GET /api/save` — reads the bundled bytes, not `0xFF`.
5. Confirm an in-game save then persists and takes precedence on the next boot.

:::warning Known follow-up: source coupling
`copy_default_save.mjs` hard-codes a specific source `.srm` (Pokémon Yellow). If the
active ROM changes, the default save may not match the game. A future improvement is to
pair the default save with `roms/active.gb`.
:::
