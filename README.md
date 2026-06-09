# espN64 — Controlador N64 + Transfer Pak sobre ESP32-C3

Convierte un único **ESP32-C3** en un **controlador de Nintendo 64** y un **Transfer
Pak** experimental, sin almacenamiento externo, sin co-procesador y sin lector de
cartuchos Game Boy. La consola ve un controlador estándar con un accesorio en su
slot de expansión; ese accesorio expone una ROM de Game Boy y 32 KB de save RAM con
batería, todo guardado en la flash interna del ESP32-C3.

Un SoftAP Wi-Fi embebido sirve un portal web para input en vivo, reemplazo de ROM y
subida/descarga de saves.

> **Verificado en hardware real:** detección de controlador, input web en las
> respuestas de poll, detección de Transfer Pak, Pokémon Yellow arrancando en GB
> Tower, carga de save, gameplay y guardado en juego.

---

## Documentación

La documentación completa de la implementación está en [`docs/`](docs/), formateada
para integrarse luego en un sitio **Docusaurus** (frontmatter + `_category_.json`).

| Sección | Contenido |
|---|---|
| [Overview](docs/intro.md) | Qué hace el proyecto y cómo se organiza la doc |
| [Architecture](docs/architecture/) | Visión del sistema, secuencia de arranque, particiones y storage |
| [Joy-Bus](docs/joybus/) | Transporte RMT, controlador, Transfer Pak y notas de hardware |
| [Cartridge](docs/cartridge/) | ROM mmap, mappers MBC, sistema de save y **save por defecto** |
| [Web Portal](docs/web-portal/) | SoftAP, API HTTP/WebSocket y endpoints de ROM/save |
| [Build & Flash](docs/build-and-flash/) | Compilar y **aprovisionar** un equipo (incluye el save por defecto) |
| [Reference](docs/reference/) | Tunables de `board_config.h` y build flags |

---

## Características

- **Emula un controlador N64** sobre el Joy-Bus de un solo hilo (open-drain), con
  respuestas de timing por hardware generadas desde un callback de recepción RMT.
- **Emula un Transfer Pak** para que Pokémon Stadium (GB Tower) detecte el accesorio,
  lea el header de la ROM, cargue el save, juegue y guarde de vuelta.
- **Guarda una ROM activa + un save activo** en particiones de flash interna, ambos
  reemplazables desde el portal web.
- **Persiste los saves en juego** de forma segura — con debounce fuera del camino
  crítico del Joy-Bus, y una vía de flush de emergencia ante pérdida de energía.
- **Sirve un portal web** para input, estado y manejo de archivos sobre un SoftAP.

---

## Hardware soportado

| Item | Valor |
|---|---|
| MCU | ESP32-C3 (RISC-V single-core) |
| Flash | 4 MB internos (sin SD, sin flash/PSRAM externos) |
| Línea de datos Joy-Bus | GPIO7 (`N64_JOYBUS_DATA_GPIO` lo sobreescribe) |
| Sensado de power-loss (opcional) | `POWER_LOSS_SENSE_GPIO` (deshabilitado por defecto: `-1`) |

La línea de datos del Joy-Bus es **single-wire open-drain**: el firmware solo tira a
LOW o libera la línea, **nunca** la maneja a HIGH. El ESP32-C3 y el puerto del
controlador deben compartir GND. Protección recomendada: resistencia serie 220 Ω–1 kΩ
y diodo ESD en la línea de datos. Ver [Joy-Bus → Hardware](docs/joybus/hardware.md).

> **Restricción ESP32-C3-only:** el proyecto usa exclusivamente el ESP32-C3 — flash
> interna, SRAM interna y protección pasiva. Sin microSD, flash/PSRAM externos, RTC
> ni segundo MCU. Requisitos originales en [`SPECS/MAIN.MD`](SPECS/MAIN.MD).

---

## Requisitos

| Herramienta | Versión | Para qué |
|---|---|---|
| PlatformIO | reciente (`platform = espressif32`) | Toolchain primario (build + flash + ROM) |
| ESP-IDF | v6.0+ (alternativa) | El portal usa WebSocket async de `esp_http_server` v6 |
| Node.js | 18+ | Recompilar la web UI (`webui/`) y generar el save por defecto |
| Python | 3.9+ | Provisto por el virtualenv de IDF/PlatformIO |

