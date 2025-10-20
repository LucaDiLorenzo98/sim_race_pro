# Dynamic FFB – SIM Race PRO (Windows)

This guide takes you from a fresh Windows install to running the simulator with **two Arduinos** talking to each other and a **Python** process that emulates a **virtual Xbox controller**.

> **Tested on Windows 10/11.**  
> Python 3.11/3.12 recommended.

---

## 1) Install Python (Windows)

1. Download the **Windows x86-64 executable installer** for Python 3.11 or 3.12.
2. Run the installer and **check**:
   - **Add Python to PATH**
   - *(Optional)* Install for all users
3. Verify in **PowerShell**:
   ```powershell
   python --version
   pip --version
   ```
   If not found, try `py --version`, then reopen the terminal.

---

## 2) Install ViGEmBus (virtual gamepad driver)

The Python script uses **vgamepad**, which requires the **ViGEmBus** driver to create a virtual Xbox controller.

- Install **ViGEmBus**, then **restart Windows**.
- After reboot, when the Python script is running you should see an Xbox controller in **Windows Game Controllers**.

> Without ViGEmBus, `vgamepad` will fail to initialize.

---

## 3) Project Layout

Place these files in the same folder:

- `sim_race_pro_script.py` — main runner (serial I/O, virtual gamepad bridge)
- `telemetry_sources.py` — placeholder for future telemetry integration
- `sim_race_pro_wheel_script.ino` — Arduino firmware for the **wheel**
- `sim_race_pro_box_script.ino` — Arduino firmware for the **box**

---

## 4) Install Python libraries

Open **PowerShell** in the project folder and run:

```powershell
pip install pyserial vgamepad keyboard pyaccsharedmemory
```

### 4.1 Verify packages (already installed?)
```powershell
pip show pyserial
pip show vgamepad
pip show keyboard
pip show pyaccsharedmemory
```
If any shows **WARNING: Package(s) not found**, install it with `pip install <name>`.

---

## 5) Install Arduino libraries

Open **Arduino IDE** → **Tools → Manage Libraries…** and install:

- **Encoder** (by Paul Stoffregen) — required for rotary encoders

> If your sketches need extras (e.g., `Bounce2`, `Keypad`, OLED display libs), the IDE will show missing libraries during compile. Install them from **Manage Libraries**.

---

## 6) Upload the two Arduino sketches

You have **two separate Arduinos**. Upload **each** sketch to the correct board.

### 6.1 Prepare
1. Connect the **first Arduino** (for the **wheel**) via USB.
2. In Arduino IDE:
   - **Tools → Board**: select the correct board (e.g., Arduino Nano)
   - **Tools → Port**: select the matching COM port

### 6.2 Upload Wheel firmware
1. Open `sim_race_pro_wheel_script.ino`
2. **Sketch → Upload** (Ctrl+U)
3. Wait for **“Upload complete.”**

### 6.3 Upload Box/Dashboard firmware
1. Disconnect the first Arduino, connect the **second Arduino** (box)
2. **Tools → Port**: select the **new** COM port
3. Open `sim_race_pro_box_script.ino`
4. **Sketch → Upload**
5. Wait for **“Upload complete.”**

> **Tip:** Note which COM port belongs to the **box** (you’ll set it in Python).

---

## 7) Configure `sim_race_pro_script.py`

Open the file and adjust the configuration block (near the top), e.g.:

```python
# Serial port to the Arduino that communicates with the PC
SERIAL_PORT = 'COM6'   # <-- change to your actual COM port
BAUD_RATE   = 115200

# Feature toggles and tuning
SEND_DATA            = True
TX_RATE_HZ           = 20     # 20 Hz (every 50 ms)
HANDBRAKE_ENABLED    = False
MANUAL_TX_ENABLED    = False
KEYBOARD_SIM_ENABLED = True

# Steering mapping (wheel -> virtual gamepad)
ANGLE_MIN            = -450.0
ANGLE_MAX            =  450.0
ANGLE_DEADZONE_DEG   = 0.5
STEER_GAIN           = 3.5    # increase if you hit max stick too early/late
```

**What it does:**
- Creates a **virtual Xbox controller** (via `vgamepad`)
- Maps wheel angle to **left joystick**, throttle to **RT**, brake to **LT**
- Sends control and status data to the **box Arduino** over serial

---

## 8) Run the simulator

1. Ensure **both Arduinos** are powered via USB.
2. In the project folder:
   ```powershell
   python sim_race_pro_script.py
   ```
3. Expected logs (examples):
   - `Virtual gamepad ready`
   - `Serial open on COMxx @ 115200`

If you see errors, check **Troubleshooting** below.

---

## 9) Quick checks

### 9.1 Find the correct COM port (Windows)
- Open **Device Manager**, expand **Ports (COM & LPT)**
- Plug/unplug the Arduino to see which **COM** appears/disappears

### 9.2 Verify packages at once
```powershell
pip list | findstr /I "pyserial vgamepad keyboard"
```

### 9.3 Verify ViGEm (virtual controller)
- When the script is running, open **Windows Game Controllers** and you should see an Xbox controller

---

## 10) Tuning tips

- **Steering sensitivity**: increase `STEER_GAIN` if the virtual stick reaches full left/right too late; decrease if it saturates too early.
- **Deadzone**: adjust `ANGLE_DEADZONE_DEG` to smooth small jitters around center.
- **Handbrake / H-Pattern**: set `HANDBRAKE_ENABLED = True` or `MANUAL_TX_ENABLED = True` and calibrate thresholds (see comments in the script).

---

## 11) Troubleshooting

- **Virtual gamepad not detected**
  - Install **ViGEmBus**, then **reboot**
  - Ensure the script prints something like: `Virtual gamepad ready`

- **Serial port errors**
  - Wrong **COM** in `SERIAL_PORT`
  - Port busy (another app open)
  - Cable/board issue or mismatched `BAUD_RATE`

- **Lag or instability**
  - Lower `TX_RATE_HZ` (e.g., 10–15)
  - Disable verbose logs
  - Close other tools using the same COM port

---

## 12) Data flow overview

- **Arduino → PC (serial)**: wheel angle, pedals, buttons (and optional IMU gx/gy)
- **PC**:
  - Emulates a **virtual Xbox controller**
  - Builds and sends control data frames
- **PC → Arduino (box, serial)**: sends compact frame  
  ```
  gx-gy-gz-yaw-pitch-roll-speed-gear-rpm-oncurb-curbside-rumble-pwmsx-pwmdx\n
  ```

---

## 13) Support checklist (before asking for help)

- Python 3.11/3.12 installed and on PATH
- `pyserial`, `vgamepad`, `keyboard` installed
- ViGEmBus installed and system restarted
- Correct `SERIAL_PORT` set for the **box** Arduino

---

**Enjoy your drive!**
