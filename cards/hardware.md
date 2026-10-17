# hardware

Wiring, board pinout, pins to avoid, and how the sensor is mounted relative to the screen.

## Parts

- **Waveshare ESP32-C6-LCD-1.47**: ESP32-C6FH4 (RISC-V, 160 MHz, **no FPU**), 4 MB flash, ST7789 1.47" 172×320 IPS LCD, microSD, WS2812 RGB LED, BOOT and RESET buttons, USB-C (native USB Serial/JTAG, shows up as `/dev/cu.usbmodem*`, VID:PID 303A:1001).
- **Pimoroni MLX90640 breakout**: 32×24 thermopile, I²C address 0x33, onboard pull-ups to its supply, Qw/ST connector.
- Schematic: `docs/ESP32-C6-LCD-1.47_schemetics.pdf`, a local gitignored copy of Waveshare's https://files.waveshare.com/wiki/ESP32-C6-LCD-1.47/ESP32-C6-LCD-1.47_schemetics.pdf (`make schematic` fetches it). It's the authoritative pin source; Waveshare's wiki has no header table. Don't commit it: it's Waveshare's document.

## Wiring (verified on hardware)

| MLX90640 | Header pin | Signal |
|---|---|---|
| GND | 11 | GND |
| 3–6V | 12 | 3V3 |
| SDA | 13 | GPIO0 |
| SCL | 14 | GPIO1 |

Power the breakout from **3V3, never 5V**: its pull-ups go to its supply rail, and the ESP32-C6 is not 5 V tolerant.

GPIO0/1 are the C6's XTAL_32K pins. This board has no 32 kHz crystal (only the 40 MHz one), and they aren't strapping pins, so they're free.

## Header U2 (from the schematic)

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | BOOT (IO9) | 10 | 5V |
| 2 | IO18 | 11 | GND |
| 3 | IO19 | 12 | 3V3 |
| 4 | IO20 | 13 | IO0 |
| 5 | IO23 | 14 | IO1 |
| 6 | USB_N (IO12) | 15 | IO2 |
| 7 | USB_P (IO13) | 16 | IO3 |
| 8 | RXD | 17 | IO4 (SD_CS) |
| 9 | TXD | 18 | IO5 (SD_MISO) |

Free for new uses: IO2, IO3, IO18, IO19, IO20, IO23.

## On-board, not on headers

- LCD: MOSI 6, SCLK 7, CS 14, DC 15, RST 21, backlight 22 (via MOSFET, high = on).
- microSD: CS 4, MISO 5, shares MOSI 6 / SCLK 7 with the LCD. Used for snapshots; see [sdcard](sdcard.md) for sharing the bus.
- RGB LED: GPIO8 (also a strapping pin).
- BOOT button: GPIO9, external 10k pull-up, pressed = low. Used as the freeze button.

## Pins to avoid

- **Strapping:** GPIO8, GPIO9, GPIO15 (and GPIO4/5 are MTMS/MTDI). Holding BOOT during reset enters the ROM bootloader.
- **USB:** GPIO12/13; using them kills flashing and serial.

## Sensor orientation

The breakout is mounted with the sensor's 32-pixel axis along the board's **short** edge. This is why the UI is portrait and the image is only 172×129: any orientation of the board gives the same image size, short of physically turning the breakout. With `SCREEN_ROTATION 0`, `FLIP_X 1`, `FLIP_Y 0` the image matches the scene (hand from above enters at the top, from the left at the left); verified on hardware.