---

## Estructura del proyecto

```
espN64/
├── platformio.ini             # Envs PlatformIO + extra scripts (pre/post)
├── CMakeLists.txt             # Proyecto ESP-IDF; flashea roms/active.gb a la part. rom
├── partitions.csv             # factory + rom + storage (SPIFFS) + emgsave
├── partitions / sdkconfig.*   # Tabla de particiones y configs por env
├── main/
│   ├── main.cpp               # app_main → n64_runtime_init + loop
│   ├── n64_runtime.*          # Orden de init y loop cooperativo
│   ├── board_config.h         # Pines, timing, SSID, tunables
│   ├── joybus/                # Transporte Joy-Bus + controlador + accesorio + Transfer Pak
│   │   ├── joybus_rmt.*       #   RMT RX + respuesta open-drain (transporte default)
│   │   ├── n64_joybus.*       #   Bit-bang fallback (diagnóstico)
│   │   ├── n64_controller.*   #   Respuestas status/poll, estado de botones/stick
│   │   ├── n64_accessory.*    #   Ruteo de accessory read/write + CRC
│   │   └── transfer_pak.*     #   Registros power/access/bank, bloques de 32 bytes
│   ├── cartridge/             # Cartucho Game Boy emulado
│   │   ├── gb_cartridge.*     #   ROM mmap + parseo de header + cache de save RAM
│   │   ├── mbc1_mapper.*      #   Banking MBC1 / MBC5
│   │   ├── save_store.*       #   Persistencia SPIFFS con debounce + slot de emergencia
│   │   ├── power_monitor.*    #   Flush de save ante pérdida de energía
│   │   └── pokemon_stadium_compat.*
│   └── web/web_portal.*       # SoftAP + HTTP + WebSocket + endpoints ROM/save
├── webui/                     # UI Vite (build → data/)
├── data/                      # Imagen SPIFFS: web UI + save.srm por defecto
├── roms/active.gb             # ROM activa, flasheada a la partición rom
├── game/*.srm                 # Save fuente del que sale data/save.srm
└── scripts/                   # copy_default_save.mjs, platformio_active_rom.py,
                               # platformio_default_save.py
```

---

## Build y flash

El build primario es **PlatformIO**; ESP-IDF funciona como alternativa.

### Environments PlatformIO

| Environment | Transporte | Nota |
|---|---|---|
| `esp32-c3-devkitm-1` | RMT (default) | Build por defecto |
| `esp32-c3-spike` | RMT | Alias legacy del default |
| `esp32-c3-bitbang` | Bit-bang | Sólo diagnóstico (`-DN64_JOYBUS_BITBANG`) |

```bash
# Firmware (+ ROM via post-script) y monitor
pio run --target upload
pio device monitor

# Alternativa ESP-IDF
. /ruta/a/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p PORT flash monitor
```

### Reflash de día a día vs. aprovisionamiento

Son dos operaciones distintas — confundirlas es la causa típica de "no cargó el save
por defecto" o "se borró mi save":

```bash
# Reflash de día a día: app (+ ROM). NO toca la partición storage → conserva el save del jugador.
pio run --target upload          # o: idf.py -p PORT app-flash

# Aprovisionamiento completo de un equipo nuevo: app + ROM + imagen SPIFFS (web UI + save default).
cd webui && npm run upload:all   # build → pio upload → pio uploadfs
```

> **Aprovisionar sobrescribe el save persistido.** `uploadfs` reescribe toda la
> partición `storage`, reemplazando el save en curso con el default. Reservalo para
> equipos nuevos o resets deliberados. Detalle en
> [Build & Flash](docs/build-and-flash/flashing-and-provisioning.md).

---

## Save por defecto (bundled)

El save por defecto vive en `data/save.srm` y se entrega **dentro de la imagen SPIFFS
`storage`**. En el arranque, `save_store_load()` lo carga en la RAM del cartucho
cuando todavía no hay un save persistido del usuario; un save persistido siempre tiene
prioridad sobre el default.

`data/save.srm` se regenera desde el `.srm` fuente (`scripts/copy_default_save.mjs`,
validado a 32 KB) antes de construir la imagen SPIFFS — tanto en `npm run build` como
en el build de PlatformIO (`scripts/platformio_default_save.py`, requiere `node`).

