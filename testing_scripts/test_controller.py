# vgamepad_sanity_check.py
import time, vgamepad as vg

print("Creating VX360 gamepad...")
gp = vg.VX360Gamepad()
gp.update()
print("Created. Now pulsing A, moving stick, pressing triggers...")

# Press A
gp.press_button(button=vg.XUSB_BUTTON.XUSB_GAMEPAD_A); gp.update()
time.sleep(0.2)
gp.release_button(button=vg.XUSB_BUTTON.XUSB_GAMEPAD_A); gp.update()

# Move left stick left -> right
gp.left_joystick(x_value=-32768, y_value=0); gp.update(); time.sleep(0.2)
gp.left_joystick(x_value= 32767, y_value=0); gp.update(); time.sleep(0.2)
gp.left_joystick(x_value=     0, y_value=0); gp.update()

# Triggers
gp.left_trigger(value=200); gp.right_trigger(value=200); gp.update(); time.sleep(0.2)
gp.left_trigger(value=0);   gp.right_trigger(value=0);   gp.update()
print("Done. Check joy.cpl for 'Controller (XBOX 360 For Windows)'.")
