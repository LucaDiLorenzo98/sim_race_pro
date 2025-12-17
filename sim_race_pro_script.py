import serial
import serial.tools.list_ports
import threading
import time
import re
import sys
import tkinter as tk
from tkinter import ttk, messagebox
from enum import Enum
from dataclasses import dataclass
from typing import Optional

# Try importing vgamepad, else mock it for testing if needed (but user has it)
try:
    import vgamepad as vg
    _VG_AVAILABLE = True
except ImportError:
    _VG_AVAILABLE = False

# Try importing keyboard
try:
    import keyboard as kb
    _KB_AVAILABLE = True
except ImportError:
    _KB_AVAILABLE = False

from telemetry_sources import F1TelemetryReader, ACCTelemetryReader, ForzaTelemetryReader

VERSION = "3.0.0 (GUI)"

# ==============================================================================
# ENUMS & CONSTANTS
# ==============================================================================
class Game(Enum):
    NONE = "NONE"
    ACC = "ACC"
    F1 = "F1"
    FORZA = "FORZA"

GEAR_Y_MAP = {"up_max": 125, "down_min": 140}
INVERT_GX = True
X_RIGHT_MAX, X_CENTER_MIN, X_CENTER_MAX, X_LEFT_MIN = 104, 110, 132, 138
STEERING_LOCK = 180

MAX_RPM_MAP = {Game.F1: 13500, Game.ACC: 9000, Game.FORZA: 8000}

# Default Button Map (Arduino Index -> Xbox Button)
DEFAULT_BUTTON_MAP = {
    0: "START", 
    1: "A", 
    2: "X", 
    3: "DPAD_RIGHT",
    4: "DPAD_LEFT", 
    5: "DPAD_UP", 
    6: "DPAD_DOWN", 
    7: "BACK",
    8: "LEFT_THUMB", 
    9: "RIGHT_THUMB",
    10: "LEFT_SHOULDER", 
    11: "RIGHT_SHOULDER", 
    12: "B", 
    13: "Y", 
}

# Supported Xbox Buttons for Dropdown
XBOX_BUTTONS = [
    "NONE", "A", "B", "X", "Y", "START", "BACK",
    "LEFT_SHOULDER", "RIGHT_SHOULDER", "LEFT_THUMB", "RIGHT_THUMB",
    "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "GUIDE"
]

def get_xbox_btn_code(name):
    if not _VG_AVAILABLE: return 0
    if name == "NONE": return 0
    # Map string to vgamepad constant
    # vgamepad uses XUSB_BUTTON.XUSB_GAMEPAD_...
    attr_name = f"XUSB_GAMEPAD_{name}"
    try:
        return getattr(vg.XUSB_BUTTON, attr_name)
    except AttributeError:
        return 0

@dataclass
class TelemetryPacket:
    gx: float=0.0; gy: float=0.0; gz: float=0.0
    yaw: float=0.0; pitch: float=0.0; roll: float=0.0
    speed: float=0.0; gear: int=0; rpm: int=0
    oncurb: int=0; curbside: int=0; rumble: int=0
    pwm_sx: int=0; pwm_dx: int=0

def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v

