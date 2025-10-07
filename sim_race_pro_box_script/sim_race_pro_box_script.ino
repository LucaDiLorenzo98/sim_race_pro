/* Written by Luca Di Lorenzo 01/09/2025
   Latest update: 01/10/2025 (watch version) */

// No need to install SoftwareSerial, but you have to
// install Encoder library (use arduino IDE Library Manager)

#include <SoftwareSerial.h>
#include <Encoder.h>

const char *FW_VERSION = "ver. 1.0.0";

// #####################################################
// MANDATORY VAR. SETTINGS (READ THIS BEFORE UPLOAD THE SCRIPTS)

// true only if you want wheel without pedals
bool ONLY_WHEEL = false;

// only one of the following BOX_ variables can be true,
// otherwhise the code will not work

bool BOX_FULL = true;    // force-feedback, high-res. encoder
bool BOX_MEDIUM = false; // high-res. encoder
bool BOX_BUDGET = false; // pot. rotation sensor

// true only if you added the clutch (third pedal) into
// pedals base (check clutch pin var.)
bool PEDALS_CLUTCH = false;

// this var enables the additional modules
bool HANDBRAKE_ENABLED = true;
bool MANUAL_TX_ENABLED = true;

// #####################################################

// Serial Comm. var.
static const uint8_t RX_PIN = 5; // master RX (connect to slave TX)
static const uint8_t TX_PIN = 6; // master TX (optional to slave RX)
SoftwareSerial link(RX_PIN, TX_PIN);
static const unsigned long LINK_BAUD = 57600; // must match the slave
static const unsigned long USB_BAUD = 115200; // PC Serial Monitor

// High-res. encoder var.
Encoder myEnc(2, 3);        // A->D2, B->D3
const long maxTicks = 3000; // safety clamp
long zeroOffset = 0;        // ticks to subtract (dynamic zero)
long lastPosition = 0;      // reserved for future use

// Throttle and brake var.
const uint8_t ACC_PIN = A1;
const uint8_t BRK_PIN = A0;
int ACC_OFFSET = 0;
int BRK_OFFSET = 0;
int ACC_DEADZONE = 10;
int BRK_DEADZONE = 10;

// Motor controller var.
const int RPWM = 11; // PWM pin (Timer2)
const int LPWM = 10; // PWM pin (Timer1)
const int REN = 9;
const int LEN = 8;

// force-feedback settings var.
const int pwm_threshold = 2;
const int pwm_floor = 15;
const int pwm_max = 255;
const int pwm_min = 90;

// pot. var. (only BOX_BUDGET)
const int POT_PIN = A4;

// clutch (third pedal) var.
const int CLUTCH_PIN = A2;

// Additional modules var.
const int HANDBRAKE_PIN = 4;
const int MANUAL_TX_POT1_PIN = A7;
const int MANUAL_TX_POT2_PIN = A6;

// other var.
char lineBuf[96]; // slave line ~34 chars; keep margin
uint8_t lineLen = 0;
bool lastResetBit = false;
bool extractResetBit(const char *s);

// method prototypes (don't remove)
int proportionalControl(float degrees);
void vibration(int vel, int delayTime);
void moveMotorToRight(int vel);
void moveMotorToLeft(int vel);
void stopMotor();
void enableMotor();
void disableMotor();

