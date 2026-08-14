// =============================================================================
// sim_race_pro_wheel_script.ino  -  SimRacePro Wheel Arduino Nano firmware
// =============================================================================
// The "Wheel" Arduino Nano lives inside the steering wheel and connects to:
//   - Box Nano    via SoftwareSerial (half-duplex binary, CRC-8) - RX from Box
//                 @ 9600 baud, TX to Box @ 38400 baud (see linkRx/linkTx below
//                 for why the two directions differ)
//   - SSD1306 OLED 128×64          via I2C  (Adafruit_SSD1306)
//   - 4×4 button matrix            (rows: 4-7, columns: 8-11)
//   - Reset / re-zero button        (pin 12, active LOW)
//   - 3× RPM LEDs                  (A1, A0, A2)
//
// Main loop (~20 ms cycle):
//   1. Scan the 4×4 button matrix and the reset button.
//   2. Send a 4-byte button packet and (every 2 s) a 5-byte version packet
//      to the Box Nano via SoftwareSerial.
//   3. Wait up to 20 ms for a 9-byte display packet from the Box Nano.
//   4. Update the 3 RPM LEDs.
//   5. Refresh only the changed regions of the OLED display.
//
// Protocols (CRC-8 polynomial 0x07):
//   Wheel → Box: [btnHi][btnLo][rst][CRC8]                   4 bytes
//   Wheel → Box: [0xAE][maj][min][pat][CRC8]                  5 bytes (version, every 2 s)
//   Box → Wheel: [0xCC][angHi][angLo][acc][brk][rpmPct][speed][packed][CRC8]  9 bytes
//     packed: bits[7:1] = gearCode (0=R,1=N,2=1st…11=10th), bit[0] = blink
// =============================================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SoftwareSerial.h>

// =============================================================================
// Display
// =============================================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

const char *FW_VERSION = "ver. 3.2.0";

// =============================================================================
// Button matrix wiring
// =============================================================================
const uint8_t rowPins[4] = {4, 5, 6, 7};   // driven LOW one at a time during scan
const uint8_t colPins[4] = {8, 9, 10, 11}; // read with INPUT_PULLUP
const uint8_t RESET_PIN  = 12;              // re-zero encoder / calibrate pedals

// =============================================================================
// RPM LEDs
// =============================================================================
const uint8_t ledPins[3]         = {A1, A0, A2}; // green, yellow, red
const int RPM_LED_1_THRESHOLD    = 74;   // % of maxRpm for LED 1 (green)
const int RPM_LED_2_THRESHOLD    = 83;   // % of maxRpm for LED 2 (yellow)
const int RPM_LED_3_THRESHOLD    = 92;   // % of maxRpm for LED 3 (red)
const unsigned long BLINK_INTERVAL = 100; // LED blink interval in ms (shift-light)

// =============================================================================
// SoftwareSerial link to Box Nano (RX=2, TX=3) - split into two instances
// =============================================================================
// The Box's own RX (of this Wheel's TX) is stock SoftwareSerial and blocks the
// Box's steering-angle encoder interrupts for ~1 byte-time per received byte
// (cli() held for the whole byte). That blocking window is inversely
// proportional to baud rate, so this Wheel transmits as fast as is proven
// reliable (38400 baud, same rate the pre-regression firmware used end-to-end)
// to keep the Box's encoder-blind window short and avoid steering-angle drift.
// The Box, in turn, transmits to this Wheel via its own ISR-safe bit-banger
// at a fixed 9600 baud (chosen for THAT link's timing-jitter tolerance, see
// isrSafeTxByte() in the box firmware) - so this Wheel's RX must stay at 9600
// to decode it correctly.
// A single SoftwareSerial instance ties TX and RX to one shared baud, so two
// separate instances are used instead - each given a spare, otherwise-unused
// pin to satisfy the constructor for the direction it doesn't actually use.
SoftwareSerial linkRx(2, A3);   // RX from Box @ 9600 baud  (A3 = unused dummy TX pin)
SoftwareSerial linkTx(13, 3);   // TX to Box   @ 38400 baud (13 = unused dummy RX pin)

