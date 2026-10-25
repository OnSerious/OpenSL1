# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -pthread
USB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
USB_LIBS := $(shell pkg-config --libs libusb-1.0)

CORE := build/sl1_pkt.o build/sl1_ring.o build/wav.o

# Streaming backend: IOUSBHost on macOS (libusb's macOS path panicked the
# kernel with the SL1, see docs/MACOS.md), libusb elsewhere.
ifeq ($(shell uname -s),Darwin)
USB_BACKEND ?= iousbhost
else
USB_BACKEND ?= libusb
endif
ifeq ($(USB_BACKEND),iousbhost)
USB := build/sl1_usb_mac.o
STREAM_LIBS := -framework Foundation -framework IOKit -framework IOUSBHost
else
USB := build/sl1_usb.o
STREAM_LIBS := $(USB_LIBS)
endif
TOOLS := build/sl1probe build/sl1rec build/sl1tone

all: $(TOOLS) build/test_core

build:
	mkdir -p build

build/%.o: src/%.c src/*.h | build
	$(CC) $(CFLAGS) $(USB_CFLAGS) -c -o $@ $<

build/sl1_usb_mac.o: src/sl1_usb_mac.m src/*.h | build
	$(CC) $(CFLAGS) -fobjc-arc -c -o $@ $<

build/sl1probe: tools/sl1probe.c | build
	$(CC) $(CFLAGS) $(USB_CFLAGS) -o $@ $< $(USB_LIBS)

build/sl1rec: tools/sl1rec.c tools/common.h $(CORE) $(USB)
	$(CC) $(CFLAGS) $(USB_CFLAGS) -o $@ $< $(CORE) $(USB) $(STREAM_LIBS) -lm

build/sl1tone: tools/sl1tone.c tools/common.h $(CORE) $(USB)
	$(CC) $(CFLAGS) $(USB_CFLAGS) -o $@ $< $(CORE) $(USB) $(STREAM_LIBS) -lm

# CoreAudio driver bundle (macOS only), universal so one installer fits every Mac.
DRIVER := build/OpenSL1.driver
DRIVER_ARCHS ?= -arch arm64 -arch x86_64 -mmacosx-version-min=12.0
DRIVER_SRC := driver/sl1_driver.c src/sl1_pkt.c src/sl1_ring.c src/sl1_usb_mac.m
DRIVER_LIBS := -framework CoreAudio -framework CoreFoundation -framework Foundation \
	-framework IOKit -framework IOUSBHost
# Optional: SIGN_ID="Developer ID Application: …" and PKG_SIGN_ID="Developer ID Installer: …".
SIGN_ID ?= -
VERSION := $(shell /usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" driver/Info.plist 2>/dev/null)

driver: $(DRIVER) build/hal_harness build/ca_check build/ca_loop

$(DRIVER): $(DRIVER_SRC) driver/Info.plist src/*.h
	rm -rf $@ && mkdir -p $@/Contents/MacOS
	cp driver/Info.plist $@/Contents/Info.plist
	$(CC) $(CFLAGS) $(DRIVER_ARCHS) -fobjc-arc -bundle -o $@/Contents/MacOS/OpenSL1 $(DRIVER_SRC) \
		$(DRIVER_LIBS) -lm
	rm -rf build/OpenSL1.dSYM && mv $@/Contents/MacOS/OpenSL1.dSYM build/OpenSL1.dSYM 2>/dev/null || true
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" $@

# Installer and uninstaller packages: build/OpenSL1-<version>.pkg, build/OpenSL1-Uninstall-<version>.pkg
PKG := build/OpenSL1-$(VERSION).pkg
UNPKG := build/OpenSL1-Uninstall-$(VERSION).pkg
PKG_SIGN := $(if $(PKG_SIGN_ID),--sign "$(PKG_SIGN_ID)",)

pkg: $(PKG) $(UNPKG)

$(PKG): $(DRIVER) packaging/distribution.xml packaging/scripts/* packaging/resources/*/* LICENSE
	rm -rf build/pkgroot build/pkgres && mkdir -p build/pkgroot build/pkgres
	cp -R $(DRIVER) build/pkgroot/
	cp -R packaging/resources/ build/pkgres/
	for l in build/pkgres/*.lproj; do cp LICENSE $$l/license.txt; done
	# Never relocate (write to /Library, not to a copy found elsewhere); always replace, even at the same version.
	pkgbuild --analyze --root build/pkgroot build/components.plist
	plutil -replace 0.BundleIsRelocatable -bool NO build/components.plist
	plutil -replace 0.BundleIsVersionChecked -bool NO build/components.plist
	pkgbuild --root build/pkgroot --install-location /Library/Audio/Plug-Ins/HAL \
		--component-plist build/components.plist \
		--identifier org.opensl1.driver --version $(VERSION) --scripts packaging/scripts \
		build/OpenSL1-component.pkg
	sed 's/@VERSION@/$(VERSION)/' packaging/distribution.xml > build/distribution.xml
	productbuild --distribution build/distribution.xml --resources build/pkgres \
		--package-path build $(PKG_SIGN) $@

$(UNPKG): packaging/uninstall-distribution.xml packaging/uninstall-scripts/* packaging/resources/*/* | build
	rm -rf build/unpkgres && mkdir -p build/unpkgres && cp -R packaging/resources/ build/unpkgres/
	pkgbuild --nopayload --identifier org.opensl1.uninstall --version $(VERSION) \
		--scripts packaging/uninstall-scripts build/OpenSL1-Uninstall-component.pkg
	sed 's/@VERSION@/$(VERSION)/' packaging/uninstall-distribution.xml > build/uninstall-distribution.xml
	productbuild --distribution build/uninstall-distribution.xml --resources build/unpkgres \
		--package-path build $(PKG_SIGN) $@

HAL_DIR := /Library/Audio/Plug-Ins/HAL

# Needs sudo; coreaudiod restarts and loads the driver.
install-driver: $(DRIVER)
	rm -rf $(HAL_DIR)/OpenSL1.driver
	cp -R $(DRIVER) $(HAL_DIR)/
	killall coreaudiod

uninstall-driver:
	rm -rf $(HAL_DIR)/OpenSL1.driver
	killall coreaudiod

build/ca_check: tests/ca_check.c | build
	$(CC) $(CFLAGS) -o $@ $< -framework CoreAudio -framework CoreFoundation -lm

build/ca_loop: tests/ca_loop.c | build
	$(CC) $(CFLAGS) -o $@ $< -framework CoreAudio -framework CoreFoundation -lm

build/hal_harness: tests/hal_harness.c | build
	$(CC) $(CFLAGS) -o $@ $< -framework CoreAudio -framework CoreFoundation -lm

build/test_core: tests/test_core.c $(CORE)
	$(CC) $(CFLAGS) -o $@ $< $(CORE)

test: build/test_core
	./build/test_core

clean:
	rm -rf build

.PHONY: all test clean driver pkg install-driver uninstall-driver
