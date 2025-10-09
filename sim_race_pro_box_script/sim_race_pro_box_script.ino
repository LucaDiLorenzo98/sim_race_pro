/* Written by Luca Di Lorenzo 01/09/2025
   Latest update: 01/10/2025 (watch version) */

// No need to install SoftwareSerial, but you have to
// install Encoder library (use Arduino IDE Library Manager)

#include <SoftwareSerial.h>
#include <Encoder.h>

const char *FW_VERSION = "ver. 1.1.0";

// #####################################################
// MANDATORY VAR. SETTINGS

// Set true only if you want the wheel without pedals
bool ONLY_WHEEL = false;

// Only one of the following BOX_ variables can be true
bool BOX_FULL = true;    // force-feedback, high-res. encoder
bool BOX_MEDIUM = false; // high-res. encoder
bool BOX_BUDGET = false; // potentiometer rotation sensor

// Set true only if you added the clutch (third pedal)
bool PEDALS_CLUTCH = false;

// These variables enable additional modules
bool HANDBRAKE_ENABLED = false;
bool MANUAL_TX_ENABLED = false;

// #####################################################

// Serial communication variables (master <-> slave MCU)
static const uint8_t RX_PIN = 5; // master RX (connect to slave TX)
static const uint8_t TX_PIN = 6; // master TX (optional to slave RX)
SoftwareSerial link(RX_PIN, TX_PIN);
static const unsigned long LINK_BAUD = 57600; // must match the slave
static const unsigned long USB_BAUD = 115200; // PC Serial Monitor

// High-res. wheel encoder (quadrature on D2,D3)
Encoder myEnc(2, 3);        // A->D2, B->D3
const long maxTicks = 3000; // safety clamp
long zeroOffset = 0;        // ticks to subtract (dynamic zero)

// Throttle and brake (analog)
const uint8_t ACC_PIN = A1;
const uint8_t BRK_PIN = A0;
int ACC_OFFSET = 0;
int BRK_OFFSET = 0;
int ACC_DEADZONE = 10;
int BRK_DEADZONE = 10;

// Motor controller
const int RPWM = 11; // PWM pin (Timer2)
const int LPWM = 10; // PWM pin (Timer1)
const int REN = 9;
const int LEN = 8;

// Force-feedback settings
const int pwm_threshold = 2;
const int pwm_floor = 15;
const int pwm_max = 255;
const int pwm_min = 90;

// Rotation sensor (only BOX_BUDGET)
const int POT_PIN = A4;

// Clutch (third pedal)
const int CLUTCH_PIN = A2;

// Additional modules
const int HANDBRAKE_PIN = 4;       // button between pin and GND
const int MANUAL_TX_POT1_PIN = A7; // X-axis (left/right)
const int MANUAL_TX_POT2_PIN = A6; // Y-axis (up/down)

// Buffers and state
char lineBuf[96]; // slave line ~34 chars; keep margin
uint8_t lineLen = 0;
bool lastResetBit = false;

// Prototypes
bool extractResetBit(const char *s);
int proportionalControlBasic(float degrees, int acc, int brake);
void vibration(int vel, int delayTime);
void moveMotorToRight(int vel);
void moveMotorToLeft(int vel);
void stopMotor();
void enableMotor();
void disableMotor();
char readHandbrakeBit(); // returns '1' when pulled (active-low)

