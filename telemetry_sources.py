# telemetry_sources.py
# Readers for F1 24 (UDP) and Assetto Corsa Competizione (shared memory).
# Returns a unified TelemetryFrame so your main script can stay game-agnostic.

from __future__ import annotations
from dataclasses import dataclass
from typing import Optional, Tuple
import time
import socket
import struct

# =========================
# Unified Telemetry Frame
# =========================
@dataclass(slots=True)
class TelemetryFrame:
    game: str
    speed_kmh: float = 0.0
    gear: int = 0                 # -1=R, 0=N, 1..8
    throttle: float = 0.0         # 0..1
    brake: float = 0.0            # 0..1
    steer: Optional[float] = None # -1..1 if available
    rpm: Optional[int] = None
    g_lat: Optional[float] = None
    g_lon: Optional[float] = None
    g_vert: Optional[float] = None
    on_curb: Optional[bool] = None
    curb_side: Optional[str] = None  # "left" | "right" | "center" | None
    rumble: float = 0.0           # 0..1 generic vibration intensity


# =========================
# F1 23/24/25 — UDP reader
# =========================
class F1TelemetryReader:
    """
    F1 UDP reader (Supports 2023, 2024, 2025 formats):
    - Binds to 0.0.0.0:20777.
    - Expects UDP Format '2023' or newer in game settings.
    - Header is 29 bytes (ver 2023+).
    """
    PACKET_ID_MOTION = 0
    PACKET_ID_TELEM  = 6

    # Header: <HBBBBBQfIIBB (29 bytes)
    HDR = struct.Struct("<HBBBBBQfIIBB")

    # Car motion struct (60 bytes)
    CAR_MOTION = struct.Struct("<ffffffhhhhhhffffff")

    # Car telemetry struct (60 bytes)
    CAR_TELEM  = struct.Struct("<HfffBbHBBH4H4B4BH4f4B")

    def __init__(self, host: str = "0.0.0.0", port: int = 20777):
        self.addr = (host, port)
        self.sock: Optional[socket.socket] = None
        self.player_idx = 0
        self._last_motion: dict = {}
        self._last_telem: dict = {}
        self._format_detected = False

    def start(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(0.02)
        sock.bind(self.addr)
        self.sock = sock
        print(f"F1 Reader started on {self.addr}. Waiting for F1 23/24/25 packets...", flush=True)

    def _parse_header(self, buf: bytes) -> Tuple[int, int, int]:
        (packetFormat, gameYear, gameMajor, gameMinor,
         packetVersion, packetId, sessionUID, sessionTime,
         frameId, overallFrameId, playerCarIndex, secondaryIndex) = self.HDR.unpack_from(buf, 0)
        
        if not self._format_detected:
            print(f"F1 Packet Detected! Year: {packetFormat} (Game: {gameYear})", flush=True)
            self._format_detected = True
            
        return packetId, playerCarIndex, self.HDR.size

    def read_frame(self, timeout_s: float = 0.05) -> Optional[TelemetryFrame]:
        if not self.sock:
            return None
        t0 = time.time()

        # Read multiple packets within timeout
        while time.time() - t0 < timeout_s:
            try:
                buf, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                break
                
            # Check for 2023+ Header Size (29 bytes)
            # If user uses "2022" format (24 bytes), this check fails intentionally to avoid struct errors
            if len(buf) < self.HDR.size:
                continue

            try:
                packetId, playerIdx, base = self._parse_header(buf)
            except struct.error:
                continue # Skip malformed

            if packetId == self.PACKET_ID_MOTION:
                start = base + playerIdx * self.CAR_MOTION.size
                if start + self.CAR_MOTION.size <= len(buf):
                    (_px,_py,_pz,_vx,_vy,_vz,_fx,_fy,_fz,_rx,_ry,_rz,
                     g_lat, g_lon, g_vert, yaw, pitch, roll) = self.CAR_MOTION.unpack_from(buf, start)
                    self._last_motion.update(dict(g_lat=g_lat, g_lon=g_lon, g_vert=g_vert,
                                                  yaw=yaw, pitch=pitch, roll=roll))
            elif packetId == self.PACKET_ID_TELEM:
                start = base + playerIdx * self.CAR_TELEM.size
                if start + self.CAR_TELEM.size <= len(buf):
                    data = self.CAR_TELEM.unpack_from(buf, start)
                    speed = float(data[0])
                    throttle = float(data[1])
                    steer = float(data[2])
                    brake = float(data[3])
                    gear = int(data[5])
                    rpm = int(data[6])
                    surfaces = data[-4:]
                    on_curb = any(s == 1 for s in surfaces)

                    # side estimate from g_lat
                    g_lat = self._last_motion.get("g_lat", None)
                    if g_lat is None:
                        curb_side = None
                    else:
                        curb_side = "left" if g_lat < -0.5 else ("right" if g_lat > 0.5 else "center")

                    self._last_telem.update(dict(
                        speed_kmh=speed, throttle=throttle, brake=brake, steer=steer,
                        gear=gear, rpm=rpm, on_curb=on_curb, curb_side=curb_side
                    ))

        if not self._last_telem:
            return None

        return TelemetryFrame(
            game=f"F1 20{self._last_telem.get('year', 'xx')}", # Generic
            speed_kmh=self._last_telem["speed_kmh"],
            gear=self._last_telem["gear"],
            throttle=self._last_telem["throttle"],
            brake=self._last_telem["brake"],
            steer=self._last_telem["steer"],
            rpm=self._last_telem["rpm"],
            g_lat=self._last_motion.get("g_lat"),
            g_lon=self._last_motion.get("g_lon"),
            g_vert=self._last_motion.get("g_vert"),
            on_curb=self._last_telem["on_curb"],
            curb_side=self._last_telem["curb_side"],
            rumble=0.0
        )

    def close(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            finally:
                self.sock = None


# =========================
# ACC — Shared memory reader
# =========================
class ACCTelemetryReader:
    """
    ACC shared memory reader via pyaccsharedmemory.
    pip install pyaccsharedmemory
    Reads Physics block for speed/gas/brake/gear/rpms and G-forces.
    kerb_vibration > small threshold -> on_curb True.
    """
    def __init__(self):
        self.asm = None

    def start(self) -> None:
        try:
            from pyaccsharedmemory import accSharedMemory
        except ImportError as e:
            raise RuntimeError("pyaccsharedmemory not installed. pip install pyaccsharedmemory") from e
        self.accSharedMemory = accSharedMemory  # store class
        self.asm = self.accSharedMemory()

    def read_frame(self, timeout_s: float = 0.05) -> Optional[TelemetryFrame]:
        if not self.asm:
            return None
        # no active wait; ACC SHM is always the latest snapshot
        sm = self.asm.read_shared_memory()
        if not sm or not sm.Physics:
            # brief sleep to avoid busy-wait if desired
            if timeout_s > 0:
                time.sleep(min(timeout_s, 0.01))
            return None

        phy = sm.Physics
        g = getattr(phy, "g_force", None)
        g_lat = float(getattr(g, "x", 0.0)) if g else 0.0
        g_lon = float(getattr(g, "y", 0.0)) if g else 0.0
        g_vert = float(getattr(g, "z", 0.0)) if g else 0.0

        on_curb = bool(getattr(phy, "kerb_vibration", 0.0) > 0.02)

        # ACC Gears: 0=R, 1=N, 2=1st ... Script expects: -1=R, 0=N, 1=1st ...
        raw_gear = int(getattr(phy, "gear", 0))
        fixed_gear = raw_gear - 1

        # Vibration / Rumble Logic
        # AC provides specific vibrations. We sum them up for a "rich" feel.
        v_kerb = float(getattr(phy, "kerb_vibration", 0.0))
        v_slip = float(getattr(phy, "slip_vibration", 0.0))
        v_g = float(getattr(phy, "g_vibration", 0.0))
        v_abs = float(getattr(phy, "abs_vibration", 0.0))
        
        # Combined rumble (clamped 0..1 generic scale here, multiplied by 255 later)
        total_rumble = (v_kerb * 1.0) + (v_slip * 0.5) + (v_g * 0.2) + (v_abs * 0.8)
        
        # Force on_curb if kerb vibe is significant
        if v_kerb > 0.05: on_curb = True

        return TelemetryFrame(
            game="Assetto Corsa Competizione",
            speed_kmh=float(getattr(phy, "speed_kmh", 0.0)),
            gear=fixed_gear,
            throttle=float(getattr(phy, "gas", 0.0)),
            brake=float(getattr(phy, "brake", 0.0)),
            steer=None,  # You can compute from steerAngle/lock if needed later
            rpm=int(getattr(phy, "rpms", 0)),
            g_lat=g_lat, g_lon=g_lon, g_vert=g_vert,
            on_curb=on_curb,
            curb_side=None,  # ACC doesn't directly expose left/right curb
            rumble=total_rumble
        )

    def close(self) -> None:
        if self.asm:
            try:
                self.asm.close()

# =========================
# Forza Horizon/Motorsport — UDP reader
# =========================
class ForzaTelemetryReader:
    """
    Forza "Data Out" Reader.
    Supports Forza Horizon 4/5 and Motorsport 7.
    - Default Port: 5300 (or user configured)
    - Data Out format (Sled/Dash) ~324 bytes usually (Standard 'Data Out' in settings).
    """

    # We use a struct to unpack the standard 'Data Out' format (Dash).
    # Format: s32 IsRaceOn, u32 TimestampMS, f32 MaxRPM, f32 IdleRPM, f32 CurRPM ...
    
    def __init__(self, host: str = "0.0.0.0", port: int = 5300):
        self.addr = (host, port)
        self.sock: Optional[socket.socket] = None

    def start(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(0.02)
        sock.bind(self.addr)
        self.sock = sock
        print(f"Forza Reader started on {self.addr}. Set 'Data Out' in game options.", flush=True)

    def read_frame(self, timeout_s: float = 0.05) -> Optional[TelemetryFrame]:
        if not self.sock: return None
        t0 = time.time()
        
        last_data = None
        while time.time() - t0 < timeout_s:
            try:
                buf, _ = self.sock.recvfrom(1024)
                if len(buf) >= 311:
                    last_data = buf
            except socket.timeout:
                break
        
        if not last_data: return None

        # Helper to unpack
        def get_f32(offset): return struct.unpack_from("<f", last_data, offset)[0]
        def get_u8(offset):  return struct.unpack_from("<B", last_data, offset)[0]
        def get_s8(offset):  return struct.unpack_from("<b", last_data, offset)[0]
        
        # Forza 'Dash' is usually 324 bytes. 'Sled' is 232.
        # We try to detect by size or just try parsing.
        # If it's the full Dash format:
        
        # RPM at 16
        rpm = get_f32(16)
        
        # Speed.. often we can use Magnitude of Velocity(X,Y,Z) at 32,36,40
        vx = get_f32(32)
        vy = get_f32(36)
        vz = get_f32(40)
        speed_ms = (vx*vx + vy*vy + vz*vz) ** 0.5
        speed_kmh = speed_ms * 3.6
        
        # Accel/G-force from 20,24,28
        # These are usually in m/s^2. 1G ~= 9.8
        gx = get_f32(20) / 9.8
        gy = get_f32(24) / 9.8
        gz = get_f32(28) / 9.8
        
        # Inputs & Gear are at the end of the Dash packet (offset ~300+)
        # If the packet is large enough (>= 311 or 324)
        if len(last_data) > 300:
            gear_raw = get_u8(307) # 0=R, 1=N, 2=1st... ? or 11=N?
            # Forza: 0=Reverse, 11=Neutral? No, check specific version.
            # Usually: 0=R, 11=N (in some), or just 0=R, 1=N, 2=1st.
            # Let's map standard assumption:
            # 0=Reverse, 11=Neutral (common in Forza), 1..10 = Gears.
            # BUT older Forza or some settings: 0=R, 1=N, 2=1...
            # We'll try to deduce or just map 0->R, 11->N for now.
            # Actually, most docs say: 0 = Reverse, 11 = Neutral, 1-10 = Gears 1-10.
            
            gear = 0
            if gear_raw == 0: gear = -1
            elif gear_raw == 11: gear = 0
            else: gear = gear_raw
            
            accel_in = get_u8(303) / 255.0
            brake_in = get_u8(304) / 255.0
            steer_in = get_s8(308) / 127.0
        else:
            # Fallback for Sled (no inputs/gear in Sled usually? Sled is motion only?)
            # Sled actually has CarOrdinal etc but maybe not inputs.
            gear = 0
            accel_in = 0
            brake_in = 0
            steer_in = 0

        # Rumble / Curb
        # SurfaceRumble can be used. It's an array of 4 floats at offset 188?
        # 4*4 ints (WheelOnRumble) at 132 maybe?
        # Offsets:
        # 0: RaceOn .. 16: RPM
        # 20: Accel .. 32: Vel .. 44: AngVel .. 56: Yaw
        # 68: NormSusp (4)
        # 84: SlipRatio (4)
        # 100: WheelRot (4)
        # 116: WheelOnRumble (4 ints) -> 116, 120, 124, 128
        
        rumble_fl = struct.unpack_from("<i", last_data, 116)[0]
        rumble_fr = struct.unpack_from("<i", last_data, 120)[0]
        rumble_rl = struct.unpack_from("<i", last_data, 124)[0]
        rumble_rr = struct.unpack_from("<i", last_data, 128)[0]
        
        on_curb = (rumble_fl + rumble_fr + rumble_rl + rumble_rr) > 0
        
        # Calculate total rumble intensity from TireSlip or SurfaceRumble if available
        # SurfaceRumble (4 floats) at 148?
        # 116 (OnRumble - 4s32 = 16 bytes) -> 132
        # 132 (Puddle - 4f32 = 16 bytes) -> 148
        # 148 (SurfaceRumble - 4f32 = 16 bytes) -> 164
        
        surf_fl = struct.unpack_from("<f", last_data, 148)[0]
        surf_fr = struct.unpack_from("<f", last_data, 152)[0]
        
        rumble_intensity = max(surf_fl, surf_fr) / 10.0 # Scaling guess
        rumble_intensity = min(max(rumble_intensity, 0.0), 1.0)
        
        return TelemetryFrame(
            game="Forza",
            speed_kmh=speed_kmh,
            gear=gear,
            throttle=accel_in,
            brake=brake_in,
            steer=steer_in,
            rpm=int(rpm),
            g_lat=gx, # Forza X is Right? Y is Up? Z is Fwd? 
            # Forza: X=Right, Y=Up, Z=Forward.
            # Standard: g_lat usually means Lateral (Left/Right). X is Lat.
            # g_lon usually means Longitudinal (Accel/Brake). Z is Lon.
            g_lon=gz, 
            g_vert=gy, 
            on_curb=bool(on_curb),
            rumble=rumble_intensity
        )

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None