// Set to true to echo button state to the Arduino IDE Serial Monitor (debug).
bool enableSerialTX = false;

// =============================================================================
// Version packet helpers
// =============================================================================

// Sync byte for the Wheel → Box version packet
#define SYNC_WHEEL_VER 0xAE

// Parse a firmware version string "ver. X.Y.Z" into three uint8_t components.
static void parseVersion(const char* verStr, uint8_t& maj, uint8_t& min, uint8_t& pat) {
  const char* p = verStr;
  while (*p && !(*p >= '0' && *p <= '9')) p++;  // skip "ver. "
  maj = atoi(p); while (*p && *p != '.') p++; if (*p) p++;
  min = atoi(p); while (*p && *p != '.') p++; if (*p) p++;
  pat = atoi(p);
}

// =============================================================================
// Box → Wheel display packet protocol
// =============================================================================
// Sync byte 0xCC, 9 bytes total:
//   [0xCC][angHi][angLo][acc][brk][rpmPct][speed][packed][CRC8]
//   packed: bits[7:1] = gearCode, bit[0] = blink flag
#define SYNC_BOX2WHEEL    0xCC
#define PKT_BOX2WHEEL_LEN 9

static uint8_t rxBuf[PKT_BOX2WHEEL_LEN];
static uint8_t rxIdx  = 0;
static bool    rxSync = false;

// =============================================================================
// CRC-8 lookup table (polynomial 0x07, identical to Box firmware)
// =============================================================================
static const uint8_t CRC8_TABLE[256] PROGMEM = {
    0x00,0x07,0x0E,0x09,0x1C,0x1B,0x12,0x15,0x38,0x3F,0x36,0x31,0x24,0x23,0x2A,0x2D,
    0x70,0x77,0x7E,0x79,0x6C,0x6B,0x62,0x65,0x48,0x4F,0x46,0x41,0x54,0x53,0x5A,0x5D,
    0xE0,0xE7,0xEE,0xE9,0xFC,0xFB,0xF2,0xF5,0xD8,0xDF,0xD6,0xD1,0xC4,0xC3,0xCA,0xCD,
    0x90,0x97,0x9E,0x99,0x8C,0x8B,0x82,0x85,0xA8,0xAF,0xA6,0xA1,0xB4,0xB3,0xBA,0xBD,
    0xC7,0xC0,0xC9,0xCE,0xDB,0xDC,0xD5,0xD2,0xFF,0xF8,0xF1,0xF6,0xE3,0xE4,0xED,0xEA,
    0xB7,0xB0,0xB9,0xBE,0xAB,0xAC,0xA5,0xA2,0x8F,0x88,0x81,0x86,0x93,0x94,0x9D,0x9A,
    0x27,0x20,0x29,0x2E,0x3B,0x3C,0x35,0x32,0x1F,0x18,0x11,0x16,0x03,0x04,0x0D,0x0A,
    0x57,0x50,0x59,0x5E,0x4B,0x4C,0x45,0x42,0x6F,0x68,0x61,0x66,0x73,0x74,0x7D,0x7A,
    0x89,0x8E,0x87,0x80,0x95,0x92,0x9B,0x9C,0xB1,0xB6,0xBF,0xB8,0xAD,0xAA,0xA3,0xA4,
    0xF9,0xFE,0xF7,0xF0,0xE5,0xE2,0xEB,0xEC,0xC1,0xC6,0xCF,0xC8,0xDD,0xDA,0xD3,0xD4,
    0x69,0x6E,0x67,0x60,0x75,0x72,0x7B,0x7C,0x51,0x56,0x5F,0x58,0x4D,0x4A,0x43,0x44,
    0x19,0x1E,0x17,0x10,0x05,0x02,0x0B,0x0C,0x21,0x26,0x2F,0x28,0x3D,0x3A,0x33,0x34,
    0x4E,0x49,0x40,0x47,0x52,0x55,0x5C,0x5B,0x76,0x71,0x78,0x7F,0x6A,0x6D,0x64,0x63,
    0x3E,0x39,0x30,0x37,0x22,0x25,0x2C,0x2B,0x06,0x01,0x08,0x0F,0x1A,0x1D,0x14,0x13,
    0xAE,0xA9,0xA0,0xA7,0xB2,0xB5,0xBC,0xBB,0x96,0x91,0x98,0x9F,0x8A,0x8D,0x84,0x83,
    0xDE,0xD9,0xD0,0xD7,0xC2,0xC5,0xCC,0xCB,0xE6,0xE1,0xE8,0xEF,0xFA,0xFD,0xF4,0xF3,
};

