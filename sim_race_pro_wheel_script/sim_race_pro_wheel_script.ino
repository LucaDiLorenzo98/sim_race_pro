#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SoftwareSerial.h>
#include <string.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ====== VERSIONE (modifica qui) ======
const char* FW_VERSION  = "ver. 1.0.0";

// ====== HEADER fisso (sempre visibile) ======
const char* HEADER_TEXT = "SIM RACE PRO";
const uint8_t HEADER_SIZE = 1;                  // il più piccolo possibile
const int16_t HEADER_Y    = 6;
const int16_t HEADER_H    = 8 * HEADER_SIZE;    // 8 px

// Matrice 4x4
const uint8_t rowPins[4] = {4, 5, 6, 7};   // Righe: D4..D7
const uint8_t colPins[4] = {8, 9, 10, 11}; // Colonne: D8..D11

// Tasto esterno su D12 (verso GND, INPUT_PULLUP)
const uint8_t RESET_PIN = 12;

// LED (solo dichiarati; usati per animazione all'avvio)
const uint8_t ledPins[3] = {A2, A1, A0};

// SoftwareSerial su D2 (RX) e D3 (TX)
SoftwareSerial link(2, 3); // RX=D2, TX=D3
const unsigned long BAUD = 57600;

// Stato display
int lastActive = -1; // -1 per forzare primo disegno
unsigned long lastFrameMs = 0;
const unsigned long frameIntervalMs = 25; // ~40 fps

// ---------- UTIL GRAFICA ----------
void drawCenteredLine(const char* s, uint8_t size, int16_t y) {
  int16_t textW = (int16_t)strlen(s) * 6 * size;
  int16_t x = (SCREEN_WIDTH - textW) / 2;
  display.setTextSize(size);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(x, y);
  display.print(s);
}

// Disegna l’header in alto (non fa display.display())
void renderHeader() {
  // Clamp Y per sicurezza (niente valori negativi o fuori schermo)
  int16_t y = HEADER_Y;
  if (y < 0) y = 0;
  if (y > SCREEN_HEIGHT - HEADER_H) y = SCREEN_HEIGHT - HEADER_H;

  // Pulisco fascia header e scrivo il testo
  display.fillRect(0, 0, SCREEN_WIDTH, y + HEADER_H, SSD1306_BLACK);
  int16_t textW = (int16_t)strlen(HEADER_TEXT) * 6 * HEADER_SIZE;
  int16_t x = (SCREEN_WIDTH - textW) / 2;
  display.setTextSize(HEADER_SIZE);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(x, y);
  display.print(HEADER_TEXT);
}

void clearContentArea() {
  int16_t y = HEADER_Y;
  if (y < 0) y = 0;
  if (y > SCREEN_HEIGHT - HEADER_H) y = SCREEN_HEIGHT - HEADER_H;

  display.fillRect(0, y + HEADER_H, SCREEN_WIDTH,
                   SCREEN_HEIGHT - (y + HEADER_H), SSD1306_BLACK);
}

// Disegna testo centrato SOTTO l’header e ridisegna SEMPRE l’header
void drawContentCentered(const String &s, uint8_t size) {
  // Prima ripristino l'header (nel buffer), poi pulisco e stampo il contenuto
  renderHeader();
  clearContentArea();

  int16_t y = HEADER_Y;
  if (y < 0) y = 0;
  if (y > SCREEN_HEIGHT - HEADER_H) y = SCREEN_HEIGHT - HEADER_H;

  int16_t textW = s.length() * 6 * size;
  int16_t textH = 8 * size;
  int16_t x = (SCREEN_WIDTH  - textW) / 2;
  int16_t cy = (y + HEADER_H) + (SCREEN_HEIGHT - (y + HEADER_H) - textH) / 2 - 3;

  display.setTextSize(size);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(x, cy);
  display.print(s);
}

void displayNumberCentered(uint8_t num) {
  drawContentCentered(String(num), 4);
}

// ---------- SPLASH ----------
void showSplash() {
  // SIM RACE (size 2 -> 16px), PRO (size 5 -> 40px), versione (size 1 -> 8px)
  // posizioni: 0, 16, 56 (totale 64 px)
  display.clearDisplay();
  drawCenteredLine("SIM RACE", 2, 0);
  drawCenteredLine("PRO",      5, 16);
  drawCenteredLine(FW_VERSION, 1, 56);
  display.display();
  delay(1800);
}

