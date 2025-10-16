import serial, threading, time, re, sys
import vgamepad as vg
from dataclasses import dataclass
from typing import Optional
from telemetry_sources import TelemetryFrame, F1TelemetryReader, ACCTelemetryReader

VERSION = "1.4.0"
print(f"SIM RACE BOX ver. {VERSION}", flush=True)

# =========================================================
# Configuration
# =========================================================
SERIAL_PORT = 'COM16'
BAUD_RATE = 115200
DEBUG_SERIAL_LOGS = True
DEBUG_RAW_GXGY = False
SEND_TELEMETRY = True          # Enable serial telemetry output
TX_RATE_HZ = 20                # Frequency of telemetry transmission (20 Hz = every 50ms)

# Module toggles
HANDBRAKE_ENABLED = False
MANUAL_TX_ENABLED = False

ANGLE_MIN = -450.0
ANGLE_MAX = 450.0
ANGLE_DEADZONE_DEG = 0.5
STEER_GAIN = 3.5
KEYBOARD_SIM_ENABLED = True

SELECTED_GAME = "F1"   # or "ACC"

# Manual transmission thresholds (0..255)
GEAR_Y_MAP = {
    "up_max": 125,
    "down_min": 140
}
INVERT_GX = True
X_RIGHT_MAX = 104
X_CENTER_MIN = 110
X_CENTER_MAX = 132
X_LEFT_MIN = 138

# Keyboard
try:
    import keyboard as kb
    _kb_ok = True
except Exception as e:
    print(f"[WARNING] Keyboard module not available or lacks permissions: {e}", flush=True)
    _kb_ok = False

# =========================================================
# Gamepad helpers
# =========================================================
gamepad = None
def _log(msg): print(msg, flush=True)

def create_gamepad():
    global gamepad
    try:
        gamepad = vg.VX360Gamepad()
        gamepad.update()
        _log("[GP] Virtual gamepad ready.")
    except Exception as e:
        _log(f"[ERROR] Could not create virtual gamepad: {e}")
        sys.exit(1)

create_gamepad()

# Button mapping (kept from your version)
button_map = {
    0: vg.XUSB_BUTTON.XUSB_GAMEPAD_START,
    1: vg.XUSB_BUTTON.XUSB_GAMEPAD_A,
    5: vg.XUSB_BUTTON.XUSB_GAMEPAD_X,
    6: vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_RIGHT,
    7: vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_LEFT,
    9: vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_UP,
    10: vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_DOWN
}

# =========================================================
# Serial initialization
# =========================================================
try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    _log(f"[INFO] Serial open on {SERIAL_PORT} @ {BAUD_RATE}.")
except Exception as e:
    print(f"[ERROR] Unable to open serial port: {e}", flush=True)
    sys.exit(1)

# =========================================================
# Shared state
# =========================================================
last_throttle_val = 0
last_brake_val = 0
last_angle = 0.0
last_hb_bit = 0
last_gear_idx = 0
gear_key_map = {1:'1', 2:'2', 3:'3', 4:'4', 5:'5', 6:'6'}

# =========================================================
# Helper functions
# =========================================================
def clamp(v, lo, hi): return lo if v < lo else hi if v > hi else v

def update_gamepad(throttle=None, brake=None, steer_angle=None):
    """Updates the virtual Xbox controller state."""
    if gamepad is None:
        return

    # Steering axis
    if steer_angle is not None:
        ax_raw = 0.0 if abs(steer_angle) < ANGLE_DEADZONE_DEG else steer_angle
        ax = clamp(ax_raw * STEER_GAIN, ANGLE_MIN, ANGLE_MAX)
        norm = (ax - ANGLE_MIN) / (ANGLE_MAX - ANGLE_MIN)
        x_val = int(norm * 65535) - 32768
        x_val = clamp(x_val, -32768, 32767)
        gamepad.left_joystick(x_value=x_val, y_value=0)
        gamepad.update()

    # Throttle
    if throttle is not None:
        th = clamp(int(throttle), 0, 255)
        gamepad.right_trigger(value=th)
        gamepad.update()

    # Brake
    if brake is not None:
        br = clamp(int(brake), 0, 255)
        gamepad.left_trigger(value=br)
        gamepad.update()

