---
id: r36s-client
title: R36S Client
sidebar_label: R36S Client
sidebar_position: 2
description: Build, deploy, and use the R36S handheld app that connects to the ESP32 over Wi-Fi to control the game and swap ROMs/saves.
---

# R36S Client

The R36S app (`r36s-input-demo/`) is a Rust TUI that connects to a provisioned ESP32
over Wi-Fi, streams controller input, and uploads ROMs/saves from the handheld's
filesystem — all with the gamepad. It reads `/dev/input/js0` and talks plain
`ws://`/`http://` (no TLS), so it builds as a single static `aarch64-unknown-linux-musl`
binary.

## Prerequisites

- The ESP32 is [provisioned](./wifi-provisioning) onto the same Wi-Fi as the R36S, and
  you know its STA IP (e.g. `192.168.5.171`).

## Build & deploy

```bash
cd r36s-input-demo
rustup target add aarch64-unknown-linux-musl   # once
cargo build --release --target aarch64-unknown-linux-musl
```

Copy the binary into a ports entry on the R36S (ArkOS), as in the original demo:

```text
/roms/ports/espn64/espn64                # the binary (rename as you like)
/roms/ports/espN64.sh                    # launcher that runs it in a terminal
```

The app saves the last-used address next to its binary (`espn64.conf`), so it
pre-fills on the next launch.

## Using the app

### 1 — Connect

The address screen shows four octets. The last-used address (default
`192.168.5.171`) is pre-filled.

| Control | Action |
| --- | --- |
| D-pad ◄ ► | Select octet |
| D-pad ▲ ▼ | Octet ±1 |
| L1 / R1 | Octet ∓10 / ±10 |
| **A** | Connect |

On success you move to the play screen; on failure the status line shows the error.

### 2 — Play

Input streams to the game while on the play screen. Mapping:

| R36S | N64 |
| --- | --- |
| A / B | A / B |
| START | START |
| D-pad | D-pad |
| L1 / R1 | L / R |
| L2 | Z |
| Left stick | Analog stick |
| Right stick | C-buttons (C-up/down/left/right) |

### 3 — Menu (swap ROM / save)

Hold **SELECT** to enter menu mode (input is paused so nothing sticks):

| Hold SELECT + | Action |
| --- | --- |
| **X** | Browse ROMs (`.gb`/`.gbc`) |
| **Y** | Browse saves (`.srm`) |
| **START** | Quit the app |

The file browser opens in the **games directory** `/roms/gb/n64` (then `/roms`, then
`/`). In it: D-pad ▲▼ to move, **A** to enter a folder or select a file, **B** to go up
a directory, **SELECT** to cancel. Selecting a file uploads it to the ESP32
(`POST /api/rom` or `/api/save`); the status line reports the result. Changing the ROM
resets the active game; an invalid save (wrong size) is rejected and surfaced.

**Quit any time:** SELECT + START.

## Save auto-sync (N64 → R36S)

While you play, the app watches the device's state stream. The moment you **save inside
the game**, the console's save-write sequence advances; once it settles (~1.5 s) the app
downloads the save (`GET /api/save`) and writes it — **you don't have to power the
console off**. (The app also periodically asks the device for fresh state, so a save is
caught even if you aren't pressing buttons.)

Two copies are written under `/roms/gb/n64`:

```text
/roms/gb/n64/<game>.srm                     # live copy, overwritten each save
/roms/gb/n64/backups/<game>-YYYYMMDD-HHMMSS.srm   # timestamped history
```

Load the live `<game>.srm` in the R36S's own Game Boy emulator to continue; keep or copy
a `backups/` file to roll back to an earlier save.

**The save is named after the actual ROM.** When syncing, the app looks in
`/roms/gb/n64` for the `.gb`/`.gbc` whose Game Boy header title matches the running game
and names the save after **that file** — so it matches your ROM regardless of how the
ROM reached the ESP32 (web portal, bundled default, or the app's uploader). Only if no
matching ROM file is found does it fall back to the uploaded name, then the sanitized
cartridge title. The status line shows each sync (`Saved → …  (+backup)`).

So: keep your Transfer-Pak Game Boy ROMs in `/roms/gb/n64`, and the synced `.srm` will
land right next to the matching ROM with the same name.

:::note Emulator save location
This writes the `.srm` next to the ROM in `/roms/gb/n64`. If your RetroArch/ArkOS reads
saves from a central *saves directory* instead, either enable "save files in content
directory" or point the save directory at `/roms/gb/n64`.
:::

## Pokédex look & cartridge image

The UI is styled like a first-gen Pokédex — a red shell with a blue lens, red/yellow/green
indicator lights, and a dark "screen" panel. On the play screen the screen panel shows
the **cartridge currently inserted** in the N64 (Red / Blue / Yellow), picked from the
running game's title.

The image is rendered with **[ratatui-image](https://github.com/ratatui/ratatui-image)**,
which auto-detects the terminal's graphics capability at startup and uses the best
available: **Sixel / Kitty / iTerm2** for real pixel graphics, falling back to
**unicode half-blocks** on plain terminals. Put the artwork next to the binary on the
device:

```text
/roms/ports/r36s_input_demo/red.png
/roms/ports/r36s_input_demo/blue.png
/roms/ports/r36s_input_demo/yellow.png
```

PNG is used because it's lossless and decodes via the pure-Rust `image` crate (png
feature only), which — together with `ratatui-image` and its pure-Rust `icy_sixel`
encoder — cross-compiles cleanly to the static musl binary (no C deps). If an image is
missing or the running game isn't recognized, a themed **placeholder** is shown and
everything else keeps working.

:::note Graphics support on the device
On a terminal with Sixel/Kitty support you get a real image; otherwise it falls back to
half-blocks automatically. The Pokédex palette uses truecolor (24-bit) — if the ArkOS
terminal is 256-color only (`echo $COLORTERM`), colors approximate but the art still
reads, and a small terminal font looks best.
:::

## How it works

- `src/net.rs` — `Connection` (WebSocket, non-blocking; parses `state` frames for the
  save sequence + title), `http_post` (upload) and `http_get` (save download).
- `src/input_map.rs` — single R36S→N64 mapping table; emits only changes.
- `src/app.rs` — screen state machine (`AddressEntry → Playing ↔ Browser`) + save
  auto-sync to `/roms/gb/n64`.
- `src/config.rs` — remembers the last address.
- `src/theme.rs` — Pokédex color palette.
- `src/art.rs` — locates + decodes the cartridge PNG to an `image::DynamicImage`, plus the
  themed placeholder.
- image rendering via `ratatui-image` (`Picker` + `StatefulImage`); per-cartridge
  protocols are cached in `App`.

See the [Input Protocol](./input-protocol) for the exact WebSocket messages.

:::note Verify button indices on your unit
The button index map comes from the original input demo. If a control feels wrong on
your specific R36S revision, confirm the indices on the device (the demo's readout
screen is handy) and adjust `BUTTON_MAP` in `src/input_map.rs`.
:::
