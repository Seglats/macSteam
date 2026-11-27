# Controller input latency

Started 2026-11-25 (Rasmus): controller input latency in THE FINALS on the SteamDriver tool,
Steam Input off, for every connection (8BitDo Ultimate 2 on its 2.4 GHz dongle, Bluetooth, USB cable).

## The path

```
controller ─ USB / Bluetooth ─▶ macOS HID ─▶ SDL2 (HIDAPI, MFi or IOKit driver)
  └ winedevice.exe: winebus.so's SDL loop ─▶ winebus.sys ─▶ hidclass.sys
      └ game process: xinput / dinput / Windows.Gaming.Input read the HID report ─▶ the game's next frame ─▶ present
```

- `winebus.sys` (`dlls/winebus.sys/bus_sdl.c`, `sdl_bus_wait`) loops on `SDL_WaitEventTimeout(&event, 10)`.
  winedevice.exe has no SDL window, so SDL polls: pump, then `SDL_Delay(1)` (`EVENT_POLL_INTERVAL_MS`).
- XInput, DirectInput and Windows.Gaming.Input all read through hidclass, so timing taken at the HID
  level covers whichever one THE FINALS uses (its DLL loads are not in the tool's Wine log).

## Stages

1. **macOS side, no Wine (`pad_probe.c`).** Runs the runtime's own `libSDL2` exactly as winebus does
   (same hints, init flags, wait loop on a second thread, CFRunLoop on the main thread, x86_64 under
   Rosetta) next to an IOHIDManager that receives the same controller's raw reports with the kernel's
   timestamp. Gives per connection: the SDL driver, the report rate and jitter, and report → SDL event.
2. **Wine side (`macfinals-156`, `wine-pad-latency-trace.patch`).** Trace-class timestamps on the
   `macfinals_pad` channel, QueryPerformanceCounter microseconds, one clock for all processes,
   only for reports that changed: `usb` (winebus.so's Xbox 360 bus, report completed) → `hid`
   (winebus.sys hands it to hidclass) → `xinput` / `dinput` (new state ready in the game process)
   → `game` (the game's first XInputGetState / GetDeviceState that returns it). Which of xinput
   or dinput prints also says how THE FINALS reads the pad (Windows.Gaming.Input goes through
   dinput in Wine). Launch options
   `WINEDEBUG=err+all,fixme-all,+loaddll,+macfinals_pad %command%`, play a minute, quit, then
   `python3 pad_trace.py` (reads THE FINALS' `steamdriver-wine.log`; `--from-end 50` reads only
   its last 50 MB).
3. **End to end.** 240 fps slow-motion video of the button and the screen, controller against mouse
   in the same scene (the mouse path is already tuned, so it is the baseline).

## Running the probe

```sh
clang -arch x86_64 -O2 -I ~/SteamDriver-build/finals/phase-2/clean-deps/include \
  -framework CoreFoundation -framework IOKit -o /tmp/pad_probe pad_probe.c
/tmp/pad_probe 60        # [seconds] [path to libSDL2-2.0.0.dylib]; default: the installed tool's runtime
```

Connect one controller at a time (unplug the dongle when testing Bluetooth), quit Steam or at least
the game so nothing else holds the controller, then tap one face button about 30 times at an
uneven pace, and also hold a stick still for a while and move it for a while (idle and busy report
rates differ on some pads). The summary prints at the time limit or Ctrl-C.

## XInput pads (`xinput_usb_probe.c`)

The 8BitDo's "normal" mode (dongle, and on the cable) is **not HID**: it presents a USB interface of
class 255 / subclass 93 / protocol 1 (Microsoft's Xbox 360 wired protocol), plus HID keyboard and
mouse interfaces. So:

- macOS's HID layer, its GameController framework (`GCController.controllers()` is empty) and
  therefore SDL never see it. **The game gets no controller in this mode today.**
- winebus.sys's own Xbox 360 USB bus (`bus_xbox360.c`, CrossOver's) would read it directly, but it
  accepts only Microsoft's vendor id (`is_xbox_gamepad`, `unixlib.h`) and switches itself off on
  macOS 15 and later (`sequoia_or_later()`, assuming GameController handles wired 360 pads).
- Logitech G HUB's `lghub_agent` opens the same interface; quit it while testing, since two
  readers of one pipe each get only part of the reports.

`xinput_usb_probe` opens the interface as that bus does (IOUSBLib, `ReadPipeAsync` on pipe 1, a
CFRunLoop source) for any vendor. `--init` first sends what Linux's xpad sends to Xbox 360 pads:
the vendor control read `0xC1 0x01 0x0100` (20 bytes) and the player-LED command `01 03 02` on
the OUT pipe. Whether the 8BitDo needs them is open: the cable runs without them got no reports,
but those may have had no movement and G HUB reading; after one `--init` run the dongle sends
without them until replugged.

## macOS Steam holds XInput pads

macOS Steam (`steam_osx`) reads Xbox-protocol USB pads itself, through its bundled SDL3 and
libusb, and opens their interface **exclusively** (`0xe00002c5` for everyone else). It does that
with "Enable Steam Input for Xbox controllers" off, after a Steam restart, and while THE FINALS
runs with Steam Input off (checked 2026-11-25). So `wine-winebus-xinput-any-pad.patch` finds the
pad but cannot open it while Steam runs.

**Fix found:** SDL3's `SDL_HIDAPI_IGNORE_DEVICES` in Steam's environment makes Steam leave the
device alone. Steam on this Mac is started by launchd (NotProton's Steam profile), so
`launchctl setenv SDL_HIDAPI_IGNORE_DEVICES 0x2dc8/0x310b` and a full Steam restart was enough:
no holder on the interface afterwards, and Wine's bus opened it (pipes, start-up read and LED
all succeeded). `launchctl setenv` lasts until reboot; undo with
`launchctl unsetenv SDL_HIDAPI_IGNORE_DEVICES` and restart Steam. Steam then no longer sees that
pad at all (no Big Picture or Guide button with it).

## Results

| Date | Connection | Path | Report interval (median / p95) | Report → SDL (median / p95) |
|---|---|---|---|---|
| 2026-11-25 | 8BitDo Ultimate 2, dongle, Nintendo mode (`057e:2009`) | SDL HIDAPI (Switch Pro) | **40.0 / 40.0 ms (25 Hz) in use and idle** | buttons 1.13 / 1.48 ms, triggers 1.19 / 1.43, sticks 1.20 / 1.46 |
| 2026-11-25 | 8BitDo Ultimate 2, dongle, normal mode (`2dc8:310b`) | XInput USB → winebus Xbox 360 bus (`macfinals-150`) | **1.00 / 1.05 ms (1000 Hz)** in use and idle | – (no SDL in the path) |
| 2026-11-25 | 8BitDo Ultimate 2, Bluetooth LE (`2dc8:6012`) | SDL IOKit, **not a game controller** (no SDL mapping) | 15.0 / 29.9 ms (67 Hz) | – |

**Working in THE FINALS on `macfinals-150` (2026-11-25):** normal mode on the dongle, with Steam
started under `SDL_HIDAPI_IGNORE_DEVICES=0x2dc8/0x310b`; the interface was held by
`winedevice.exe` while the game ran and Rasmus confirmed the game sees the controller.
(`macfinals-150` inverted both sticks' up/down: CrossOver's bus flipped Y, but since Wine 9 winebus
gamepad reports use the XUSB convention, up = positive, and xinput reads them unchanged; measured with
`xinput_state_probe.c` (sticks held up gave -32767). `macfinals-153` keeps Y as the pad sends it.
`macfinals-147` had the same patch with another bug: the interface search was recursive, so the USB
hubs the pad sits behind were registered as pads too; 150 accepts only a device's own interface.)

**Stage 2, first run (2026-11-25, `macfinals-156`, dongle, normal mode, pad connected while the
game was up):** THE FINALS reads the pad through **XInput** (`XINPUT1_4.dll` in `Discovery-d.exe`;
no dinput lines) and polls it from one thread about every millisecond: a new state is read
**0.72 ms median, 1.19 p95, 1.48 p99** after xinput has it. The `usb`/`hid` lines were missing:
Steam's install-script helper started the prefix's Wine session (winedevice.exe) without the
launch options' WINEDEBUG, and the game joined it. The run script now restarts the session before
the game when WINEDEBUG comes from the launch options.

**Stage 2 results (2026-11-25, `macfinals-159`, dongle, normal mode, 30-minute session, 68,000
changed reports, 53,000 game reads of a new state):**

| Step | median | p95 | p99 |
|---|---|---|---|
| USB report completed → handed to hidclass (winedevice.exe) | 0.08 ms | 0.17 | 0.24 |
| USB → new XInput state in the game process | 0.55 ms | 1.01 | 1.41 |
| XInput state → the game's read | 0.72 ms | 1.24 | 1.79 |
| **USB → game read**, while the game reads at ~1 kHz | **1.29 ms** | 1.76 | 2.12 |
| USB → game read, while it reads once a frame | 1.25 ms | 4.65 | 8.00 |

The game reads XInput in two modes: about every 1.1 ms for long stretches (most of the session),
and in others once a frame (8–11 ms apart, so a press waits up to a frame; presumably menus).
Wine's part (USB → XInput state ready) is about 0.5 ms, of which 0.1 ms in winedevice.exe and
0.45 ms hidclass → the game process's xinput thread; the pad's own 1 ms report interval adds
0.5 ms on average before that. What is left is the game: its frame and present (stage 3).

**Controller switched on while the game runs (fixed in `macfinals-159`, 2026-11-25).** In that
first stage-2 run the game's first read of the pad came 67 s after it connected. Wine announces a
new pad the way Windows does (`hotplug_probe.c`: `WM_INPUT_DEVICE_CHANGE`, `DBT_DEVNODES_CHANGED`,
`DBT_DEVICEARRIVAL` for the HID and XInput interfaces), but the last `DBT_DEVNODES_CHANGED` came
before xinput had opened the device, and Unreal checks empty slots only on that message. With
`wine-xinput-hotplug-check.patch`, `hotplug_probe.exe 70 nopoll` (asks XInput only on each
message, like Unreal) shows `0 CONNECTED` on that broadcast, 4 ms before xinput's own notice.

Open: make the Steam hint permanent (launchctl setenv is lost at reboot); the end-to-end
measurement (stage 3) and Wine-side timestamps (stage 2).

Nintendo mode on the dongle also sends almost no stick movement: the left stick's raw value moved
about 6 % of its range and the right stick not at all, and SDL passed on exactly that. Normal mode
gives both sticks their full range (±27,000) and analog triggers (0–255).

The probe times buttons, triggers and sticks separately. On Switch-protocol pads SDL reports ZL/ZR
as trigger axes, not buttons; a trigger counts as pressed past half travel. Stick changes smaller
than 24/4096 are treated as jitter at rest. Report intervals are split into "untouched" and
"in use" (something changed within the last 100 ms), because the 8BitDo slows to 25 Hz when idle.