# ==============================================================================
# LOGIC CLASS
# ==============================================================================
class SimRaceLogic:
    def __init__(self):
        self.running = False
        self.thread = None
        
        # Configuration
        self.selected_port = ""
        self.selected_game = Game.NONE
        self.button_map = DEFAULT_BUTTON_MAP.copy()
        
        # State
        self.ser = None
        self.reader = None
        self.gamepad = None
        self.connection_status = False # True if serial is open
        self.last_pressed_btn_idx = -1
        
        # Manual Gear Logic
        self.manual_gear_enabled = False
        self.gear_key_map = {1:'1', 2:'2', 3:'3', 4:'4', 5:'5', 6:'6'}
        
        # Internal Loop Vars
        self.last_throttle = 0
        self.last_brake = 0
        self.last_angle = 0
        self.last_gear_idx = 0
        self.last_hb_bit = 0
        
        # Init Gamepad
        if _VG_AVAILABLE:
            try:
                self.gamepad = vg.VX360Gamepad()
            except Exception as e:
                print(f"Error creating gamepad: {e}")

    def start(self):
        if self.running: return
        self.running = True
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=1.0)
        self.close_serial()
        if self.reader:
            try: self.reader.close()
            except: pass

    def set_port(self, port):
        if port == self.selected_port: return
        self.selected_port = port
        # The loop will handle reconnection if port changed
        self.close_serial()

    def set_game(self, game_str):
        try:
            new_game = Game(game_str)
        except ValueError:
            new_game = Game.NONE
            
        if new_game != self.selected_game:
            self.selected_game = new_game
            # Close old reader
            if self.reader:
                try: self.reader.close()
                except: pass
                self.reader = None
            
            # Create new reader
            if self.selected_game == Game.F1:
                self.reader = F1TelemetryReader(port=20777)
                self.reader.start()
            elif self.selected_game == Game.ACC:
                self.reader = ACCTelemetryReader()
                self.reader.start()
            elif self.selected_game == Game.FORZA:
                self.reader = ForzaTelemetryReader(port=5300)
                self.reader.start()
            else:
                self.reader = None

    def update_binding(self, btn_idx, btn_name):
        self.button_map[btn_idx] = btn_name
        
    def _gear_from_gx_gy(self, gx, gy):
        # 3 Rows (Up, Mid, Down) x 3 Cols (Left, Center, Right)
        # Up/Down Logic
        row = "up" if gy <= GEAR_Y_MAP["up_max"] else "down" if gy >= GEAR_Y_MAP["down_min"] else "mid"
        
        # Left/Right Logic (Inverted GX check)
        if INVERT_GX:
            col = "right" if gx <= X_RIGHT_MAX else "center" if X_CENTER_MIN <= gx <= X_CENTER_MAX else "left" if gx >= X_LEFT_MIN else "mid"
        else:
            col = "left" if gx >= X_LEFT_MIN else "center" if X_CENTER_MIN <= gx <= X_CENTER_MAX else "right" if gx <= X_RIGHT_MAX else "mid"
        
        # Map Grid to Gear
        if col == "left": return 1 if row == "up" else 2
        if col == "center": return 3 if row == "up" else 4
        if col == "right": return 5 if row == "up" else 6
        return 0

    def close_serial(self):
        self.connection_status = False
        if self.ser:
            try: self.ser.close()
            except: pass
            self.ser = None

    def _loop(self):
        print("Logic Loop Started")
        pat = re.compile(r'^\s*([+-]?\d+(?:\.\d+)?)\-(\d+)\-(\d+)\-(.*)\s*$')
        pkt = TelemetryPacket()
        
        while self.running:
            # 1. Manage Serial Connection
            if not self.ser and self.selected_port:
                try:
                    self.ser = serial.Serial(self.selected_port, 115200, timeout=1)
                    self.ser.reset_input_buffer()
                    self.connection_status = True
                    print(f"Connected to {self.selected_port}")
                except Exception as e:
                    self.connection_status = False
                    time.sleep(1.0) # Wait before retry
                    continue
            
            if not self.ser:
                time.sleep(0.5)
                continue

            # 2. Read from Serial
            try:
                raw_line = self.ser.readline()
                if not raw_line: continue
                line = raw_line.decode('utf-8', errors='ignore').strip()
            except Exception:
                self.connection_status = False
                self.ser = None
                continue

            if not line: continue

            # 3. Parse Data
            self.last_pressed_btn_idx = -1 # Reset for UI feedback
            try:
                m = pat.match(line)
                if m:
                    self.last_angle = float(m.group(1))
                    self.last_throttle = clamp(int(m.group(2)), 0, 255)
                    self.last_brake = clamp(int(m.group(3)), 0, 255)
                    
                    tparts = m.group(4).split('-')
                    if len(tparts) >= 2:
                        gx, gy = int(tparts[-2]), int(tparts[-1])
                        mid = tparts[:-2]
                        hb = int(mid[-1]) if mid else 0
                        btns = [int(x) for x in mid[:-1]] if len(mid) > 1 else []

                        # Gamepad Updates
                        if self.gamepad:
                            # Steering
                            half_lock = STEERING_LOCK / 2.0
                            val = int((clamp(self.last_angle, -half_lock, half_lock) / half_lock) * 32767)
                            self.gamepad.left_joystick(x_value=clamp(val, -32768, 32767), y_value=0)
                            
                            # Pedals
                            self.gamepad.right_trigger(value=clamp(int(self.last_throttle), 0, 255))
                            self.gamepad.left_trigger(value=clamp(int(self.last_brake), 0, 255))

                            # Buttons
                            for i, s in enumerate(btns):
                                if s == 1:
                                    self.last_pressed_btn_idx = i # Store for UI
                                    btn_name = self.button_map.get(i, "NONE")
                                    code = get_xbox_btn_code(btn_name)
                                    if code: self.gamepad.press_button(button=code)
                                else:
                                    btn_name = self.button_map.get(i, "NONE")
                                    code = get_xbox_btn_code(btn_name)
                                    if code: self.gamepad.release_button(button=code)

                            self.gamepad.update()

                        # Keyboard Handbrake / Gear
                        if hb == 1 and self.last_hb_bit == 0:
                            if _KB_AVAILABLE: 
                                try: kb.press_and_release('space')
                                except: pass
                        self.last_hb_bit = hb

                        # Manual Gear Emulation (via Keyboard)
                        if self.manual_gear_enabled and _KB_AVAILABLE:
                            g_idx = self._gear_from_gx_gy(clamp(gx,0,255), clamp(gy,0,255))
                            if g_idx != self.last_gear_idx:
                                if g_idx in self.gear_key_map:
                                    try: kb.press_and_release(self.gear_key_map[g_idx])
                                    except: pass
                                self.last_gear_idx = g_idx
                        
            except Exception as e:
                pass

            # 4. Telemetry Response
            frame = self.reader.read_frame(timeout_s=0.005) if self.reader else None
            
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

                resp = self._build_serial_resp(pkt)
            else:
                resp = "0;N;0;127;0;0\n"

            if self.ser and self.connection_status:
                try: self.ser.write(resp.encode("ascii"))
                except: pass

    def _build_serial_resp(self, pkt):
        max_rpm = MAX_RPM_MAP.get(self.selected_game, 10000)
        rpm_val = int(pkt.rpm)
        gear_str = str(pkt.gear)
        if pkt.gear == 0: gear_str = "N"
        elif pkt.gear == -1: gear_str = "R"
        
        speed_val = int(pkt.speed)
        gx_mapped = int(pkt.gx * 60) + 127
        gx_send = clamp(gx_mapped, 0, 255)
        
        # Simple Logic for now
        rumble_val = 0
        if pkt.oncurb: rumble_val = 200
        rumble_val = max(rumble_val, int(pkt.rumble * 2.5))
        rumble_send = clamp(rumble_val, 0, 255)

        rpm_pct = clamp(int((rpm_val / max_rpm) * 100), 0, 100)
        return f"{rpm_val};{gear_str};{speed_val};{gx_send};{rumble_send};{rpm_pct}\n"


