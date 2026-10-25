<div align="center">

# 🛠️ Development

**Build, test and hack on OpenSL1.**

<img alt="C11" src="https://img.shields.io/badge/C11-%2B%20Objective--C-555555?style=flat-square&logo=c"> <img alt="make" src="https://img.shields.io/badge/build-make-427819?style=flat-square"> <img alt="Xcode CLT" src="https://img.shields.io/badge/Xcode-Command%20Line%20Tools-147efb?style=flat-square&logo=xcode&logoColor=white">

[← README](../README.md) · [macOS driver](MACOS.md) · [USB protocol](PROTOCOL.md)

</div>

---

<details open>
<summary><b>📑 Contents</b></summary>

- [Build](#-build)
- [Test tools](#-test-tools)
- [Driver log](#-driver-log)
- [Repository layout](#-repository-layout)
- [Linux](#linux)
- [Clean-room rules](#-clean-room-rules)

</details>

> [!NOTE]
> Just want to DJ? You don't need any of this – grab the installer from the
> [latest release](https://github.com/OnSerious/OpenSL1/releases/latest) and follow the [README](../README.md).

## 🔨 Build

Needs a C compiler and `make` – on macOS the Xcode Command Line Tools (`xcode-select --install`).

```bash
make            # tools: build/sl1probe, build/sl1rec, build/sl1tone; unit tests: build/test_core
make test       # unit tests, no hardware needed
make driver     # build/OpenSL1.driver (universal: arm64 + x86_64, macOS 12+), hal_harness, ca_check, ca_loop
make pkg        # build/OpenSL1-<version>.pkg and build/OpenSL1-Uninstall-<version>.pkg
```

| Target | Notes |
|---|---|
| `sl1probe` | uses libusb: `brew install libusb pkg-config` |
| `sl1rec`, `sl1tone` | use IOUSBHost on macOS automatically |
| Developer install | `sudo make install-driver` / `sudo make uninstall-driver` (no package needed) |
| Signed release | `make pkg SIGN_ID="Developer ID Application: …" PKG_SIGN_ID="Developer ID Installer: …"`, then notarize with `xcrun notarytool` |

> [!CAUTION]
> **Do not force `USB_BACKEND=libusb` on macOS.** With the SL1, libusb's macOS path triggered a kernel panic on
> macOS 27 (see [MACOS.md](MACOS.md#known-issue-libusb-causes-a-kernel-panic)).

## 🧪 Test tools

```bash
build/hal_harness -d 10   # loads the driver in-process and drives it like the HAL (no install needed)
build/ca_check -d 6       # tests the installed driver through CoreAudio, like an app
build/ca_loop -t -50      # round-trip latency; cable Line 1 left → Input 1 left
build/sl1probe            # read-only: interfaces, endpoints, formats
build/sl1rec -i A -d 5 -o deckA.wav     # record Input 1 (A, B or mic)
build/sl1tone -o A -f 440 -l -12 -d 5   # test tone on Line 1 (A or B)
```

> [!IMPORTANT]
> The tools need exclusive access to the SL1 – **quit Mixxx first**. A test that grabs the SL1 while Mixxx plays
> makes Mixxx fail to start its audio.

> [!WARNING]
> The SL1's inputs are **phono level** and have no line switch. Keep `ca_loop` quiet (start at `-t -50`).

The tools print statistics at the end (packets, frames per packet, errors, over-/underruns).

<details>
<summary><b>🪝 Test hooks</b></summary>

| Variable | Used by | Effect |
|---|---|---|
| `OPENSL1_TEST_STALL=<packet>` | `hal_harness` | ignores Input 1 for 500 packets from that packet on → exercises the watchdog |
| `OPENSL1_TEST_RESET=<n>` | `hal_harness` | the n-th open treats the interfaces as missing → exercises the bus reset |
| `OPENSL1_TEST_OPEN_FAIL=<n>` | `hal_harness` | the first n opens fail → exercises the free-running clock, the watchdog retries and the log throttling |
| `OPENSL1_TRACE=<file>` | tools | writes every USB setup step with `fsync`, so the last step survives a kernel panic |
</details>

## 🩺 Driver log

```bash
/usr/bin/log stream --predicate 'subsystem == "org.opensl1.driver"'          # live
/usr/bin/log show --last 2h --predicate 'subsystem == "org.opensl1.driver"'  # history
```

Every 30 s and on every stop: packets, USB errors, restarts, headroom, input levels and gaps. Power-source changes
are logged too. In zsh `log` is a shell builtin – use `/usr/bin/log`. What each line means:
[MACOS.md → Debugging](MACOS.md#-debugging).

## 📁 Repository layout

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
docs/              PROTOCOL.md, MACOS.md, DEVELOPMENT.md, usb-descriptors.txt
udev/              access rule for Linux
```

Driver internals (clock, watchdog, sandbox, known issues): [MACOS.md](MACOS.md). USB protocol: [PROTOCOL.md](PROTOCOL.md).

## Linux

> [!TIP]
> Linux does not need OpenSL1: since kernel 6.0, `snd-usb-audio` supports the SL1 and Mixxx uses it through ALSA
> (`hw:X,0` = Deck A, `hw:X,1` = Deck B, `hw:X,2` = mic).

The tools and the libusb backend also build on Linux, which is useful for measurements:

```bash
sudo apt install libusb-1.0-0-dev pkg-config
make && make test
sudo cp udev/50-opensl1.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

Tested on CachyOS (kernel 7.2) with Mixxx 2.5.6.

## 🧼 Clean-room rules

| | Rule |
|:---:|---|
| ✅ | Base contributions on public documentation and measurements of the hardware (descriptors, USB captures, audio tests) |
| 🚫 | Never submit code, binaries or disassembly from Rane/Serato software |
| 🚫 | Never make the SL1 pose as other hardware or bypass a licence check – OpenSL1 publishes it under its own identity (USB `13e5:0001`, model UID `OpenSL1:13e5:0001`) |
| ✅ | Every source file carries `SPDX-License-Identifier: GPL-3.0-or-later` |
