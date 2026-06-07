---
id: building
title: Building
sidebar_label: Building
sidebar_position: 1
description: Building the firmware with PlatformIO or ESP-IDF, building the Vite web UI, and the build environments.
---

# Building

espN64 builds with **PlatformIO** (primary) or **ESP-IDF** directly. The firmware
source is in `main/`; the web UI source is in `webui/` and builds into `data/`.

## PlatformIO environments

Defined in [`platformio.ini`](https://github.com/):

| Environment | Transport | Notes |
| --- | --- | --- |
| `esp32-c3-devkitm-1` | RMT (default) | Default build |
| `esp32-c3-spike` | RMT | Legacy alias of the default |
| `esp32-c3-bitbang` | Bit-bang | Diagnosis only (`-DN64_JOYBUS_BITBANG`) |

```bash
# Build firmware
pio run

# Build + upload firmware (and ROM via the post-script)
pio run --target upload

# Serial monitor
pio device monitor
```

### Extra scripts

Two extra scripts run during the PlatformIO build:

- **`pre:scripts/platformio_default_save.py`** — regenerates `data/save.srm` from the
  source game save before the SPIFFS image is built (requires `node` on PATH; falls
  back to validating an existing 32 KB save). See [Default Save](../cartridge/default-save).
- **`post:scripts/platformio_active_rom.py`** — flashes `roms/active.gb` to `0x190000`
  on `upload`.

## Web UI (Vite)

The UI source lives in [`webui/`](https://github.com/) and outputs to `data/`
(`emptyOutDir`), which becomes the SPIFFS image:

```bash
cd webui
npm install
npm run build        # vite build → data/, then regenerates data/save.srm
```

Useful npm scripts:

| Script | Does |
| --- | --- |
| `build` | Vite build → `data/`, then `copy_default_save.mjs` |
| `build:fs` | `build` then `pio run --target buildfs` |
| `upload:fs` | `build` then `pio run --target uploadfs` |
| `upload:all` | `build` → `pio run -t upload` → `pio run -t uploadfs` (full provisioning) |

## ESP-IDF (alternative)

```bash
. /path/to/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p PORT flash monitor
```

## Audio toggle

`GB_ENABLE_AUDIO` (default `1`) compiles in the APU/WebSocket audio path. Build with
`-DGB_ENABLE_AUDIO=0` to disable it.