# ==============================================================================
# GUI CLASS
# ==============================================================================
class SimRaceGUI:
    def __init__(self, root, logic):
        self.root = root
        self.logic = logic
        self.root.title(f"Sim Race Pro - {VERSION}")
        self.root.geometry("500x700")
        
        # Styles
        style = ttk.Style()
        style.theme_use('clam')
        
        # --- HEADER ---
        header_frame = ttk.Frame(root, padding=10)
        header_frame.pack(fill=tk.X)
        
        lbl_title = ttk.Label(header_frame, text="SIM RACE PRO", font=("Arial", 16, "bold"))
        lbl_title.pack(side=tk.LEFT)
        
        # Connection Status
        self.canvas_status = tk.Canvas(header_frame, width=20, height=20, highlightthickness=0)
        self.canvas_status.pack(side=tk.RIGHT, padx=5)
        self.status_circle = self.canvas_status.create_oval(2, 2, 18, 18, fill="red", outline="black")
        
        # --- CONFIG SECTION ---
        config_frame = ttk.LabelFrame(root, text="Configuration", padding=10)
        config_frame.pack(fill=tk.X, padx=10, pady=5)
        
        # COM Port
        frame_com = ttk.Frame(config_frame)
        frame_com.pack(fill=tk.X, pady=2)
        ttk.Label(frame_com, text="Serial Port:").pack(side=tk.LEFT, width=15)
        self.cb_port = ttk.Combobox(frame_com, values=self.get_com_ports())
        self.cb_port.pack(side=tk.LEFT, fill=tk.X, expand=True)
        self.cb_port.bind("<<ComboboxSelected>>", self.on_port_change)
        ttk.Button(frame_com, text="Refresh", command=self.refresh_ports).pack(side=tk.RIGHT, padx=5)
        
        # Game Selection
        frame_game = ttk.Frame(config_frame)
        frame_game.pack(fill=tk.X, pady=2)
        ttk.Label(frame_game, text="Telemetry Game:").pack(side=tk.LEFT, width=15)
        self.cb_game = ttk.Combobox(frame_game, values=[g.name for g in Game], state="readonly")
        self.cb_game.set(Game.NONE.name)
        self.cb_game.pack(side=tk.LEFT, fill=tk.X, expand=True)
        self.cb_game.bind("<<ComboboxSelected>>", self.on_game_change)
        
        # Manual Gear Checkbox
        self.chk_manual = ttk.Checkbutton(config_frame, text="Enable Manual Gear (Tilt)", command=self.on_manual_toggle)
        self.chk_manual.pack(anchor=tk.W, pady=5)
        self.chk_manual.state(['!selected'])
        
        # --- BINDINGS SECTION ---
        bind_frame = ttk.LabelFrame(root, text="Button Bindings (Press button on wheel to identify)", padding=10)
        bind_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)
        
        # Canvas for scrolling
        canvas = tk.Canvas(bind_frame)
        scrollbar = ttk.Scrollbar(bind_frame, orient="vertical", command=canvas.yview)
        self.scrollable_frame = ttk.Frame(canvas)
        
        self.scrollable_frame.bind(
            "<Configure>",
            lambda e: canvas.configure(scrollregion=canvas.bbox("all"))
        )
        
        canvas.create_window((0, 0), window=self.scrollable_frame, anchor="nw")
        canvas.configure(yscrollcommand=scrollbar.set)
        
        canvas.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")
        
        # Create Binding Rows (0-15)
        self.bind_widgets = []
        for i in range(16):
            row = ttk.Frame(self.scrollable_frame)
            row.pack(fill=tk.X, pady=2)
            
            # Indicator (Label acting as LED)
            lbl_idx = tk.Label(row, text=f"Button {i}", width=10, bg="#f0f0f0", anchor="w")
            lbl_idx.pack(side=tk.LEFT)
            
            cb_xbox = ttk.Combobox(row, values=XBOX_BUTTONS, state="readonly", width=15)
            # Set default
            def_btn = DEFAULT_BUTTON_MAP.get(i, "NONE")
            cb_xbox.set(def_btn)
            cb_xbox.pack(side=tk.LEFT, padx=10)
            cb_xbox.bind("<<ComboboxSelected>>", lambda e, idx=i, cb=cb_xbox: self.on_bind_change(idx, cb))
            
            self.bind_widgets.append((lbl_idx, cb_xbox))
            
        # Start updates
        self.refresh_ports()
        self.root.after(100, self.update_ui)
        
    def get_com_ports(self):
        return [p.device for p in serial.tools.list_ports.comports()]
        
    def refresh_ports(self):
        ports = self.get_com_ports()
        self.cb_port['values'] = ports
        if ports and not self.cb_port.get():
            self.cb_port.set(ports[0])
            self.on_port_change(None) # Auto select first

    def on_port_change(self, event):
        p = self.cb_port.get()
        print(f"Port selected: {p}")
        self.logic.set_port(p)
        
    def on_game_change(self, event):
        g = self.cb_game.get()
        print(f"Game selected: {g}")
        self.logic.set_game(g)
        
    def on_manual_toggle(self):
        val = self.chk_manual.instate(['selected'])
        print(f"Manual Gear: {val}")
        self.logic.manual_gear_enabled = val
        
    def on_bind_change(self, idx, cb):
        val = cb.get()
        print(f"Binding Chaged: Btn {idx} -> {val}")
        self.logic.update_binding(idx, val)
        
    def update_ui(self):
        # 1. Connection Status
        is_conn = self.logic.connection_status
        color = "green" if is_conn else "red"
        self.canvas_status.itemconfig(self.status_circle, fill=color)
        
        # 2. Binding Feedback
        pressed_idx = self.logic.last_pressed_btn_idx
        for i, (lbl, cb) in enumerate(self.bind_widgets):
            if i == pressed_idx:
                lbl.configure(bg="#00ff00") # Highlight Green
            else:
                lbl.configure(bg="#f0f0f0") # Default Gray
                
        self.root.after(50, self.update_ui)

# ==============================================================================
# MAIN
# ==============================================================================
if __name__ == "__main__":
    # Create Logic
    logic = SimRaceLogic()
    logic.start()
    
    # Create GUI
    try:
        root = tk.Tk()
        app = SimRaceGUI(root, logic)
        root.mainloop()
    except KeyboardInterrupt:
        pass
    finally:
        logic.stop()