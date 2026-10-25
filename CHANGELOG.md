# Changelog

## 0.1.0 – 2026-10-25

First version of the macOS driver.

- CoreAudio driver (AudioServerPlugIn): device “Rane SL1” with 4 inputs (Input 1/2) and 4 outputs (Line 1/2) at
  44.1 kHz, hot-plug, clock derived from the SL1's own sample stream. Universal (Apple Silicon + Intel), macOS 12+.
- The device stays published through an unplug while an app plays; inputs read silence and audio resumes when the
  SL1 is back. Statistics in the unified log every 30 s.
- Built for macOS 12 and later (the binary previously required the build machine's macOS).
- Watchdog: reopens the SL1 after 300 ms without data and continues on the same timeline.
- If the SL1 comes back incomplete or stops answering after a drop-out (seen when the Mac's charger was plugged in
  or out), the driver resets the device instead of retrying forever.
- Power source changes are logged next to USB events, so drop-outs can be matched to the charger.
- Stable device UID `OpenSL1:13e5:0001:1` (independent of the USB port); never the system default device.
- IOUSBHost USB backend for macOS (libusb's macOS path triggered a kernel panic with the SL1 on macOS 27).
- Installer and uninstaller packages (`make pkg`), English with German localisation.
- Input meter in the log every 30 s (level, peak, clipping, L/R correlation, gaps) to check the timecode signal.
- Output safety offset 12 ms (measured headroom in the real HAL).
- Tools `sl1probe`, `sl1rec`, `sl1tone`; test programs `hal_harness`, `ca_check` and `ca_loop` (round-trip latency).
- Protocol notes from our own measurements (`docs/PROTOCOL.md`).

Tested with Serato CV02 timecode on both decks in Mixxx, sleep/wake, unplug/re-plug and charger changes during
playback.

Known limitations: microphone input not exposed; only 44.1 kHz; packages are ad-hoc signed only; the SL1 can drop
off USB when the Mac's power source changes and occasionally needs to be replugged; an app started while the SL1 is
missing does not pick it up by itself.