def press_instant_buttons(buttons, hold_s=0.08):
    """Presses and releases Xbox buttons quickly."""
    if not buttons or gamepad is None: return
    _log(f"[GP] Pressing {len(buttons)} gamepad button(s).")
    for btn in buttons:
        gamepad.press_button(button=btn)
    gamepad.update()
    time.sleep(hold_s)
    for btn in buttons:
        gamepad.release_button(button=btn)
    gamepad.update()

def kb_press(keyname):
    """Simulates a keyboard key press (if enabled)."""
    if KEYBOARD_SIM_ENABLED and _kb_ok:
        try:
            kb.press(keyname)
            _log(f"[KB] Pressed key {keyname}")
        except Exception as e:
            _log(f"[KB] Press {keyname} failed: {e}")
    else:
        _log(f"[KB] (simulated) Would press {keyname}")

def handle_handbrake(hb_bit):
    """Triggers handbrake if enabled."""
    global last_hb_bit
    if not HANDBRAKE_ENABLED:
        return
    if hb_bit == 1 and last_hb_bit == 0:
        kb_press('space')
    last_hb_bit = hb_bit

def gear_from_gx_gy(gx, gy):
    """
    Determines the current gear position from gyroscope / accelerometer
    values (gx, gy). Used for H-pattern shifter logic.
    Returns (gear_index, row, column):
      - gear_index: int (0 for neutral)
      - row: "up" | "down" | "mid"
      - column: "left" | "center" | "right" | "mid"
    """
    if not MANUAL_TX_ENABLED:
        return 0, "off", "off"

    # Determine shift row
    if gy <= GEAR_Y_MAP["up_max"]:
        row = "up"
    elif gy >= GEAR_Y_MAP["down_min"]:
        row = "down"
    else:
        return 0, "mid", "mid"

    # Determine column depending on inversion
    if INVERT_GX:
        if gx <= X_RIGHT_MAX:
            col = "right"
        elif X_CENTER_MIN <= gx <= X_CENTER_MAX:
            col = "center"
        elif gx >= X_LEFT_MIN:
            col = "left"
        else:
            return 0, row, "mid"
    else:
        if gx >= X_LEFT_MIN:
            col = "left"
        elif X_CENTER_MIN <= gx <= X_CENTER_MAX:
            col = "center"
        elif gx <= X_RIGHT_MAX:
            col = "right"
        else:
            return 0, row, "mid"

    # Determine gear number based on position grid
    if col == "left":   return (1 if row == "up" else 2), row, col
    if col == "center": return (3 if row == "up" else 4), row, col
    if col == "right":  return (5 if row == "up" else 6), row, col
    return 0, row, col


_last_raw_print = 0.0
def maybe_log_raw_gxy(gx, gy, interval_s=0.1):
    """
    Prints raw GX / GY values periodically for debugging.
    Controlled by DEBUG_RAW_GXGY flag.
    """
    if not DEBUG_RAW_GXGY:
        return
    global _last_raw_print
    now = time.time()
    if now - _last_raw_print >= interval_s:
        _last_raw_print = now
        _log(f"[RAW] gx={gx} gy={gy}")


# =========================================================
# ❶ Telemetry "mask" class + helper methods
# =========================================================
@dataclass(slots=True)
class TelemetryPacket:
    gx: float = 0.0
    gy: float = 0.0
    gz: float = 0.0
    yaw: float = 0.0
    pitch: float = 0.0
    roll: float = 0.0
    speed: float = 0.0
    gear: int = 0
    rpm: int = 0
    oncurb: int = 0
    curbside: int = 0
    rumble: int = 0
    pwm_sx: int = 0
    pwm_dx: int = 0

