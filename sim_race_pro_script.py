import serial, threading, time, re, sys
import vgamepad as vg
from dataclasses import dataclass
from telemetry_sources import F1TelemetryReader, ACCTelemetryReader

VERSION = "2.0.0"
print(f"SIM RACE BOX ver. {VERSION}", flush=True)

SERIAL_PORT = 'COM16'
BAUD_RATE = 115200
TX_RATE_HZ = 15
HANDBRAKE_ENABLED = False
MANUAL_TX_ENABLED = False
KEYBOARD_SIM_ENABLED = True

# CHOOSE BETWEEN "ACC" OR "F1" (F1 23/24/25)
SELECTED_GAME = "ACC"

GEAR_Y_MAP = {"up_max": 125, "down_min": 140}
INVERT_GX = True
X_RIGHT_MAX, X_CENTER_MIN, X_CENTER_MAX, X_LEFT_MIN = 104, 110, 132, 138

STEERING_LOCK = 180

def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v

try:
    import keyboard as kb
    _kb_ok = True
except:
    _kb_ok = False

gamepad = None
def create_gamepad():
    global gamepad
    try:
        gamepad = vg.VX360Gamepad()
    except Exception as e:
        print(f"Error creating gamepad: {e}")
        sys.exit(1)
create_gamepad()

# Mappa dei pulsanti: Indice Arduino -> Tasto Xbox
button_map = {
    0: vg.XUSB_BUTTON.XUSB_GAMEPAD_START, 
    1: vg.XUSB_BUTTON.XUSB_GAMEPAD_A,
    2: vg.XUSB_BUTTON.XUSB_GAMEPAD_X, 
    3: vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_RIGHT,
    4: vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_LEFT, 
    5: vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_UP,
    6: vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_DOWN, 
    7: vg.XUSB_BUTTON.XUSB_GAMEPAD_BACK,
    8: vg.XUSB_BUTTON.XUSB_GAMEPAD_LEFT_THUMB, 
    9: vg.XUSB_BUTTON.XUSB_GAMEPAD_RIGHT_THUMB,
    10: vg.XUSB_BUTTON.XUSB_GAMEPAD_LEFT_SHOULDER, 
    11: vg.XUSB_BUTTON.XUSB_GAMEPAD_RIGHT_SHOULDER,
    12: vg.XUSB_BUTTON.XUSB_GAMEPAD_B, 
    13: vg.XUSB_BUTTON.XUSB_GAMEPAD_Y,
}

try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
except Exception as e:
    print(f"Error opening serial port {SERIAL_PORT}: {e}")
    sys.exit(1)

last_throttle_val = last_brake_val = last_angle = last_hb_bit = last_gear_idx = 0
gear_key_map = {1:'1', 2:'2', 3:'3', 4:'4', 5:'5', 6:'6'}

def update_gamepad(throttle=None, brake=None, steer_angle=None):
    if not gamepad: return
    if steer_angle is not None:
        half_lock = STEERING_LOCK / 2.0
        val = int((clamp(steer_angle, -half_lock, half_lock) / half_lock) * 32767)
        gamepad.left_joystick(x_value=clamp(val, -32768, 32767), y_value=0)
    if throttle is not None: gamepad.right_trigger(value=clamp(int(throttle), 0, 255))
    if brake is not None: gamepad.left_trigger(value=clamp(int(brake), 0, 255))
    gamepad.update()

def kb_press(k):
    if KEYBOARD_SIM_ENABLED and _kb_ok:
        try: kb.press(k)
        except: pass

def gear_from_gx_gy(gx, gy):
    if not MANUAL_TX_ENABLED: return 0
    row = "up" if gy <= GEAR_Y_MAP["up_max"] else "down" if gy >= GEAR_Y_MAP["down_min"] else "mid"
    if INVERT_GX:
        col = "right" if gx <= X_RIGHT_MAX else "center" if X_CENTER_MIN <= gx <= X_CENTER_MAX else "left" if gx >= X_LEFT_MIN else "mid"
    else:
        col = "left" if gx >= X_LEFT_MIN else "center" if X_CENTER_MIN <= gx <= X_CENTER_MAX else "right" if gx <= X_RIGHT_MAX else "mid"
    
    if col == "left": return 1 if row == "up" else 2
    if col == "center": return 3 if row == "up" else 4
    if col == "right": return 5 if row == "up" else 6
    return 0

@dataclass(slots=True)
class TelemetryPacket:
    gx: float=0.0; gy: float=0.0; gz: float=0.0; yaw: float=0.0; pitch: float=0.0; roll: float=0.0
    speed: float=0.0; gear: int=0; rpm: int=0; oncurb: int=0; curbside: int=0; rumble: int=0; pwm_sx: int=0; pwm_dx: int=0

MAX_RPM_MAP = {"F1": 13500, "ACC": 9000}
MAX_RPM = MAX_RPM_MAP.get(SELECTED_GAME, 10000)

