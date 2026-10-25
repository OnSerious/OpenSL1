# OpenSL1 on macOS

## Driver design

`driver/sl1_driver.c` is a CoreAudio **AudioServerPlugIn** (a user-space driver loaded by `coreaudiod`). It publishes
one device while an SL1 is plugged in:

| | |
|---|---|
| Name / manufacturer | “Rane SL1” / “Rane” (model name “SL1 (OpenSL1 driver)”) |
| Device UID | `OpenSL1:13e5:0001:1` – the SL1 has no serial number and only one SL1 is supported, so the UID does not depend on the USB port |
| Model UID | `OpenSL1:13e5:0001` |
| Transport | USB |
| Input stream | 4 ch: Input 1 L/R, Input 2 L/R |
| Output stream | 4 ch: Line 1 L/R, Line 2 L/R |
| Format | 44 100 Hz, 32-bit float (the SL1 itself streams 16-bit) |

- **Hot-plug:** IOKit matching notifications publish the device when the SL1 appears. When it is unplugged while an
  app runs IO (Mixxx keeps its audio running all the time), the device **stays published**: the clock keeps running,
  inputs read silence, and the watchdog reopens the SL1 when it is plugged in again – no need to restart Mixxx. When
  nothing uses it, an unplugged SL1 disappears from CoreAudio until it is back. The USB interfaces are only opened
  while IO is running, so the tools can use the SL1 when no app does.
- **USB:** through `IOUSBHost.framework` (`src/sl1_usb_mac.m`): set configuration 1 if needed, open the streaming interfaces 1/2/4/5/7,
  select alt 1, schedule isochronous requests of 2 packets at explicit bus frame numbers: 12 requests in flight per
  input (24 ms of slack, no extra latency), 6 per output.
- **Sandbox:** `coreaudiod` only lets plug-ins open the IOKit user clients listed in `Info.plist` under
  `AudioServerPlugIn_IOKitUserClients`: `AppleUSBHostFrameworkDeviceClient` and
  `AppleUSBHostFrameworkInterfaceClient` (class names measured with `ioreg` while the device was open).
- **Clock:** the device sample counter is the number of frames received on Input 1. Bus frames map to host time with
  the nominal slope (1 ms) and an offset that follows the lower envelope of `frameNumberWithTime:` pairs (each pair is
  0–1 ms late because the frame number is whole), smoothed. Together they give the zero timestamps (period 16 384
  frames). Missing packets and skipped bus frames are filled with silence so the counter never falls behind the
  clock. Output packets are sized from the same counter, so the HAL, the input and the output never drift apart.
  Measured over 60 s: 44 100.22 Hz, timestamp step error ≤ 27 µs.
- **Buffers:** both directions use rings indexed by device sample time. Safety offsets: input 265 frames (6 ms),
  output 706 frames (16 ms). Measured headroom over 60 s: input ≥ 170 frames, output ≥ 84 frames, 0 late accesses.
  The driver logs this headroom on every stop; check it after changing the scheduling constants.
- **Not the default device:** `CanBeDefaultDevice` is 0, so macOS never routes system alerts or other apps to the
  SL1 on its own. Apps such as Mixxx select it explicitly.

## Robustness

- **Watchdog:** while IO runs, the driver checks Input 1 every 100 ms. After 300 ms without data (USB glitch,
  re-plug, wake from sleep) it reopens the SL1 and continues on the same timeline, so the app keeps running without
  restarting its audio. Tested with `OPENSL1_TEST_STALL=<packet>` in `hal_harness`: one restart, 0 timestamp errors.
- **Reset after a drop-out:** if the SL1 comes back with its interfaces missing, does not answer
  SET_CONFIGURATION, or (while still on the bus) does not answer SET_INTERFACE, the driver resets (re-enumerates)
  it, at most once every 10 s. Tested with `OPENSL1_TEST_RESET=<n>` (the n-th open treats the interfaces as
  missing) together with `OPENSL1_TEST_STALL`: audio was back about 0.5 s after the reset.
- **No stale input:** frames that have not arrived yet (or were overwritten) read as silence, never as old timecode
  from a previous lap of the ring buffer.
