import serial
import threading
import time
import re
import vgamepad as vg

# === Version / Banner ===
VERSION = "1.0.0"
print(f"SIM RACE BOX ver. {VERSION}")

# === Config ===
SERIAL_PORT = 'COM6'
BAUD_RATE = 115200
ANGLE_MIN = -450.0
ANGLE_MAX =  450.0
ANGLE_DEADZONE_DEG = 0.5   # reduces jitter around 0°

STEER_GAIN = 5  # integer: 1=default, 2=more sensitivity, 3,4,... even more

# Enable keyboard simulation (space = handbrake hold, 1..6 = gears 1..5 and reverse)
KEYBOARD_SIM_ENABLED = True

# Keyboard module (optional). If not available or lacking permissions, keyboard simulation is disabled.
try:
    import keyboard as kb
    _kb_ok = True
except Exception as e:
    print(f"[WARNING] 'keyboard' module not available or lacks permissions: {e}. Keyboard simulation disabled.")
    _kb_ok = False

# Map 16 bit inputs (index 0..15) to Xbox buttons
button_map = {
    0:  vg.XUSB_BUTTON.XUSB_GAMEPAD_START,
    1:  vg.XUSB_BUTTON.XUSB_GAMEPAD_A,
    5:  vg.XUSB_BUTTON.XUSB_GAMEPAD_X,
    6:  vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_RIGHT,
    7:  vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_LEFT,
    9:  vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_UP,
    10: vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_DOWN
}

# === Serial init ===
try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
except Exception as e:
    print(f"[ERROR] Unable to open serial port: {e}")
    input("Press ENTER to exit...")
    raise SystemExit(1)

# === Gamepad init (+ connected message) ===
try:
    gamepad = vg.VX360Gamepad()
    gamepad.update()
    print("Connected")
except Exception as e:
    print(f"[ERROR] Virtual gamepad not available: {e}")
    input("Press ENTER to exit...")
    raise SystemExit(1)

# === Shared state ===
last_throttle_val = 0
last_brake_val = 0
last_angle = 0.0

# Keyboard simulation state
last_hb_bit = 0            # 0/1
last_gear_idx = 0          # 0 = Neutral, 1..5 = gears, 6 = Reverse
gear_key_map = {1:'1', 2:'2', 3:'3', 4:'4', 5:'5', 6:'6'}  # 6 = Reverse

def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v

def aggiorna_gamepad(asse_x=None, throttle=None, brake=None):
    # Update left stick X from steering angle
    if asse_x is not None:
        ax_raw = 0.0 if abs(asse_x) < ANGLE_DEADZONE_DEG else asse_x
        ax = ax_raw * STEER_GAIN
        ax = clamp(ax, ANGLE_MIN, ANGLE_MAX)
        norm = (ax - ANGLE_MIN) / (ANGLE_MAX - ANGLE_MIN)
        x_val = int(norm * 65535) - 32768
        x_val = clamp(x_val, -32768, 32767)
        gamepad.left_joystick(x_value=x_val, y_value=0)

    # Update triggers from throttle/brake
    if throttle is not None:
        th = clamp(int(throttle), 0, 255)
        gamepad.right_trigger(value=th)

    if brake is not None:
        br = clamp(int(brake), 0, 255)
        gamepad.left_trigger(value=br)

    gamepad.update()

def premi_pulsanti_istantanei(buttons, hold_s=0.08):
    # Instantaneous press/release for a list of buttons
    if not buttons:
        return
    for btn in buttons:
        gamepad.press_button(button=btn)
    gamepad.update()
    time.sleep(0.08 if hold_s is None else hold_s)
    for btn in buttons:
        gamepad.release_button(button=btn)
    gamepad.update()

# === Serial parser
# Supported formats (backward compatible):
# 1) <deg>-<acc>-<brk>-<16bits>-<reset>
# 2) <deg>-<acc>-<brk>-<16bits>-<reset>-<HB>-<GEARS6>
#    where:
#      HB     = '0' or '1'
#      GEARS6 = 6 chars '0'/'1' one-hot -> [1st,2nd,3rd,4th,5th,Reverse]
# If the trailing fields are absent, HB=0 and GEARS6="000000" are assumed.
_regex = re.compile(
    r'^\s*([\-+]?\d+(?:\.\d+)?)\-(\d+)\-(\d+)\-(.*?)(?:\-(0|1)\-([01]{6}))?\s*$'
)

