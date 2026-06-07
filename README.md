# ESP32-C3 Game Boy (ESP-IDF port)

Emulador de Game Boy clásico (DMG) para ESP32-C3 con pantalla redonda GC9A01, panel
táctil capacitivo CST816D y portal web embebido (HTTP + WebSocket sobre Wi-Fi
SoftAP).

---

## Hardware soportado

- **MCU**: ESP32-C3 (RISC-V single-core, 4 MB de flash)
- **Pantalla**: GC9A01 240×240 redonda sobre SPI2
  - SCLK = GPIO6, MOSI = GPIO7, CS = GPIO10, DC = GPIO2, BL = GPIO3
- **Touch**: CST816D capacitivo sobre I²C0
  - SDA = GPIO4, SCL = GPIO5, RST = GPIO1, INT = GPIO0
- Pinout completo en [`main/board_config.h`](main/board_config.h). Si tu placa
  difiere, edita ese archivo — el resto del código se ajusta solo.

---

## Requisitos

| Herramienta | Versión |
|---|---|
| ESP-IDF | v6.0 o superior (probado con v6.x). El portal usa APIs WebSocket async de `esp_http_server` v6. |
| Toolchain | `riscv32-esp-elf` (lo instala el script de IDF) |
| Python | 3.9+ (lo provee el virtualenv de IDF) |
| Node.js | 18+ (sólo si vas a recompilar la web UI bajo `webui/`) |

Si todavía no instalaste el entorno de IDF:

```bash
cd /ruta/a/esp-idf
./install.sh esp32c3
. ./export.sh
```

---

## Estructura del proyecto

```
esp32-socket-react/
├── CMakeLists.txt              # Top-level CMake
├── partitions.csv              # 1.5 MB app + 1 MB ROM + SPIFFS + emergency save
├── sdkconfig.defaults          # Config base (target c3, WS, SPIFFS, …)
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml       # Pulls espressif/esp_lcd_gc9a01
│   ├── main.cpp                # app_main → bucle de frames
│   ├── board_config.h          # Pinout + dimensiones + SSID + paleta
│   ├── gb_time.h               # Shims millis/micros/delay sobre esp_timer
│   ├── emulator_runtime.*      # Orquesta init y un frame
│   ├── display.*               # esp_lcd + driver GC9A01
│   ├── cst816d.*               # Driver i2c_master del táctil
│   ├── touch_input.*           # Mapeo táctil + web → botones GB
│   ├── web_portal.*            # SoftAP + HTTP + WebSocket
│   ├── cartridge/              # ROM mmap, mappers, save persistence
│   ├── joybus/                 # N64 controller + Transfer Pak transport
│   └── web/                    # SoftAP + HTTP + WebSocket
```

---

## Build y flash

### Primera vez

```bash
. /ruta/a/esp-idf/export.sh
cd /ruta/a/esp32-socket-react
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

### Iteraciones

```bash
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor       # Ctrl-] para salir
```

### Limpieza

```bash
idf.py fullclean             # borra build/
idf.py reconfigure           # tras cambios en sdkconfig / componentes
```

---

## Web UI y SPIFFS

El portal web sirve los assets embebidos en el firmware y también puede montar
la partición SPIFFS `storage` en `/spiffs` para saves y assets actualizados. Con
PlatformIO, `pio run` genera la imagen SPIFFS desde `data/` cuando existe.

### 1. Compilar la UI Vite

La fuente de la UI está en [`webui/`](webui/)
(se reusa intacta — el wire-protocol del WebSocket es idéntico).

```bash
cd webui
npm install
npm run build              # genera dist/
```

### 2. Generar la imagen SPIFFS

ESP-IDF incluye `spiffsgen.py`. Tamaño = `0x167000` bytes (ver
`partitions.csv`).

```bash
python $IDF_PATH/components/spiffs/spiffsgen.py \
  1478656 \
  data build/storage.bin