def fill_telemetry_packet(pkt: TelemetryPacket,
                          *,
                          frame: Optional[object] = None,
                          overrides: Optional[dict] = None) -> TelemetryPacket:
    """
    Populates a TelemetryPacket either from:
      - a unified telemetry 'frame' (optional, e.g., F1 or ACC adapter)
      - a dict of 'overrides' with direct values
    Any missing fields remain unchanged.
    """
    if frame is not None:
        pkt.gx = float(getattr(frame, "g_lat", pkt.gx) or pkt.gx)
        pkt.gy = float(getattr(frame, "g_lon", pkt.gy) or pkt.gy)
        pkt.gz = float(getattr(frame, "g_vert", pkt.gz) or pkt.gz)
        pkt.speed = float(getattr(frame, "speed_kmh", pkt.speed) or pkt.speed)
        pkt.gear = int(getattr(frame, "gear", pkt.gear) or pkt.gear)
        pkt.rpm = int(getattr(frame, "rpm", pkt.rpm) or pkt.rpm)
        on_curb = getattr(frame, "on_curb", None)
        pkt.oncurb = 1 if on_curb else 0
        side = (getattr(frame, "curb_side", None) or "center").lower()
        pkt.curbside = -1 if side.startswith("l") else (1 if side.startswith("r") else 0)

    if overrides:
        for k, v in overrides.items():
            if hasattr(pkt, k):
                setattr(pkt, k, v)

    pkt.rumble = max(0, min(255, int(pkt.rumble)))
    pkt.pwm_sx = max(0, min(255, int(pkt.pwm_sx)))
    pkt.pwm_dx = max(0, min(255, int(pkt.pwm_dx)))
    pkt.oncurb = 1 if pkt.oncurb else 0
    pkt.curbside = -1 if pkt.curbside < 0 else (1 if pkt.curbside > 0 else 0)
    return pkt

def build_serial_line(pkt: TelemetryPacket) -> str:
    """
    Builds a line string formatted as:
    gx-gy-gz-yaw-pitch-roll-speed-gear-rpm-oncurb-curbside-rumble-pwmsx-pwmdx\n
    """
    parts = [
        f"{pkt.gx:.3f}", f"{pkt.gy:.3f}", f"{pkt.gz:.3f}",
        f"{pkt.yaw:.3f}", f"{pkt.pitch:.3f}", f"{pkt.roll:.3f}",
        f"{int(round(pkt.speed))}", f"{int(pkt.gear)}", f"{int(pkt.rpm)}",
        f"{int(pkt.oncurb)}", f"{int(pkt.curbside)}",
        f"{int(pkt.rumble)}", f"{int(pkt.pwm_sx)}", f"{int(pkt.pwm_dx)}"
    ]
    return "-".join(parts) + "\n"

def send_telemetry(ser_obj: serial.Serial, pkt: TelemetryPacket):
    """Encodes and writes the telemetry packet through the serial port."""
    try:
        line = build_serial_line(pkt)
        ser_obj.write(line.encode("ascii"))
    except Exception as e:
        _log(f"[WARN] send_telemetry error: {e}")

# =========================================================
# Serial reader (unchanged)
# =========================================================
def serial_reader():
    global last_throttle_val, last_brake_val, last_angle, last_gear_idx
    _log("[INFO] Serial reader active.")
    pattern = re.compile(r'^\s*([+-]?\d+(?:\.\d+)?)\-(\d+)\-(\d+)\-(.*)\s*$')

    while True:
        try:
            raw = ser.readline().decode('utf-8', errors='ignore').strip()
            if not raw:
                continue
            if DEBUG_SERIAL_LOGS:
                _log(f"[SERIAL] {raw}")

            m = pattern.match(raw)
            if not m:
                continue

            try:
                angle = float(m.group(1))
                acc = int(m.group(2))
                brk = int(m.group(3))
            except ValueError:
                continue

            tail = m.group(4)
            tparts = tail.split('-')
            if len(tparts) < 2:
                continue

            try:
                gx = int(tparts[-2])
                gy = int(tparts[-1])
            except ValueError:
                gx = gy = 0

            mid = tparts[:-2]
            btn_bits = []
            hb_bit = 0
            if mid:
                tmp = []
                for p in mid:
                    try:
                        tmp.append(int(p))
                    except:
                        tmp.append(0)
                hb_bit = tmp[-1] if tmp else 0
                btn_bits = tmp[:-1]

            last_angle = angle
            last_throttle_val = clamp(acc, 0, 255)
            last_brake_val = clamp(brk, 0, 255)
            maybe_log_raw_gxy(gx, gy)

            to_press = []
            for idx, state in enumerate(btn_bits):
                if state == 1 and idx in button_map:
                    to_press.append(button_map[idx])
            if to_press:
                press_instant_buttons(to_press)

            if HANDBRAKE_ENABLED:
                handle_handbrake(1 if hb_bit == 1 else 0)

            if MANUAL_TX_ENABLED:
                gear_idx, row, col = gear_from_gx_gy(clamp(gx,0,255), clamp(gy,0,255))
                if gear_idx != last_gear_idx:
                    if gear_idx in gear_key_map:
                        kb_press(gear_key_map[gear_idx])
                        _log(f"[GEAR] {gear_idx} (row={row}, col={col})")
                    elif gear_idx == 0 and last_gear_idx != 0:
                        _log(f"[GEAR] Neutral (row={row}, col={col})")
                    last_gear_idx = gear_idx

        except Exception as e:
            _log(f"[WARN] Reader error: {e}")
            time.sleep(0.01)