def _parse_gear_index(gear_str):
    # Convert 6-bit one-hot string to gear index: 0 = Neutral, 1..5 = gears, 6 = Reverse
    if not isinstance(gear_str, str) or len(gear_str) != 6 or any(c not in '01' for c in gear_str):
        return 0
    try:
        idx = gear_str.index('1') + 1  # yields 1..6
    except ValueError:
        return 0
    return idx  # 1..6 (6 = Reverse)

def _keyboard_press_and_release(keyname):
    # Send a quick press+release for a given key
    if KEYBOARD_SIM_ENABLED and _kb_ok:
        try:
            kb.press_and_release(keyname)
        except Exception:
            pass

def _keyboard_set_space(pressed):
    # Hold/release space as long as handbrake is active
    if not (KEYBOARD_SIM_ENABLED and _kb_ok):
        return
    try:
        if pressed:
            kb.press('space')
        else:
            kb.release('space')
    except Exception:
        pass

def leggi_seriale():
    global last_throttle_val, last_brake_val, last_angle
    global last_hb_bit, last_gear_idx

    while True:
        try:
            raw = ser.readline().decode('utf-8', errors='ignore').strip()
            if not raw:
                continue

            m = _regex.match(raw)
            if not m:
                continue

            angle_s, acc_s, brk_s, slave_tail, hb_opt, gears_opt = m.groups()

            # Parse main numeric fields
            try:
                angle = float(angle_s)
                acc   = int(acc_s)
                brk   = int(brk_s)
            except ValueError:
                continue

            # Parse slave block: must be 16 button bits + 1 reset bit
            bits = slave_tail.split('-')
            if len(bits) < 17:
                continue

            try:
                btn_bits = [int(x) for x in bits[:16]]
                reset    = int(bits[16])
            except ValueError:
                continue

            # Update shared axis state
            last_angle = angle
            last_throttle_val = clamp(acc, 0, 255)
            last_brake_val    = clamp(brk, 0, 255)

            # Handle reset
            if reset:
                last_angle = 0.0
                last_throttle_val = 0
                last_brake_val = 0
                aggiorna_gamepad(asse_x=0.0, throttle=0, brake=0)

                # Release space if held and clear gear state
                if last_hb_bit == 1:
                    _keyboard_set_space(False)
                last_hb_bit = 0
                last_gear_idx = 0

            # Instant buttons (existing behavior)
            to_press = []
            for idx, state in enumerate(btn_bits):
                if state == 1 and idx in button_map:
                    to_press.append(button_map[idx])
            if to_press:
                premi_pulsanti_istantanei(to_press)

            # New fields: handbrake and gears (optional)
            hb_bit = 0 if hb_opt is None else int(hb_opt)
            gear_str = "000000" if gears_opt is None else gears_opt
            gear_idx = _parse_gear_index(gear_str)  # 0=N, 1..5, 6=R

            # Handbrake mapped to SPACE (hold while hb_bit==1)
            if hb_bit != last_hb_bit:
                _keyboard_set_space(hb_bit == 1)
                last_hb_bit = hb_bit

            # Gears mapped to number keys 1..6 (pulse on change, ignore Neutral)
            if gear_idx != last_gear_idx:
                if gear_idx in gear_key_map:
                    _keyboard_press_and_release(gear_key_map[gear_idx])
                last_gear_idx = gear_idx

        except Exception:
            # Keep the reader loop alive on line-level errors
            pass

# === Start serial reader thread ===
t = threading.Thread(target=leggi_seriale, daemon=True)
t.start()

# === Main loop: update gamepad ===
try:
    while True:
        time.sleep(0.01)  # ~100 Hz
        aggiorna_gamepad(
            throttle=last_throttle_val,
            brake=last_brake_val,
            asse_x=last_angle
        )
except KeyboardInterrupt:
    pass
