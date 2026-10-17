# Project plan

Shared state for every agent and session. Read it first; update **Current state** (and Decisions or Milestones if they changed) before finishing a task.

## Goal

A handheld thermal camera: Waveshare ESP32-C6-LCD-1.47 + Pimoroni MLX90640, showing a smooth, readable live thermal image with useful temperature readouts, built with PlatformIO.

## Non-goals

- Radiometric accuracy beyond what the Melexis API gives (no per-object emissivity tables or calibration rigs).
- Changing the vendored Melexis API files.
- Battery/enclosure design (unless the user asks).

## Milestones

- [x] Wiring and pinout from the schematic; sensor on GPIO0/GPIO1
- [x] Firmware: Melexis API + Wire shim, ironbow bilinear image, auto-range
- [x] Fixed-point rendering (2 → 4 fps), then 16 Hz subpages (~7.9 fps)
- [x] Portrait layout matching the sensor mounting; orientation verified on hardware
- [x] Smoothed centre reading, hot-spot marker, BOOT freeze with debouncer
- [x] Cold-spot marker and readout, as an alternative to the hot spot
- [x] BOOT menu (long press): spot off / hot / cold, Wi-Fi on/off
- [x] Spot mode and Wi-Fi saved to NVS, with a settings version for migrations
- [x] HTML mockup in sync with the firmware; README; git repo
- [x] Wi-Fi access point "IR-Cam" with a captive-portal page: live image, readouts, spot/freeze options, save PNG
- [x] Snapshots to microSD (PNG + CSV), from the menu and the web page; list, download and delete over Wi-Fi
- [ ] Next ideas (not committed to): sensor-ambient readout, palette switching

## Decisions

- 2026-10-16: Use Melexis' official API (vendored unchanged from pimoroni/mlx90640-library @ a92229a) instead of the Adafruit library.
- 2026-10-16: I²C on GPIO0/GPIO1 (pins 13/14, next to 3V3/GND). Breakout powered from 3V3.
- 2026-10-16: Portrait layout, 172×129 image. The sensor's long axis runs along the board's short edge, so no orientation gives a bigger image without turning the breakout. `SCREEN_ROTATION 0`, `FLIP_X 1`, `FLIP_Y 0` verified on hardware.
- 2026-10-16: Per-pixel rendering in fixed point; the C6 has no FPU (float version: 324 ms/frame).
- 2026-10-16: 16 Hz subpage rate (~7.9 fps). Only the centre reading is smoothed (EMA 0.25); the image stays raw.
- 2026-10-16: Freeze on BOOT (GPIO9) with a 5 ms polled, 8-sample debouncer.
- 2026-10-16: One-button menu: long press (600 ms, fires while held) opens; short steps; long selects and closes; 6 s timeout. Items: Spot off, Hot spot, Cold spot (exclusive). Freeze stays on the short press outside the menu, not in the menu. Default spot: hot.
- 2026-10-16: Wi-Fi as an open access point (not station mode) with a captive portal, so the live page opens as the sign-in page. The web server runs in its own task and only exchanges a snapshot and request flags with `loop()`; the browser renders the image from raw temperatures.
- 2026-10-17: Wi-Fi can be switched off from the menu (the web task stops the AP). Spot mode and Wi-Fi persist in NVS via `Preferences`, versioned (`SETTINGS_VERSION`) so later firmware can migrate stored keys; freeze is not persisted.
- 2026-10-17: Snapshots on the microSD card as IMG_nnnn.png (screen image) + .csv (32×24 °C) in /ircam, FAT32. The PNG has a readout strip below the image (colour bar, min/max, centre, active spot) and the same values in a tEXt Comment. It is stored uncompressed (~86 KB): the ROM's miniz deflate needs ~165 KB of heap and didn't compress even when it got it. Triggered by menu item Save (first in the menu) or the web page. The SD card and LCD share SPI2 but not a lock, so `loop()` holds our own recursive bus mutex except while reading the sensor, and the web task streams files in 2 KB chunks under it.
- 2026-10-16: Cold spot uses the same cyan as the hot spot (green was too dark on the panel). Only one spot is shown at a time (off / hot / cold), so both share one marker and one readout line.

## Current state

Everything above works on the hardware and is committed, including SD snapshots (save from the menu and the web page, list, download, delete). Snapshots include a readout strip and are stored as uncompressed PNG. Not yet measured: fps while a phone is watching.

## Open questions

- None.
