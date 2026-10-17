# display

Screen layout, rendering pipeline and drawing conventions in `src/main.cpp`.

## Panel setup

- `Arduino_ESP32SPI(DC 15, CS 14, SCLK 7, MOSI 6)` + `Arduino_ST7789(bus, RST 21, 0, true /*IPS*/, 172, 320, 34, 0, 34, 0)`. The 172-wide panel sits in a 240-wide controller RAM, hence the **34-pixel column offsets**.
- `gfx->begin(80000000)` (80 MHz SPI). Backlight GPIO22 driven high in `setup()`.
- `setRotation(SCREEN_ROTATION)`: 0 = portrait (current). Rotation 2 is the board upside down; `px()` flips both axes automatically so the image stays correct.
- Arduino_GFX ≥ 1.5 names colours `RGB565_WHITE`, `RGB565_CYAN`, etc. The old `WHITE`/`BLACK` names don't compile.

## Layout (portrait 172×320)

| Element | Position | Notes |
|---|---|---|
| Thermal image | x 0, y 4, 172×129 | bilinear from 32×24 (5.375×) |
| Crosshair | image centre, ±8 px | white, drawn into `imgBuf` |
| Spot ring | r 5, cyan + black outer ring, for hot and cold alike | drawn into `imgBuf`; one at a time per `spotMode` |
| Colour bar | x 10, y 147, 152×10 | cold left → hot right; static, drawn once |
| Min / max | y 163, size 2 | smoothed auto-range |
| "centre" label | y 206, size 1 | static, drawn once |
| Centre temp | x 20, y 222, size 4 + "C" size 2 | smoothed (EMA 0.25) |
| Spot temp | x 26, y 268, size 2, cyan, `hot  ` / `cold ` prefix | raw hottest or coldest pixel; blanked when spot is off |
| fps | x 10, y 306, size 1, dark grey | |
| SSID | x 70, y 306, size 1, dark grey | `AP_SSID`, only while Wi-Fi is on (`drawWifiLabel()`) |
| HOLD badge | bottom right, size 2, red | only while frozen |
| Message | spot line (x 26, y 268), size 2, 10 chars | `saved nnnn` green / `no SD card` red for 2 s (`showMessage()`, `msgUntilMs`); `drawPanel()` leaves it up, `handleButton()` restores the spot line after |
| Menu | y 196–297, replaces centre/spot readouts | five rows size 2, 18 px apart from y 202, text x 14; selected row black on yellow; `*` marks the current state |

Text uses the built-in 5×7 font in 6×8 cells × size, printed with `setTextColor(fg, bg)` so each update overwrites the previous value. Numbers are fixed-width (`%5.1f`) so stale digits never remain.

`mockup/screen-mockup.html` reproduces this layout pixel for pixel (same palette, taps, font glyphs, smoothing). Keep it in sync when the layout changes; it's published as a private claude.ai artifact.

## Per-frame pipeline (`loop()`)

0. `loop()` holds `spiBusLock()` throughout except during `readFrame()`, because the SD card shares the LCD's SPI bus ([sdcard](sdcard.md)).
1. `handleButton()` and `handleWeb()` act on button events and web-page requests (freeze, menu, spot mode, save); while frozen, return early so the last frame stays.
2. `readFrame()`: two subpages → `frame[768]` in °C, sensor order.
3. Frame min/max → enforce ≥ 5 °C span → smooth range 30 % per frame.
4. Centre = mean of the 4 middle pixels, EMA-smoothed. Spot (in `drawThermal()`) = max or min pixel per `spotMode`, refined by a 3×3 weighted centroid (pixels within 2 °C of the extreme); `findSpot(±1)`.
5. `drawThermal()`: `renderImage()` into `imgBuf` (172×129 RGB565 on the heap), then crosshair and the spot ring (if any) into `imgBuf`, then one `draw16bitRGBBitmap`. Drawing overlays into the buffer avoids flicker and clips them to the image. Per-frame values live in file-level statics so a frozen frame can be redrawn when the spot mode changes.
6. `drawPanel(!menuOpen)`: min/max and fps always; centre and spot only when the menu is closed.
7. `publishWeb()`: snapshot for the web page (see [wifi](wifi.md)).

## Rendering is fixed point

The ESP32-C6 has no FPU. The original float bilinear loop took ~324 ms per frame and capped the camera at ~2 fps. Now: sensor values are mapped once to `palette index << 8` (768 float ops), and the per-pixel loop uses integers with precomputed `tapX`/`tapY` tables (`buildTaps`, weights 0..256). It takes ~9 ms. Don't put float maths back into any per-output-pixel loop; per-sensor-pixel (768) float work is fine.

Measured per full frame (earlier 224×168 image, 8 Hz): render 9 ms, LCD push 20 ms, `CalculateTo` 46 ms, rest waiting on the sensor.

## BOOT button and menu

BOOT (GPIO9) is the only button. `loop()` blocks in `readFrame()` for most of each frame, so `pollButton()` samples it every 5 ms from a `Ticker`. A press counts after 8 consecutive low samples (40 ms) and must see 8 high samples before re-arming. Held 600 ms → `longPress` fires immediately (while still held); released earlier → `shortPress` on release. An earlier edge-interrupt + 200 ms lockout double-toggled on release bounce when the button was held longer than 200 ms.

- Outside the menu: short press toggles freeze (HOLD), long press opens the menu.
- In the menu: short press steps through Save / Spot off / Hot spot / Cold spot (exclusive; `SpotMode` order, offset by `MENU_SPOT`) / Wi-Fi (toggle); long press closes the menu and acts. Save is first so hold, hold takes a snapshot ([sdcard](sdcard.md)). It also closes after 6 s without a press. Freeze isn't in the menu; the short press covers it.
- Events are handled once per `loop()`, i.e. up to one frame (~125 ms) late while live, every 20 ms while frozen.
- Default spot mode: hot; Wi-Fi on.

## Saved settings

Spot mode and Wi-Fi are stored with `Preferences` (NVS namespace `ircam`, keys `ver`, `spot`, `wifi`), written by `saveSettings()` only when they change. Freeze is not saved. `loadSettings()` in `setup()` runs `migrateSettings()` first: `ver` is `SETTINGS_VERSION` (now 1); a missing `ver` (first boot) or one from newer firmware resets to defaults. When a stored key changes meaning, bump `SETTINGS_VERSION` and add a conversion step from the previous version there instead of clearing.
