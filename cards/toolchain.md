# toolchain

Building, flashing and reading serial output on this machine, including the PlatformIO quirks that cost time before.

## Which pio

Use **`~/.platformio/penv/bin/pio`**. The `pio` on `PATH` is an obsolete PlatformIO 6.1.18 on Python 3.9 (`~/Library/Python/3.9/bin/pio`), and the pioarduino platform refuses it ("Python version must be 3.10 …").

## Platform

`platformio.ini` uses the community **pioarduino** platform (`platform-espressif32` release 54.03.20 → Arduino-ESP32 3.2.0 on ESP-IDF 5.4), because the official espressif32 platform doesn't ship Arduino core 3.x, which the ESP32-C6 needs. Board definition: `esp32-c6-devkitc-1`. `ARDUINO_USB_CDC_ON_BOOT=1` maps `Serial` to the native USB port.

The platform's build script runs `python -m pip list` inside PlatformIO's venv. That venv had no pip; it was fixed once with `~/.platformio/penv/bin/python -m ensurepip`. If the build dies with `No module named pip`, run that again.

## Make targets

`make` (no target) shows a numbered menu. `make build`, `make flash`, `make serial`, `make serial_reset`, `make devices`, `make compiledb`, `make clean`, `make setup` (ensurepip + submodule). `PORT` is auto-detected; `SECONDS` sets the serial read time. The raw commands follow.

## Flash

```sh
~/.platformio/penv/bin/pio device list              # board shows as /dev/cu.usbmodem*, 303A:1001
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/cu.usbmodem101
```

If upload can't connect, hold BOOT while plugging in USB, then retry.

## Serial output

`pio device monitor` is interactive and unusable from an agent. Use:

```sh
~/.platformio/penv/bin/python tools/serial-read.py --seconds 6           # just listen
~/.platformio/penv/bin/python tools/serial-read.py --reset --seconds 8   # restart board first
```

The firmware prints only once at startup ("Thermal camera ready" or an error) and on frame read failures. That startup line usually comes before USB re-enumerates after a reset, so silence is normal. For measurements, add a temporary `Serial.printf` in `loop()`, read it, then remove it.

## Editor (clangd)

`pio run -t compiledb` writes `compile_commands.json` (gitignored, machine-specific). `.clangd` strips GCC/RISC-V flags clang rejects. System headers (`stdio.h`) still need clangd's `--query-driver=~/.platformio/packages/toolchain-riscv32-esp/bin/*`, which is an editor setting. Clang diagnostics in this repo are often false; trust `pio run`.

## Gotchas

- macOS `sed` has no `\b`; use `perl -pi -e` for word-boundary replacements.
- `.pio/` holds a cached copy of the vendored lib; `pio run` rebuilds it automatically when `lib/` changes.