- **No notifications from StartIO/StopIO:** the HAL tracks `DeviceIsRunning` itself. OpenSL3 removed such
  notifications in the release that fixed `coreaudiod` spinning after start/stop cycles; we follow that.
- **OwnedObjects** honours the class-ID qualifier (e.g. a query for controls returns none, not the streams).
- **Open retry:** StartIO retries opening the SL1 three times (for example while another program releases it).
- **Defensive input handling:** packet lengths are clamped to the endpoint size; property calls check object, size
  and pointers; the IO thread never blocks (no locks, bounded seqlock read).
- **Security:** the driver runs inside the `coreaudiod` sandbox; its only extra rights are the two IOUSBHost user
  clients. It opens no files and no network connections. The installer writes root-owned files (`0/0`, 755/644).

## Known issue: the SL1 drops off USB when the Mac's power source changes

The SL1 is bus-powered (300 mA) and dates from USB 1.x. On a MacBook Air (M4, macOS 27) it repeatedly fell off the
bus when the charger was plugged in or unplugged, sometimes only 5–21 s later while running on battery. Observed
with a passive USB-A to USB-C adapter (25 Oct 2026):

- With audio running, most drop-outs heal by themselves: the SL1 comes back, the driver resets it and the app keeps
  playing. Silence was at most about 3 s; Mixxx needed no click.
- Occasionally the SL1 hangs: it no longer answers control requests and does not come back after a reset. Only
  unplugging and replugging it helps.
- After a drop-out macOS may ask whether to allow the accessory. While that prompt is open, or even after allowing,
  the SL1 may stay incomplete until it is replugged.

What we can and cannot see: the system did not sleep, Low Power Mode was off, and audio held a sleep assertion. macOS
exposes no voltage or current for the port (only its 3 A limit). Why the port drops the SL1 – power-role
renegotiation, a voltage dip, or port power management on battery – is unconfirmed.

Recommendations for a gig:

- Plug the charger in before the set and do not touch it during the set.
- Prefer a direct USB-C to USB-B cable over a USB-A to USB-C adapter; a loose or marginal adapter is one more
  contact that can drop the device (untested).
- A powered USB hub between the Mac and the SL1 may isolate it from the port's power changes (untested).
- If the SL1 is missing when the app starts, the app may forget its device setup. Connect the SL1 first.

## Known issue: libusb causes a kernel panic

On macOS 27.0.1 (26A434, Apple M4), `sl1rec` built with libusb 1.0.30 panicked the kernel 1.5 s after start:
“Kernel data abort” at address 0x3 in `com.apple.iokit.IOUSBHostFamily`. The user-space stack, resolved with `atos`,
was:

```
libusb_set_interface_alt_setting → darwin_set_interface_altsetting → get_endpoints
  → darwin_get_pipe_properties → IOKit → kernel
```

So it happened while switching interface 2 to alt 1 and reading the pipe properties through the legacy IOUSBLib
interface. A user process must never be able to panic the kernel, so this is a macOS bug. The SL1's unusual
descriptors (vendor class with UAC1 descriptors, 9-byte endpoint descriptors) are a plausible trigger but this is
unconfirmed. The IOUSBHost path used by OpenSL1 has not shown the problem. Therefore the Makefile uses IOUSBHost on
macOS; do not force `USB_BACKEND=libusb` there. `sl1probe` only reads descriptors through libusb and is safe.

## Debugging

- While IO runs the driver logs a statistics line every 30 s and on every stop (packets, errors, restarts,
  headroom): `/usr/bin/log show --last 2h --predicate 'subsystem == "org.opensl1.driver"'`. After a gig this shows
  whether anything went wrong. (In zsh, `log` is a shell builtin – use `/usr/bin/log`.)

- `OPENSL1_TRACE=/path/file build/sl1rec …` writes every USB setup step to the file with `fsync`, so the last step
  survives a kernel panic.
- `build/hal_harness` loads the driver bundle in-process (no install); `build/ca_check` tests the installed driver
  through the CoreAudio API and can dump the input with `-r file.f32`.
