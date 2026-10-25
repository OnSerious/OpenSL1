# Development

Everything you need to build, test and hack on OpenSL1. For installing and using the driver, see the
[README](../README.md).

## Build

Needs a C compiler and `make` – on macOS the Xcode Command Line Tools (`xcode-select --install`).

```sh
make            # tools: build/sl1probe, build/sl1rec, build/sl1tone; unit tests: build/test_core
make test       # unit tests, no hardware needed
make driver     # build/OpenSL1.driver (universal: arm64 + x86_64, macOS 12+), hal_harness, ca_check, ca_loop
make pkg        # build/OpenSL1-<version>.pkg and build/OpenSL1-Uninstall-<version>.pkg
```

- `sl1probe` uses libusb: `brew install libusb pkg-config`.
- `sl1rec`/`sl1tone` use IOUSBHost on macOS automatically. **Do not force `USB_BACKEND=libusb` on macOS**: with the
  SL1, libusb's macOS path triggered a kernel panic on macOS 27 (see [MACOS.md](MACOS.md)).
- Developer install without the package: `sudo make install-driver` / `sudo make uninstall-driver`.
- Signed release: `make pkg SIGN_ID="Developer ID Application: …" PKG_SIGN_ID="Developer ID Installer: …"`, then
  notarize with `xcrun notarytool`.

## Test tools

```sh
build/hal_harness -d 10   # loads the driver in-process and drives it like the HAL (no install needed)
build/ca_check -d 6       # tests the installed driver through CoreAudio, like an app
build/ca_loop -t -50      # round-trip latency; cable Line 1 left → Input 1 left (phono level: stay quiet!)
build/sl1probe            # read-only: interfaces, endpoints, formats
build/sl1rec -i A -d 5 -o deckA.wav     # record Input 1 (A, B or mic)
build/sl1tone -o A -f 440 -l -12 -d 5   # test tone on Line 1 (A or B)
```

The tools need exclusive access to the SL1 – quit Mixxx first. They print statistics at the end (packets, frames per
packet, errors, over-/underruns).

Test hooks for `hal_harness`: `OPENSL1_TEST_STALL=<packet>` simulates a USB stall, `OPENSL1_TEST_RESET=<n>` makes
the n-th open treat the interfaces as missing (exercises the bus-reset path). `OPENSL1_TRACE=<file>` writes every USB
setup step with `fsync`, so the last step survives a kernel panic.

## Driver log

```sh
/usr/bin/log stream --predicate 'subsystem == "org.opensl1.driver"'      # live
/usr/bin/log show --last 2h --predicate 'subsystem == "org.opensl1.driver"'
```

Every 30 s and on every stop: packets, USB errors, restarts, headroom, input levels and gaps. Power-source changes are
logged too. In zsh `log` is a shell builtin – use `/usr/bin/log`.

## Repository layout

```
src/sl1_proto.h    device constants (from our own measurements)
src/sl1_pkt.*      44.1 kHz → 1 ms packets (44/45 frames)
src/sl1_ring.*     lock-free single-producer/single-consumer ring buffer
src/sl1_usb.*      libusb backend (Linux), shared API in sl1_usb.h
src/sl1_usb_mac.m  IOUSBHost backend (macOS)
src/wav.*          WAV writer for tests
driver/            CoreAudio AudioServerPlugIn + Info.plist
packaging/         installer/uninstaller (pkgbuild/productbuild), uninstall.sh
tools/             sl1probe, sl1rec, sl1tone
tests/             unit tests, hal_harness, ca_check, ca_loop
docs/              PROTOCOL.md, MACOS.md, usb-descriptors.txt
udev/              access rule for Linux
```

Driver internals (clock, watchdog, sandbox, known issues): [MACOS.md](MACOS.md). USB protocol: [PROTOCOL.md](PROTOCOL.md).

## Linux

Linux does not need OpenSL1: since kernel 6.0, `snd-usb-audio` supports the SL1 and Mixxx uses it through ALSA
(`hw:X,0` = Deck A, `hw:X,1` = Deck B). The tools and the libusb backend also build on Linux, which is useful for
measurements:

```sh
sudo apt install libusb-1.0-0-dev pkg-config
make && make test
sudo cp udev/50-opensl1.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

Tested on CachyOS (kernel 7.2) with Mixxx 2.5.6.

## Clean-room rules

Contributions must be based on public documentation and on measurements of the hardware (descriptors, USB captures,
audio tests). **Do not submit code, binaries or disassembly from Rane/Serato software.** OpenSL1 does not make the SL1
pose as other hardware and does not bypass any licence check; it publishes the SL1 under its own identity
(USB `13e5:0001`, model UID `OpenSL1:13e5:0001`). Every source file carries `SPDX-License-Identifier:
GPL-3.0-or-later`.
