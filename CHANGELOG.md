<div align="center">

# 📜 Changelog

All notable changes to OpenSL1. Format inspired by [Keep a Changelog](https://keepachangelog.com/).

[← README](README.md) · [Releases](https://github.com/OnSerious/OpenSL1/releases)

</div>

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
| An app started while the SL1 is missing does not pick it up by itself | Connect the SL1 first, or click *Apply* in Mixxx's sound settings |
| Mixxx 2.6.0-beta can freeze when sound settings are applied with the SL1 | Set up sound before the set; under investigation |
| Microphone input not exposed; only 44.1 kHz | – |
| Packages are ad-hoc signed only (not notarized) | *System Settings → Privacy & Security → Open Anyway* |
