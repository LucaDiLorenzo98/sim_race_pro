#ifndef MOTOR_CONTROL_H
#define MOTOR_CONTROL_H

#include <Arduino.h>

// BTS7960 H-bridge pin assignments
extern const int RPWM;  // right / clockwise PWM   (pin 11)
extern const int LPWM;  // left  / CCW    PWM   (pin 10)
extern const int REN;   // right half-bridge enable (pin 9)
extern const int LEN;   // left  half-bridge enable (pin 8)

// Configure motor pins, set timer prescaler to ~31 kHz (silent PWM), enable H-bridge.
void setupMotor();

// Low-level interface (used by sim_race_pro_box_script.ino):
void moveMotorToLeft(int pwm);   // clockwise
void moveMotorToRight(int pwm);  // counter-clockwise
void stopMotor();
void enableMotor();
void disableMotor();

#endif
