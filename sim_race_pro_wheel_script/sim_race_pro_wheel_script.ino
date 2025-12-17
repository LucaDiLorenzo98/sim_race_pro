#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SoftwareSerial.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

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

int lastActive = -1;
unsigned long lastFrameMs = 0;

bool enableSerialTX = true;
bool enableSerialRX = true;

const byte MAX_CHARS = 128;

char masterRxBuffer[MAX_CHARS];
byte masterIndex = 0;
bool masterMsgReady = false;

void scanMatrix(bool keyStates[16], uint8_t *firstPressed)
{
  *firstPressed = 0;
  for (uint8_t i = 0; i < 16; i++)
    keyStates[i] = false;

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
      if (pressed)
      {
        keyStates[idx] = true;
        if (*firstPressed == 0)
          *firstPressed = idx + 1;
      }
    }
    pinMode(rowPins[r], INPUT);
  }
}


void setup()
{

  Serial.println("Starting Setup...");

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

  link.begin(38400);
  Serial.begin(115200);

  Wire.begin();
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  
  // Custom Splash Screen
  display.clearDisplay(); // clear adafruit logo
  display.setTextColor(SSD1306_WHITE);
  
  display.setTextSize(2);
  display.setCursor(10, 10);
  display.println("SIM RACE");
  display.setCursor(40, 30);
  display.println("PRO");
  
  display.setTextSize(1);
  display.setCursor(35, 50);
  display.println(FW_VERSION);
  
  display.display();
  delay(2000); // Show splash for 2 seconds

  lastActive = 0;

  Serial.println("Setup Complete.");
}

