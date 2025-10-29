#ifdef BUILD_WHEEL

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SoftwareSerial.h>
#include <string.h>
#include "sim_telemetry.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ######### CONFIG #########
const char *FW_VERSION = "ver. 2.0.0";
const char *HEADER_TEXT = "SIM RACE PRO";

const uint8_t HEADER_SIZE = 1;
const int16_t HEADER_Y = 6;
const int16_t HEADER_H = 8 * HEADER_SIZE;

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
void drawCenteredLine(const char *s, uint8_t size, int16_t y)
{
  int16_t textW = strlen(s) * 6 * size;
  int16_t x = (SCREEN_WIDTH - textW) / 2;
  display.setTextSize(size);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(x, y);
  display.print(s);
}

void renderHeader()
{
  int16_t y = constrain(HEADER_Y, 0, SCREEN_HEIGHT - HEADER_H);
  display.fillRect(0, 0, SCREEN_WIDTH, y + HEADER_H, SSD1306_BLACK);
  drawCenteredLine(HEADER_TEXT, HEADER_SIZE, y);
}

void clearContentArea()
{
  int16_t y = constrain(HEADER_Y, 0, SCREEN_HEIGHT - HEADER_H);
  display.fillRect(0, y + HEADER_H, SCREEN_WIDTH, SCREEN_HEIGHT - (y + HEADER_H), SSD1306_BLACK);
}

void drawContentCentered(const String &s, uint8_t size)
{
  renderHeader();
  clearContentArea();
  drawCenteredLine(s.c_str(), size, (SCREEN_HEIGHT + HEADER_H) / 2 - 12);
}

void showSplash()
{
  display.clearDisplay();
  drawCenteredLine("SIM RACE", 2, 0);
  drawCenteredLine("PRO", 5, 16);
  drawCenteredLine(FW_VERSION, 1, 56);
  display.display();
  delay(1800);
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

void drawSideBars(int acc, int brk)
{
  const uint8_t BAR_W = 8, TOP = 12, BOT = 2, CUT = 4;
  const uint8_t H = (64 - TOP - BOT) - CUT;
  int y0 = TOP + CUT;

  display.fillRect(0, T0P, BAR_W, 64 - TOP - BOT, SSD1306_BLACK);
  display.fillRect(120, TOP, BAR_W, 64 - TOP - BOT, SSD1306_BLACK);

  int barBrk = map(brk, 0, 255, 0, H);
  int barAcc = map(acc, 0, 255, 0, H);

  display.drawRect(0, y0, BAR_W, H, SSD1306_WHITE);
  display.drawRect(120, y0, BAR_W, H, SSD1306_WHITE);

  display.fillRect(0, y0 + (H - barBrk), BAR_W, barBrk, SSD1306_WHITE);
  display.fillRect(120, y0 + (H - barAcc), BAR_W, barAcc, SSD1306_WHITE);
}

void drawAngleSmall(float degrees)
{
  const uint8_t Y = 55, H = 10;
  char num[12];
  dtostrf(degrees, 0, 1, num);

  display.fillRect(8, Y, 112, H, SSD1306_BLACK);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(num, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, Y + 1);
  display.print(num);
}

void setup()
{
  for (uint8_t i = 0; i < 3; i++)
  {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW);
  }

  pinMode(RESET_PIN, INPUT_PULLUP);

  for (uint8_t c = 0; c < 4; c++)
    pinMode(colPins[c], INPUT_PULLUP);
  for (uint8_t r = 0; r < 4; r++)
    pinMode(rowPins[r], INPUT);

  Wire.begin();
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  link.begin(BAUD);

  showSplash();
  startupSweep();

  display.clearDisplay();
  renderHeader();
  drawContentCentered("N", 4);
  lastActive = 0;
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
  if (link.available())
  {
    static char rxBuf[48];
    static uint8_t rxLen = 0;
    static float lastDeg = 0;
    static int lastAcc = 0, lastBrk = 0;

    while (link.available())
    {
      char c = link.read();

      if (c == '\n')
      {
        if (rxLen > 0 && rxBuf[rxLen - 1] == '\r')
          rxLen--;
        rxBuf[rxLen] = '\0';

        char *p1 = strtok(rxBuf, "-");
        char *p2 = strtok(NULL, "-");
        char *p3 = strtok(NULL, "-");

        if (p1 && p2 && p3)
        {
          lastDeg = atof(p1);
          lastAcc = constrain(atoi(p2), 0, 255);
          lastBrk = constrain(atoi(p3), 0, 255);

          uint8_t lvl = (lastAcc >= 200) ? 3 : (lastAcc >= 100) ? 2
                                           : (lastAcc >= 10)    ? 1
                                                                : 0;
          digitalWrite(ledPins[0], lvl >= 1);
          digitalWrite(ledPins[1], lvl >= 2);
          digitalWrite(ledPins[2], lvl >= 3);

          drawSideBars(lastAcc, lastBrk);
          drawAngleSmall(lastDeg);
          display.display();
        }
        rxLen = 0;
      }
      else if (rxLen < sizeof(rxBuf) - 1)
        rxBuf[rxLen++] = c;
      else
        rxLen = 0;
    }
  }

  // ######### WRITE INFO ON DISPLAY #########

  int current = resetPressed ? 100 : (fp ? fp : 0);
  if (current != lastActive)
  {
    if (current == 0)
      drawContentCentered("N", 4);
    else if (current == 100)
      drawContentCentered("R", 4);
    else
      drawContentCentered(String(current), 4);
    lastActive = current;
  }
}

#endif