static uint8_t crc8(const uint8_t* data, uint8_t len) {
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++)
        crc = pgm_read_byte(&CRC8_TABLE[crc ^ data[i]]);
    return crc;
}

// =============================================================================
// scanMatrix  -  read the 4×4 button matrix
// =============================================================================
// Drives each row LOW in turn and reads the four column inputs.
// All rows are INPUT (high-impedance) when not being driven.
void scanMatrix(bool keyStates[16]) {
  for (uint8_t i = 0; i < 16; i++) keyStates[i] = false;
  for (uint8_t r = 0; r < 4; r++) pinMode(rowPins[r], INPUT);
  for (uint8_t r = 0; r < 4; r++) {
    pinMode(rowPins[r], OUTPUT);
    digitalWrite(rowPins[r], LOW);
    delayMicroseconds(5); // settle time before reading columns
    for (uint8_t c = 0; c < 4; c++) {
      if (digitalRead(colPins[c]) == LOW) keyStates[r * 4 + c] = true;
    }
    pinMode(rowPins[r], INPUT); // release row
  }
}

// =============================================================================
// gearCodeToStr  -  convert packed gear code to a display string
// =============================================================================
// Code 0=R, 1=N, 2=1st gear, 3=2nd, …, 11=10th.
void gearCodeToStr(uint8_t code, char* out) {
  if      (code == 0) strcpy(out, "R");
  else if (code == 1) strcpy(out, "N");
  else                itoa(code - 1, out, 10);
}

// =============================================================================
// setup
// =============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println(F("Wheel: boot"));
  linkTx.begin(38400);
  linkRx.begin(9600);
  linkRx.listen();  // only one SoftwareSerial instance can actively receive at
                     // a time; make sure it's linkRx, not linkTx (which never
                     // needs to receive anything)
  Serial.println(F("Wheel: link serial ready (RX=pin2 TX=pin3)"));

  // Initialise LEDs (off at startup).
  for (uint8_t i = 0; i < 3; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW);
  }

  // Reset button (active LOW, internal pull-up).
  pinMode(RESET_PIN, INPUT_PULLUP);

  // Column inputs with internal pull-ups (button press pulls LOW).
  for (uint8_t c = 0; c < 4; c++) pinMode(colPins[c], INPUT_PULLUP);
  for (uint8_t r = 0; r < 4; r++) pinMode(rowPins[r], INPUT);

  // Initialise OLED and show splash screen.
  Serial.println(F("Wheel: pins ready, initializing OLED..."));
  Wire.begin();
  bool dispOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  Serial.print(F("Wheel: display.begin() -> "));
  Serial.println(dispOk ? F("OK") : F("FAILED (no/wrong OLED wiring? check SDA/SCL/GND)"));
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  // Line 1: "SimRacePro" - TextSize 2 (12px/char), 10 chars = 120px, cx=(128-120)/2=4
  display.setTextSize(2);
  display.setCursor(4, 2); display.println("SimRacePro");
  // Line 2: "Custom Driver" - TextSize 1 (6px/char), 13 chars = 78px, cx=(128-78)/2=25
  display.setTextSize(1);
  display.setCursor(25, 24); display.println("Custom Driver");
  // Line 3: version - "ver. x.y.z" = 10 chars = 60px, cx=(128-60)/2=34
  display.setCursor(34, 48); display.println(FW_VERSION);
  display.display();
  // Bootscreen stays visible until first valid packet from Box arrives.
  // Base layout is drawn in loop() on first packet (see firstPacketReceived flag).
  Serial.println(F("Wheel: setup complete, entering loop()"));
}