// only the first time, then loop
void setup()
{
  // USB serial
  Serial.begin(USB_BAUD);
  delay(50);
  Serial.print(F("[MASTER] "));
  Serial.println(FW_VERSION);

  // #####################################################
  // VALIDATION RULES
  if ((BOX_FULL + BOX_MEDIUM + BOX_BUDGET) != 1)
  {
    Serial.println("SETUP ERROR: "); // TODO **********************************************************************
    return;
  }

  // TODO: add other validation rules **********************************************************************

  // #####################################################
  // WHEEL SETUP

  link.begin(LINK_BAUD); // communication with slave

  // #####################################################
  // BOX SETUP

  if (BOX_FULL == true)
  {
    // Motor pins (safe state)
    pinMode(RPWM, OUTPUT);
    pinMode(LPWM, OUTPUT);
    pinMode(REN, OUTPUT);
    pinMode(LEN, OUTPUT);

    // Raise PWM frequency above audible (~31 kHz)
    // TCCR1B = (TCCR1B & 0b11111000) | 0x01;  // Timer1 -> pins 9,10  (LPWM=10)
    // TCCR2B = (TCCR2B & 0b11111000) | 0x01;  // Timer2 -> pins 3,11  (RPWM=11)

    enableMotor(); // enable driver

    Serial.println("Force-feedback setup done");
  }

  if (BOX_FULL == true || BOX_MEDIUM == true)
  {
    zeroOffset = myEnc.read(); // Initial zero at current position
    Serial.println("High-res. encoder setup done");
  }

  if (BOX_BUDGET)
  {
    pinMode(POT_PIN, OUTPUT);
    Serial.println("High-res. encoder setup done");
  }

  // #####################################################
  // PEDALS SETUP

  if (ONLY_WHEEL == false)
  {
    pinMode(ACC_PIN, INPUT);
    pinMode(BRK_PIN, INPUT);

    ACC_OFFSET = analogRead(ACC_PIN);
    BRK_OFFSET = analogRead(BRK_PIN);

    Serial.println("Pedals setup done");

    if (PEDALS_CLUTCH == true)
    {
      pinMode(PEDALS_PIN, OUTPUT);

      Serial.println("Clutch pedal setup done");
    }
  }
  else
  {
    Serial.println("ONLY Wheel option enabled");
  }

  // #####################################################
  // ADDITIONAL MODULE SETUP
  if (HANDBRAKE_ENABLED)
  {
  }

  if (MANUAL_TX_ENABLED)
  {
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

      // #####################################################

      // TODO: FARE UNA SPIEGAZIONE MILGIORE E COMPLETA EPR TUTTI I CASI DELLE INFO TRASMESSE **********************************************************************

      // --- Print: <degrees>-<accel255>-<brake255>-<line_from_slave>

      // #####################################################

      Serial.print(degrees, 1);
      Serial.print('-');
      Serial.print(acc);
      Serial.print('-');
      Serial.print(brk);
      Serial.print('-');
      Serial.println(lineBuf);

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

int proportionalControlBasic(float degrees, int acc, int brake)
{
  int pwm = 0;

  // stop se angolo fuori range
  if (degrees >= 450.0f || degrees <= -450.0f)
  {
    stopMotor();
    return 0;
  }

  // Scegli l'"effort": il freno ha priorità se > 0, altrimenti usa acc
  int effort = (brake > 0) ? brake : (acc > 0 ? acc : 0);

  if (effort > 0)
  {
    // Sterzo proporzionale
    if (degrees >= pwm_threshold)
    {
      long angle = (long)degrees;       // 0..450
      long prod = angle * (long)effort; // 0..114750 (450*255)
      pwm = map(prod, 0L, 114750L, pwm_min, pwm_max);
      if (pwm > 0 && pwm < pwm_floor)
        pwm = pwm_floor;

      // stessa direzione della logica esistente
      moveMotorToLeft(pwm);
    }
    else if (degrees <= -pwm_threshold)
    {
      long angle = (long)(-degrees);    // 0..450
      long prod = angle * (long)effort; // 0..114750
      pwm = map(prod, 0L, 114750L, pwm_min, pwm_max);
      if (pwm > 0 && pwm < pwm_floor)
        pwm = pwm_floor;

      moveMotorToRight(pwm);
    }
    else
    {
      // nessuno sterzo -> nessuna azione sul motore (come tua logica originale)
      stopMotor();
      pwm = 0;
    }
  }
  else
  {
    // né acc né brake

    // TODO: introdurre attrtito tra ruote e cemento **********************************************************************

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
