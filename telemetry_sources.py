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
            finally:
                self.asm = None
