# pip install pyaccsharedmemory
from pyaccsharedmemory import accSharedMemory
import time

asm = accSharedMemory()

try:
    while True:
        sm = asm.read_shared_memory()
        if sm is None:
            print("In attesa di ACC...")
            time.sleep(0.5)
            continue

        phy = sm.Physics
        gfx = sm.Graphics
        sta = sm.Static

        print(
            f"Speed: {phy.speed_kmh:6.1f} km/h | "
            f"Gear: {phy.gear:2d} | "
            f"Throttle: {phy.gas:.2f} | Brake: {phy.brake:.2f} | "
            f"Gx/Gy/Gz: ({phy.g_force.x:+.2f},{phy.g_force.y:+.2f},{phy.g_force.z:+.2f}) | "
            f"Kerb vib: {phy.kerb_vibration:.2f}"
        )
        time.sleep(0.02)  # ~50 Hz
finally:
    asm.close()
