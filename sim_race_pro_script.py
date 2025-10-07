import serial
import threading
import time
import re
import vgamepad as vg

# === Versione / Banner ===
VERSION = "1.0.0"
print(f"SIM RACE BOX ver. {VERSION}")

# === Config ===
SERIAL_PORT = 'COM6'
BAUD_RATE = 115200
ANGLE_MIN = -450.0
ANGLE_MAX =  450.0
ANGLE_DEADZONE_DEG = 0.5   # riduce jitter intorno a 0°

STEER_GAIN = 5 # intero: 1=default, 2=+sensibilità, 3,4,... ancora di più

# Mappa i 16 tasti (indice 0..15) a pulsanti Xbox
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
    print(f"[ERRORE] Impossibile aprire la porta seriale: {e}")
    input("Premi INVIO per uscire...")
    raise SystemExit(1)

# === Gamepad init (+ messaggio connected) ===
try:
    gamepad = vg.VX360Gamepad()
    gamepad.update()
    print("connected")
except Exception as e:
    print(f"[ERRORE] Gamepad non disponibile: {e}")
    input("Premi INVIO per uscire...")
    raise SystemExit(1)

# === Stato condiviso ===
last_throttle_val = 0
last_brake_val = 0
last_angle = 0.0

def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v

def aggiorna_gamepad(asse_x=None, throttle=None, brake=None):
    if asse_x is not None:
        ax_raw = 0.0 if abs(asse_x) < ANGLE_DEADZONE_DEG else asse_x
        ax = ax_raw * STEER_GAIN
        ax = clamp(ax, ANGLE_MIN, ANGLE_MAX)
        norm = (ax - ANGLE_MIN) / (ANGLE_MAX - ANGLE_MIN)
        x_val = int(norm * 65535) - 32768
        x_val = clamp(x_val, -32768, 32767)
        gamepad.left_joystick(x_value=x_val, y_value=0)

    if throttle is not None:
        th = clamp(int(throttle), 0, 255)
        gamepad.right_trigger(value=th)

    if brake is not None:
        br = clamp(int(brake), 0, 255)
        gamepad.left_trigger(value=br)

    gamepad.update()

def premi_pulsanti_istantanei(buttons, hold_s=0.08):
    if not buttons:
        return
    for btn in buttons:
        gamepad.press_button(button=btn)
    gamepad.update()
    time.sleep(0.08 if hold_s is None else hold_s)
    for btn in buttons:
        gamepad.release_button(button=btn)
    gamepad.update()

# === Lettura seriale: degrees-acc-brk-<16 tasti>-<reset> ===
_regex = re.compile(r'^\s*([\-+]?\d+(?:\.\d+)?)\-(\d+)\-(\d+)\-(.*)\s*$')

def leggi_seriale():
    global last_throttle_val, last_brake_val, last_angle
    while True:
        try:
            raw = ser.readline().decode('utf-8', errors='ignore').strip()
            if not raw:
                continue

            m = _regex.match(raw)
            if not m:
                continue

            angle_s, acc_s, brk_s, tail = m.groups()
            bits = tail.split('-')  # 16 tasti + 1 reset atteso
            if len(bits) < 17:
                continue

            try:
                angle = float(angle_s)
                acc   = int(acc_s)
                brk   = int(brk_s)
            except ValueError:
                continue

            try:
                btn_bits = [int(x) for x in bits[:16]]
                reset    = int(bits[16])
            except ValueError:
                continue

            last_angle = angle
            last_throttle_val = clamp(acc, 0, 255)
            last_brake_val    = clamp(brk, 0, 255)

            if reset:
                last_angle = 0.0
                last_throttle_val = 0
                last_brake_val = 0
                aggiorna_gamepad(asse_x=0.0, throttle=0, brake=0)

            to_press = []
            for idx, state in enumerate(btn_bits):
                if state == 1 and idx in button_map:
                    to_press.append(button_map[idx])

            if to_press:
                premi_pulsanti_istantanei(to_press)

        except Exception:
            pass

# === Avvia thread lettura ===
t = threading.Thread(target=leggi_seriale, daemon=True)
t.start()

# === Loop principale: aggiorna gamepad ===
try:
    while True:
        time.sleep(0.01)  # 100 Hz
        aggiorna_gamepad(
            throttle=last_throttle_val,
            brake=last_brake_val,
            asse_x=last_angle
        )
except KeyboardInterrupt:
    pass