**La imagen SPIFFS no se flashea en un `upload` normal:** un `upload` graba firmware +
ROM (el juego aparece) pero no toca `storage`, así que en un equipo nuevo
`/spiffs/save.srm` no existe y la consola ve el save en blanco (`0xFF`). Por eso un
equipo nuevo se aprovisiona con `npm run upload:all` (o `pio run -t uploadfs`). Detalle
completo en [Cartridge → Default Save](docs/cartridge/default-save.md).

> Acoplamiento conocido (follow-up): `copy_default_save.mjs` apunta a un `.srm` fijo
> (Pokémon Yellow). Si cambiás la ROM activa, el save por defecto puede no
> corresponder al juego; conviene parear el default con `roms/active.gb`.

---

## Cargar otro juego

La ROM activa no se compila en el firmware: vive en la partición `rom` (`0x190000`,
1 MB) y se reemplaza desde el portal con **Replace ROM** o por HTTP. Si existe
`roms/active.gb`, se flashea automáticamente en `upload`. Cambiar la ROM borra el save
activo para no presentar la SRAM de otro juego.

```bash
curl --data-binary @tu_juego.gb  http://192.168.4.1/api/rom
curl --data-binary @save.srm     http://192.168.4.1/api/save
curl -o save.srm                 http://192.168.4.1/api/save
```

---

## Conectarse al portal

1. Tras el flash/monitor deberías ver:
   ```
   I (xxxx) web_portal: AP up: SSID=GameBoy-Link ip=192.168.4.1
   ```
2. Conectá el cliente Wi-Fi a `GameBoy-Link` (password `gameboy123`).
3. Abrí `http://192.168.4.1` en el browser.

### Endpoints

| Método | Ruta | Función |
|---|---|---|
| GET  | `/`               | UI Vite (desde SPIFFS). 503 si SPIFFS está vacío. |
| GET  | `/api/state`      | JSON con red, heap, stream y entrada |
| GET  | `/api/input_state`| Alias de `/api/state` |
| POST | `/api/input`      | Form-encoded `control=a\|b\|select\|start\|up\|down\|left\|right`, `pressed=1\|0` |
| GET  | `/api/save`       | Descarga el save activo (`save.srm`) |
| POST | `/api/save`       | Sube un save de 32 KB y lo persiste |
| POST | `/api/rom`        | Reemplaza la ROM activa en la partición `rom` |
| GET  | `/ws`             | Upgrade a WebSocket (HTTP+WS comparten puerto 80) |

### Mensajes WebSocket

- **Cliente → server**:
  - `{"type":"input","control":"a","pressed":true}`
  - `{"type":"audio","enabled":true}` (no-op si `GB_ENABLE_AUDIO=0`)
- **Server → cliente**:
  - Texto: `{"type":"state", … }` (mismo schema que `/api/state`).
  - Binario `GBF` (frame): `[ 'G' 'B' 'F' 0x01 W H | bytes 2bpp packed ]`.
  - Binario `GBA` (audio, sólo si APU activa): `[ 'G' 'B' 'A' 0x01 SR_lo SR_hi N_lo N_hi | PCM ]`.

> El WebSocket comparte el puerto **80** con HTTP vía upgrade (`ws://<ip>/ws`).
> Clientes que asumen `:81` deben apuntar a `/ws` en el puerto 80.

---

## Controles

El dispositivo **es** el controlador: el input llega desde el portal web (UI o
`POST /api/input`) y se traduce a las respuestas de poll del Joy-Bus. Una pulsación web
tiene un mínimo de **180 ms** (`WEB_MIN_PRESS_MS`) para que la ROM la registre, y se
libera automáticamente tras **1.5 s** de inactividad (`WEB_INPUT_TIMEOUT_MS`). Ver
[Joy-Bus → Controller](docs/joybus/controller.md).

---

## Configuración

Casi todo es ajustable en [`main/board_config.h`](main/board_config.h) (namespace
`board::`):

