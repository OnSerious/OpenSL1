# OpenSL1

*Deutsch: [README.de.md](README.de.md)*

A free driver for the **Rane SL1** (Serato Scratch Live) so the box keeps working with timecode vinyl in
[Mixxx](https://mixxx.org) – above all on Apple Silicon Macs, where no vendor driver exists any more.

OpenSL1 is an independent project and is **not affiliated with, endorsed by or supported by Rane, Serato or
inMusic**. Rane, SL1, Serato and Scratch Live are trademarks of their respective owners and are used here only to
name the hardware this driver works with. It contains no code or
files from Rane or Serato; everything is based on our own measurements of the device (see
[`docs/PROTOCOL.md`](docs/PROTOCOL.md)).

## Status

| Part | State |
|---|---|
| Linux | Works without OpenSL1: the kernel (`snd-usb-audio`, Linux ≥ 6.0) supports the SL1, Mixxx uses it via ALSA |
| macOS CoreAudio driver (`driver/`) | Device “Rane SL1” with 4 inputs / 4 outputs at 44.1 kHz. **Tested with Serato CV02 timecode on both decks in Mixxx** (MacBook Air M4, macOS 27.0.1): 0 USB errors, no gaps in the timecode, sleep/wake and re-plugging during playback recover by themselves |
| macOS USB backend (IOUSBHost, `src/sl1_usb_mac.m`) | Tested with an SL1 on macOS 27: all inputs/outputs, 0 errors |
| Portable core (packet sizing, ring buffer) and libusb backend | Unit tests pass; libusb backend tested with an SL1 on Linux |
| Tools `sl1probe`, `sl1rec`, `sl1tone` | Tested on Linux and macOS: timecode capture, test tone on Line 1/2 |
| Robustness | Watchdog reopens the SL1 after a USB dropout without restarting the app; never becomes the system default output |
| Microphone input | Not exposed (not needed so far) |
| Latency | Hardware loop (USB + converters) 2.9 ms; timecode in → line out about 35 ms with a 5.8 ms buffer |

## Install on macOS

Download `OpenSL1-<version>.pkg` from the releases page (or build it, see below) and double-click it. The installer copies
`OpenSL1.driver` to `/Library/Audio/Plug-Ins/HAL/` and restarts the macOS audio server. Requires macOS 12 or later,
Apple Silicon or Intel.

**Unsigned builds:** release packages are only ad-hoc signed for now (no Apple Developer ID). macOS blocks a package
downloaded from the internet at first; allow it under *System Settings → Privacy & Security → Open Anyway*.

To uninstall, open `OpenSL1-Uninstall-<version>.pkg`, or run `sudo packaging/uninstall.sh`.

### Mixxx setup

1. Plug the SL1 straight into the Mac (no hub if possible) and start Mixxx. When macOS asks whether Mixxx may use the
   microphone, **allow** it – otherwise Mixxx receives no input and no timecode.
2. *Preferences → Sound Hardware*: Sound API **CoreAudio**, sample rate **44100 Hz**.
   - Outputs: *Deck 1* → “Rane SL1” channels 1–2, *Deck 2* → channels 3–4. Leave *Main* and *Headphones* empty when
     you use an external mixer.
   - Inputs: *Vinyl Control 1* → channels 1–2, *Vinyl Control 2* → channels 3–4.
3. *Preferences → Vinyl Control*: *Serato CV02 Vinyl, Side A*, 33⅓ RPM, mode **REL** recommended.
   - For scratching choose the **alpha-beta filter**. With the Kalman filter the track kept running slowly for a
     moment after the record was stopped by hand (Mixxx 2.6.0-beta); with alpha-beta it stops at once and only the
     tempo display jitters a little.
4. After starting Mixxx, wait a few seconds before dropping the needle: right after the stream starts the SL1's
   inputs carry a short burst of noise.
5. Connect the SL1 before starting Mixxx and keep the charger plugged in during a set (see
   [docs/MACOS.md](docs/MACOS.md)).

Do not add a second sound card to the same Mixxx setup – this includes the Mac's built-in speakers as *Main* or
*Headphones*. Mixxx then takes the other device as clock reference and has to match two independent clocks, which
can cause crackles and pitch jumps. Check the Mixxx log for “as output sound device clock reference”.

Stay at 44.1 kHz: the SL1 only runs at 44.1 kHz (32 kHz untested), and Mixxx is reported to read timecode slower at
48 kHz.

### Troubleshooting

- **No “Rane SL1” device:** check the cable, then restart the audio service: `sudo killall coreaudiod`.
- **Unplugged during a set:** keep Mixxx running and plug the SL1 back in; audio resumes by itself. An app that was
  *not* playing when the SL1 was unplugged may only see it again after the app restarts.
- **Silence and a tool is running:** only one program can own the SL1; quit `sl1rec`/`sl1tone` (they are not needed
  to play).
- **Logs:** `/usr/bin/log show --last 1h --predicate 'subsystem == "org.opensl1.driver"'`.
- **Audio apps hang / high CPU after many audio-service restarts:** restart Apple's AirPlay helper with
  `sudo killall -9 AirPlayXPCHelper` (reported by the OpenSL3 project for the same kind of driver).
- **Mixxx 2.6.0-beta hangs when you apply sound settings:** a deadlock inside Mixxx/PortAudio (the settings dialog
  waits for the audio thread while the audio thread waits for the dialog); the driver keeps running. Force-quit and
  restart Mixxx. Change sound settings before a set, or use a stable Mixxx release.
- **The SL1 drops out when the charger is plugged in or out:** see the known issue in
  [docs/MACOS.md](docs/MACOS.md#known-issue-the-sl1-drops-off-usb-when-the-macs-power-source-changes).
- Anything else wrong with the audio service: uninstall with the uninstaller package.

## Build

Needs a C compiler and `make`; on macOS the Xcode Command Line Tools (`xcode-select --install`).

```sh
make            # tools: build/sl1probe, build/sl1rec, build/sl1tone; unit tests: build/test_core
make test       # unit tests, no hardware needed
make driver     # macOS: build/OpenSL1.driver (universal), build/hal_harness, build/ca_check
make pkg        # macOS: build/OpenSL1-<version>.pkg and build/OpenSL1-Uninstall-<version>.pkg
```

- `sl1probe` and the Linux backend use libusb: `brew install libusb pkg-config` or `apt install libusb-1.0-0-dev pkg-config`.
- On macOS `sl1rec`/`sl1tone` use IOUSBHost automatically. **Do not force `USB_BACKEND=libusb` on macOS**: with the
  SL1, libusb's macOS path triggered a kernel panic on macOS 27 (see [`docs/MACOS.md`](docs/MACOS.md)).
- Developer install without the package: `sudo make install-driver` / `sudo make uninstall-driver`.
- Signed release: `make pkg SIGN_ID="Developer ID Application: …" PKG_SIGN_ID="Developer ID Installer: …"`, then
  notarize with `xcrun notarytool`.

## Testing

```sh
build/hal_harness -d 10   # loads the driver in-process and drives it like the HAL (no install, SL1 plugged in)
build/ca_check -d 6       # tests the installed driver through the CoreAudio API, like an app
build/ca_loop -t -50      # round-trip latency; needs a cable Line 1 left → Input 1 left (phono level: stay quiet)
build/sl1probe            # read-only: interfaces, endpoints, formats
build/sl1rec -i A -d 5 -o deckA.wav     # record Input 1 (A, B or mic)
build/sl1tone -o A -f 440 -l -12 -d 5   # test tone on Line 1 (A or B)
```

The tools print statistics at the end (packets, frames per packet, errors, over-/underruns). They need exclusive access
to the SL1, so quit Mixxx first. On Linux, install the udev rule once so they can open the device:

```sh
sudo cp udev/50-opensl1.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

## Layout

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

## Clean-room policy

Contributions must be based on public documentation and on measurements of the hardware (descriptors, USB captures,
audio tests). Do not submit code, binaries or disassembly from Rane/Serato software. OpenSL1 does not make the SL1
pose as other hardware and does not bypass any licence check; it publishes the SL1 under its own identity
(USB `13e5:0001`, model UID `OpenSL1:13e5:0001`).

## Tested with

- Rane SL1 (`13e5:0001`) on a **MacBook Air 13-inch, M4, 2025** (`Mac16,12`, 10 cores, 16 GB) with
  **macOS 27.0.1** (26A434), connected through a passive USB-A to USB-C adapter; Mixxx 2.6.0-beta, Serato CV02
  control vinyl, external Pioneer mixer.
- Linux (without this driver): CachyOS, kernel 7.2, Mixxx 2.5.6.

## Acknowledgements

- [OpenSL3](https://github.com/mrbrainz/OpenSL3) by mrbrainz showed that a user-space AudioServerPlugIn with
  IOUSBHost works for Rane hardware on Apple Silicon. OpenSL1 is written from scratch – no OpenSL3 code is included –
  but several robustness ideas come from its changelog (keeping the device through an unplug, filtering owned
  objects, no notifications from StartIO/StopIO, building for older macOS versions).
- [libusb](https://libusb.info) (LGPL-2.1) is used by the Linux backend and `sl1probe`; it is linked, not included.
- [Mixxx](https://mixxx.org) and its xwax-based vinyl control make all of this useful.

## Licence

GPL-3.0-or-later, see [`LICENSE`](LICENSE). Every source file carries `SPDX-License-Identifier: GPL-3.0-or-later`.
