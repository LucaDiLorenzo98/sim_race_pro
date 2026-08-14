# Changelog

All notable changes to the SimRacePro Custom Driver Software.
This project uses [semantic-ish versioning](https://semver.org/) — the application, the
base firmware and the wheel firmware always ship on the same version number.

## [3.2.0] — 2026-08-09

First public release.

### Force feedback

- Spring / centring force with configurable start angle, full-lock angle and curve
  linearity.
- Lateral-G force fed from the game's own acceleration telemetry.
- Separate ABS and traction-control torque pulses, driven by the game's ABS/TC flags.
- Surface rumble from suspension travel and velocity — kerbs, gravel, bumps — routed to
  the wheel rumble motor and optionally to the pedal rumble motors.
- All 15 parameters adjustable live from the *Force Feedback Settings* window, applied to
  the base immediately and persisted to `ffb.ini`.

### Telemetry

Full telemetry integration for Assetto Corsa, Assetto Corsa Competizione, iRacing,
rFactor 2 / Le Mans Ultimate, Automobilista 2 / Project CARS 3, RaceRoom Racing
Experience, EA Sports WRC / WRC Generations, Euro Truck Simulator 2 / American Truck
Simulator, F1 2018–F1 25, Forza Motorsport and Horizon 4/5/6, DiRT Rally 1/2 and
DiRT 2–5, GRID Autosport / GRID 2019, and BeamNG.drive.

- The running game is detected automatically and the matching backend switched in.
- Gear numbering is normalised across engines, so `R`/`N`/`1` read the same everywhere.
- Games without a telemetry backend still get spring force and full gamepad support.

### Hardware and firmware

- Binary PC↔base protocol with sync bytes, fixed packet length and a CRC-8 on every
  packet; corrupted packets are dropped rather than accepted with wrong values.
- Base configuration is stored in the Arduino's EEPROM and survives a power cycle with no
  PC attached.
- Sim setup tiers (Full / Medium / Budget) plus per-item toggles for pedal rumble,
  handbrake, clutch, H-pattern shifter and "wheel only".
- Motor tuning: max PWM, separate min-PWM left/right, soft-ramp step, stall protection.
- *Pin Configuration* window remaps seven base peripheral pins (throttle, brake, 2× pedal
  rumble, clutch, shifter X/Y) in software — wiring mistakes no longer need a soldering
  iron.
- Interrupt-safe bit-banged base→wheel transmission, so sending a packet no longer stalls
  the encoder interrupt and drifts the steering zero point.
- Firmware version is transmitted on connect and checked against the software, with a
  dialog naming the board that needs re-flashing.

### Interface

- COM port auto-detection across all ports, USB-serial adapters probed first, with
  automatic reconnect after an unplug.
- Two guided first-run wizards: button wiring (which physical button sits where) and
  button mapping (which Xbox function each one gets). Single buttons are remappable
  afterwards by clicking the wheel image.
- Virtual Xbox 360 gamepad through ViGEmBus, including optional clutch, handbrake and
  H-pattern shifter axes.
- Built-in manual with per-game telemetry setup instructions for every supported title.
- Dependency check on first run — CH340 driver, flashed Arduinos, ViGEmBus — each with a
  download link.
- Wheel OLED shows live gear, speed, RPM bar, throttle/brake and steering angle;
  shift-light LEDs use the game's native upshift point.
- System tray mode, autostart and start-minimised options; window positions are
  remembered.
- Update check against this repository on startup, offering the releases page when a newer
  version is published.
- `-debug` command-line flag for packet-level logging.

### Configuration

Settings live in `%APPDATA%\SimRacePro` — `config.ini` (button mapping), `layout.ini`
(button wiring), `hardware.ini` (hardware and motor setup), `ffb.ini` (force feedback),
`windows.ini` (window positions), `lastversion.ini` (version marker).

Settings survive updates. They are only reset when a release actually changes the on-disk
config format, and the software says so on the first start after such an update.

### Known limitations

- Clutch, handbrake and H-pattern shifter are implemented but have seen limited testing —
  the reference rig does not have them fitted.
- The base→wheel serial receive path blocks the encoder interrupt for roughly 4 ms per
  incoming packet, which can cause slight steering drift while turning. Known and
  accepted; two interrupt-safe receive implementations were tried and rejected.
- The manual shifter has no visual representation in the GUI yet.
