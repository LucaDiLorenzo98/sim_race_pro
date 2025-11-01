#include "sim_telemetry.h"
struct TelemetryFrame;

#include <SoftwareSerial.h>
#include <Encoder.h>
#include <ctype.h>

// #define BUILD_BOX // Comment this line

#ifdef BUILD_BOX

/* Written by Luca Di Lorenzo 01/09/2025
   Latest update: 01/10/2025 (watch version) */

// No need to install SoftwareSerial, but you have to
// install Encoder library (use Arduino IDE Library Manager)

const char *FW_VERSION = "ver. 2.0.0";

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

// Set true only if you added the Pedals Vibration System
bool PEDALS_VIBRATION_ENABLED = true;

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
int POT_OFFSET = 0; // only BOX_BUDGET
int ACC_DEADZONE = 10;
int BRK_DEADZONE = 10;

// Pedals Vibratrion System (optional)
const uint8_t VIB_PIN = A2;
const uint8_t VIB2_PIN = A3;

// Motor controller
const int RPWM = 11; // PWM pin (Timer2)
const int LPWM = 10; // PWM pin (Timer1)
const int REN = 9;
const int LEN = 8;

// Rotation sensor (only BOX_BUDGET)
const int POT_PIN = A4;

// Clutch (third pedal)
const int CLUTCH_PIN = 13;

// Additional modules
const int HANDBRAKE_PIN = 4;       // button between pin and GND
const int MANUAL_TX_POT1_PIN = A7; // X-axis (left/right)
const int MANUAL_TX_POT2_PIN = A6; // Y-axis (up/down)

// ############### Force-feedback settings for proportional control ###############
const int pwm_threshold = 3;
const int pwm_floor = 15;
const int pwm_max = 255;
const int pwm_min = 90;

// ############### Force-feedback advanced settings ###############

// Spring centering gain scaling with speed
static const float K_CENTER_BASE = 0.12f;  // Nm/deg at 0 km/h
static const float K_CENTER_MAX = 0.40f;   // Nm/deg max at high speed
static const float SPEED_FOR_MAX = 180.0f; // Speed where centering reaches K_CENTER_MAX

// Damping to avoid oscillations: force opposes wheel speed
static const float K_DAMP = 0.015f; // Nm / (deg/s)

// Conversion from physics torque → PWM motor command
static const float TORQUE_TO_PWM = 260.0f;

// Minimum torque threshold to avoid motor buzzing/jitter
static const float DEAD_TORQUE = 0.02f;

// Soft limiter for wheel mechanical angle
static const float DEG_LIMIT = 450.0f; // degrees

// Curb effect vibration
static const uint16_t CURB_FREQ_HZ = 50; // vibration frequency
static const float CURB_TORQUE = 0.10f;  // torque amplitude

// PWM safety cap
static const uint8_t PWM_MAX = 255;

// Buffers and state
#define BUF_SIZE 128
char linkBuffer[BUF_SIZE];
char pcBuffer[BUF_SIZE];

bool lastResetBit = false;

uint8_t linkIndex = 0;
uint8_t pcIndex = 0;

// ############### METHODS ###############

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

char readHandbrakeBit()
{
  int v = digitalRead(HANDBRAKE_PIN);
  return (v == LOW) ? '1' : '0';
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

int proportionalControlBasic(float degrees, int acc, int brake)
{
  int pwm = 0;

  if (degrees >= 450.0f || degrees <= -450.0f)
  {
    stopMotor();
    disableMotor();
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

      enableMotor();
      moveMotorToLeft(pwm);
    }
    else if (degrees <= -pwm_threshold)
    {
      long angle = (long)(-degrees);
      long prod = angle * (long)effort;
      pwm = map(prod, 0L, 114750L, pwm_min, pwm_max);
      if (pwm > 0 && pwm < pwm_floor)
        pwm = pwm_floor;

      enableMotor();
      moveMotorToRight(pwm);
    }
    else
    {
      disableMotor();
      stopMotor();
      pwm = 0;
    }
  }
  else
  {
    disableMotor();
    stopMotor();
    pwm = 0;
  }

  // TODO: use potentiometer for steering angle **********************************************************************

  if (PEDALS_VIBRATION_ENABLED)
  {
    // if (brake > 60 && brake < 150) // emulate ABS behaviour
    //  digitalWrite(VIB_PIN, HIGH);

    // digitalWrite(VIB2_PIN, HIGH);
  }

  return pwm;
}