// =============================================================================
// Display state for partial-update optimisation
// =============================================================================
// Stores the previously rendered value for each display region.
// A region is only redrawn when its value changes, avoiding full clearDisplay().
static int  prev_speed   = -1, prev_rpm_pct = -1;
static char prev_gear[4] = "XX";
static int  prev_brk     = -1, prev_thr = -1, prev_ang = -999;
static bool prev_blink   = false;

// Set to true after the first valid packet from the Box is received.
// The base display layout is drawn once at that point so the screen
// transitions directly from the bootscreen to a fully populated display.
static bool firstPacketReceived = false;

// updateDisplay: refresh only changed display regions.
// Regions: RPM bar (y=10), gear (large, centre), speed (bottom-left),
//          brake (top-left), throttle (top-right), steering angle (bottom-right).
static void updateDisplay(int speed, int rpmPct, const char* gear,
                          int thr, int brk, int ang) {
  bool needFlush = false;

  // ── RPM bar (y = 10..13, full width) ────────────────────────────────────
  if (abs(rpmPct - prev_rpm_pct) > 0) {
    display.fillRect(0, 10, SCREEN_WIDTH, 4, SSD1306_BLACK);
    int barWidth = map(rpmPct, 0, 100, 0, SCREEN_WIDTH);
    if (barWidth > 0) display.fillRect(0, 10, barWidth, 4, SSD1306_WHITE);
    prev_rpm_pct = rpmPct;
    needFlush = true;
  }

  // ── Gear (large text, centre, y = 18..50) ───────────────────────────────
  if (strcmp(gear, prev_gear) != 0) {
    display.fillRect(36, 18, 56, 32, SSD1306_BLACK);
    display.setTextSize(4);
    // Adjust cursor X so each gear string is visually centred.
    int gX = 52;
    if (gear[0] == '1' && gear[1] == 0)  gX = 54; // single-digit "1"
    if (gear[0] == '1' && gear[1] == '0') gX = 40; // two-digit "10"
    display.setCursor(gX, 18);
    display.print(gear);
    strcpy(prev_gear, gear);
    needFlush = true;
  }

  // ── Speed (bottom-left, y = 48..64) ────────────────────────────────────
  if (abs(speed - prev_speed) > 0) {
    display.fillRect(0, 48, 40, 16, SSD1306_BLACK);
    display.setTextSize(2); display.setCursor(0, 48);
    if (speed < 100) display.print(' '); // right-align 3-digit field
    if (speed < 10)  display.print(' ');
    display.print(speed);
    display.setTextSize(1); display.setCursor(42, 55); display.print(F("kmh"));
    prev_speed = speed;
    needFlush = true;
  }

  // ── Brake (top-left, y = 0..9) ─────────────────────────────────────────
  if (abs(brk - prev_brk) > 0) {
    display.fillRect(0, 0, 44, 9, SSD1306_BLACK);
    display.setTextSize(1); display.setCursor(0, 0);
    display.print(F("B:")); display.print(brk);
    prev_brk = brk;
    needFlush = true;
  }

  // ── Throttle (top-right, y = 0..9) ─────────────────────────────────────
  if (abs(thr - prev_thr) > 0) {
    display.fillRect(86, 0, 42, 9, SSD1306_BLACK);
    display.setTextSize(1); display.setCursor(86, 0);
    display.print(F("T:"));
    if (thr < 100) display.print(' ');
    if (thr < 10)  display.print(' ');
    display.print(thr);
    prev_thr = thr;
    needFlush = true;
  }

  // ── Steering angle (bottom-right, y = 55..63) ───────────────────────────
  if (abs(ang - prev_ang) > 1) {
    display.fillRect(88, 55, 40, 9, SSD1306_BLACK);
    display.setTextSize(1); display.setCursor(88, 55);
    display.print(F("A:")); display.print(ang);
    prev_ang = ang;
    needFlush = true;
  }

  // Only push the frame buffer to the display if at least one region changed.
  if (needFlush) display.display();
}