```

### 3. Flashear sólo la partición de datos

```bash
python -m esptool --chip esp32c3 -p /dev/cu.usbserial-XXXX \
  write-flash 0x290000 build/storage.bin
```

El offset `0x290000` debe coincidir con el de la entrada `storage` de
`partitions.csv`. Si editas las particiones, actualiza ese número.

### Bypass: usar la API sin web UI

Si no flasheás SPIFFS, el AP sigue activo y los endpoints REST + WebSocket
funcionan; sólo `GET /` devuelve **503 Service Unavailable**. Útil para
pruebas con `curl` o con el cliente WebSocket que prefieras.

---

## Cargar otro juego

La ROM activa ya no se compila en el firmware. Vive en la partición `rom`
(`0x190000`, 1 MB) y se puede reemplazar desde el portal con **Replace ROM** o
por HTTP:

```bash
curl --data-binary @tu_juego.gb http://192.168.4.1/api/rom
```

Si existe `roms/active.gb`, PlatformIO lo flashea automáticamente en la
partición `rom` durante `pio run --target upload`. Si no existe, el equipo
arranca con la partición en blanco y el portal muestra que no hay ROM válida.

Cambiar la ROM borra el save activo para no presentar una SRAM de otro juego.
Después podés subir el `.srm` correspondiente desde **Upload save** o por HTTP:

```bash
curl --data-binary @save.srm http://192.168.4.1/api/save
curl -o save.srm http://192.168.4.1/api/save
```

### Save por defecto (bundled)

El save por defecto vive en `data/save.srm` y se entrega **dentro de la imagen
SPIFFS `storage`** (la misma que lleva la web UI). En el arranque,
`save_store_load()` lo carga en la RAM del cartucho cuando todavía no hay un save
persistido del usuario; un save persistido siempre tiene prioridad sobre el default.

`data/save.srm` se regenera automáticamente desde el `.srm` fuente
(`scripts/copy_default_save.mjs`, validado a 32 KB) antes de construir la imagen
SPIFFS — tanto en `npm run build` como en el build de PlatformIO
(`scripts/platformio_default_save.py`, requiere `node` en el PATH).

**Importante — la imagen SPIFFS no se flashea en un `upload` normal.** Un
`pio run --target upload` graba el firmware y la ROM (`roms/active.gb`), por lo que
el juego aparece, pero **no** toca la partición `storage`. Para entregar el save por
defecto en un equipo nuevo hay que flashear también la imagen SPIFFS:

```bash
# Provisión completa de un equipo nuevo (app + ROM + SPIFFS con save default):
cd webui && npm run upload:all      # build → pio upload → pio uploadfs

# Sólo la imagen SPIFFS (save default + web UI):
pio run --target uploadfs
```

Reflashear SPIFFS **sobrescribe el save persistido del usuario** con el default, así
que reservalo para provisión inicial. Para reflasheos de día a día usá `upload` /
`app-flash`, que dejan intacto el save del jugador en `storage`.

> Acoplamiento conocido (follow-up): `copy_default_save.mjs` apunta a un `.srm`
> fijo (Pokémon Yellow). Si cambiás la ROM activa, el save por defecto puede no
> corresponder al juego; conviene parear el default con `roms/active.gb`.

---

## Conectarse al portal

1. Tras `idf.py flash monitor` deberías ver:
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
| GET  | `/api/input_state`| Alias del anterior |
| POST | `/api/input`      | Form-encoded `control=a|b|select|start|up|down|left|right`, `pressed=1\|0` |
| GET  | `/api/save`       | Descarga el save activo (`save.srm`) |
| POST | `/api/save`       | Sube un save de 32 KB y lo persiste |
| POST | `/api/rom`        | Reemplaza la ROM activa en la partición `rom` |
| GET  | `/ws`             | Upgrade a WebSocket (HTTP+WS comparten puerto 80) |

### Mensajes WebSocket

- **TX (cliente → server)**:
  - `{"type":"input","control":"a","pressed":true}`
  - `{"type":"audio","enabled":true}` (no-op si `GB_ENABLE_AUDIO=0`)
- **RX (server → cliente)**:
  - Texto: `{"type":"state", … }` (mismo schema que `/api/state`).
  - Binario `GBF` (frame): `[ 'G' 'B' 'F' 0x01 W H | bytes 2bpp packed ]`.
  - Binario `GBA` (audio, sólo si APU activa): `[ 'G' 'B' 'A' 0x01 SR_lo SR_hi N_lo N_hi | PCM bytes ]`.

> El puerto del WebSocket cambió respecto al original: ahora es **80** (mismo
> que HTTP, vía upgrade). Si reusas el cliente Vite original sin tocar nada,
> probablemente intente conectarse a `:81`. Hay que ajustar el endpoint en la
> UI a `ws://<ip>/ws`.

