#!/usr/bin/env python3
"""Read the board's USB serial output for a few seconds and print it.

Non-interactive alternative to `pio device monitor`, for scripts and agents.
Run with PlatformIO's Python, which has pyserial:

    ~/.platformio/penv/bin/python tools/serial-read.py [--port P] [--seconds N] [--reset]

--reset pulses RTS to restart the board first, to catch setup() messages.
The ESP32-C6 USB serial re-enumerates on reset, so the port is reopened.
"""
import argparse
import glob
import sys
import time

import serial


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not ports:
        sys.exit("No USB serial port found. Is the board plugged in?")
    return ports[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", default=None)
    ap.add_argument("--seconds", type=float, default=6)
    ap.add_argument("--reset", action="store_true")
    args = ap.parse_args()
    port = args.port or find_port()

    if args.reset:
        s = serial.Serial(port, 115200)
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False
        s.close()
    time.sleep(2)  # let USB serial (re)enumerate

    s = serial.Serial(port, 115200, timeout=0.5)
    end = time.time() + args.seconds
    while time.time() < end:
        data = s.read(512)
        if data:
            sys.stdout.write(data.decode(errors="replace"))
            sys.stdout.flush()


if __name__ == "__main__":
    main()