// =============================================================================
// loop
// =============================================================================
void loop() {
  unsigned long now = millis();

  // Link diagnostics: counts survive across loop iterations so a status line
  // printed once a second gives an at-a-glance health check of the physical
  // Box<->Wheel link when this Wheel is bench-tested standalone via its own
  // USB (Box does not need to be connected/working for this to be useful).
  static unsigned long txLoopCount = 0, rxByteCount = 0, rxValidCount = 0;
  static unsigned long lastLinkStatusPrint = 0;
  txLoopCount++;

  // ── 1. SCAN BUTTONS AND SEND PACKET TO BOX ───────────────────────────────
  // SoftwareSerial is half-duplex: transmit first, then open the receive window.
  // The Box responds within ~1 ms of receiving the button packet, so a 20 ms
  // receive timeout is more than sufficient.
  bool keyStates[16];
  scanMatrix(keyStates);
  bool    resetPressed = (digitalRead(RESET_PIN) == LOW);
  uint8_t btnHigh = 0, btnLow = 0;
  for (uint8_t i = 0; i < 8; i++) if (keyStates[i])     btnLow  |= (1 << i);
  for (uint8_t i = 0; i < 8; i++) if (keyStates[i + 8]) btnHigh |= (1 << i);
  uint8_t rst = resetPressed ? 0x01 : 0x00;

  // Send 4-byte button packet: [btnHigh][btnLow][rst][CRC8]
  uint8_t btnData[3] = {btnHigh, btnLow, rst};
  linkTx.write(btnData, 3);
  linkTx.write(crc8(btnData, 3));

  // Send version packet every 2 s (immediately after the button packet so the
  // Box can forward it to the PC without a gap in button data).
  static unsigned long lastVerSend = 0;
  if (now - lastVerSend >= 2000) {
    lastVerSend = now;
    uint8_t maj, min, pat;
    parseVersion(FW_VERSION, maj, min, pat);
    uint8_t vpkt[4] = {maj, min, pat, 0};
    vpkt[3] = crc8(vpkt, 3);
    linkTx.write(SYNC_WHEEL_VER);
    linkTx.write(vpkt, 4);
  }

  // Optional debug output to USB Serial (does not affect normal operation).
  if (enableSerialTX) {
    Serial.print(F("BTN H=0x")); Serial.print(btnHigh, HEX);
    Serial.print(F(" L=0x"));    Serial.print(btnLow,  HEX);
    Serial.print(F(" RST="));    Serial.println(rst);
  }

  // ── 2. RECEIVE DISPLAY PACKET FROM BOX ───────────────────────────────────
  // Wait up to 20 ms for the 9-byte display packet.
  // The Box sends it every 100 ms; the 20 ms window keeps the loop at ~50 Hz
  // while still catching packets reliably.
  bool packetReady = false;
  unsigned long startWait = millis();
  while (!packetReady && (millis() - startWait < 20)) {
    while (linkRx.available()) {
      uint8_t b = (uint8_t)linkRx.read();
      rxByteCount++;
      if (!rxSync) {
        if (b == SYNC_BOX2WHEEL) { rxBuf[0] = b; rxIdx = 1; rxSync = true; }
      } else {
        rxBuf[rxIdx++] = b;
        if (rxIdx >= PKT_BOX2WHEEL_LEN) {
          rxIdx = 0; rxSync = false;
          if (crc8(rxBuf + 1, 7) == rxBuf[8]) { packetReady = true; rxValidCount++; }
        }
      }
    }
  }

  // Once-a-second link health line. rxByteCount staying at 0 means nothing
  // is arriving on pin 2 at all (wiring/GND/power, not a baud/CRC issue);
  // rxByteCount > 0 but rxValidCount = 0 points at a baud-rate or wiring-swap
  // problem instead (bytes arrive but never form a valid 0xCC packet).
  if (now - lastLinkStatusPrint >= 1000) {
    lastLinkStatusPrint = now;
    Serial.print(F("[link] loops=")); Serial.print(txLoopCount);
    Serial.print(F(" rxBytesFromBox=")); Serial.print(rxByteCount);
    Serial.print(F(" rxValidFromBox=")); Serial.println(rxValidCount);
  }

  if (!packetReady) {
    // No valid packet received. Keep LEDs blinking if the shift-light was active.
    static unsigned long lastBlinkTime = 0;
    static bool blinkState = false;
    if (prev_blink && (millis() - lastBlinkTime > BLINK_INTERVAL)) {
      blinkState = !blinkState; lastBlinkTime = millis();
      digitalWrite(ledPins[0], blinkState ? HIGH : LOW);
      digitalWrite(ledPins[1], blinkState ? HIGH : LOW);
      digitalWrite(ledPins[2], blinkState ? HIGH : LOW);
    }
    return;
  }

  // ── 3. DECODE DISPLAY PACKET ──────────────────────────────────────────────
  // On the very first valid packet: clear bootscreen and draw static labels.
  if (!firstPacketReceived) {
    firstPacketReceived = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(42, 55); display.print(F("kmh"));
    // Force full redraw of all regions on next render pass
    prev_speed = -1; prev_rpm_pct = -1; prev_brk = -1;
    prev_thr = -1; prev_ang = -999;
    strcpy(prev_gear, "XX");
    prev_blink = false;
  }

  int16_t angInt   = (int16_t)((rxBuf[1] << 8) | rxBuf[2]);
  float   degrees  = angInt / 10.0f;   // 0.1° resolution
  int     acc      = rxBuf[3];          // throttle 0-255
  int     brk      = rxBuf[4];          // brake    0-255
  int     rpmPct   = rxBuf[5];          // RPM percentage 0-100
  int     speed    = rxBuf[6];          // speed in km/h (capped at 255)
  uint8_t packed   = rxBuf[7];
  bool    do_blink = (packed & 0x01) != 0;        // bit 0 = shift-light blink
  uint8_t gearCode = (packed >> 1) & 0x7F;        // bits [7:1] = gear code

  char current_gear[4];
  gearCodeToStr(gearCode, current_gear);

  // Convert 0-255 pedal values to 0-100 % for display.
  int current_thr = map(acc, 0, 255, 0, 100);
  int current_brk = map(brk, 0, 255, 0, 100);
  int current_ang = (int)degrees;

  // ── 4. UPDATE RPM LEDS ───────────────────────────────────────────────────
  static unsigned long lastBlinkTime = 0;
  static bool blinkState = false;
  prev_blink = do_blink;

  if (do_blink) {
    // Shift-light: blink all three LEDs at BLINK_INTERVAL.
    if (now - lastBlinkTime > BLINK_INTERVAL) {
      blinkState = !blinkState;
      lastBlinkTime = now;
    }
    digitalWrite(ledPins[0], blinkState ? HIGH : LOW);
    digitalWrite(ledPins[1], blinkState ? HIGH : LOW);
    digitalWrite(ledPins[2], blinkState ? HIGH : LOW);
  } else {
    // Normal RPM bar: light LEDs progressively as RPM rises.
    blinkState = false;
    digitalWrite(ledPins[0], (rpmPct >= RPM_LED_1_THRESHOLD) ? HIGH : LOW); // green  ≥74%
    digitalWrite(ledPins[1], (rpmPct >= RPM_LED_2_THRESHOLD) ? HIGH : LOW); // yellow ≥83%
    digitalWrite(ledPins[2], (rpmPct >= RPM_LED_3_THRESHOLD) ? HIGH : LOW); // red    ≥92%
  }

  // ── 5. UPDATE OLED DISPLAY ───────────────────────────────────────────────
  updateDisplay(speed, rpmPct, current_gear, current_thr, current_brk, current_ang);
}
