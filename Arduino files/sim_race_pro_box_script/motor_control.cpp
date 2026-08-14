#include "motor_control.h"

const int RPWM = 11;
const int LPWM = 10;
const int REN  =  9;
const int LEN  =  8;

void setupMotor() {
    pinMode(RPWM, OUTPUT);
    pinMode(LPWM, OUTPUT);
    pinMode(REN,  OUTPUT);
    pinMode(LEN,  OUTPUT);
    // Set Timer1+2 prescaler to 1 -> ~31 kHz PWM (eliminates audible motor whine).
    TCCR1B = (TCCR1B & 0b11111000) | 0x01;
    TCCR2B = (TCCR2B & 0b11111000) | 0x01;
    enableMotor();
}

// Low-level: direct PWM value 0-255.
void moveMotorToLeft(int pwm) {
    analogWrite(RPWM, pwm);
    analogWrite(LPWM, 0);
}

void moveMotorToRight(int pwm) {
    analogWrite(RPWM, 0);
    analogWrite(LPWM, pwm);
}

void stopMotor() {
    analogWrite(RPWM, 0);
    analogWrite(LPWM, 0);
}

void enableMotor() {
    digitalWrite(REN, HIGH);
    digitalWrite(LEN, HIGH);
}

void disableMotor() {
    digitalWrite(REN, LOW);
    digitalWrite(LEN, LOW);
}