void telemetryControlAdvance(const struct TelemetryFrame &t, float wheelDeg)
{
  // Limit steering for safety
  if (wheelDeg > DEG_LIMIT)
    wheelDeg = DEG_LIMIT;
  if (wheelDeg < -DEG_LIMIT)
    wheelDeg = -DEG_LIMIT;

  // Wheel angular speed (deg/s)
  static unsigned long lastMs = 0;
  static float lastDeg = 0.0f;

  unsigned long now = millis();
  float dt = (now - lastMs) * 0.001f;
  if (dt <= 0.0f)
    dt = 0.001f;
  float ddeg = (wheelDeg - lastDeg) / dt;

  lastDeg = wheelDeg;
  lastMs = now;

  // Speed-based centering stiffness
  float v = max(0.0f, t.speed_kmh);
  float k_center = K_CENTER_BASE + (K_CENTER_MAX - K_CENTER_BASE) * (v / SPEED_FOR_MAX);
  if (k_center > K_CENTER_MAX)
    k_center = K_CENTER_MAX;

  // Spring to center + damping
  float torque = -k_center * wheelDeg - K_DAMP * ddeg;

  // Curb vibration
  if (t.has_on_curb && t.on_curb)
  {
    unsigned long period = 1000UL / (CURB_FREQ_HZ * 2UL);
    bool phase = ((now / period) & 1UL);
    torque += phase ? +CURB_TORQUE : -CURB_TORQUE;
  }

  // Deadband
  if (fabs(torque) < DEAD_TORQUE)
  {
    stopMotor();
    return;
  }

  // Torque → PWM
  int pwm = (int)(fabs(torque) * TORQUE_TO_PWM);
  if (pwm > PWM_MAX)
    pwm = PWM_MAX;

  enableMotor();
  if (torque > 0)
    moveMotorToRight(pwm);
  else
    moveMotorToLeft(pwm);
}

void processSimulation(const char *linkData, const char *pcData)
{
  // ############### READ ALL BUTTONS AND SENSORS ###############

  bool resetBit = extractResetBit(linkData);
  if (resetBit && !lastResetBit)
  {
    zeroOffset = myEnc.read();

    if (BOX_BUDGET)
    {
      POT_OFFSET = analogRead(POT_PIN);
    }

    ACC_OFFSET = analogRead(ACC_PIN);
    BRK_OFFSET = analogRead(BRK_PIN);
  }
  lastResetBit = resetBit;

  // TODO: add read of potentiometer for BOX_BUDGET (POT_PIN) ***********************************************************************

  long ticks = myEnc.read() - zeroOffset;
  if (ticks > maxTicks)
    ticks = maxTicks;
  if (ticks < -maxTicks)
    ticks = -maxTicks;

  float degrees = (ticks / 2400.0f) * 360.0f; // 2400 ticks = 360°

  // TODO: use potentiometer for steering angle **********************************************************************

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

  // ############### FORCE-FEEDBACK CONTROL IF ENABLED (ONLY BOX_FULL) ###############
  if (BOX_FULL == true)
  {

    if (pcData[0] == '\0')
    {
      proportionalControlBasic(degrees, acc, brk);
    }
    else
    {
      // telemetry data received from PC
      static TelemetryFrame telemetryFrame;
      if (parseTelemetry(pcData, telemetryFrame))
      {
        telemetryControlAdvance(telemetryFrame, degrees);
      }
      else
      {
        // fallback to basic control
        proportionalControlBasic(degrees, acc, brk);
      }
    }
  }

  // ############### SEND DATA TO PC AND WHEEL ###############

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
   * 4) <slave>   : Raw line from wheel.
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
  Serial.print(linkData);
  Serial.print('-');
  Serial.print(hbBit);
  Serial.print('-');
  Serial.print(gx255);
  Serial.print('-');
  Serial.println(gy255);

  // Print to wheel
  link.print(degrees, 1);
  link.print('-');
  link.print(acc);
  link.print('-');
  link.print(brk);
  link.print('-');
  link.println(pcData);
}

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

    // ~3.9 kHz su pin 9-10 (Timer1) e 3-11 (Timer2)
    TCCR1B = (TCCR1B & 0b11111000) | 0x02; // Timer1 prescaler 8
    TCCR2B = (TCCR2B & 0b11111000) | 0x02; // Timer2 prescaler 8

    // Raise PWM frequency above audible (~31 kHz)
    // TCCR1B = (TCCR1B & 0b11111000) | 0x01;  // Timer1 -> pins 9,10  (LPWM=10)
    // TCCR2B = (TCCR2B & 0b11111000) | 0x01;  // Timer2 -> pins 3,11  (RPWM=11)

    // enableMotor();
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

  if (PEDALS_VIBRATION_ENABLED)
  {
    pinMode(VIB_PIN, OUTPUT);
    pinMode(VIB2_PIN, OUTPUT);
    Serial.println("Pedals vibration system setup done");
  }
  else
  {
    Serial.println("Pedals vibration system not enabled");
  }
}

void loop()
{

  bool linkLineReadyThisLoop = false;

  // read data from wheel (mandatory -> you need the wheel messages)
  while (link.available())
  {
    char ch = link.read();

    if (ch == '\n')
    {
      linkBuffer[linkIndex] = '\0';
      linkIndex = 0;
      linkLineReadyThisLoop = true;
    }
    else
    {
      if (linkIndex < sizeof(linkBuffer) - 1)
      {
        linkBuffer[linkIndex++] = ch;
      }
    }
  }

  // read data from PC (optional -> only if telemetry is enabled)
  while (Serial.available())
  {
    char c = Serial.read();

    if (c == '\n')
    {
      pcBuffer[pcIndex] = '\0';
      pcIndex = 0;
    }
    else
    {
      if (pcIndex < sizeof(pcBuffer) - 1)
      {
        pcBuffer[pcIndex++] = c;
      }
    }
  }

  // process only if a full line from wheel is ready
  if (!linkLineReadyThisLoop)
    return;

  // process simulation data by passing the buffers
  processSimulation(linkBuffer, pcBuffer);
}

#endif