---

## Controles

### Touch físico (sobre la pantalla GC9A01 de 240×240)

| Zona | Botón GB |
|---|---|
| Top-izq (x<80, y<60)             | **SELECT** |
| Top-medio (80≤x<160, y<60)       | **UP** |
| Top-der (x≥160, y<60)            | **START** |
| Izquierda (x<80, 60≤y<160)       | **LEFT** |
| Derecha (x>200, 60≤y<160)        | **RIGHT** |
| Centro-derecha (160≤x≤200)       | **A** |
| Bottom-medio (80≤x<160, y≥160)   | **DOWN** |
| Bottom-der (x≥160, y≥160)        | **B** |

La pantalla muestra finas barras blancas/grises en los bordes para hint de
estas zonas (no hay rótulos de texto en esta versión).

### Web

Los mismos botones desde la UI o `POST /api/input`. Una pulsación web tiene
un mínimo de 180 ms para que la ROM la registre, y se libera automáticamente
si no hay actividad por 1.5 s (timeout configurable en `board_config.h`).

---

## Configuración

Casi todo es ajustable desde [`main/board_config.h`](main/board_config.h):

```cpp
WEB_AP_SSID          = "GameBoy-Link"
WEB_AP_PASSWORD      = "gameboy123"   // ≥8 chars o queda como AP abierto
TFT_WRITE_HZ         = 40 MHz         // Velocidad SPI del LCD
TARGET_FPS           = 60
WEB_STREAM_INTERVAL_MS = 100          // 10 fps al WebSocket
SAVE_FLUSH_DEBOUNCE_MS = 1000         // Debounce de SRAM persistence
GB_ENABLE_AUDIO      = 0              // Activá APU (caro en C3)
DEBUG_TOUCH_INPUT    = false
DEBUG_WEB_INPUT      = false
```

Para activar audio:
```cmake
# main/CMakeLists.txt
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    "INTER_MODULE_OPT=1"
    "GB_ENABLE_AUDIO=1"
)
```

`sdkconfig` se genera automáticamente la primera vez. Para tunear más:
`idf.py menuconfig`. Los defaults útiles ya están en `sdkconfig.defaults`
(stack del main task, soporte WebSocket, SPIFFS, etc.).

---

## Arquitectura interna

### Bucle principal (`emulator_run_frame`)

```
┌─────────────────────────────────────────┐
│  while !screen_updated:                 │
│     cycles = cpu_cycle()                │
│     screen_updated = lcd_cycle(cycles)  │
│     timer_cycle(cycles)                 │
│     [apu_cycle(cycles) si GB_AUDIO]     │
│  sdl_update()    ← lee touch + push LCD │
│  service_web_portal(now, framebuffer)   │
│  mem_persist(now) ← guarda SRAM si toca │
│  sleep hasta completar 16.67 ms         │
└─────────────────────────────────────────┘
```

Una sola task de FreeRTOS (la del `app_main`, con stack 8 KB) corre todo:
emulación + presentación + housekeeping del HTTP server. El WebSocket usa el
worker propio de `esp_http_server`, así que no bloquea el loop de emulación.

