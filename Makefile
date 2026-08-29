# Device parameters
DEVICE ?= tanmatsu
PORT ?= /dev/ttyACM0

# Build parameters
IDF_VERSION ?= v6.0.2
BUILD ?= build/$(DEVICE)
SDKCONFIG_DEFAULTS ?= sdkconfigs/general;sdkconfigs/$(DEVICE)
SDKCONFIG ?= sdkconfig_$(DEVICE)

# Application metadata (see metadata/metadata.json)
APP_SLUG ?= com.annejan.ssh
APP_TITLE ?= SSH
APP_REVISION ?= 1
APP_DIR ?= /int/apps/$(APP_SLUG)

# SDK
IDF_PATH ?= $(shell if [ -f .IDF_PATH ]; then cat .IDF_PATH; elif [ -d "`pwd`/esp-idf" ]; then echo "`pwd`/esp-idf"; else echo '$(HOME)/.espressif/$(IDF_VERSION)/esp-idf'; fi)
IDF_TOOLS_PATH ?= $(shell if [ -f .IDF_TOOLS_PATH ]; then cat .IDF_TOOLS_PATH; elif [ -d "`pwd`/esp-idf-tools" ]; then echo "`pwd`/esp-idf-tools"; else echo '$(HOME)/.espressif/tools'; fi)
IDF_SOURCE ?= $(IDF_PATH)/export.sh
IDF_EXPORT_QUIET ?= 1
IDF_GITHUB_ASSETS ?= dl.espressif.com/github_assets
IDF_INSTALL_PATH ?= $(shell echo `pwd`/esp-idf)

MAKEFLAGS += --silent
SHELL := /usr/bin/env bash

# Every supported device is an ESP32-P4 board
IDF_TARGET ?= esp32p4

IDF_PARAMS := -B $(BUILD) -DDEVICE=$(DEVICE) -DSDKCONFIG_DEFAULTS="$(SDKCONFIG_DEFAULTS)" -DSDKCONFIG=$(SDKCONFIG) -DIDF_TARGET=$(IDF_TARGET)

export IDF_TOOLS_PATH
export IDF_GITHUB_ASSETS

.PHONY: all
all: build

# Preparation

.PHONY: prepare
prepare: submodules

# Nothing to do when the sources are already there, which is the case after a
# clone with --recursive or a CI checkout that fetched submodules itself.
.PHONY: submodules
submodules:
	@if [ ! -f components/libssh2/libssh2/src/session.c ]; then \
		git submodule update --init --recursive; \
	fi

.PHONY: sdk
sdk:
	if test -d "$(IDF_INSTALL_PATH)"; then echo "ESP-IDF target folder exists, remove it first."; exit 1; fi
	git clone --recursive --branch "$(IDF_VERSION)" https://github.com/espressif/esp-idf.git "$(IDF_INSTALL_PATH)" --depth=1 --shallow-submodules
	cd "$(IDF_INSTALL_PATH)"; bash install.sh all

.PHONY: check-sdk
check-sdk:
	@if test -d $(IDF_PATH); then \
		printf '%s\n' "ESP-IDF found at $(IDF_PATH)"; \
	else \
		printf 'ESP-IDF SDK not found. Run "make sdk", or point .IDF_PATH at an existing %s checkout.\n' "$(IDF_VERSION)" >&2; \
		exit 1; \
	fi

.PHONY: menuconfig
menuconfig:
	source "$(IDF_SOURCE)" && idf.py menuconfig $(IDF_PARAMS)

# Building

.PHONY: build
build: check-sdk submodules
	source "$(IDF_SOURCE)" >/dev/null && idf.py $(IDF_PARAMS) build

.PHONY: reconfigure
reconfigure: check-sdk
	source "$(IDF_SOURCE)" >/dev/null && idf.py $(IDF_PARAMS) reconfigure

.PHONY: clean
clean:
	rm -rf $(BUILD)
	rm -rf managed_components
	rm -f sdkconfig_*

.PHONY: fullclean
fullclean: clean
	rm -rf build
	rm -f sdkconfig sdkconfig.old

# Hardware

.PHONY: flash
flash: build
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) flash -p $(PORT)

.PHONY: flashmonitor
flashmonitor: build
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) flash -p $(PORT) monitor

.PHONY: monitor
monitor:
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) monitor -p $(PORT)

.PHONY: erase
erase:
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) erase-flash -p $(PORT)

.PHONY: size
size:
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) size

# Badgelink: install the app into the badge's app filesystem and start it

.PHONY: badgelink
badgelink:
	rm -rf badgelink
	git clone https://github.com/badgeteam/esp32-component-badgelink.git badgelink
	cd badgelink/tools; ./install.sh

.PHONY: install
install: build install-metadata
	cd badgelink/tools; ./badgelink.sh appfs upload $(APP_SLUG) "$(APP_TITLE)" $(APP_REVISION) ../../$(BUILD)/application.bin

.PHONY: install-metadata
install-metadata:
	cd badgelink/tools; \
	./badgelink.sh fs mkdir $(APP_DIR) || true; \
	./badgelink.sh fs upload $(APP_DIR)/metadata.json ../../metadata/metadata.json; \
	./badgelink.sh fs upload $(APP_DIR)/icon16.png ../../metadata/icon16.png; \
	./badgelink.sh fs upload $(APP_DIR)/icon32.png ../../metadata/icon32.png; \
	./badgelink.sh fs upload $(APP_DIR)/icon64.png ../../metadata/icon64.png

.PHONY: run
run:
	cd badgelink/tools; ./badgelink.sh start $(APP_SLUG)

# Icons: rasterised from metadata/icon.svg. The PNGs are checked in, and this
# is not part of the build: git does not preserve modification times, so on a
# fresh clone make cannot tell whether the SVG is newer than the PNGs, and it
# would reach for an SVG renderer that a build container has no reason to have.
# Run it by hand after editing the icon.
.PHONY: icons
icons:
	tools/render_icon.sh metadata/icon.svg metadata/icon16.png 16
	tools/render_icon.sh metadata/icon.svg metadata/icon32.png 32
	tools/render_icon.sh metadata/icon.svg metadata/icon64.png 64

# Formatting

.PHONY: format
format:
	find main/ components/libssh2/port -iname '*.h' -o -iname '*.c' | xargs clang-format -i
