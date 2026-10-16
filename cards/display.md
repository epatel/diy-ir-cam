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
| Hot-spot ring | r 5, cyan + black outer ring | drawn into `imgBuf` |
| Colour bar | x 10, y 147, 152×10 | cold left → hot right; static, drawn once |
| Min / max | y 163, size 2 | smoothed auto-range |
| "centre" label | y 206, size 1 | static, drawn once |
| Centre temp | x 20, y 222, size 4 + "C" size 2 | smoothed (EMA 0.25) |
| Hot temp | y 268, size 2, cyan | raw hottest pixel |
| fps | x 10, y 306, size 1, dark grey | |
| HOLD badge | bottom right, size 2, red | only while frozen |

Text uses the built-in 5×7 font in 6×8 cells × size, printed with `setTextColor(fg, bg)` so each update overwrites the previous value. Numbers are fixed-width (`%5.1f`) so stale digits never remain.

`mockup/screen-mockup.html` reproduces this layout pixel for pixel (same palette, taps, font glyphs, smoothing). Keep it in sync when the layout changes; it's published as a private claude.ai artifact.

## Per-frame pipeline (`loop()`)

1. If the freeze flag toggled, flip `frozen` and draw/clear the HOLD badge; while frozen, return early so the last frame stays.
2. `readFrame()`: two subpages → `frame[768]` in °C, sensor order.
3. Frame min/max → enforce ≥ 5 °C span → smooth range 30 % per frame.
4. Centre = mean of the 4 middle pixels, EMA-smoothed. Hot spot = max pixel refined by a 3×3 weighted centroid (pixels within 2 °C of max).
5. `renderImage()` into `imgBuf` (172×129 RGB565 on the heap), then crosshair and ring into `imgBuf`, then one `draw16bitRGBBitmap`. Drawing overlays into the buffer avoids flicker and clips them to the image.
6. Panel text.

## Rendering is fixed point

The ESP32-C6 has no FPU. The original float bilinear loop took ~324 ms per frame and capped the camera at ~2 fps. Now: sensor values are mapped once to `palette index << 8` (768 float ops), and the per-pixel loop uses integers with precomputed `tapX`/`tapY` tables (`buildTaps`, weights 0..256). It takes ~9 ms. Don't put float maths back into any per-output-pixel loop; per-sensor-pixel (768) float work is fine.

Measured per full frame (earlier 224×168 image, 8 Hz): render 9 ms, LCD push 20 ms, `CalculateTo` 46 ms, rest waiting on the sensor.

## Freeze button

BOOT (GPIO9) toggles HOLD. `loop()` blocks in `readFrame()` for most of each frame, so the button is sampled every 5 ms by a `Ticker`. A press counts after 8 consecutive low samples (40 ms) and must see 8 high samples before re-arming. An earlier edge-interrupt + 200 ms lockout double-toggled on release bounce when the button was held longer than 200 ms.