def build_serial_line(pkt):
    rpm_val = int(pkt.rpm)
    gear_str = str(pkt.gear)
    if pkt.gear == 0: gear_str = "N"
    elif pkt.gear == -1: gear_str = "R"
    speed_val = int(pkt.speed)

    gx_mapped = int(pkt.gx * 60) + 127
    gx_send = clamp(gx_mapped, 0, 255)

    # Rumble Logic: Road Noise + Curb + Collision
    g_total = abs(pkt.gx or 0) + abs(pkt.gy or 0)
    is_collision = g_total > 5.0 # High Impact

    rumble_val = 0
    if is_collision:
        rumble_val = 255 # MAX VIBRATION (Collision)
    elif pkt.oncurb:
        rumble_val = 220 # STRONG VIBRATION (Curb)
    else:
        rumble_val = int((pkt.rumble or 0) * 2.0) # Background noise

    rumble_send = clamp(rumble_val, 0, 255)

    rpm_pct = clamp(int((rpm_val / MAX_RPM) * 100), 0, 100)

    # Format: rpm;gear;speed;gx;rumble;rpm_pct
    return f"{rpm_val};{gear_str};{speed_val};{gx_send};{rumble_send};{rpm_pct}\n"

# Removed threaded serial_reader
# Main Synchronous Loop

try:
    if SELECTED_GAME == "F1": reader = F1TelemetryReader(port=20777)
    elif SELECTED_GAME == "ACC": reader = ACCTelemetryReader()
    if reader: reader.start()
except: reader = None

try:
    print("Starting Synchronous Loop. Waiting for Box...", flush=True)
    ser.reset_input_buffer()
    pat = re.compile(r'^\s*([+-]?\d+(?:\.\d+)?)\-(\d+)\-(\d+)\-(.*)\s*$')
    pkt = TelemetryPacket()
    
    while True:
        # 1. Wait for packet from Box (Blocking)
        try:
            raw_line = ser.readline()
            if not raw_line: continue # Timeout or empty
            line = raw_line.decode('utf-8', errors='ignore').strip()
        except Exception:
            time.sleep(0.01)
            continue
            
        if not line: continue

        # 2. Process received data
        try:
            m = pat.match(line)
            if m:
                last_angle = float(m.group(1))
                last_throttle_val = clamp(int(m.group(2)), 0, 255)
                last_brake_val = clamp(int(m.group(3)), 0, 255)
                
                tparts = m.group(4).split('-')
                if len(tparts) >= 2:
                    gx, gy = int(tparts[-2]), int(tparts[-1])
                    mid = tparts[:-2]
                    hb = int(mid[-1]) if mid else 0
                    btns = [int(x) for x in mid[:-1]] if len(mid) > 1 else []

                    # Button Handling
                    for i, s in enumerate(btns):
                        if i in button_map:
                            if s == 1:
                                gamepad.press_button(button=button_map[i])
                            else:
                                gamepad.release_button(button=button_map[i])
                    
                    if HANDBRAKE_ENABLED and hb == 1 and last_hb_bit == 0: kb_press('space')
                    last_hb_bit = hb

                    if MANUAL_TX_ENABLED:
                        g_idx = gear_from_gx_gy(clamp(gx,0,255), clamp(gy,0,255))
                        if g_idx != last_gear_idx:
                            if g_idx in gear_key_map: kb_press(gear_key_map[g_idx])
                            last_gear_idx = g_idx
                
                # Update Emulator
                update_gamepad(last_throttle_val, last_brake_val, last_angle)
        except Exception as e:
            # Ignore malformed packets
            pass

        # 3. Read Telemetry & Send Response
        # We process telemetry immediately to respond as fast as possible
        frame = reader.read_frame(timeout_s=0.005) if reader else None
        
        if frame:
            pkt.gx = float(getattr(frame, "g_lat", 0))
            pkt.gy = float(getattr(frame, "g_lon", 0))
            pkt.gz = float(getattr(frame, "g_vert", 0))
            pkt.speed = float(getattr(frame, "speed_kmh", 0))
            pkt.gear = int(getattr(frame, "gear", 0))
            pkt.rpm = int(getattr(frame, "rpm", 0))
            pkt.oncurb = 1 if getattr(frame, "on_curb", 0) else 0
            pkt.rumble = int(float(getattr(frame, "rumble", 0)) * 100)
            side = str(getattr(frame, "curb_side", "")).lower()
            pkt.curbside = -1 if side.startswith('l') else 1 if side.startswith('r') else 0
            
            print(f"UDP Recv: Speed={pkt.speed:.1f} Gear={pkt.gear} RPM={pkt.rpm}", end='\r')
            resp = build_serial_line(pkt)
            try: ser.write(resp.encode("ascii"))
            except: pass
        else:
            # Default / No Game
            pkt = TelemetryPacket() # Reset
            neutral_data = "0;N;0;127;0;0\n"
            try: ser.write(neutral_data.encode("ascii"))
            except: pass

except KeyboardInterrupt:
    if reader: reader.close()