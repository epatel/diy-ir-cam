SHELL := bash

# PlatformIO's own install, not the obsolete pio on PATH (see cards/toolchain.md)
PIO     := $(HOME)/.platformio/penv/bin/pio
PY      := $(HOME)/.platformio/penv/bin/python
PORT    ?= $(firstword $(wildcard /dev/cu.usbmodem*) $(wildcard /dev/ttyACM*))
SECONDS ?= 6

info: menu select

menu:
	echo "1 make build                - build the firmware"
	echo "2 make flash                - build and flash (PORT=$(PORT))"
	echo "3 make serial               - read serial output for SECONDS=$(SECONDS) s"
	echo "4 make serial_reset         - restart the board, then read serial"
	echo "5 make monitor              - interactive serial monitor (Ctrl-C to quit)"
	echo "6 make devices              - list serial ports"
	echo "7 make mockup               - open the screen mockup in a browser"
	echo "8 make compiledb            - refresh compile_commands.json for clangd"
	echo "9 make clean                - remove build output"
	echo "10 make setup               - one-time setup: pip in PlatformIO venv, submodule"
	echo "11 make update_phony        - update .PHONY in Makefile"

select:
	read -p ">>> " P ; "$(MAKE)" --no-print-directory menu | grep "^$$P " | cut -d ' ' -f2-3 | bash

.SILENT:

.PHONY: info menu select build flash serial serial_reset monitor devices mockup compiledb clean setup update_phony

build:
	$(PIO) run

flash:
	$(PIO) run -t upload $(if $(PORT),--upload-port $(PORT))

serial:
	$(PY) tools/serial-read.py --seconds $(SECONDS) $(if $(PORT),--port $(PORT))

serial_reset:
	$(PY) tools/serial-read.py --reset --seconds $(SECONDS) $(if $(PORT),--port $(PORT))

monitor:
	$(PIO) device monitor $(if $(PORT),--port $(PORT))

devices:
	$(PIO) device list

mockup:
	open mockup/screen-mockup.html

compiledb:
	$(PIO) run -t compiledb

clean:
	$(PIO) run -t clean

setup:
	$(PY) -m pip --version >/dev/null 2>&1 || $(PY) -m ensurepip
	git submodule update --init

update_phony:
	echo "##### Updating .PHONY targets #####"
	targets=$$(grep -E '^[a-zA-Z_][a-zA-Z0-9_-]*:' Makefile | grep -v '=' | cut -d: -f1 | tr '\n' ' '); \
	sed -i.bak "s/^\.PHONY:.*/.PHONY: $$targets/" Makefile && \
	echo "Updated .PHONY: $$targets" && \
	rm -f Makefile.bak