// ---------- LED STARTUP ----------
void startupSweep() {
  const int d = 300; // ms
  digitalWrite(ledPins[0], HIGH); delay(d);
  digitalWrite(ledPins[0], LOW);
  digitalWrite(ledPins[1], HIGH); delay(d);
  digitalWrite(ledPins[1], LOW);
  digitalWrite(ledPins[2], HIGH); delay(d);
  digitalWrite(ledPins[2], LOW);
  digitalWrite(ledPins[1], HIGH); delay(d);
  digitalWrite(ledPins[1], LOW);
  digitalWrite(ledPins[0], HIGH); delay(d);
  digitalWrite(ledPins[0], LOW);

  delay(d);

  for (int k = 0; k < 2; k++) {
    digitalWrite(ledPins[0], HIGH);
    digitalWrite(ledPins[1], HIGH);
    digitalWrite(ledPins[2], HIGH);
    delay(d);
    digitalWrite(ledPins[0], LOW);
    digitalWrite(ledPins[1], LOW);
    digitalWrite(ledPins[2], LOW);
    delay(d);
  }
}

// ---------- SCANSIONE MATRICE MULTI-PRESS ----------
void scanMatrix(bool keyStates[16], uint8_t *firstPressed) {
  *firstPressed = 0;
  for (uint8_t i = 0; i < 16; i++) keyStates[i] = false;

  // tutte le righe Hi-Z prima della scansione
  for (uint8_t r = 0; r < 4; r++) pinMode(rowPins[r], INPUT);

  for (uint8_t r = 0; r < 4; r++) {
    // Attiva solo la riga r: OUTPUT LOW
    pinMode(rowPins[r], OUTPUT);
    digitalWrite(rowPins[r], LOW);
    delayMicroseconds(5); // stabilizzazione

    // Leggi colonne
    for (uint8_t c = 0; c < 4; c++) {
      bool pressed = (digitalRead(colPins[c]) == LOW);
      uint8_t idx = r * 4 + c; // 0..15
      if (pressed) {
        keyStates[idx] = true;
        if (*firstPressed == 0) *firstPressed = idx + 1; // per display
      }
    }

    // Disattiva riga: torna in Hi-Z
    pinMode(rowPins[r], INPUT);
  }
}

// ---------- SETUP ----------
void setup() {
  // LED
  for (uint8_t i = 0; i < 3; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW);
  }

  // Reset
  pinMode(RESET_PIN, INPUT_PULLUP);

  // Matrice: colonne in INPUT_PULLUP; righe in Hi-Z (INPUT)
  for (uint8_t c = 0; c < 4; c++) pinMode(colPins[c], INPUT_PULLUP);
  for (uint8_t r = 0; r < 4; r++) pinMode(rowPins[r], INPUT);

  // Display
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    // se fallisce display, proseguo comunque
  }

  // Seriale su D2/D3
  link.begin(BAUD);

  // Splash -> poi schermata operativa con header fisso e "N"
  showSplash();
  startupSweep();
  display.clearDisplay();
  renderHeader();              // disegna l’header (buffer)
  drawContentCentered("N", 4); // disegna la N sotto e aggiorna il display
  lastActive = 0;
}

void drawSideBars(int acc, int brk) {
  const uint8_t BAR_W = 8;
  const uint8_t MARGIN_TOP = 12;
  const uint8_t MARGIN_BOTTOM = 2;
  const uint8_t CUT_TOP = 4;             // <- accorcio di qualche pixel sopra

  const uint8_t H_full = 64 - MARGIN_TOP - MARGIN_BOTTOM;
  const uint8_t H = H_full - CUT_TOP;    // altezza utile accorciata

  int leftX  = 0;
  int rightX = 128 - BAR_W;

  // pulizia colonne complete (per evitare residui)
  display.fillRect(leftX,  MARGIN_TOP, BAR_W, H_full, SSD1306_BLACK);
  display.fillRect(rightX, MARGIN_TOP, BAR_W, H_full, SSD1306_BLACK);

  int y0 = MARGIN_TOP + CUT_TOP;         // abbassa la “testa” della barra

  int hBrk = map(brk, 0, 255, 0, H);
  int hAcc = map(acc, 0, 255, 0, H);

  display.drawRect(leftX,  y0, BAR_W, H, SSD1306_WHITE);
  display.drawRect(rightX, y0, BAR_W, H, SSD1306_WHITE);

  display.fillRect(leftX,  y0 + (H - hBrk),  BAR_W, hBrk, SSD1306_WHITE);
  display.fillRect(rightX, y0 + (H - hAcc),  BAR_W, hAcc, SSD1306_WHITE);
}

