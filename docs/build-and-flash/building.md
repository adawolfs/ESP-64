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

## ESP32-C3 0.42 OLED status display

OLED status display support is compile-time optional and is disabled by default.
Default builds do not initialize I2C and do not reserve the OLED pins.

Enable it for the ESP32-C3 Super Mini 0.42-inch OLED board with:

```bash
PLATFORMIO_BUILD_FLAGS="-DOLED_STATUS_ENABLED=1" pio run
```

To flash the OLED-enabled firmware:

```bash
PLATFORMIO_BUILD_FLAGS="-DOLED_STATUS_ENABLED=1" pio run --target upload
```

Rollback is a normal build without the flag, or an explicit disabled build:

```bash
PLATFORMIO_BUILD_FLAGS="-DOLED_STATUS_ENABLED=0" pio run --target upload
```

When enabled, the firmware drives the onboard SSD1306-compatible display at I2C
address `0x3C`, using GPIO5 for SDA and GPIO6 for SCL. The driver renders into
the 72x40 visible window inside the 128x64 controller buffer using offset `(30,24)`.

Expected OLED status messages include:

- `BOOT`, `READY`, and `RUNTIME FAIL` during startup.
- AP/STA network mode and the reachable IP address.
- Loaded Game Boy title, ROM/header status, save load/flush state, pending save,
  and recovered-save indication.
- ROM/save upload receiving, success, rejected, and failed states.
- Compact Transfer Pak/accessory/link indicators.

Validation on the actual OLED board should confirm the boot test pattern is inside
the visible 72x40 region, the above status pages are readable, and gameplay or
Transfer Pak timing remains stable while OLED updates are enabled.

## Audio toggle

`GB_ENABLE_AUDIO` (default `1`) compiles in the APU/WebSocket audio path. Build with
`-DGB_ENABLE_AUDIO=0` to disable it.