void loop()
{
  // READ BTNs VALUES
  bool keyStates[16];
  uint8_t firstPressed = 0;
  scanMatrix(keyStates, &firstPressed);

  bool resetPressed = (digitalRead(RESET_PIN) == LOW);
  
  // SEND BTNs VALUES
  for (uint8_t i = 0; i < 16; i++)
  {
    link.print(keyStates[i] ? '1' : '0');
    link.print('-');

    if (enableSerialTX)
    {
      Serial.print(keyStates[i] ? '1' : '0');
      Serial.print('-');
    }
  }

  link.println(resetPressed ? '1' : '0');

  if (enableSerialTX)
    Serial.print(resetPressed ? '1' : '0');

  // WAIT UNTIL THE INFO ARE READY

  // READ BOX VALUES
  while(link.available()) link.read(); // Drain old data
  
  bool responseReceived = false;
  uint32_t startWait = millis();
  
  while (!responseReceived && (millis() - startWait < 500))
  {
    if (link.available())
    {
    static char rxBuf[50];
    static uint8_t rxLen = 0;

    static float lastDegrees = 0.0f;
    static int lastAcc = 0;
    static int lastBrk = 0;

    while (link.available())
    {
      char c = (char)link.read();

      if (c == '\n')
      {

        if (rxLen > 0 && rxBuf[rxLen - 1] == '\r')
          rxLen--;
        rxBuf[rxLen] = '\0';

        char *p1 = strtok(rxBuf, "|");
        char *p2 = strtok(NULL, "|");
        char *p3 = strtok(NULL, "|");
        char *p4 = strtok(NULL, "|");

        if (p1 && p2 && p3)
        {
          float degrees = atof(p1);
          int acc = constrain(atoi(p2), 0, 255);
          int brk = constrain(atoi(p3), 0, 255);

          lastDegrees = degrees;
          lastAcc = acc;
          lastBrk = brk;

          if (enableSerialRX)
          {
            if (enableSerialTX)
              Serial.print(" | ");

            Serial.print(degrees);
            Serial.print('-');
            Serial.print(acc);
            Serial.print('-');
            Serial.print(brk);
            Serial.print('-');
            Serial.println(p4);
          }

          // State variables to prevent flickering
          static char prev_gear[4] = "";
          static int prev_speed = -1;
          static int prev_rpm_pct = -1;
          static int prev_thr = -1;
          static int prev_brk = -1;
          static int prev_ang = -999;

          char current_gear[4] = "N";
          int current_speed = 0;
          int current_rpm_pct = 0;

          // Use data from Box (p1, p2, p3)
          int current_thr = map(acc, 0, 255, 0, 100);
          int current_brk = map(brk, 0, 255, 0, 100);
          int current_ang = (int)degrees;

          if (p4 && strcmp(p4, "NONE") != 0)
          {
            // Valid Packet: gear;speed;rpm_pct
            char *t_gear = strtok(p4, ";");
            char *t_speed = strtok(NULL, ";");
            char *t_rpm_pct = strtok(NULL, ";");

            if (t_gear && t_speed && t_rpm_pct)
            {
              strncpy(current_gear, t_gear, 3);
              current_gear[3] = '\0';
              current_speed = atoi(t_speed);
              current_rpm_pct = atoi(t_rpm_pct);
            }
          }

          // Only update display if something changed
          if (strcmp(current_gear, prev_gear) != 0 ||
              current_speed != prev_speed ||
              current_rpm_pct != prev_rpm_pct ||
              current_thr != prev_thr ||
              current_brk != prev_brk ||
              current_ang != prev_ang)
          {
            // Update previous values
            strcpy(prev_gear, current_gear);
            prev_speed = current_speed;
            prev_rpm_pct = current_rpm_pct;
            prev_thr = current_thr;
            prev_brk = current_brk;
            prev_ang = current_ang;

            // Hybrid LED Logic
            // If we have RPM from game (Sim Mode), use it.
            // If RPM is 0 (Legacy Mode / Idle), map Throttle to LEDs for visual feedback.
            int ledValue = current_rpm_pct;
            if (ledValue == 0) {
               ledValue = current_thr;
            }

            // Update LEDs (Green 50%, Yellow 75%, Blue 85% - easier to see)
            digitalWrite(ledPins[0], (ledValue >= 50) ? HIGH : LOW);
            digitalWrite(ledPins[1], (ledValue >= 75) ? HIGH : LOW);
            digitalWrite(ledPins[2], (ledValue >= 85) ? HIGH : LOW);

            // Draw Display
            display.clearDisplay();
            display.setTextSize(1);
            
            // Top Row: B:xxx (Left)   T:xxx (Right)
            display.setCursor(0, 0); 
            display.print(F("B:")); 
            display.print(current_brk);

            // Right align Throttle (approx)
            // T:100 is 5 chars = 30px. 128-30=98
            display.setCursor(90, 0); 
            display.print(F("T:")); 
            if (current_thr < 100) display.print(' ');
            if (current_thr < 10) display.print(' ');
            display.print(current_thr);

            // RPM Bar
            int barWidth = map(current_rpm_pct, 0, 100, 0, SCREEN_WIDTH);
            display.fillRect(0, 10, barWidth, 4, SSD1306_WHITE);

            // Gear (Center) - OR Button Index if pressed
            if (firstPressed > 0) {
              display.setTextSize(3);
              display.setCursor(25, 20); 
              display.print(F("B:"));
              display.print(firstPressed - 1);
            } else {
              display.setTextSize(4);
              int gX = 52;
              if (current_gear[0] == 'N') gX = 52;
              else if (current_gear[0] == '1') gX = 54;
              else if (strcmp(current_gear, "10") == 0) gX = 40;
              
              display.setCursor(gX, 16); 
              display.print(current_gear);
            }

            // Speed (Bottom Left)
            display.setTextSize(2);
            display.setCursor(0, 48); // Moved up to 48 (fits 14px height)
            if (current_speed < 100) display.print(' ');
            if (current_speed < 10) display.print(' ');
            display.print(current_speed);
            
            display.setTextSize(1);
            display.setCursor(42, 55); // Next to speed
            display.print(F("kmh"));

            // Angle (Bottom Right)
            // Align "A:xxx" to right.
            display.setCursor(94, 55);
            display.print(F("A:"));
            display.print(current_ang);

            display.display();
          }
        }

        rxLen = 0;
        responseReceived = true;
      }
      else
      {
        if (rxLen < sizeof(rxBuf) - 1)
          rxBuf[rxLen++] = c;
        else
          rxLen = 0;
      }
    }
  }
  }
}

void readSerialData(Stream &source, char *buffer, byte &idx, bool &isReady)
{
  while (source.available() > 0 && !isReady)
  {
    char rc = source.read();

    if (rc == '\n')
    {
      buffer[idx] = '\0';
      isReady = true;
      idx = 0;
    }

    else if (rc != '\r')
    {
      if (idx < MAX_CHARS - 1)
      {
        buffer[idx] = rc;
        idx++;
      }
    }
  }
}