void setup()
{
  // USB serial
  Serial.begin(USB_BAUD);
  delay(50);
  Serial.print(F("[MASTER] "));
  Serial.println(FW_VERSION);

  // Validation rules
  if ((BOX_FULL + BOX_MEDIUM + BOX_BUDGET) != 1)
  {
    Serial.println("SETUP ERROR: Only one BOX_ variable can be true");
    return;
  }

  // Link with slave
  link.begin(LINK_BAUD);

  // Box setup
  if (BOX_FULL == true)
  {
    pinMode(RPWM, OUTPUT);
    pinMode(LPWM, OUTPUT);
    pinMode(REN, OUTPUT);
    pinMode(LEN, OUTPUT);

    // Raise PWM frequency above audible (~31 kHz)
    // TCCR1B = (TCCR1B & 0b11111000) | 0x01;  // Timer1 -> pins 9,10  (LPWM=10)
    // TCCR2B = (TCCR2B & 0b11111000) | 0x01;  // Timer2 -> pins 3,11  (RPWM=11)

    enableMotor();
    Serial.println("Force-feedback setup done");
  }

  if (BOX_FULL == true || BOX_MEDIUM == true)
  {
    zeroOffset = myEnc.read(); // initial zero at current position
    Serial.println("High-res. encoder setup done");
  }

  if (BOX_BUDGET)
  {
    pinMode(POT_PIN, INPUT);
    Serial.println("High-res. encoder setup done");
  }

  // Pedals setup
  if (ONLY_WHEEL == false)
  {
    pinMode(ACC_PIN, INPUT);
    pinMode(BRK_PIN, INPUT);
    ACC_OFFSET = analogRead(ACC_PIN);
    BRK_OFFSET = analogRead(BRK_PIN);
    Serial.println("Pedals setup done");

    if (PEDALS_CLUTCH == true)
    {
      pinMode(CLUTCH_PIN, INPUT);
      Serial.println("Clutch pedal setup done");
    }
  }
  else
  {
    Serial.println("ONLY Wheel option enabled");
  }

  // Additional modules setup
  if (HANDBRAKE_ENABLED)
  {
    pinMode(HANDBRAKE_PIN, INPUT_PULLUP); // button to GND
    Serial.println("Handbrake setup done");
  }
  else
  {
    Serial.println("Handbrake not enabled");
  }

  if (MANUAL_TX_ENABLED)
  {
    pinMode(MANUAL_TX_POT1_PIN, INPUT);
    pinMode(MANUAL_TX_POT2_PIN, INPUT);
    Serial.println("Manual gearbox setup done");
  }
  else
  {
    Serial.println("Manual gearbox not enabled");
  }
}

void loop()
{
  // Read characters from slave until newline
  while (link.available())
  {
    char ch = (char)link.read();

    if (ch == '\n')
    {
      if (lineLen > 0 && lineBuf[lineLen - 1] == '\r')
        lineLen--;
      lineBuf[lineLen] = '\0';

      bool resetBit = extractResetBit(lineBuf);
      if (resetBit && !lastResetBit)
      {
        zeroOffset = myEnc.read();
        ACC_OFFSET = analogRead(ACC_PIN);
        BRK_OFFSET = analogRead(BRK_PIN);
      }
      lastResetBit = resetBit;

      long ticks = myEnc.read() - zeroOffset;
      if (ticks > maxTicks)
        ticks = maxTicks;
      if (ticks < -maxTicks)
        ticks = -maxTicks;
      float degrees = (ticks / 2400.0f) * 360.0f; // 2400 ticks = 360°

      int acc = abs(analogRead(ACC_PIN) - ACC_OFFSET);
      int brk = abs(analogRead(BRK_PIN) - BRK_OFFSET);

      acc = constrain(map(acc, 0, 109, 0, 255), 0, 255);
      brk = constrain(map(brk, 0, 109, 0, 255), 0, 255);

      if (acc > 255)
        acc = 255;
      if (brk > 255)
        brk = 255;

      if (acc < ACC_DEADZONE)
        acc = 0;
      if (brk < BRK_DEADZONE)
        brk = 0;

      proportionalControlBasic(degrees, acc, brk);

      // Handbrake and manual transmission raw analogs for host
      char hbBit = '0';
      if (HANDBRAKE_ENABLED)
      {
        hbBit = readHandbrakeBit();
      }
      else
      {
        hbBit = '0';
      }

      int gx255 = 0;
      int gy255 = 0;
      if (MANUAL_TX_ENABLED)
      {
        int gx = analogRead(MANUAL_TX_POT1_PIN); // 0..1023
        int gy = analogRead(MANUAL_TX_POT2_PIN); // 0..1023
        gx255 = constrain(map(gx, 0, 1023, 0, 255), 0, 255);
        gy255 = constrain(map(gy, 0, 1023, 0, 255), 0, 255);
      }

      /*
       * SERIAL LINE FORMAT (USB Serial to PC)
       *
       * Dash-separated:
       *
       *   <degrees>-<acc>-<brk>-<slave>-<hb>-<gx>-<gy>\r\n
       *
       * 1) <degrees> : Steering angle in degrees (float, 1 decimal).
       * 2) <acc>     : Throttle mapped to 0..255.
       * 3) <brk>     : Brake mapped to 0..255.
       * 4) <slave>   : Raw line from slave MCU.
       * 5) <hb>      : Handbrake bit ('1' pulled, '0' otherwise).
       * 6) <gx>      : Manual transmission X analog in 0..255.
       * 7) <gy>      : Manual transmission Y analog in 0..255.
       */

      // Print to USB (host)
      Serial.print(degrees, 1);
      Serial.print('-');
      Serial.print(acc);
      Serial.print('-');
      Serial.print(brk);
      Serial.print('-');
      Serial.print(lineBuf);
      Serial.print('-');
      Serial.print(hbBit);
      Serial.print('-');
      Serial.print(gx255);
      Serial.print('-');
      Serial.println(gy255);

      // Print to slave (minimal format)
      link.print(degrees, 1);
      link.print('-');
      link.print(acc);
      link.print('-');
      link.println(brk);

      lineLen = 0;
    }
    else
    {
      if (lineLen < sizeof(lineBuf) - 1)
      {
        lineBuf[lineLen++] = ch;
      }
      else
      {
        lineBuf[lineLen] = '\0';
        Serial.println(lineBuf);
        lineLen = 0;
      }
    }
  }
}