# ---------------------------------------------------------
# Start the serial reader thread (Arduino -> PC inputs)
# ---------------------------------------------------------
t = threading.Thread(target=serial_reader, daemon=True)
t.start()

# ---------------------------------------------------------
# External game telemetry selection
# ---------------------------------------------------------
# Set this somewhere near your config:
# SELECTED_GAME = "F1"  # or "ACC" (you can change it later)

reader = None
try:
    if SELECTED_GAME.upper() == "F1":
        reader = F1TelemetryReader(port=20777)
        reader.start()
        _log("[TEL] F1 24 reader started (UDP 20777).")
    elif SELECTED_GAME.upper() == "ACC":
        reader = ACCTelemetryReader()
        reader.start()
        _log("[TEL] ACC reader started (shared memory).")
    else:
        _log("[TEL] No external telemetry selected; sending zeros.")
except Exception as e:
    _log(f"[TEL] Failed to start telemetry reader: {e}")
    reader = None

# =========================================================
# Main loop
# =========================================================
try:
    last_tx = 0.0
    pkt = TelemetryPacket()  # reusable instance

    while True:
        time.sleep(0.01)

        # Update virtual gamepad from Arduino input
        update_gamepad(
            throttle=last_throttle_val,
            brake=last_brake_val,
            steer_angle=last_angle
        )

        # Periodic telemetry send to Arduino (PC -> Arduino)
        if SEND_TELEMETRY:
            now = time.time()
            if now - last_tx >= (1.0 / TX_RATE_HZ):
                last_tx = now

                frame = None
                if reader is not None:
                    try:
                        # Non-blocking-ish fetch of latest game telemetry
                        frame = reader.read_frame(timeout_s=0.02)
                    except Exception as e:
                        _log(f"[TEL] read_frame error: {e}")
                        frame = None

                if frame is not None:
                    # Fill from real game frame; keep PWM/rumble as you compute them
                    fill_telemetry_packet(pkt, frame=frame, overrides={
                        "pwm_sx": 0,
                        "pwm_dx": 0,
                        "rumble": 0,
                    })
                else:
                    # Fallback: send zeros / placeholders (keeps protocol stable)
                    fill_telemetry_packet(pkt, overrides={
                        "gx": 0.0, "gy": 0.0, "gz": 0.0,
                        "yaw": 0.0, "pitch": 0.0, "roll": 0.0,
                        "speed": 0.0, "gear": 0, "rpm": 0,
                        "oncurb": 0, "curbside": 0,
                        "rumble": 0,
                        "pwm_sx": 0, "pwm_dx": 0,
                    })

                # Send the unified packet out to Arduino
                send_telemetry(ser, pkt)

except KeyboardInterrupt:
    print("\n[EXIT] User interrupted.", flush=True)
finally:
    # Clean shutdown of the external reader
    try:
        if reader is not None:
            reader.close()
            _log("[TEL] Reader closed.")
    except Exception as e:
        _log(f"[TEL] Close error: {e}")
