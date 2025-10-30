#include "sim_telemetry.h"
struct TelemetryFrame;

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SoftwareSerial.h>
#include <string.h>

// #define BUILD_WHEEL // Comment this line

#ifdef BUILD_WHEEL

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ######### CONFIG #########

const uint8_t rowPins[4] = {4, 5, 6, 7};
const uint8_t colPins[4] = {8, 9, 10, 11};
const uint8_t RESET_PIN = 12;
const uint8_t ledPins[3] = {A2, A1, A0};

SoftwareSerial link(2, 3);
const unsigned long BAUD = 57600;

int lastActive = -1;
unsigned long lastFrameMs = 0;
const unsigned long frameIntervalMs = 25;

// ######### METHODS #########

void renderDisplay(bool telemetryMode,
                   float degrees,
                   int acc,
                   int brk,
                   const struct TelemetryFrame &tf,
                   int currentKey)
{

  if (!telemetryMode)
  {
    display.clearDisplay();

    int16_t x = (SCREEN_WIDTH) / 2;
    int16_t y = (SCREEN_HEIGHT - 32) / 2;

    display.setTextSize(4);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(x, y);

    if (currentKey == 0)
    {
      display.print(F("N"));
    }
    else if (currentKey == 100)
    {
      display.print(F("R"));
    }
    else
    {
      display.print(currentKey);
    }

    display.display();

    return;
  }

  // telemetry mode: show speed and gear only
  display.clearDisplay();

  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 4);
  display.print((int)tf.speed_kmh);
  display.print(F(" km/h"));

  display.setCursor(100, 4);
  display.print(tf.gear);

  display.display();
}

void startupSweep()
{
  const int d = 300;
  for (int i = 0; i < 3; i++)
  {
    digitalWrite(ledPins[i], HIGH);
    delay(d);
    digitalWrite(ledPins[i], LOW);
  }
  for (int k = 0; k < 2; k++)
  {
    for (int i = 0; i < 3; i++)
      digitalWrite(ledPins[i], HIGH);
    delay(d);
    for (int i = 0; i < 3; i++)
      digitalWrite(ledPins[i], LOW);
    delay(d);
  }
}

void scanMatrix(bool keyStates[16], uint8_t *firstPressed)
{
  *firstPressed = 0;
  memset(keyStates, 0, 16);

  for (uint8_t r = 0; r < 4; r++)
    pinMode(rowPins[r], INPUT);

  for (uint8_t r = 0; r < 4; r++)
  {
    pinMode(rowPins[r], OUTPUT);
    digitalWrite(rowPins[r], LOW);
    delayMicroseconds(5);

    for (uint8_t c = 0; c < 4; c++)
    {
      bool pressed = (digitalRead(colPins[c]) == LOW);
      uint8_t idx = r * 4 + c;
      keyStates[idx] = pressed;
      if (pressed && *firstPressed == 0)
        *firstPressed = idx + 1;
    }
    pinMode(rowPins[r], INPUT);
  }
}

void setup()
{
  Wire.begin();
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  Serial.begin(BAUD);
  link.begin(BAUD);

  pinMode(RESET_PIN, INPUT_PULLUP);

  for (uint8_t i = 0; i < 3; i++)
    pinMode(ledPins[i], OUTPUT);

  for (uint8_t c = 0; c < 4; c++)
    pinMode(colPins[c], INPUT_PULLUP);
  for (uint8_t r = 0; r < 4; r++)
    pinMode(rowPins[r], INPUT);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(10, 10);
  display.print(F("SIM RACE"));
  display.setTextSize(3);
  display.setCursor(10, 30);
  display.print(F("PRO"));

  display.setTextSize(1);
  display.setCursor(10, 50);
  display.print(F("ver. 2.0"));

  display.display();

  startupSweep();

  delay(2000);

  display.clearDisplay();
  display.display();
}

void loop()
{
  // ######### READ SENSORS AND THEN WRITE DATA TO BOX #########
  bool keys[16];
  uint8_t fp;
  scanMatrix(keys, &fp);
  bool resetPressed = (digitalRead(RESET_PIN) == LOW);

  unsigned long now = millis();
  if (now - lastFrameMs >= frameIntervalMs)
  {
    lastFrameMs = now;
    for (uint8_t i = 0; i < 16; i++)
    {
      link.print(keys[i] ? '1' : '0');
      link.print('-');
    }
    link.print(resetPressed ? '1' : '0');
    link.print('\n');
  }

  // ######### READ DATA FROM BOX (TELEMETRY) #########

  static char rxBuf[64];
  static uint8_t rxLen = 0;

  static float lastDeg = 0;
  static int lastAcc = 0, lastBrk = 0;

  static TelemetryFrame lastTf;
  static bool telemetry_mode = false;

  while (link.available())
  {
    char c = (char)link.read();

    if (c == '\n')
    {
      if (rxLen > 0 && rxBuf[rxLen - 1] == '\r')
        rxLen--;
      rxBuf[rxLen] = '\0';

      float deg;
      int acc, brk;
      TelemetryFrame tfTmp;
      bool full = parseWheelPacket(rxBuf, deg, acc, brk, tfTmp, ',');

      lastDeg = deg;
      lastAcc = constrain(acc, 0, 255);
      lastBrk = constrain(brk, 0, 255);

      telemetry_mode = full;
      if (full)
        lastTf = tfTmp;

      rxLen = 0;
    }
    else if (rxLen < sizeof(rxBuf) - 1)
    {
      rxBuf[rxLen++] = c;
    }
    else
    {
      rxLen = 0;
    }
  }

  // ######### WRITE INFO ON DISPLAY #########
  int current = resetPressed ? 100 : (fp ? fp : 0);

  if (!telemetry_mode && current != lastActive)
    lastActive = current;

  renderDisplay(telemetry_mode, lastDeg, lastAcc, lastBrk, lastTf, current);
}

#endif
