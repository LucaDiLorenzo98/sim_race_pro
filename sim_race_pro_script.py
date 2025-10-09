import serial, threading, time, re, sys
import vgamepad as vg

VERSION = "1.3.14"
print(f"SIM RACE BOX ver. {VERSION}", flush=True)

# Config
SERIAL_PORT = 'COM6'
BAUD_RATE = 115200
DEBUG_SERIAL_LOGS = False  # True = print full serial lines
DEBUG_RAW_GXGY = False     # True = print only [RAW] gx/gy

ANGLE_MIN = -450.0
ANGLE_MAX = 450.0
ANGLE_DEADZONE_DEG = 0.5
STEER_GAIN = 5
KEYBOARD_SIM_ENABLED = True

# Manual transmission thresholds (0..255) with gaps (based on your centers)
# Y (row): up / (gap) / down
GEAR_Y_MAP = {
    "up_max": 125,   # up if gy <= 125   (centers ~113-114)
    "down_min": 140  # down if gy >= 140 (centers ~146-147)
}
# X (column) inverted: low gx = right; add small gaps between bands
INVERT_GX    = True
X_RIGHT_MAX  = 104     # right  if gx <= 104   (centers ~90)
X_CENTER_MIN = 110     # center if 110..132    (centers ~115-116)
X_CENTER_MAX = 132
X_LEFT_MIN   = 138     # left   if gx >= 138   (centers ~141-142)
# gaps: 105..109 (right→center), 133..137 (center→left)

# Keyboard
try:
    import keyboard as kb
    _kb_ok = True
except Exception as e:
    print(f"[WARNING] Keyboard module not available or lacks permissions: {e}", flush=True)
    _kb_ok = False

# Gamepad helpers
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

# Button mapping
button_map = {
    0: vg.XUSB_BUTTON.XUSB_GAMEPAD_START,
    1: vg.XUSB_BUTTON.XUSB_GAMEPAD_A,
    5: vg.XUSB_BUTTON.XUSB_GAMEPAD_X,
    6: vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_RIGHT,
    7: vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_LEFT,
    9: vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_UP,
    10: vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_DOWN
}

# Serial init
try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    _log(f"[INFO] Serial open on {SERIAL_PORT} @ {BAUD_RATE}.")
except Exception as e:
    print(f"[ERROR] Unable to open serial port: {e}", flush=True)
    sys.exit(1)

# Shared state
last_throttle_val = 0
last_brake_val = 0
last_angle = 0.0
last_hb_bit = 0
last_gear_idx = 0
gear_key_map = {1:'1', 2:'2', 3:'3', 4:'4', 5:'5', 6:'6'}

# Helpers
def clamp(v, lo, hi): return lo if v < lo else hi if v > hi else v

def aggiorna_gamepad(asse_x=None, throttle=None, brake=None):
    if gamepad is None: return
    if asse_x is not None:
        ax_raw = 0.0 if abs(asse_x) < ANGLE_DEADZONE_DEG else asse_x
        ax = clamp(ax_raw * STEER_GAIN, ANGLE_MIN, ANGLE_MAX)
        norm = (ax - ANGLE_MIN) / (ANGLE_MAX - ANGLE_MIN)
        x_val = int(norm * 65535) - 32768
        x_val = clamp(x_val, -32768, 32767)
        gamepad.left_joystick(x_value=x_val, y_value=0)
        gamepad.update()
    if throttle is not None:
        th = clamp(int(throttle), 0, 255)
        gamepad.right_trigger(value=th)
        gamepad.update()
    if brake is not None:
        br = clamp(int(brake), 0, 255)
        gamepad.left_trigger(value=br)
        gamepad.update()

def premi_pulsanti_istantanei(buttons, hold_s=0.08):
    if not buttons or gamepad is None: return
    _log(f"[GP] Pressing {len(buttons)} gamepad button(s).")
    for btn in buttons:
        gamepad.press_button(button=btn)
    gamepad.update()
    time.sleep(hold_s)
    for btn in buttons:
        gamepad.release_button(button=btn)
    gamepad.update()

# Keyboard (press only, no release)
def kb_press(keyname):
    if KEYBOARD_SIM_ENABLED and _kb_ok:
        try:
            kb.press(keyname)
            _log(f"[KB] Pressed key {keyname}")
        except Exception as e:
            _log(f"[KB] Press {keyname} failed: {e}")
    else:
        _log(f"[KB] (simulated) Would press {keyname}")

