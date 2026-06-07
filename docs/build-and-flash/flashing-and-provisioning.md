---
id: flashing-and-provisioning
title: Flashing & Provisioning
sidebar_label: Flashing & Provisioning
sidebar_position: 2
description: The difference between routine firmware reflashing and full device provisioning, and how to flash each partition without losing player saves.
---

# Flashing & Provisioning

There are two distinct operations, and conflating them is the usual cause of "the
default save didn't load" or "my save got wiped".

## Routine reflash (keeps player saves)

Day-to-day firmware iteration. Flashes the app (and the ROM via the post-script) but
**not** the SPIFFS `storage` partition, so the player's persisted `save.srm` survives.

```bash
pio run --target upload          # app + ROM, leaves storage intact
# or
idf.py -p PORT app-flash         # app only
```

## Full provisioning (ships the default save)

Sets up a fresh device end to end: app + ROM + the SPIFFS image that carries the web UI
and the **bundled default save**.

```bash
cd webui && npm run upload:all   # build → pio upload → pio uploadfs
```

:::danger Provisioning overwrites the persisted save
`uploadfs` rewrites the whole `storage` partition, replacing any in-progress player save
with the bundled default. Reserve it for provisioning a fresh device or a deliberate
reset.
:::

## Flashing individual partitions

```bash
# SPIFFS image only (default save + web UI) — offset from partitions.csv
pio run --target uploadfs
# or, manually with esptool:
python -m esptool --chip esp32c3 -p PORT write-flash 0x290000 build/storage.bin

# ROM only (handled by the post-script on `upload`, or manually):
python -m esptool --chip esp32c3 -p PORT write-flash 0x190000 roms/active.gb

# Erase everything (including saves) — required after a partition-table change:
pio run --target erase            # or: idf.py -p PORT erase-flash
```

The `0x290000` (storage) and `0x190000` (ROM) offsets must match
[`partitions.csv`](../architecture/partitions-and-storage). If you change the partition
table, erase the whole flash and re-flash once.

## Why a plain `upload` doesn't ship the default save

The default save is only inside the SPIFFS image, and a plain `upload` skips the
filesystem. That is the delivery gap documented in
[Default Save](../cartridge/default-save) — provision with `upload:all` (or run
`uploadfs`) to put the bundled save on the device.

## Bypass: API without the web UI

If you don't flash SPIFFS, the SoftAP and REST/WebSocket endpoints still work; only
`GET /` returns **503**. Handy for `curl`-driven testing.
