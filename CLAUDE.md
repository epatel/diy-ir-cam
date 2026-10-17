# DIY IR Camera

Arduino firmware (PlatformIO) for a handheld thermal camera: a Waveshare ESP32-C6-LCD-1.47 reads a Pimoroni MLX90640 over I²C and draws a 172×129 ironbow image plus temperature readouts on its portrait 172×320 ST7789 screen. Almost all logic is in `src/main.cpp`; the Wi-Fi access point and its web page are in `src/web.cpp` / `src/web_page.h`; snapshots to the microSD card are in `src/sdcard.cpp`; the sensor driver is vendored in `lib/MLX90640/`. The user tests on real hardware connected over USB.

@project-plan.md

## Known stack

- **PlatformIO + pioarduino** (`platformio.ini`): Arduino-ESP32 3.2 on ESP-IDF 5.4, board `esp32-c6-devkitc-1`. Not the official espressif32 platform.
- **Arduino-ESP32**: `Wire` for I²C (128-byte buffer), `Ticker` for the button poll, `Serial` is native USB CDC. `WiFi` (soft AP), `WebServer` and `DNSServer` from the core for the web page; no extra libraries.
- **Arduino_GFX** (moononournation): `Arduino_ESP32SPI` + `Arduino_ST7789`; colours are `RGB565_*`.
- **Melexis MLX90640 API**: `lib/MLX90640/`, upstream C code unchanged; project code is only `MLX90640_I2C_Driver.cpp`.
- **ESP32-C6**: RISC-V without FPU. Float maths is software-emulated and slow.
- `mockup/screen-mockup.html`: standalone HTML/JS replica of the screen; no build.
- `docs/`: reference material only, never built (`docs/mlx90640-library` is a git submodule).

## Commands

```sh
make build                 # build
make flash                 # build + flash, auto-detects /dev/cu.usbmodem* (PORT=... to override)
make serial SECONDS=6      # read serial non-interactively (serial_reset restarts the board first)
make compiledb             # refresh compile_commands.json
make                       # numbered menu of all targets
```

The Makefile wraps `~/.platformio/penv/bin/pio` and `tools/serial-read.py`. Never the `pio` on PATH (obsolete, wrong Python).

Enforced by `.claude/settings.json`: the commands above run without prompts; edits to `src/` or `lib/` trigger an incremental build that reports compiler errors; editing the vendored Melexis files asks first; `docs/mlx90640-library/` (submodule) can't be edited.

## Cards

Read a card when the situation on the right applies. They're plain links, loaded on demand.

### Domains

- [hardware](cards/hardware.md) — wiring, choosing a GPIO, anything involving pins, buttons, power or how the sensor is mounted
- [mlx90640](cards/mlx90640.md) — touching sensor reads, I²C, refresh rate, emissivity, or the vendored Melexis API
- [wifi](cards/wifi.md) — the access point, captive-portal web page, `/frame.json`, or anything shared between the web task and `loop()`
- [sdcard](cards/sdcard.md) — saving snapshots, the microSD card, or anything sharing the LCD's SPI bus (drawing outside `loop()`)
- [display](cards/display.md) — changing what's on screen, the layout, rendering speed, overlays, the freeze button, or the mockup
- [toolchain](cards/toolchain.md) — a build, flash, serial or editor problem, or before running pio for the first time in a session

## Keeping docs current

In any commit that changes logic, settles a design choice, or makes a card wrong, update the affected card(s) and `project-plan.md` (Decisions, Current state). When the screen layout changes, update `mockup/screen-mockup.html` to match.