def hb_handle(hb_bit):
    global last_hb_bit
    if hb_bit == 1 and last_hb_bit == 0:
        kb_press('space')  # press-only on rising edge
    last_hb_bit = hb_bit

# Gear logic with gaps
def gear_from_gx_gy(gx, gy):
    # Y row with gap
    if gy <= GEAR_Y_MAP["up_max"]:
        row = "up"        # 1/3/5
    elif gy >= GEAR_Y_MAP["down_min"]:
        row = "down"      # 2/4/R
    else:
        return 0, "mid", "mid"   # neutral Y band

    # X column with gap (inverted)
    if INVERT_GX:
        if gx <= X_RIGHT_MAX:
            col = "right"        # 5/R
        elif X_CENTER_MIN <= gx <= X_CENTER_MAX:
            col = "center"       # 3/4
        elif gx >= X_LEFT_MIN:
            col = "left"         # 1/2
        else:
            return 0, row, "mid" # neutral X band
    else:
        if gx >= X_LEFT_MIN:
            col = "left"
        elif X_CENTER_MIN <= gx <= X_CENTER_MAX:
            col = "center"
        elif gx <= X_RIGHT_MAX:
            col = "right"
        else:
            return 0, row, "mid"

    # Map to gear index
    if col == "left":   return (1 if row == "up" else 2), row, col
    if col == "center": return (3 if row == "up" else 4), row, col
    if col == "right":  return (5 if row == "up" else 6), row, col
    return 0, row, col

_last_raw_print = 0.0
def maybe_log_raw_gxy(gx, gy, interval_s=0.1):
    if not DEBUG_RAW_GXGY:
        return
    global _last_raw_print
    now = time.time()
    if now - _last_raw_print >= interval_s:
        _last_raw_print = now
        _log(f"[RAW] gx={gx} gy={gy}")

# Serial reader
def serial_reader():
    global last_throttle_val, last_brake_val, last_angle, last_gear_idx
    _log("[INFO] Serial reader active.")
    while True:
        try:
            raw = ser.readline().decode('utf-8', errors='ignore').strip()
            if not raw:
                continue
            if DEBUG_SERIAL_LOGS:
                _log(f"[SERIAL] {raw}")

            parts = raw.split('-')
            if len(parts) < 6:
                continue

            try:
                angle = float(parts[0])
                acc   = int(parts[1])
                brk   = int(parts[2])
            except ValueError:
                continue

            try:
                gx = int(parts[-2])
                gy = int(parts[-1])
            except ValueError:
                continue

            middle = parts[3:-2]
            if not middle:
                hb_bit = 0
                btn_bits = []
            else:
                tmp = []
                for p in middle:
                    try:
                        tmp.append(int(p))
                    except:
                        tmp.append(0)
                hb_bit = tmp[-1]    # last middle bit = handbrake
                btn_bits = tmp[:-1] # previous middle bits = buttons

            last_angle = angle
            last_throttle_val = clamp(acc, 0, 255)
            last_brake_val    = clamp(brk, 0, 255)

            maybe_log_raw_gxy(gx, gy)

            to_press = []
            for idx, state in enumerate(btn_bits):
                if state == 1 and idx in button_map:
                    to_press.append(button_map[idx])
            if to_press:
                premi_pulsanti_istantanei(to_press)

            hb_handle(1 if hb_bit == 1 else 0)

            gear_idx, row, col = gear_from_gx_gy(clamp(gx,0,255), clamp(gy,0,255))
            if gear_idx != last_gear_idx:
                if gear_idx in gear_key_map:
                    kb_press(gear_key_map[gear_idx])  # press only
                    _log(f"[GEAR] {gear_idx} (row={row}, col={col})")
                elif gear_idx == 0 and last_gear_idx != 0:
                    _log(f"[GEAR] Neutral (row={row}, col={col})")
                last_gear_idx = gear_idx

        except Exception as e:
            _log(f"[WARN] Reader error: {e}")
            time.sleep(0.01)

# Start reader thread
t = threading.Thread(target=serial_reader, daemon=True)
t.start()

# Main loop
try:
    while True:
        time.sleep(0.01)
        aggiorna_gamepad(throttle=last_throttle_val, brake=last_brake_val, asse_x=last_angle)
except KeyboardInterrupt:
    print("\n[EXIT] User interrupted.", flush=True)