bool extractResetBit(const char *s)
{
  int len = 0;
  while (s[len] != '\0')
    len++;
  for (int i = len - 1; i >= 0; --i)
  {
    if (s[i] == '0')
      return false;
    if (s[i] == '1')
      return true;
  }
  return false; // default if not found
}

// Returns '1' when handbrake is pulled (active-low), '0' otherwise
char readHandbrakeBit()
{
  int v = digitalRead(HANDBRAKE_PIN);
  return (v == LOW) ? '1' : '0';
}

// Minimal proportional control for motor assist
int proportionalControlBasic(float degrees, int acc, int brake)
{
  int pwm = 0;

  if (degrees >= 450.0f || degrees <= -450.0f)
  {
    stopMotor();
    return 0;
  }

  int effort = (brake > 0) ? brake : (acc > 0 ? acc : 0);

  if (effort > 0)
  {
    if (degrees >= pwm_threshold)
    {
      long angle = (long)degrees;
      long prod = angle * (long)effort;
      pwm = map(prod, 0L, 114750L, pwm_min, pwm_max);
      if (pwm > 0 && pwm < pwm_floor)
        pwm = pwm_floor;
      moveMotorToLeft(pwm);
    }
    else if (degrees <= -pwm_threshold)
    {
      long angle = (long)(-degrees);
      long prod = angle * (long)effort;
      pwm = map(prod, 0L, 114750L, pwm_min, pwm_max);
      if (pwm > 0 && pwm < pwm_floor)
        pwm = pwm_floor;
      moveMotorToRight(pwm);
    }
    else
    {
      stopMotor();
      pwm = 0;
    }
  }
  else
  {
    stopMotor();
    pwm = 0;
  }

  return pwm;
}

void vibration(int vel, int delayTime)
{
  analogWrite(RPWM, vel);
  analogWrite(LPWM, 0);
  delay(delayTime);

  analogWrite(RPWM, 0);
  analogWrite(LPWM, 0);
  delay(delayTime);

  analogWrite(RPWM, 0);
  analogWrite(LPWM, vel);
  delay(delayTime);

  analogWrite(RPWM, 0);
  analogWrite(LPWM, 0);
  delay(delayTime);
}

void moveMotorToLeft(int vel)
{
  analogWrite(RPWM, vel);
  analogWrite(LPWM, 0);
}

void moveMotorToRight(int vel)
{
  analogWrite(RPWM, 0);
  analogWrite(LPWM, vel);
}

void stopMotor()
{
  analogWrite(RPWM, 0);
  analogWrite(LPWM, 0);
}

void enableMotor()
{
  digitalWrite(REN, HIGH);
  digitalWrite(LEN, HIGH);
}

void disableMotor()
{
  digitalWrite(REN, LOW);
  digitalWrite(LEN, LOW);
}