void drawAngleSmall(float degrees) {
  const uint8_t ANGLE_Y = 55;
  const uint8_t STRIP_H = 10;

  char num[12];
  dtostrf(degrees, 0, 1, num);      // es: "-12.3"

  display.fillRect(8, ANGLE_Y, 128-16, STRIP_H, SSD1306_BLACK);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(num, 0, 0, &x1, &y1, &w, &h);
  int x = (128 - (int)w) / 2;
  display.setCursor(x, ANGLE_Y + 1);
  display.print(num);               // niente simbolo di grado
}

// ---------- LOOP ----------
void loop() {
  // Letture
  bool keyStates[16];
  uint8_t firstPressed = 0; // 1..16 se almeno uno premuto, 0 altrimenti
  scanMatrix(keyStates, &firstPressed);

  bool resetPressed = (digitalRead(RESET_PIN) == LOW);

  // Aggiorna area contenuti (header resta fisso)
  int current = 0;
  if (resetPressed)      current = 100;
  else if (firstPressed) current = firstPressed;
  else                   current = 0;

  if (current != lastActive) {
    if (current == 0)           drawContentCentered("N", 4);
    else if (current == 100)    drawContentCentered("R", 4);
    else                        displayNumberCentered(current);
    lastActive = current;
  }

  // Invio linea su seriale D2/D3 (una riga per frame)
  unsigned long now = millis();
  if (now - lastFrameMs >= frameIntervalMs) {
    lastFrameMs = now;
    // 16 tasti matrice in ordine 1..16
    for (uint8_t i = 0; i < 16; i++) {
      link.print(keyStates[i] ? '1' : '0');
      link.print('-');
    }
    // 17° valore = tasto su D12
    link.print(resetPressed ? '1' : '0');
    link.print('\n');
  }

   // --- Lettura NON bloccante dal Master (solo se ci sono dati) ---
  if (link.available()) {
    static char    rxBuf[48];
    static uint8_t rxLen = 0;

    // Ultimi valori ricevuti (se ti servono altrove nel codice)
    static float lastDegrees = 0.0f;
    static int   lastAcc = 0;
    static int   lastBrk = 0;

    while (link.available()) {
      char c = (char)link.read();

      if (c == '\n') {
        // termina riga
        if (rxLen > 0 && rxBuf[rxLen - 1] == '\r') rxLen--;
        rxBuf[rxLen] = '\0';

        // parsing CSV: degrees,acc,brk  (es: "12.3,200,0")
        char *p1 = strtok(rxBuf, "-");
        char *p2 = strtok(NULL, "-");
        char *p3 = strtok(NULL, "-");

        if (p1 && p2 && p3) {
          float degrees = atof(p1);
          int   acc     = atoi(p2);
          int   brk     = atoi(p3);

          // clamp basilare
          if (acc < 0)   acc = 0;   if (acc > 255) acc = 255;
          if (brk < 0)   brk = 0;   if (brk > 255) brk = 255;

          // salva ultimi valori ricevuti (nessuna altra logica toccata)
          lastDegrees = degrees;
          lastAcc     = acc;
          lastBrk     = brk;

          // Soglie progressive (regolabili)
          const uint8_t T1 = 10;   // inizia da qui per evitare rumore
          const uint8_t T2 = 100;
          const uint8_t T3 = 200;

          uint8_t level = 0;
          if      (acc >= T3) level = 3;
          else if (acc >= T2) level = 2;
          else if (acc >= T1) level = 1;
          else                level = 0;

          // Accensione graduale 1→2→3
          digitalWrite(ledPins[0], (level >= 1) ? HIGH : LOW);
          digitalWrite(ledPins[1], (level >= 2) ? HIGH : LOW);
          digitalWrite(ledPins[2], (level >= 3) ? HIGH : LOW);

          drawSideBars(lastAcc, lastBrk);
          drawAngleSmall(lastDegrees);
          display.display();
        }

        // reset buffer per la prossima riga
        rxLen = 0;

      } else {
        // accumula caratteri finché c'è spazio
        if (rxLen < sizeof(rxBuf) - 1) {
          rxBuf[rxLen++] = c;
        } else {
          // overflow: scarta e riparti
          rxLen = 0;
        }
      }
    }
  }
}