| Símbolo | Default | Para qué |
|---|---|---|
| `N64_JOYBUS_DATA_GPIO` | `7` | Pin de datos del Joy-Bus |
| `POWER_LOSS_SENSE_GPIO` | `-1` | Sensado de power-loss; `-1` deshabilita el monitor |
| `WEB_AP_SSID` / `WEB_AP_PASSWORD` | `GameBoy-Link` / `gameboy123` | SoftAP |
| `WEB_HTTP_PORT` / `WEB_SOCKET_PORT` | `80` | HTTP + WebSocket unificados |
| `WEB_STREAM_INTERVAL_MS` | `100` | Pacing del stream para no saturar el TX del AP |
| `WEB_MIN_PRESS_MS` / `WEB_INPUT_TIMEOUT_MS` | `180` / `1500` | Timing del input web |
| `SAVE_FLUSH_DEBOUNCE_MS` | `1000` | Ventana de quietud antes de persistir el save |
| `GB_ENABLE_AUDIO` | `1` | Compila el path de APU/audio por WebSocket |

Build flags útiles: `-DN64_JOYBUS_BITBANG` (transporte bit-bang),
`-DN64_JOYBUS_DATA_GPIO=<n>` (pin alterno), `-DGB_ENABLE_AUDIO=0` (sin audio). Ver
[Reference → Configuration](docs/reference/configuration.md).

---

## Mapa de particiones

Definido en [`partitions.csv`](partitions.csv) (flash interna de 4 MB):

| Nombre | Tipo | SubType | Offset | Tamaño | Propósito |
|---|---|---|---|---|---|
| `nvs` | data | nvs | `0x9000` | `0x6000` | Configuración |
| `phy_init` | data | phy | `0xF000` | `0x1000` | Calibración RF |
| `factory` | app | factory | `0x10000` | `0x180000` | Firmware (1.5 MB) |
| `rom` | data | `0x41` | `0x190000` | `0x100000` | ROM Game Boy activa (mmap) |
| `storage` | data | spiffs | `0x290000` | `0x167000` | SPIFFS: web UI + `save.srm` |
| `emgsave` | data | `0x40` | `0x3F7000` | `0x9000` | Slot de save de emergencia |

Los offsets `0x190000` (ROM) y `0x290000` (storage) deben coincidir con
`partitions.csv`. Si editás la tabla, borrá toda la flash y reflasheá una vez. Detalle
en [Architecture → Partitions & Storage](docs/architecture/partitions-and-storage.md).

---

## Troubleshooting

| Síntoma | Probable causa |
|---|---|
| `idf.py: command not found` | Falta `. ./export.sh` tras cambiar de shell. |
| Símbolos WebSocket async faltantes | ESP-IDF < v6.0. Actualizá. |
| El boot reporta el save como `missing` | No se flasheó la imagen SPIFFS. Aprovisioná con `npm run upload:all` o `pio run -t uploadfs`. |
| La consola lee el save en `0xFF` | Mismo caso anterior: el `save.srm` por defecto no llegó al equipo. |
| `503` en la raíz (`/`) | Falta flashear la imagen SPIFFS con la web UI. La API REST/WS sigue funcionando. |
| ROM no detectada en el portal | No hay ROM válida en la partición `rom`. Subila por **Replace ROM** o dejá `roms/active.gb`. |
| `invalid size` al cargar el save | El `save.srm` persistido es de otra ROM o no mide 32 KB. Se sobreescribe al primer write. |
| El WebSocket no conecta desde la UI | El cliente apunta a `:81`. Cambialo a `ws://<ip>/ws` (puerto 80). |

---

## Comandos útiles

```bash
# Sólo firmware (sin tocar SPIFFS, conserva saves)
pio run --target upload          # o: idf.py -p PORT app-flash

# Sólo imagen SPIFFS (web UI + save default) — sobreescribe el save del usuario
pio run --target uploadfs        # o, manual:
python -m esptool --chip esp32c3 -p PORT write-flash 0x290000 build/storage.bin

# Sólo ROM (también la maneja el post-script en upload)
python -m esptool --chip esp32c3 -p PORT write-flash 0x190000 roms/active.gb

# Borrar todo el flash (incluye saves) — requerido tras cambiar la tabla de particiones
pio run --target erase           # o: idf.py -p PORT erase-flash

# Tamaños e inspección (ESP-IDF)
idf.py size            # Tamaño total + por componente
idf.py partition-table # Volcar partitions.csv compilada
idf.py monitor         # Sólo serial monitor (Ctrl-] para salir)
```
