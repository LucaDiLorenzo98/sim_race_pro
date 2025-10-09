import socket
import struct
import time
import math
import sys

# ===== Config =====
UDP_IP = "0.0.0.0"
UDP_PORT = 20777
PRINT_EVERY_S = 0.2  # frequenza aggiornamento console

# ===== Header (F1 24) =====
# HBBBBB Q f I I B B
#  ^     ^    ^ ^ ^ ^
#  |     |    | | | +-- secondaryPlayerCarIndex
#  |     |    | | +---- playerCarIndex
#  |     |    | +------ overallFrameId
#  |     |    +-------- frameId
#  |     +------------- sessionTime
#  +------------------- sessionUID
HDR_STRUCT = struct.Struct("<HBBBBBQfIIBB")

# ===== Car Motion (per singola vettura) =====
# Floats: worldPosX, worldPosY, worldPosZ, worldVelX, worldVelY, worldVelZ
# Shorts: worldForwardDirX/Y/Z, worldRightDirX/Y/Z (normalizzati: short/32767.0)
# Floats: gForceLateral, gForceLongitudinal, gForceVertical, yaw, pitch, roll
CAR_MOTION_STRUCT = struct.Struct("<ffffffhhhhhhffffff")  # 60 bytes

# ===== Car Telemetry (per singola vettura) =====
#  speed(KPH)=H, throttle=f, steer=f, brake=f, clutch=B, gear=b,
#  engineRPM=H, drs=B, revLightsPercent=B,
#  brakesTemp[4]=4H, tyresSurfaceTemp[4]=4B, tyresInnerTemp[4]=4B,
#  engineTemp=H, tyresPressure[4]=4f, surfaceType[4]=4B
CAR_TELEM_STRUCT = struct.Struct("<HfffBbHBB4H4B4BH4f4B")  # 60 bytes

PACKET_ID_MOTION = 0
PACKET_ID_CARTELEM = 6

SURFACE_NAME = {
    0: "Tarmac", 1: "RumbleStrip", 2: "Concrete", 3: "Rock",
    4: "Gravel", 5: "Mud", 6: "Sand", 7: "Grass",
    8: "Water",  9: "Cobblestone", 10: "Metal", 11: "Ridged"
}

def parse_header(buf):
    (packetFormat, gameYear, gameMajor, gameMinor,
     packetVersion, packetId, sessionUID, sessionTime,
     frameId, overallFrameId, playerCarIndex, secondaryIndex) = HDR_STRUCT.unpack_from(buf, 0)
    return {
        "packetFormat": packetFormat,
        "gameYear": gameYear,
        "gameMajor": gameMajor,
        "gameMinor": gameMinor,
        "packetVersion": packetVersion,
        "packetId": packetId,
        "sessionUID": sessionUID,
        "sessionTime": sessionTime,
        "frameId": frameId,
        "overallFrameId": overallFrameId,
        "playerCarIndex": playerCarIndex,
        "secondaryPlayerCarIndex": secondaryIndex,
        "headerSize": HDR_STRUCT.size
    }

def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v

def short_to_unit(s):
    return float(s) / 32767.0

def gear_label(gear):
    # F1: -1=R, 0=N, 1..8 gears
    if gear == -1: return "R"
    if gear == 0:  return "N"
    return str(gear)

def clear_screen():
    # stampa “a blocchi”: pulizia semplice cross-platform
    sys.stdout.write("\x1b[2J\x1b[H")  # clear + home
    sys.stdout.flush()

