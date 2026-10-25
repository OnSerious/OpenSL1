<div align="center">

# 📜 Changelog

All notable changes to OpenSL1. Format inspired by [Keep a Changelog](https://keepachangelog.com/).

[← README](README.md) · [Releases](https://github.com/OnSerious/OpenSL1/releases)

</div>

---

## 🚀 [0.1.2](https://github.com/OnSerious/OpenSL1/releases/tag/v0.1.2) – 2026-10-25

**Clearer installer, ruhigeres Log – no change to the sound.**

- **Installer pages (English and German):** unofficial / use-at-your-own-risk notice, which DJ software works, why the
  installer is not notarized, power and charger notes (including the untested idea of the SL1's 9 V supply jack), and
  Mixxx setup tips.

- **Quieter log while the SL1 hangs:** a repeated failure (open, start, watchdog restart) is logged at most once
  every 10 s; when the SL1 is back, the driver reports how many repeats it suppressed.
- Headroom lines show “last at 0.00 s” when there was no late access (a negative time could appear before).
- Test hook `OPENSL1_TEST_OPEN_FAIL=<n>` for `hal_harness`: the first n opens fail.

---

## 🚀 [0.1.1](https://github.com/OnSerious/OpenSL1/releases/tag/v0.1.1) – 2026-10-25

**Mixxx never loses the SL1 any more.**

### ✨ Added

- **“Rane SL1” is always available**, also when the box is unplugged. An app that starts without the SL1 keeps its
  device setup and runs silently on a free-running clock; when the SL1 is plugged in, the driver switches to it on
  the same timeline (measured: sound 0.24–0.34 s after plug-in, 0 timestamp errors). The same applies if the SL1
  cannot be opened at start.
- **Latency reported to apps:** the measured hardware loop of 128 frames (2.9 ms) as 64 frames input + 64 frames
  output.

### 🔧 Changed

- Headroom statistics restart when the SL1 takes over from the free-running clock, so the log shows real operation
  only.
- `ca_loop` also prints how much of the measured loop the driver does not report.
- Docs: the start-up “burst” on the inputs is explained – a ~1.5 s DC settling of the SL1's analog input stage, not
  interference.

### ⚠️ Known issues

- “Rane SL1” is listed in every app and in Audio MIDI Setup even when nothing is plugged in.
- Unchanged from 0.1.0: charger drop-outs, Mixxx 2.6.0-beta freeze when applying sound settings, no microphone
  input, ad-hoc signed packages.

---

## 🚀 [0.1.0](https://github.com/OnSerious/OpenSL1/releases/tag/v0.1.0) – 2026-10-25

**First public release: the Rane SL1 is back on macOS.** Tested with Serato CV02 timecode on both decks in Mixxx,
sleep/wake, unplug/re-plug and charger changes during playback.

### ✨ Added

- **CoreAudio driver** (AudioServerPlugIn): device “Rane SL1” with 4 inputs (Input 1/2) and 4 outputs (Line 1/2) at
  44.1 kHz, hot-plug, clock derived from the SL1's own sample stream. Universal (Apple Silicon + Intel), macOS 12+.
- **IOUSBHost USB backend** for macOS (libusb's macOS path triggered a kernel panic with the SL1 on macOS 27).
- **Survives unplugging:** the device stays published while an app plays; inputs read silence and audio resumes when
  the SL1 is back – in any USB port.
- **Watchdog:** reopens the SL1 after 300 ms without data and continues on the same timeline.
- **Bus reset:** if the SL1 comes back incomplete or stops answering after a drop-out (seen when the Mac's charger
  was plugged in or out), the driver re-enumerates it instead of retrying forever.
- **Diagnostics in the macOS log** every 30 s: packets, USB errors, headroom, input levels, L/R correlation, gaps –
  plus power-source changes, so drop-outs can be matched to the charger.
- **Installer and uninstaller** packages (`make pkg`), English with German localisation.
- **Tools** `sl1probe`, `sl1rec`, `sl1tone`; **test programs** `hal_harness`, `ca_check` and `ca_loop`
  (round-trip latency).
- **Docs:** protocol notes from our own measurements, macOS driver notes, development guide, README in English and
  German.

### 🔧 Changed during testing

- Built for macOS 12 and later (the binary first required the build machine's macOS).
- Stable device UID `OpenSL1:13e5:0001:1`, independent of the USB port, so Mixxx keeps its setup after a port change.
- Never the system default device.
- Safety offsets tuned on the real audio service: input 9 ms, output 12 ms.

### ⚠️ Known issues

| Issue | Workaround |
|---|---|
| The SL1 can drop off USB when the Mac's power source changes; occasionally it must be replugged | Plug the charger in before the set |
| An app started while the SL1 is missing does not pick it up by itself | Fixed in 0.1.1 |
| Mixxx 2.6.0-beta can freeze when sound settings are applied with the SL1 | Set up sound before the set; under investigation |
| Microphone input not exposed; only 44.1 kHz | – |
| Packages are ad-hoc signed only (not notarized) | *System Settings → Privacy & Security → Open Anyway* |
