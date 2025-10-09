import socket
import struct
import time

# Formati base
HDR_STRUCT = struct.Struct("<HBBBBBQfIIBB")
CAR_MOTION_STRUCT = struct.Struct("<ffffffhhhhhhffffff")  # 60 bytes
CAR_TELEM_STRUCT = struct.Struct("<HfffBbHBBH4H4B4BH4f4B")  # 60 bytes

PACKET_ID_MOTION = 0
PACKET_ID_CARTELEM = 6

def parse_header(buf):
    (packetFormat, gameYear, gameMajor, gameMinor,
     packetVersion, packetId, sessionUID, sessionTime,
     frameId, overallFrameId, playerCarIndex, secondaryIndex) = HDR_STRUCT.unpack_from(buf, 0)
    return packetId, playerCarIndex, sessionTime, HDR_STRUCT.size

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 20777))
    print("Listening for F1 24 telemetry on UDP 20777...")
    g_lat = g_long = g_vert = roll = pitch = yaw = 0.0
    surface_FL = surface_FR = surface_RL = surface_RR = 0

    while True:
        buf, _ = sock.recvfrom(2048)
        packetId, playerIdx, sessionTime, base = parse_header(buf)

        # Motion packet
        if packetId == PACKET_ID_MOTION:
            start = base + playerIdx * CAR_MOTION_STRUCT.size
            if start + CAR_MOTION_STRUCT.size <= len(buf):
                (_px, _py, _pz, _vx, _vy, _vz,
                 _fx, _fy, _fz, _rx, _ry, _rz,
                 g_lat, g_long, g_vert, yaw, pitch, roll) = CAR_MOTION_STRUCT.unpack_from(buf, start)

        # Car Telemetry packet
        elif packetId == PACKET_ID_CARTELEM:
            start = base + playerIdx * CAR_TELEM_STRUCT.size
            if start + CAR_TELEM_STRUCT.size <= len(buf):
                data = CAR_TELEM_STRUCT.unpack_from(buf, start)
                # surfaceType[4] = data[-4:]
                surface_FL, surface_FR, surface_RL, surface_RR = data[-4:]

        # Rilevamento cordolo
        curbs = [surface_FL, surface_FR, surface_RL, surface_RR]
        on_curb = any(s == 1 for s in curbs)
        side = "left" if g_lat < -0.5 else ("right" if g_lat > 0.5 else "center")

        print(f"Time: {sessionTime:7.2f}s | Yaw {yaw:+6.3f} | Pitch {pitch:+6.3f} | Roll {roll:+6.3f} "
              f"| G_lat {g_lat:+5.2f} | G_long {g_long:+5.2f} "
              f"| Curb: {on_curb} ({side})", end="\r", flush=True)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