def print_dashboard(state):
    clear_screen()
    h = state["header"]
    m = state["motion"]
    t = state["telem"]

    # Header / Session
    print("=== Session / Header ===")
    print(f"Packet: ID={h['packetId']} | Format={h['packetFormat']} | Game={h['gameYear']}.{h['gameMajor']}.{h['gameMinor']} | Vers={h['packetVersion']}")
    print(f"Session UID: {h['sessionUID']}")
    print(f"Time: {h['sessionTime']:.2f}s  | Frame: {h['frameId']}  | OverallFrame: {h['overallFrameId']}")
    print(f"Player Car Index: {h['playerCarIndex']}  | Secondary: {h['secondaryPlayerCarIndex']}")
    print()

    # Motion
    print("=== Motion (player car) ===")
    print(f"World Pos:    X={m['pos'][0]:+8.3f}  Y={m['pos'][1]:+8.3f}  Z={m['pos'][2]:+8.3f}")
    print(f"World Vel:    X={m['vel'][0]:+8.3f}  Y={m['vel'][1]:+8.3f}  Z={m['vel'][2]:+8.3f}")
    print(f"Forward Dir:  X={m['fwd'][0]:+6.3f}  Y={m['fwd'][1]:+6.3f}  Z={m['fwd'][2]:+6.3f}   (unit)")
    print(f"Right Dir:    X={m['rgt'][0]:+6.3f}  Y={m['rgt'][1]:+6.3f}  Z={m['rgt'][2]:+6.3f}   (unit)")
    print(f"G-Forces:     Lat={m['g_lat']:+5.2f}  Long={m['g_long']:+5.2f}  Vert={m['g_vert']:+5.2f}")
    print(f"Attitude:     Yaw={m['yaw']:+7.3f} rad  Pitch={m['pitch']:+7.3f} rad  Roll={m['roll']:+7.3f} rad")
    print()

    # Telemetry
    print("=== Car Telemetry (player car) ===")
    print(f"Speed: {t['speed_kph']:>4} km/h   | Engine: {t['engineRPM']:>5} rpm   | Gear: {gear_label(t['gear'])}   | DRS: {('ON' if t['drs'] else 'OFF')}")
    print(f"Inputs: Throttle={t['throttle']*100:5.1f}%  | Brake={t['brake']*100:5.1f}%  | Steer={t['steer']:+6.3f}  | Clutch={t['clutch']:3}%")
    print(f"Rev Lights: {t['revLightsPercent']:>3}%   | Engine Temp: {t['engineTemp']} °C")
    print()

    # Brakes & Tyres
    print("=== Temperatures ===")
    bl, br, bf, bt = t['brakesTemp']  # FL, FR, RL, RR? (Codies usa ordine RL, RR, FL, FR o FL, FR, RL, RR a seconda del pacchetto)
    tsl = t['tyresSurfaceTemp']
    til = t['tyresInnerTemp']
    print(f"Brakes °C:   FL={bl:3}  FR={br:3}  RL={bf:3}  RR={bt:3}")
    print(f"Tyre Surf °C:FL={tsl[0]:3}  FR={tsl[1]:3}  RL={tsl[2]:3}  RR={tsl[3]:3}")
    print(f"Tyre Inner °C:FL={til[0]:3}  FR={til[1]:3}  RL={til[2]:3}  RR={til[3]:3}")
    print()

    print("=== Tyre Pressures (bar) ===")
    p = t['tyresPressure']
    print(f"FL={p[0]:4.2f}  FR={p[1]:4.2f}  RL={p[2]:4.2f}  RR={p[3]:4.2f}")
    print()

    # Surfaces & Curb
    print("=== Surface Types ===")
    st = t['surfaceType']
    st_names = [SURFACE_NAME.get(x, f"#{x}") for x in st]
    print(f"FL={st_names[0]:>12}  FR={st_names[1]:>12}  RL={st_names[2]:>12}  RR={st_names[3]:>12}")

    on_curb = any(x == 1 for x in st)  # 1=RumbleStrip
    side = "left" if m['g_lat'] < -0.5 else ("right" if m['g_lat'] > 0.5 else "center")
    print(f"Curb: {'YES' if on_curb else 'NO '}   | Estimated side: {side}")
    print()

    # Footer / timing
    print(f"Updated: {time.strftime('%H:%M:%S')}  | PacketID: {h['packetId']}")

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((UDP_IP, UDP_PORT))
    print(f"Listening for F1 24 telemetry on UDP {UDP_PORT} ...")

    # Stato condiviso
    header = {
        "packetFormat": 0, "gameYear": 0, "gameMajor": 0, "gameMinor": 0,
        "packetVersion": 0, "packetId": 0, "sessionUID": 0, "sessionTime": 0.0,
        "frameId": 0, "overallFrameId": 0, "playerCarIndex": 0, "secondaryPlayerCarIndex": 255,
        "headerSize": HDR_STRUCT.size
    }
    motion = {
        "pos": (0.0, 0.0, 0.0),
        "vel": (0.0, 0.0, 0.0),
        "fwd": (0.0, 0.0, 0.0),
        "rgt": (0.0, 0.0, 0.0),
        "g_lat": 0.0, "g_long": 0.0, "g_vert": 0.0,
        "yaw": 0.0, "pitch": 0.0, "roll": 0.0
    }
    telem = {
        "speed_kph": 0, "throttle": 0.0, "steer": 0.0, "brake": 0.0,
        "clutch": 0, "gear": 0, "engineRPM": 0, "drs": 0, "revLightsPercent": 0,
        "brakesTemp": (0, 0, 0, 0),
        "tyresSurfaceTemp": (0, 0, 0, 0),
        "tyresInnerTemp": (0, 0, 0, 0),
        "engineTemp": 0,
        "tyresPressure": (0.0, 0.0, 0.0, 0.0),
        "surfaceType": (0, 0, 0, 0),
    }

    last_print = 0.0

    while True:
        buf, _ = sock.recvfrom(2048)
        if len(buf) < HDR_STRUCT.size:
            continue

        h = parse_header(buf)
        header.update(h)

        pid = h["packetId"]
        idx = h["playerCarIndex"]
        base = h["headerSize"]

        # Motion
        if pid == PACKET_ID_MOTION:
            start = base + idx * CAR_MOTION_STRUCT.size
            if start + CAR_MOTION_STRUCT.size <= len(buf):
                (px, py, pz, vx, vy, vz,
                 fdx, fdy, fdz, rdx, rdy, rdz,
                 g_lat, g_long, g_vert, yaw, pitch, roll) = CAR_MOTION_STRUCT.unpack_from(buf, start)

                motion["pos"] = (px, py, pz)
                motion["vel"] = (vx, vy, vz)
                motion["fwd"] = (short_to_unit(fdx), short_to_unit(fdy), short_to_unit(fdz))
                motion["rgt"] = (short_to_unit(rdx), short_to_unit(rdy), short_to_unit(rdz))
                motion["g_lat"] = g_lat
                motion["g_long"] = g_long
                motion["g_vert"] = g_vert
                motion["yaw"] = yaw
                motion["pitch"] = pitch
                motion["roll"] = roll

        # Car Telemetry
        elif pid == PACKET_ID_CARTELEM:
            start = base + idx * CAR_TELEM_STRUCT.size
            if start + CAR_TELEM_STRUCT.size <= len(buf):
                data = CAR_TELEM_STRUCT.unpack_from(buf, start)

                # unpack nominativo (vedi layout in testa)
                off = 0
                speed = data[off]; off += 1
                throttle = data[off]; off += 1
                steer = data[off]; off += 1
                brake = data[off]; off += 1
                clutch = data[off]; off += 1
                gear = data[off]; off += 1
                engineRPM = data[off]; off += 1
                drs = data[off]; off += 1
                revLightsPercent = data[off]; off += 1

                brakesTemp = tuple(data[off:off+4]); off += 4
                tyresSurfaceTemp = tuple(data[off:off+4]); off += 4
                tyresInnerTemp = tuple(data[off:off+4]); off += 4

                engineTemp = data[off]; off += 1
                tyresPressure = tuple(data[off:off+4]); off += 4
                surfaceType = tuple(data[off:off+4]); off += 4

                telem["speed_kph"] = int(speed)
                telem["throttle"] = float(throttle)
                telem["steer"] = float(steer)
                telem["brake"] = float(brake)
                telem["clutch"] = int(clutch)
                telem["gear"] = int(gear)
                telem["engineRPM"] = int(engineRPM)
                telem["drs"] = int(drs)
                telem["revLightsPercent"] = int(revLightsPercent)
                telem["brakesTemp"] = brakesTemp
                telem["tyresSurfaceTemp"] = tyresSurfaceTemp
                telem["tyresInnerTemp"] = tyresInnerTemp
                telem["engineTemp"] = int(engineTemp)
                telem["tyresPressure"] = tyresPressure
                telem["surfaceType"] = surfaceType

        # stampa cruscotto
        now = time.time()
        if now - last_print >= PRINT_EVERY_S:
            print_dashboard({"header": header, "motion": motion, "telem": telem})
            last_print = now


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