### Optimización `INTER_MODULE_OPT`

Para que el GCC optimice cross-module, `cpu.cpp` `#include`a `interrupt.cpp`,
`mem.cpp` y `timer.cpp` *en el mismo translation unit*. Las versiones
"standalone" de esos archivos quedan vacías (su contenido vive dentro de un
`#ifndef INTER_MODULE_OPT`). El flag se setea en `main/CMakeLists.txt`. Si
removés el flag, recordá quitar el `#include` desde `cpu.cpp` o vas a tener
símbolos duplicados.

---

## Troubleshooting

| Síntoma | Probable causa |
|---|---|
| `idf.py: command not found` | Te falta `. ./export.sh` después de cambiar de shell. |
| `i2c_master.h: No such file` o símbolos WebSocket async faltantes | IDF < v6.0. Actualizá. |
| `LCD_RGB_ENDIAN_BGR` no existe | IDF < v5.4. En `main/display.cpp` podés volver a `rgb_endian = LCD_RGB_ENDIAN_BGR`. |
| Build OK pero la pantalla queda en negro | Backlight: GPIO3 debe estar HIGH. Verificá la configuración de tu placa. |
| El touch responde invertido | Las zonas X/Y están definidas en `main/touch_input.cpp` siguiendo la orientación nativa del CST816D. Si tu placa lo monta rotado, ajustá ahí. |
| `SRAM file size mismatch; ignoring` en boot | El save guardado es de otra ROM. Es esperado al cambiar de juego. Se sobreescribe al primer write. |
| `503` en la raíz | Falta flashear la imagen SPIFFS con la web UI (ver sección anterior). |
| Pantalla con colores invertidos | En `display.cpp` toggleá la línea `esp_lcd_panel_invert_color(panel_handle, true);` |
| WebSocket no se conecta desde la UI Vite | El cliente original apuntaba a `:81`. Cambialo a `ws://<ip>/ws` (puerto 80). |

---

## Diferencias visibles vs. la versión Arduino original

- **HTTP y WebSocket comparten puerto 80** (un solo `esp_http_server`). El
  original usaba 80 + 81 con `WebSocketsServer`.
- **Status messages**: en lugar de imprimir texto sobre el LCD (LovyanGFX
  traía fuente bitmap incluida), esta versión muestra una franja de color
  (verde/rojo/amarillo/cian) y emite el texto al UART vía `ESP_LOG`. Los
  rótulos del overlay táctil ("SEL", "UP", "A"…) tampoco se renderizan; en su
  lugar hay barras de hint.
- **APU**: deshabilitada por defecto, igual que en el original.
- **mkspiffs manual**: en PlatformIO, `pio run --target uploadfs` empaquetaba
  y subía. Acá hay que ejecutar `spiffsgen.py` + `esptool write-flash` a mano
  (ver más arriba).

Si necesitás restaurar texto en pantalla (status / labels), avisame y agrego
una fuente 5×7 embebida + helper de `draw_text`.

---

## Comandos útiles

```bash
idf.py size                                  # Tamaño total + por componente
idf.py size-components                       # Detalle por componente
idf.py size-files                            # Detalle por .o
idf.py menuconfig                            # Configurador interactivo
idf.py partition-table                       # Volcar partitions.csv compilada
idf.py monitor                               # Solo serial monitor

# Reflashear sólo el firmware (sin tocar SPIFFS):
idf.py -p PORT app-flash

# Reflashear sólo SPIFFS:
python -m esptool --chip esp32c3 -p PORT \
  write-flash 0x290000 build/storage.bin

# Borrar todo el flash (incluye saves):
idf.py -p PORT erase-flash

# Cambio de tabla de particiones: borrar todo y reflashear una vez.
# Necesario al pasar a la partición `rom` dedicada.
idf.py -p PORT erase-flash
idf.py -p PORT flash
```
