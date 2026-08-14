# SimRacePro Custom Driver Software

Windows driver software for a DIY sim-racing rig — a 3D-printed steering wheel and
pedal base driven by two Arduino Nanos. It reads live telemetry out of the running
game, computes force feedback from it, drives the wheel's FFB motor and rumble
motors, and exposes the whole rig to Windows as a virtual Xbox 360 controller.

**Version 3.2.0** · Windows 10/11 x64 · GPL-3.0

> This is the software half of the project. The hardware (frame, wheel, pedals,
> wiring) is the SimRacePro DIY build — see [Hardware](#hardware) below.

---

## Table of contents

- [What it does](#what-it-does)
- [Supported games](#supported-games)
- [Requirements](#requirements)
- [ViGEmBus (required driver)](#vigembus-required-driver)
- [Installation](#installation)
- [Hardware](#hardware)
- [Firmware](#firmware)
- [Building from source](#building-from-source)
- [Configuration files](#configuration-files)
- [Update check](#update-check)
- [Troubleshooting](#troubleshooting)
- [Third-party components](#third-party-components)
- [License](#license)
- [Credits](#credits)

---

## What it does

**Force feedback computed from real telemetry**

- **Spring / centring force** with a configurable start angle, full-lock angle and
  curve linearity — the wheel resists like a real steering rack instead of a
  constant pull.
- **Lateral-G force** — cornering load is fed into the motor, so the wheel goes
  light when the front end washes out.
- **ABS and traction-control pulses** — separate torque pulses fired from the
  game's own ABS/TC flags.
- **Surface rumble** — kerbs, gravel and bumps from suspension travel/velocity,
  sent to the wheel rumble motor and (optionally) the pedal rumble motors.
- Every effect is tunable live from the *Force Feedback Settings* window and
  saved to disk; nothing is hardcoded.

**Everything else**

- **Auto-detected serial connection** — all COM ports are scanned, USB-serial
  adapters (CH340, CP210x, FTDI, …) first; the link reconnects on its own after
  an unplug.
- **Automatic game detection** — the running game is recognised and the matching
  telemetry backend is switched in without user interaction.
- **Virtual Xbox 360 gamepad** via ViGEmBus — the whole rig (steering, pedals,
  buttons, optional clutch/handbrake/shifter) appears as one standard controller.
- **Two guided first-run wizards** — one learns which physical button sits at which
  wheel position (wiring differs between builds), one assigns Xbox functions to
  them. Single buttons can be remapped later by clicking the wheel image.
- **Shift-light LEDs and OLED display** on the wheel — live gear, speed, RPM bar,
  throttle/brake and steering angle, with game-native upshift points.
- **Pin configuration window** — remap seven base peripheral pins (throttle, brake,
  2× pedal rumble, clutch, shifter X/Y) in software to fix wiring mistakes without
  re-soldering.
- **Hardware profiles** — Full / Medium / Budget sim tiers plus per-item toggles for
  pedal rumble, handbrake, clutch, H-pattern shifter and "wheel only" mode. Stored
  in the base's EEPROM, so they survive a power cycle even with no PC attached.
- **Motor tuning** — max PWM, separate min-PWM left/right (to compensate an
  off-centre H-bridge), soft-ramp step and stall protection.
- **Built-in manual** with a per-game telemetry setup guide for every supported
  title, plus a system-tray mode, autostart and a start-minimised option.

---

## Supported games

Full telemetry and detailed force feedback:

| Game | Telemetry source | Setup needed |
|---|---|---|
| Assetto Corsa · Assetto Corsa Competizione | Shared memory | — |
| iRacing | Shared memory | — |
| rFactor 2 · Le Mans Ultimate | Shared memory | **Plugin** (`rF2SharedMemoryMapPlugin64.dll`) |
| Automobilista 2 · Project CARS 3 | Shared memory | — |
| RaceRoom Racing Experience | Shared memory | — |
| EA Sports WRC · WRC Generations | Shared memory | — |
| Euro Truck Simulator 2 · American Truck Simulator | Shared memory | **Plugin** (`scs-telemetry.dll`) |
| F1 2018 – F1 25 | UDP :20777 | Enable UDP telemetry in-game |
| Forza Motorsport · Forza Horizon 4 / 5 / 6 | UDP :5300 | Enable "Data Out" in-game |
| DiRT Rally 1 / 2 · DiRT 2–5 · GRID Autosport · GRID 2019 | UDP :20777 | XML config edit |
| BeamNG.drive | UDP :4444 (OutGauge) | Enable OutGauge in-game |

Every other game still gets the steering-resistance spring force and full gamepad
functionality — only the telemetry-driven effects are missing.

The exact per-game steps (which file to edit, which checkbox to tick) are in the
software: **main window → Manual**.

---

## Requirements

- Windows 10 or 11, x64
- The **ViGEmBus** driver — see below
- A SimRacePro base connected via USB
- Both Arduino Nanos flashed with the matching firmware (ships with each release)
- CH340 USB-serial driver, *only* if Windows does not pick the board up on its own

---

## ViGEmBus (required driver)

SimRacePro does not write a kernel driver of its own. It creates its virtual Xbox 360
controller through **[ViGEmBus](https://github.com/nefarius/ViGEmBus)** — the Virtual
Gamepad Emulation Bus by [Nefarius Software Solutions e.U.](https://github.com/nefarius)
— talking to it via the companion user-mode library
**[ViGEmClient](https://github.com/nefarius/ViGEmClient)**.

**ViGEmBus must be installed before first use.** Without it the software still starts
and still reads telemetry, but no controller appears and games will not see the rig.

1. Download the latest installer:
   **<https://github.com/nefarius/ViGEmBus/releases/latest>**
2. Run it, and reboot if it asks you to.
3. Verify: with SimRacePro connected, an *Xbox 360 Controller* shows up under
   Windows Settings → Bluetooth & devices → Devices.

ViGEmBus and ViGEmClient are third-party projects, licensed under the MIT License and
maintained independently of SimRacePro. Full credit for the virtual-gamepad layer goes
to them — please report ViGEm issues to their trackers, not here.

---

## Installation

1. Install [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases/latest).
2. Download `SimRacePro-v3.2.0.zip` from the
   [latest release](https://github.com/LucaDiLorenzo98/sim_race_pro/releases/latest)
   and unpack it anywhere. Keep `SimRacePro.exe` and the `Arduino files` folder
   together.
3. Flash both Arduino Nanos with the sketches from `Arduino files` — see
   [Firmware](#firmware).
4. Run `SimRacePro.exe`. The first launch walks you through hardware selection,
   button wiring and button mapping.

`FIRST_RUN_SETUP_GUIDE.txt` in the ZIP is the long-form version of this, step by step.

---

## Hardware

The rig is built around two Arduino Nano (ATmega328) boards:

**Base** — the wheel base and pedal box:

- BTS7960 H-bridge driving the force-feedback motor
- Rotary encoder for the steering angle
- Throttle, brake, and optionally clutch, handbrake and an H-pattern shifter
- Optional pedal rumble motors
- USB serial to the PC at **115200 baud**

**Wheel** — the detachable steering wheel:

- SSD1306 OLED display and shift-light LEDs
- Up to 16 buttons plus a fixed re-centre button
- Wheel rumble motor
- Connected to the base over a **9600 baud** serial link on pins 5/6

The PC↔base protocol is a binary packet format with sync bytes and a CRC-8 on every
packet, so a corrupted packet is discarded instead of silently producing a wrong
steering value.

---

## Firmware

Both sketches live under `Arduino files/` and are flashed with the **Arduino IDE 2.x**
(`Tools → Board: Arduino Nano`, ATmega328P):

| Sketch | Board | Libraries |
|---|---|---|
| `sim_race_pro_box_script/sim_race_pro_box_script.ino` | Base | Encoder (Paul Stoffregen) |
| `sim_race_pro_wheel_script/sim_race_pro_wheel_script.ino` | Wheel | Adafruit GFX, Adafruit SSD1306 |

`Wire`, `EEPROM` and `SoftwareSerial` are built into the Arduino core.

> **Firmware and software versions must match.** Both sketches carry a `FW_VERSION`
> that the software checks on connect; a mismatch raises a dialog naming the board to
> re-flash. As of 3.2.0 all three — application, base firmware and wheel firmware —
> are on `3.2.0`. Re-flash **both** boards after every update, even when a sketch did
> not change functionally.

---

## Building from source

**Requirements:** Visual Studio 2022 (v143 toolset, "Desktop development with C++"),
Windows 10 SDK. No package manager, no external dependencies to fetch — the ViGEmClient
headers and the prebuilt `lib/ViGEmClient.lib` are in the tree.

```cmd
build.cmd Release
```

or directly:

```cmd
msbuild SimRacePro.sln /p:Configuration=Release /p:Platform=x64
```

Output: `x64\Release\SimRacePro.exe`.

> **Build `Release|x64`.** The bundled `lib/ViGEmClient.lib` is a static *release*
> build (`/MT`, `_ITERATOR_DEBUG_LEVEL=0`), so a `Debug|x64` build fails to link with
> LNK2038. To debug, either build a matching debug ViGEmClient yourself or switch the
> Debug configuration to `/MT`.

**Source layout**

| File | Contents |
|---|---|
| `main.cpp` | `WinMain` entry point |
| `SimRacePro.cpp` / `.h` | Backend thread: serial I/O, telemetry readers, FFB maths, ViGEm, config manager |
| `GuiWindow.cpp` | The whole Win32 GUI — main window, FFB settings, hardware settings, pin config, manual, wizards |
| `SimRaceProDefs.h` | Expected firmware versions, config schema version, update-check URLs |
| `resource.h` / `Version.rc` / `SimRacePro.rc` | Version numbers, EXE metadata, icon and wheel bitmap |
| `src/` | Third-party SDK headers (ViGEmClient, iRacing, ACC, RaceRoom, SCS, pCARS/AMS2) |
| `Arduino files/` | Base and wheel firmware sketches |

The app is plain Win32 C++17 — no framework, no runtime redistributable. It runs on two
threads: the GUI thread and a backend thread doing hardware I/O and FFB, sharing a
mutex-and-atomics `GuiState` struct.

**Releasing a new version.** Three version numbers are deliberately separate:

1. `VER_MAJOR/MINOR/PATCH` + `VER_STRING` + `VER_STR` in `resource.h`, plus
   `version.txt` and the header of `FIRST_RUN_SETUP_GUIDE.txt` — bump every release.
2. `EXPECTED_BOX_FW_VERSION` / `EXPECTED_WHEEL_FW_VERSION` in `SimRaceProDefs.h` and
   the matching `FW_VERSION` in both sketches — must never be *lower* than
   `VER_STRING`, so bump them with it.
3. `CONFIG_SCHEMA_VERSION` in `SimRaceProDefs.h` — bump **only** when the on-disk
   config format changes. It is what triggers wiping the user's settings, so a
   gratuitous bump costs every user their button mapping and both wizard runs.

Publish the GitHub release **before** pushing the `version.txt` bump — see
[Update check](#update-check).

---

## Configuration files

Everything lives in `%APPDATA%\SimRacePro`:

| File | Contents |
|---|---|
| `config.ini` | COM port and button mapping (wheel button → Xbox function) |
| `layout.ini` | Button wiring — wheel position → GPIO bit |
| `hardware.ini` | Sim tier, optional hardware flags, motor tuning, base pin mapping |
| `ffb.ini` | All force-feedback parameters |
| `windows.ini` | Window positions |
| `lastversion.ini` | Version marker used for the update notice and schema check |

*Reset all Settings* in the main window clears mapping, wiring, hardware and FFB
settings, and both wizards run again on the next start.

---

## Update check

On startup the software fetches
[`version.txt`](https://raw.githubusercontent.com/LucaDiLorenzo98/sim_race_pro/main/version.txt)
from this repository over HTTPS and compares it numerically against its own version. If
the remote version is higher, it offers to open the releases page. Nothing is downloaded
or installed automatically, and nothing is sent to the server — it is a plain GET of one
file with no identifying data attached.

An unreachable server, a non-200 response or an unparseable body is treated as
"no update" and stays silent.

Because the check reads the file straight off the default branch, **publish the release
first, then push the `version.txt` bump** — otherwise the prompt points users at a
release that does not exist yet.

---

## Troubleshooting

**"Searching for Wheel…" never connects** — Is the base powered and plugged in? Does a
COM port appear in Device Manager (if not: install the CH340 driver)? Is another program
holding the port — most often the Arduino IDE's Serial Monitor?

**"Firmware version mismatch"** — Re-flash the board named in the dialog with the sketch
from the release you are running.

**"No response from the Wheel"** — Check the cable between wheel and base, and the
wheel's power. Steering, pedals and FFB keep working; only the wheel buttons and display
are affected.

**Games ignore the wheel** — Is ViGEmBus installed? An *Xbox 360 Controller* must appear
in Windows Settings → Bluetooth & devices → Devices while SimRacePro is connected.

**Wheel is not centred / pedals show input at rest** — Press the red button at the bottom
centre of the wheel. It re-centres the steering to 0° and re-zeros the pedal rest
positions, and cannot be remapped.

**Diagnostics** — Start with `SimRacePro.exe -debug` for packet-level logging, and use
*Show Console* in the main window for the live log.

---

## Third-party components

| Component | Used for | License |
|---|---|---|
| [ViGEmBus](https://github.com/nefarius/ViGEmBus) + [ViGEmClient](https://github.com/nefarius/ViGEmClient) — Nefarius Software Solutions e.U. | Virtual Xbox 360 gamepad (`src/ViGEmClient.h`, `src/BusShared.h`, `src/Common.h`, `lib/ViGEmClient.lib`) | MIT |
| [scs-sdk-plugin](https://github.com/RenCloud/scs-sdk-plugin) — RenCloud | ETS2/ATS shared-memory layout | MIT |
| [rF2SharedMemoryMapPlugin](https://github.com/TheIronWolfModding/rF2SharedMemoryMapPlugin) — TheIronWolf | rFactor 2 / LMU shared memory | MIT |
| [ACC shared-memory docs](https://github.com/acc-devs/acc-sharedmemory-doc) — Kunos | ACC struct layout | Kunos documentation |
| [r3e-spectator-overlay](https://github.com/sector3studios/r3e-spectator-overlay) — Sector3 Studios | RaceRoom struct layout | Sector3 SDK |
| pCARS2/AMS2 shared memory — Slightly Mad Studios | AMS2 / Project CARS 3 struct layout | SMS SDK |
| iRacing SDK | iRacing shared memory | iRacing SDK terms |
| [Encoder](https://github.com/PaulStoffregen/Encoder) — Paul Stoffregen | Base firmware, steering encoder | MIT |
| [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library) / [SSD1306](https://github.com/adafruit/Adafruit_SSD1306) | Wheel firmware, OLED | BSD |

Each third-party header keeps its original copyright and licence notice in the file.

---

## License

GPL-3.0 — see [LICENSE](LICENSE).

Third-party components listed above remain under their own licences.

---

## Credits

- Wiring schematic and the original SimRacePro hardware project by **LucaDilo**.
- Virtual gamepad layer by **[Nefarius Software Solutions e.U.](https://github.com/nefarius)**
  (ViGEmBus / ViGEmClient).
- Telemetry plugin authors listed under
  [Third-party components](#third-party-components).

> Flashing the SimRacePro firmware replaces the original LucaDilo sketches on both
> Arduinos. To go back, re-flash the original sketches.
