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


# =========================
# F1 24 — UDP reader
# =========================
class F1TelemetryReader:
    """
    Minimal F1 24 UDP reader:
    - Binds to 0.0.0.0:20777 (change port if needed).
    - Parses Motion (id=0) for G-forces, yaw/pitch/roll.
    - Parses Car Telemetry (id=6) for speed, throttle, brake, steer, gear, rpm, surface types.
    Returns a TelemetryFrame with a best-effort curb detection and side estimate from lateral G.
    """
    PACKET_ID_MOTION = 0
    PACKET_ID_TELEM  = 6

    # Header: <HBBBBBQfIIBB  (matches your f1_telemetry_test.py)
    HDR = struct.Struct("<HBBBBBQfIIBB")

    # Car motion struct (60 bytes) — g_lat/g_lon/g_vert + yaw/pitch/roll at the end
    CAR_MOTION = struct.Struct("<ffffffhhhhhhffffff")

    # Car telemetry struct (60 bytes) — speed, throttle, steer, brake, gear, rpm, surface types, etc.
    CAR_TELEM  = struct.Struct("<HfffBbHBBH4H4B4BH4f4B")

    def __init__(self, host: str = "0.0.0.0", port: int = 20777):
        self.addr = (host, port)
        self.sock: Optional[socket.socket] = None
        self.player_idx = 0
        self._last_motion: dict = {}
        self._last_telem: dict = {}

    def start(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(0.02)
        sock.bind(self.addr)
        self.sock = sock

    def _parse_header(self, buf: bytes) -> Tuple[int, int, int]:
        (packetFormat, gameYear, gameMajor, gameMinor,
         packetVersion, packetId, sessionUID, sessionTime,
         frameId, overallFrameId, playerCarIndex, secondaryIndex) = self.HDR.unpack_from(buf, 0)
        return packetId, playerCarIndex, self.HDR.size

    def read_frame(self, timeout_s: float = 0.05) -> Optional[TelemetryFrame]:
        if not self.sock:
            return None
        t0 = time.time()

        # Read multiple packets within timeout to refresh both Motion and Telemetry
        while time.time() - t0 < timeout_s:
            try:
                buf, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                break
            if len(buf) < self.HDR.size:
                continue

            packetId, playerIdx, base = self._parse_header(buf)

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
                    # surface types (last 4 bytes): 1=kerb in most docs
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
            game="F1 24",
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

        return TelemetryFrame(
            game="Assetto Corsa Competizione",
            speed_kmh=float(getattr(phy, "speed_kmh", 0.0)),
            gear=int(getattr(phy, "gear", 0)),
            throttle=float(getattr(phy, "gas", 0.0)),
            brake=float(getattr(phy, "brake", 0.0)),
            steer=None,  # You can compute from steerAngle/lock if needed later
            rpm=int(getattr(phy, "rpms", 0)),
            g_lat=g_lat, g_lon=g_lon, g_vert=g_vert,
            on_curb=on_curb,
            curb_side=None  # ACC doesn't directly expose left/right curb
        )

    def close(self) -> None:
        if self.asm:
            try:
                self.asm.close()
            finally:
                self.asm = None
