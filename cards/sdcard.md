# sdcard

Snapshots on the microSD card and the SPI bus they share with the LCD, in `src/sdcard.cpp` / `src/sdcard.h`.

## What it does

- `sdBegin(LCD_SCLK, LCD_MOSI)` in `setup()`, right after `gfx->begin()`: `SPI.begin(7, 5, 6, 4)` + `SD.begin(SD_CS 4, SPI, 20 MHz)`, creates `/ircam`, and finds the next free number from the highest `IMG_nnnn` already there. No card → not mounted; `sdSave()` tries to mount again, so a card inserted later works.
- `sdSave()` writes `/ircam/IMG_nnnn.png` (8-bit RGB, 172×129, `imgBuf` as on screen including crosshair and spot ring) and `IMG_nnnn.csv` (24 lines × 32 values in °C, display orientation, ~5 KB). Returns the number or -1; a failed write marks the card unmounted so the next save remounts.
- **PNG encoding** (`writePng()`): Sub filter on every row, deflate from miniz in the **C6 ROM** (`tdefl_init`/`tdefl_compress_buffer`, `mz_crc32`, `mz_adler32` via `<miniz.h>`), so it costs no flash. The `tdefl_compressor` is ~165 KB (ROM build uses `TDEFL_LESS_MEMORY`), malloc'd per save and freed; if that fails, the zlib stream is written as stored blocks (one per row, ~67 KB file, still valid PNG). Serial prints which path ran and the file size. IDAT chunks are 4 KB.
- Triggers: menu item **Save** (first item, so hold-hold saves) and the web page's *Save to SD* (`/save` → `webTakeSaveRequest()` in `handleWeb()`). Both end in `saveSnapshot()` in `main.cpp`, which shows `saved nnnn` (green) or `no SD card` (red) on the spot line for 2 s and bumps `saveSeq`/`savedNum` in the web snapshot.
- Card must be **FAT32** (FATFS in the core has no exFAT); 64 GB+ cards usually come exFAT and need reformatting. There's no clock, so files are numbered, not dated.

## Shared SPI bus

The card (CS 4, MISO 5) shares SCLK 7 / MOSI 6 with the LCD on the C6's only general SPI host (SPI2/FSPI). Arduino_GFX's `Arduino_ESP32SPI` keeps its **own** `spi_t` and mutex, separate from the core's that `SPIClass`/`SD` use, so their locks don't exclude each other. Every transaction re-applies its own clock and mode, so the two can take turns as long as they never overlap:

- `spiBusLock()`/`spiBusUnlock()`: one recursive mutex of ours. `loop()` holds it for everything except `readFrame()` (the I²C wait, most of each frame); the `sd*()` functions take it themselves. Drawing from anywhere else needs it too.
- **Duplex mode:** `SPI.begin()` sets the peripheral full duplex (`user.doutdin`/`usr_miso` = 1), which the card needs for reads. The LCD driver (no MISO) fills `data_buf` once and re-sends it for `fillRect`/`fillScreen`; in full duplex the 0xFF read back from idle MISO overwrites that buffer, so black fills came out **white** (seen on hardware when the menu opened). `sdEnter()`/`sdLeave()` take the bus lock and switch `GPSPI2` to full duplex only for the duration of each `sd*()` call (depth-counted, since `sdSave()` can call `sdBegin()`). Any new SD code must go through them, and `File` objects must close before `sdLeave()`.
- `/file` downloads stream in 2 KB chunks, taking the lock per chunk, so the screen keeps updating during a download.
- Order in `setup()`: `spiBusInit()` (CS 4 high) → `gfx->begin()` (resets the SPI peripheral) → `sdBegin()`. Mounting before `gfx->begin()` would be undone by that reset.

## Budget

With SD the app is ~1.15 MB of the 1.25 MB app partition (88 %). Next step if it grows: `board_build.partitions = huge_app.csv` (no OTA is used).
