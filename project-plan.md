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
- [x] HTML mockup in sync with the firmware; README; git repo
- [ ] Next ideas (not committed to): sensor-ambient readout, palette switching, snapshots to microSD, Wi-Fi image streaming

## Decisions

- 2026-10-16: Use Melexis' official API (vendored unchanged from pimoroni/mlx90640-library @ a92229a) instead of the Adafruit library.
- 2026-10-16: I²C on GPIO0/GPIO1 (pins 13/14, next to 3V3/GND). Breakout powered from 3V3.
- 2026-10-16: Portrait layout, 172×129 image. The sensor's long axis runs along the board's short edge, so no orientation gives a bigger image without turning the breakout. `SCREEN_ROTATION 0`, `FLIP_X 1`, `FLIP_Y 0` verified on hardware.
- 2026-10-16: Per-pixel rendering in fixed point; the C6 has no FPU (float version: 324 ms/frame).
- 2026-10-16: 16 Hz subpage rate (~7.9 fps). Only the centre reading is smoothed (EMA 0.25); the image stays raw.
- 2026-10-16: Freeze on BOOT (GPIO9) with a 5 ms polled, 8-sample debouncer.

## Current state

Everything above works on the hardware and is committed. No work in progress.

## Open questions

- None.
