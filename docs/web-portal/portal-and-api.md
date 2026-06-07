---
id: portal-and-api
title: Portal & API
sidebar_label: Portal & API
sidebar_position: 1
description: The SoftAP, the unified HTTP/WebSocket server on port 80, and the REST endpoints for input, ROM, and save management.
---

# Web Portal & API

[`web/web_portal.*`](https://github.com/) runs a Wi-Fi **SoftAP**, mounts the SPIFFS
`storage` partition, and serves a unified HTTP + WebSocket server on **port 80**
(`esp_http_server`). The UI is served from SPIFFS, with an embedded fallback so the
portal still works on an empty filesystem.

## Connecting

| Setting | Default |
| --- | --- |
| SSID | `GameBoy-Link` |
| Password | `gameboy123` |
| IP | `192.168.4.1` |
| HTTP / WebSocket port | 80 |

After boot you should see `web_portal: AP up: SSID=GameBoy-Link ip=192.168.4.1`. Connect
a Wi-Fi client and open `http://192.168.4.1`.

## REST endpoints

| Method | Path | Function |
| --- | --- | --- |
| GET | `/` | Vite UI from SPIFFS (503 if SPIFFS is empty and no fallback) |
| GET | `/api/state` | JSON: network, heap, stream, input |
| GET | `/api/input_state` | Alias of `/api/state` |
| POST | `/api/input` | Form-encoded `control=a\|b\|select\|start\|up\|down\|left\|right`, `pressed=1\|0` |
| GET | `/api/save` | Download the active save (`save.srm`) |
| POST | `/api/save` | Upload a 32 KB save and persist it |
| POST | `/api/rom` | Replace the active ROM in the `rom` partition |
| GET | `/ws` | Upgrade to WebSocket |

### Save and ROM management

```bash
# Download / upload the active save
curl -o save.srm http://192.168.4.1/api/save
curl --data-binary @save.srm http://192.168.4.1/api/save

# Replace the active ROM
curl --data-binary @your_game.gb http://192.168.4.1/api/rom
```

Uploads pause the Joy-Bus transport and set the save store **busy** so the runtime
persistence service doesn't fight the flash write. Replacing the ROM clears the save RAM
so a different game's SRAM isn't presented. See
[GB Cartridge & ROM](../cartridge/gb-cartridge-rom#replacing-the-rom-at-runtime).

## WebSocket messages

- **Client → server**
  - `{"type":"input","control":"a","pressed":true}`
  - `{"type":"audio","enabled":true}` (no-op if `GB_ENABLE_AUDIO=0`)
- **Server → client**
  - Text `{"type":"state", … }` — same schema as `/api/state`.
  - Binary `GBF` frame: `[ 'G' 'B' 'F' 0x01 W H | 2bpp packed bytes ]`.
  - Binary `GBA` audio (only if the APU is active): `[ 'G' 'B' 'A' 0x01 SR_lo SR_hi N_lo N_hi | PCM ]`.

:::note WebSocket port
The WebSocket shares port **80** with HTTP via upgrade (`ws://<ip>/ws`). Clients that
default to `:81` must be pointed at `/ws` on port 80.
:::

## Idle-gated servicing

The runtime only services the portal when the accessory bus is quiet, so streaming and
HTTP work never intrudes on a Transfer Pak access. Streaming runs at a conservative
interval (`WEB_STREAM_INTERVAL_MS`) to avoid saturating the SoftAP's TX buffers. See
[Configuration](../reference/configuration).
