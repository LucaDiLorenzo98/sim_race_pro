#include <SoftwareSerial.h>
#include <EEPROM.h>
#include <Encoder.h>
#include "motor_control.h"

const char *FW_VERSION = "ver. 3.2.0";


// ── Hardware config ───────────────────────────────────────────────────────────
enum SIM_SETUP { BOX_FULL, BOX_MEDIUM, BOX_BUDGET };
SIM_SETUP sim_setup           = BOX_FULL;
bool DEBUG                    = false;
bool ONLY_WHEEL               = false;
bool PEDALS_VIBRATION_ENABLED = false;
bool HANDBRAKE_ENABLED        = false;
bool CLUTCH_ENABLED           = false;
bool SHIFTER_ENABLED          = false;
bool INVERT_STEERING          = false; // flips encoder direction - for units with the encoder soldered in reverse
bool INVERT_FFB               = false; // flips motor torque output direction - for units with the H-bridge/motor wired in reverse (independent of INVERT_STEERING)

// Hard PWM cap to keep motor current below the PSU's 10A limit.
const uint8_t MOTOR_MAX_PWM   = 178;  // 70% of 255 (~7A at 12V)
// Per-direction min PWM and max PWM change per cycle are runtime-configurable
// via g_cfg.motorMinPwmLeft/Right and g_cfg.softRampStep (EEPROM, Box HW Settings).

SoftwareSerial link(5, 6);
Encoder myEnc(2, 3);

const long maxTicks   = 3000;
long zeroOffset       = 0;
long degreeOffset     = 0;

// These 7 pins are user-remappable via the PC app's "Pin Configuration"
// window (0xBE packet, see PinConfig/loadPinConfig/configurePins below) -
// no longer const, defaults are applied by loadPinConfig() at boot unless
// overridden pins are already stored in EEPROM.
uint8_t ACC_PIN = A0;
uint8_t BRK_PIN = A1;
int ACC_OFFSET = 0, BRK_OFFSET = 0, CLUTCH_OFFSET = 0;
int ACC_DEADZONE = 10, BRK_DEADZONE = 10;
int ACC_INPUT_MAX = 90, BRK_INPUT_MAX = 90, CLUTCH_INPUT_MAX = 90;

uint8_t VIB_PIN    = A2;  // A2/A3 have no hardware PWM on the Nano.
uint8_t VIB2_PIN   = A3;  // Intensity is simulated via timed on/off bursts.

// Simulated PWM for pedal vibration motors on non-PWM pins A2/A3.
// Uses millis()-based burst timing: motor runs for (duty/255 * period) ms per cycle.
// Period = 20 ms -> 50 Hz update rate, imperceptible to the driver as stepping.
static uint8_t  swPwmDuty   = 0;    // 0=off, 255=full on
static uint16_t swPwmPeriod = 20;   // burst cycle length in ms

void updateSoftPwm() {
  static unsigned long cycleStart = 0;
  unsigned long now = millis();
  if (now - cycleStart >= swPwmPeriod) cycleStart = now;
  unsigned long elapsed = now - cycleStart;
  uint16_t onTime = (uint16_t)((swPwmDuty * swPwmPeriod) / 255);
  bool on = (elapsed < onTime);
  digitalWrite(VIB_PIN, on ? HIGH : LOW);
  digitalWrite(VIB2_PIN, on ? HIGH : LOW);
}
// Not remappable - the only candidate pin on a "pure" digital pin (D4) while
// every other non-remappable digital pin (D2/D3 encoder, D5/D6 wheel link,
// D8-D11 motor) stays hardcoded too, so remapping just this one wasn't worth
// the complexity. See PinConfig below for the 7 pins that ARE remappable.
const int     HANDBRAKE_PIN = 4;
// Clutch: analog pedal pot, read/scaled the same way as ACC_PIN/BRK_PIN.
// A3 is taken by VIB2_PIN here (v2 used A3 for clutch, but v4 uses it for the
// second pedal-rumble motor), so clutch moves to the next free analog pin.
uint8_t CLUTCH_PIN     = A4;
// Shifter: 2-axis analog stick sensing H-gate position (same pins/orientation
// as v2's MANUAL_TX_POT1/2), decoded into a gear index in readShifterGear().
uint8_t SHIFTER_X_PIN  = A7;
uint8_t SHIFTER_Y_PIN  = A6;

// H-pattern shifter: 2-axis analog stick -> gear index (0=neutral/no gate,
// 1-6=gear). Zone thresholds and the 3-gate layout (1-2 / 3-4 / 5-6) are
// ported as-is from SimRaceProv2's gear_from_gx_gy() (sim_race_pro_script.py),
// where they were already tuned against the physical shifter.
uint8_t readShifterGear() {
  int gx = map(analogRead(SHIFTER_X_PIN), 0, 1023, 0, 255);
  int gy = map(analogRead(SHIFTER_Y_PIN), 0, 1023, 0, 255);

  const int Y_UP_MAX = 125, Y_DOWN_MIN = 140;
  const int X_RIGHT_MAX = 104, X_CENTER_MIN = 110, X_CENTER_MAX = 132, X_LEFT_MIN = 138;

  int row = (gy <= Y_UP_MAX) ? 1 : (gy >= Y_DOWN_MIN) ? -1 : 0;  // 1=up, -1=down, 0=between gates
  int col;  // X inverted, matching v2's INVERT_GX
  if      (gx <= X_RIGHT_MAX)                        col =  1;  // right
  else if (gx >= X_CENTER_MIN && gx <= X_CENTER_MAX) col =  0;  // center
  else if (gx >= X_LEFT_MIN)                          col = -1;  // left
  else                                                 col =  2;  // between gates

  if (row == 0 || col == 2) return 0;
  if (col == -1) return (row == 1) ? 1 : 2;
  if (col ==  0) return (row == 1) ? 3 : 4;
  /*  col ==  1 */ return (row == 1) ? 5 : 6;
}

// ── Protokoll-Konstanten ──────────────────────────────────────────────────────
#define SYNC_BOX2WHEEL    0xCC  // Box → Wheel display packet
#define SYNC_BOX2PC       0xAA  // Box → PC sensor packet
#define SYNC_PC2BOX       0xBB  // PC → Box FFB packet
#define SYNC_PC2BOX_CFG   0xBC  // PC → Box spring config packet
#define PKT_WHEEL2BOX_LEN 4     // [btnHi][btnLo][rst][CRC8]
#define SYNC_WHEEL_VER    0xAE  // Wheel → Box version packet: [0xAE][maj][min][pat][CRC8]
#define PKT_BOX2WHEEL_LEN 9     // [0xCC][...7 data...][CRC8]
// [0xAA][angHi][angLo][acc][brk][btnHi][btnLo][extra][clutch][CRC8]
// extra: bit0=wheelReset bit1=handbrake bits2-4=shifterGear(0-6) bits5-7=reserved
#define PKT_BOX2PC_LEN    10
#define PKT_PC2BOX_LEN    7     // [0xBB][torque][rumble][packed][gear][speed][CRC8]
#define PKT_CFG_LEN       7     // [0xBC][startAngle][fullAngle][strength][pedalRumbleThr][pedalRumbleStr][CRC8]
#define SYNC_BOX_SETTINGS 0xBD  // sync byte: PC -> Box hardware/motor settings
#define PKT_BOX_SETTINGS_LEN 8  // [0xBD][simSetup][flags][motorMax][minL][minR][ramp][CRC8]
#define SYNC_PIN_CONFIG   0xBE  // sync byte: PC -> Box pin remap
#define PKT_PIN_CONFIG_LEN 9    // [0xBE][acc][brk][vib][vib2][clutch][shifterX][shifterY][CRC8]

// ── CRC-8 (Polynom 0x07) ─────────────────────────────────────────────────────
// Erkennt alle 1-Bit und 2-Bit-Fehler; deutlich besser als XOR bei EMI-Rauschen.
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

// ── ISR-safe TX to Wheel ─────────────────────────────────────────────────────
// SoftwareSerial.write() calls cli() for the entire byte (~1040 µs at 9600 baud),
// creating a 9.38 ms encoder-blind window per 9-byte TX packet.  At 2400 ticks/rev
// and 2 rev/s the encoder misses ~45 ticks per TX → ~7° of cumulative drift.
// This replacement keeps interrupts enabled and uses micros() absolute timing so
// INT0/INT1 (encoder ISR) can fire freely between bit transitions.
// Worst-case bit-timing error = max ISR latency / bit_period ≈ 25 µs / 104 µs = 24%.
// UART receivers sample at the bit centre (52 µs in); 25 µs shift still leaves
// 27 µs margin → reliable reception at 9600 baud on short cable (< 30 cm).
// Pin 6 = PD6 on Arduino Nano — same physical pin as SoftwareSerial TX.
// link.begin() sets PD6 as OUTPUT HIGH (idle); we reuse it directly.
// NOTE: this 9600-baud bit timing is hardcoded here and completely decoupled
// from link.begin() below - it does not use the SoftwareSerial TX path at all,
// so changing link.begin()'s baud (which now only affects this Box's own RX,
// see below) does not affect this function.
static void isrSafeTxByte(uint8_t b) {
    const uint32_t BIT_US = 104;               // 1 000 000 / 9 600 ≈ 104 µs
    uint32_t t0 = micros();
    PORTD &= ~(1 << 6);                        // start bit LOW
    for (uint8_t i = 0; i < 8; i++) {
        while ((uint32_t)(micros() - t0) < BIT_US * (i + 1));
        if (b & (1 << i)) PORTD |=  (1 << 6);
        else               PORTD &= ~(1 << 6);
    }
    while ((uint32_t)(micros() - t0) < BIT_US * 9);
    PORTD |= (1 << 6);                         // stop bit HIGH (idle)
    while ((uint32_t)(micros() - t0) < BIT_US * 10);
}
static void isrSafeTxWrite(const uint8_t* data, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) isrSafeTxByte(data[i]);
}

// ── RX from Wheel (via SoftwareSerial) ────────────────────────────────────────
// Two attempts to replace SoftwareSerial's RX with an ISR-safe receiver (to
// stop it blocking the encoder ~4ms/packet) were tried and reverted:
//   1. loop()-polled bit-bang: broken because loop() can't service a 104µs-wide
//      bit reliably (a single analogRead() alone takes ~100-200µs) -> nearly
//      every byte sampled wrong, all wheel buttons dead.
//   2. Timer0 compare-match interrupt (bit sampling driven by hardware instead
//      of loop()): still didn't work reliably in practice (steering drift and
//      reset problems persisted) - reverted without fully root-causing why.
// Back to stock SoftwareSerial for RX (proven reliable for button/reset
// correctness). The blocking cost during RX remains (only the TX side is
// ISR-safe) but is now cut ~4x by raising link.begin() below from 9600 to
// 38400 baud: cli()-blocking duration per byte is proportional to the bit
// period, so a 4x faster baud means a 4x shorter encoder-blind window per
// received byte (~1ms/packet instead of ~4ms/packet). This matches the Wheel's
// TX rate to the Box, which was likewise raised to 38400 (see linkTx in the
// wheel firmware) - both directions must agree on this Wheel->Box baud.
// This 9600->38400 RX bump is the actual fix for a steering-drift regression:
// an earlier revision of this firmware ran the link at 38400 baud end-to-end
// (no ISR-safe TX yet) and never showed noticeable drift. When isrSafeTxByte
// was introduced, link.begin() was dropped to 9600 to give it enough timing
// margin - but since that call also set this Box's RX baud (RX is still stock
// SoftwareSerial), it inadvertently made RX's blocking window 4x longer than
// before, which is what actually caused the drift users started reporting.
// See pitfalls memory for what NOT to retry without new information.
#define RX_RING_SIZE  16

static uint8_t rxRing[RX_RING_SIZE];
static uint8_t rxRingHead = 0, rxRingTail = 0;

// Drains whatever SoftwareSerial has buffered into our own ring buffer, so the
// packet parser below can peek several bytes ahead without consuming them
// (needed for the button-packet resync logic, see loop() step 1).
static void drainLinkRx() {
  while (link.available()) {
    uint8_t next = (uint8_t)((rxRingHead + 1) % RX_RING_SIZE);
    if (next == rxRingTail) break;  // our ring is full; leave the rest buffered in SoftwareSerial
    rxRing[rxRingHead] = (uint8_t)link.read();
    rxRingHead = next;
  }
}

static uint8_t rxAvailableLink() {
  return (uint8_t)((rxRingHead - rxRingTail + RX_RING_SIZE) % RX_RING_SIZE);
}
static uint8_t rxPeekLink() { return rxRing[rxRingTail]; }
// Peek without consuming, `offset` bytes ahead of the front (0 = next byte to read).
static uint8_t rxPeekOffsetLink(uint8_t offset) {
  return rxRing[(uint8_t)((rxRingTail + offset) % RX_RING_SIZE)];
}
static uint8_t rxReadLink() {
  uint8_t b = rxRing[rxRingTail];
  rxRingTail = (uint8_t)((rxRingTail + 1) % RX_RING_SIZE);
  return b;
}

// ── Spring-Parameter (vom PC konfigurierbar via 0xBC) ─────────────────────────
static uint8_t springStartAngle = 5;    // deadzone (degrees)
static uint8_t springFullAngle  = 90;   // angle at full force (degrees)
static uint8_t springStrength   = 100;  // 0-200, 100=1.0x
static uint8_t pedalRumbleThr   = 60;   // rumble threshold for pedal vibration (0-255)
static uint8_t pedalRumbleStr   = 100;  // pedal vibration intensity (0-150, 100=1.0x)

// ── Wheel firmware version (received at startup) ─────────────────────────────
static char wheelFwVersion[16] = "unknown";

// ── Wheel button state ────────────────────────────────────────────────────────
static uint8_t wheelBtnHigh = 0;
static uint8_t wheelBtnLow  = 0;
static uint8_t wheelReset   = 0;

// ── PC reply buffers ──────────────────────────────────────────────────────────
static uint8_t pcRxBuf[PKT_PC2BOX_LEN];  // 7 bytes
static uint8_t pcRxIdx  = 0;
static bool    pcRxSync = false;

static uint8_t cfgRxBuf[PKT_CFG_LEN];
static uint8_t cfgRxIdx  = 0;
static bool    cfgRxSync = false;

// Box-settings packet receiver (sync byte 0xBD)
static uint8_t bsRxBuf[PKT_BOX_SETTINGS_LEN];
static uint8_t bsRxIdx  = 0;
static bool    bsRxSync = false;

// Pin-config packet receiver (sync byte 0xBE)
static uint8_t prRxBuf[PKT_PIN_CONFIG_LEN];
static uint8_t prRxIdx  = 0;
static bool    prRxSync = false;

// ── Timing ────────────────────────────────────────────────────────────────────
// Two separate PC liveness timestamps, deliberately NOT merged:
//
//   lastPcDataTime - last CRC-valid 0xBB packet. Drives FFB: when this goes
//     stale the motor must fall back to the local spring curve, because acting
//     on a torque value that is seconds old is worse than acting on none.
//
//   lastPcByteTime - last byte of ANY kind seen on Serial. Drives the "is the
//     PC process still there at all" question, which is what gates whether we
//     keep streaming sensor packets back (step 3) .
//
// Why they must be separate: the Wheel link's stock SoftwareSerial RX runs
// entirely inside the PCINT ISR and blocks all interrupts ~1ms per button
// packet (see the "RX from Wheel" comment above). At 115200 baud that window
// silently eats ~10 incoming bytes, i.e. a whole PC->Box FFB packet. When the
// Wheel's cadence phase-locks against the PC's 10ms FFB tick this can hit many
// packets in a row. Gating step 3 on packet validity therefore used to make the
// Box go completely silent over a fault that never touched the Box->PC
// direction at all - which the PC then reported as a wheel disconnect. Byte
// activity survives that fault (the bytes still arrive, they are just framed
// wrong), so it is the honest signal for "PC still there".
unsigned long lastPcDataTime     = 0;
unsigned long lastPcByteTime     = 0;
unsigned long lastMotorRefresh   = 0;
unsigned long lastDisplayUpdate  = 0;
const unsigned long PC_TIMEOUT_MS           = 3000;  // tolerant of ISR-caused packet loss
const unsigned long MOTOR_REFRESH_MS        = 15;
const unsigned long DISPLAY_UPDATE_INTERVAL = 73;
bool pcConnected   = false;
bool gameFFBActive = false;
// True once lastPcDataTime has gone stale while the PC is still demonstrably
// there (bytes keep arriving). Reported to the PC via bit5 of the packet's
// extra byte so it can tell the user FFB is degraded - without us having to go
// silent to signal it. Cleared again by the next valid packet.
bool pcLinkStale   = false;

// ── Motor / FFB ───────────────────────────────────────────────────────────────
static int lastTorque = 127;
static int lastRumble = 0;
// Time constant of the incoming-torque low-pass filter, in ms (see the filter
// in step 4). Was a fixed per-packet alpha of 0.4, which silently re-tuned
// itself whenever the PC's send rate changed: at the original ~1kHz that alpha
// meant tau ~2ms (corner ~80Hz), but when the PC moved its FFB send onto a
// 10ms timer the very same line became tau ~20ms (corner ~8Hz) and swallowed
// most of the kerb/impact detail - a 20Hz texture arrived at 40% amplitude.
// Expressing it as a time constant and deriving alpha from the measured packet
// interval decouples the two: the filter now behaves identically whether
// packets arrive every 4ms or every 20ms.
static const int      TORQUE_TAU_MS  = 2;
static unsigned long  lastTorqueMs   = 0;

// ── Display data (sent to Wheel) ──────────────────────────────────────────────
static int  disp_rpm    = 0;
static char disp_gear[8]= "N";
static int  disp_speed  = 0;
static int  disp_rpmPct = 0;
static int  disp_blink  = 0;

static float smoothedDegrees = 0.0f;
static float lastRawDegrees  = 0.0f;  // spike-filter memory, see loop() step 2

// ── readPcPacket (non-blocking, CRC-8) ───────────────────────────────────────
bool readPcPacket() {
  while (Serial.available()) {
    uint8_t b = (uint8_t)Serial.read();
    if (!pcRxSync) {
      if (b == SYNC_PC2BOX) { pcRxBuf[0] = b; pcRxIdx = 1; pcRxSync = true; }
      continue;
    }
    pcRxBuf[pcRxIdx++] = b;
    if (pcRxIdx < PKT_PC2BOX_LEN) continue;

    // CRC over [torque][rumble][packed][gear][speed]
    if (crc8(pcRxBuf + 1, 5) == pcRxBuf[6]) {
      pcRxIdx = 0; pcRxSync = false;
      return true;
    }

    // CRC mismatch: the 0xBB we synced on was a payload byte that merely looked
    // like a header. Drop ONLY that byte and rescan the rest of the window for
    // the next candidate, instead of discarding all 7 bytes.
    //
    // Discarding the whole window used to lock the parser permanently. The PC's
    // idle packet is byte-identical every tick - torque 127 (centred wheel, no
    // game), everything else 0, gear N - and the CRC of that payload happens to
    // be 0xBB, the sync byte itself. So the idle stream carries a "BB BB" pair
    // at every packet boundary. Lose one byte to an ISR blackout (see the "RX
    // from Wheel" notes), sync on the wrong BB of that pair, and every following
    // 7-byte window sits 6 bytes off with another wrong BB waiting right behind
    // it - failing forever, at a fixed offset, with nothing to break the cycle
    // until the payload happens to change. Verified: 500 idle packets after one
    // lost byte parse as 0 valid / 499 failed the old way, 499 valid / 1 failed
    // this way. Same self-healing resync the PC's readPacket() and the wheel
    // button parser already do; this was the one framer that never got it.
    uint8_t i = 1;
    while (i < PKT_PC2BOX_LEN && pcRxBuf[i] != SYNC_PC2BOX) i++;
    if (i < PKT_PC2BOX_LEN) {
      pcRxIdx = (uint8_t)(PKT_PC2BOX_LEN - i);
      memmove(pcRxBuf, pcRxBuf + i, pcRxIdx);
      pcRxSync = true;
    } else {
      pcRxIdx = 0; pcRxSync = false;
    }
  }
  return false;
}

// ── Lokale Centering-Spring (logarithmische Kurve) ───────────────────────────
int angleResistanceTorque(float deg) {
  float a     = abs(deg);
  float start = (float)springStartAngle;
  float full  = (float)springFullAngle;
  float str   = (float)springStrength / 100.0f;
  if (a <= start) return 127;
  float range = (full - start < 1.0f) ? 1.0f : (full - start);
  float t = constrain((a - start) / range, 0.0f, 1.0f);
  // Linear spring curve: force grows proportionally with angle.
  // Matches the PC-side computeTorque behavior (springLinearity=0).
  float r = t * 90.0f * str;
  return (int)(127.0f + (deg > 0 ? -r : r));
}

// ── applyMotor ────────────────────────────────────────────────────────────────
// Stall-Schutz: Wenn der Motor lange gegen Widerstand hält ohne dass sich der
// Winkel ändert, wird der PWM schrittweise reduziert damit der BTS7960 nicht
// wegen Überstrom abschaltet. Bei Richtungswechsel sofort voller PWM.
static unsigned long stallStartTime = 0;
static int           stallLastDir   = 0;
static float         stallLastAngle = 0.0f;
static float         stallScale     = 1.0f;

// Set to false to disable stall protection (useful when motor is already
// current-limited via MOTOR_MAX_PWM). Set to true to re-enable.
bool STALL_PROTECTION_ENABLED = false;

// EEPROM-based persistent hardware configuration
#define EEPROM_MAGIC       0xA5
#define EEPROM_ADDR_MAGIC  0
#define EEPROM_ADDR_DATA   1

struct BoxConfig {
  uint8_t simSetup;         // 0=BOX_FULL 1=BOX_MEDIUM 2=BOX_BUDGET
  uint8_t flags;            // bit0=pedalRumble bit1=handbrake bit2=clutch
                            // bit3=shifter bit4=onlyWheel bit5=stallProt
                            // bit6=invertSteering bit7=invertFFB
  uint8_t motorMaxPwm;      // max PWM (default 178 = 70%)
  uint8_t motorMinPwmLeft;  // min PWM left direction (default 62)
  uint8_t motorMinPwmRight; // min PWM right direction (default 62)
  uint8_t softRampStep;     // max PWM change per cycle (default 12)
};

BoxConfig g_cfg;

void loadConfig() {
  if (EEPROM.read(EEPROM_ADDR_MAGIC) != EEPROM_MAGIC) {
    g_cfg = { 0, 0, 178, 62, 62, 12 };  // factory defaults
    EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
    EEPROM.put(EEPROM_ADDR_DATA, g_cfg);
  } else {
    EEPROM.get(EEPROM_ADDR_DATA, g_cfg);
  }
}

void saveConfig() { EEPROM.put(EEPROM_ADDR_DATA, g_cfg); }

void applyConfig() {
  sim_setup                = (SIM_SETUP)constrain(g_cfg.simSetup, 0, 2);
  PEDALS_VIBRATION_ENABLED = (g_cfg.flags & 0x01) != 0;
  HANDBRAKE_ENABLED        = (g_cfg.flags & 0x02) != 0;
  CLUTCH_ENABLED           = (g_cfg.flags & 0x04) != 0;
  SHIFTER_ENABLED          = (g_cfg.flags & 0x08) != 0;
  ONLY_WHEEL               = (g_cfg.flags & 0x10) != 0;
  STALL_PROTECTION_ENABLED = (g_cfg.flags & 0x20) != 0;
  INVERT_STEERING          = (g_cfg.flags & 0x40) != 0;
  INVERT_FFB               = (g_cfg.flags & 0x80) != 0;
}

// Separate EEPROM region (own magic byte) for the remappable peripheral
// pins - kept independent from BoxConfig's region (addr 1-6) above so a
// firmware update that adds/changes pin defaults never disturbs an already
// configured sim tier / motor calibration, and vice versa.
#define EEPROM_PIN_MAGIC      0xA6
#define EEPROM_ADDR_PIN_MAGIC 7
#define EEPROM_ADDR_PIN_DATA  8   // 7 bytes, occupies 8-14

struct PinConfig {
  uint8_t accPin, brkPin, vibPin, vib2Pin;
  uint8_t clutchPin, shifterXPin, shifterYPin;
};

PinConfig g_pins;

void loadPinConfig() {
  if (EEPROM.read(EEPROM_ADDR_PIN_MAGIC) != EEPROM_PIN_MAGIC) {
    g_pins = { A0, A1, A2, A3, A4, A7, A6 };  // factory defaults = today's hardwired pinout
    EEPROM.write(EEPROM_ADDR_PIN_MAGIC, EEPROM_PIN_MAGIC);
    EEPROM.put(EEPROM_ADDR_PIN_DATA, g_pins);
  } else {
    EEPROM.get(EEPROM_ADDR_PIN_DATA, g_pins);
  }
}

void savePinConfig() { EEPROM.put(EEPROM_ADDR_PIN_DATA, g_pins); }

// Applies g_pins to the live pin variables and (re)configures pinMode/offset
// calibration - called once from setup() and again whenever a fresh 0xBE
// packet updates g_pins, so a remap takes effect immediately without reboot.
void configurePins() {
  ACC_PIN = g_pins.accPin;   BRK_PIN  = g_pins.brkPin;
  VIB_PIN = g_pins.vibPin;   VIB2_PIN = g_pins.vib2Pin;
  CLUTCH_PIN    = g_pins.clutchPin;
  SHIFTER_X_PIN = g_pins.shifterXPin;
  SHIFTER_Y_PIN = g_pins.shifterYPin;

  pinMode(ACC_PIN, INPUT); pinMode(BRK_PIN, INPUT);
  ACC_OFFSET = analogRead(ACC_PIN); BRK_OFFSET = analogRead(BRK_PIN);
  // Only enabled pins are configured - an unconfigured floating input pin
  // would otherwise read noise and, for the clutch/shifter pots, produce
  // spurious values once the packet starts transmitting them.
  if (CLUTCH_ENABLED)  { pinMode(CLUTCH_PIN, INPUT); CLUTCH_OFFSET = analogRead(CLUTCH_PIN); }
  if (SHIFTER_ENABLED) { pinMode(SHIFTER_X_PIN, INPUT); pinMode(SHIFTER_Y_PIN, INPUT); }
  if (PEDALS_VIBRATION_ENABLED) { pinMode(VIB_PIN, OUTPUT); pinMode(VIB2_PIN, OUTPUT); }
}

const unsigned long STALL_GRACE_MS  = 300;   // ms before stall is detected
const unsigned long STALL_RAMP_MS   = 600;   // ms bis volle Reduktion
const float         STALL_MIN_SCALE = 0.40f; // minimale Skalierung (40%)
const float         STALL_DEAD_DEG  = 1.5f;  // Winkeltoleranz für "keine Bewegung"

void applyMotor(int torque) {
  if (sim_setup != BOX_FULL) return;

  // INVERT_FFB mirrors the torque byte around its 127 centre - applied here,
  // the single place both torque sources (PC-relayed lastTorque and the
  // local angleResistanceTorque() fallback) funnel through before reaching
  // the motor, so one flip covers both. Independent of INVERT_STEERING -
  // this is for a motor/H-bridge wired backwards, not an encoder issue.
  if (INVERT_FFB) torque = constrain(254 - torque, 0, 255);

  int dir = (torque > 129) ? 1 : (torque < 125) ? -1 : 0;
  unsigned long now = millis();

  if (STALL_PROTECTION_ENABLED) {
    if (dir != 0) {
      if (dir != stallLastDir) {
        stallLastDir   = dir;
        stallStartTime = now;
        stallLastAngle = smoothedDegrees;
        stallScale     = 1.0f;
      } else {
        float moved = abs(smoothedDegrees - stallLastAngle);
        if (moved > STALL_DEAD_DEG) {
          stallStartTime = now;
          stallLastAngle = smoothedDegrees;
          stallScale     = 1.0f;
        } else {
          unsigned long held = now - stallStartTime;
          if (held > STALL_GRACE_MS) {
            float ramp = (float)(held - STALL_GRACE_MS) / STALL_RAMP_MS;
            stallScale = 1.0f - ramp * (1.0f - STALL_MIN_SCALE);
            if (stallScale < STALL_MIN_SCALE) stallScale = STALL_MIN_SCALE;
          }
        }
      }
    } else {
      stallLastDir   = 0;
      stallStartTime = now;
      stallScale     = 1.0f;
    }
  }

  if (abs(torque - 127) > 2) {
    enableMotor();
    uint8_t mMax = g_cfg.motorMaxPwm;
    uint8_t mMin = (torque > 127) ? g_cfg.motorMinPwmRight : g_cfg.motorMinPwmLeft;
    if (mMin > mMax) mMin = mMax;   // guard against a nonsensical EEPROM combo

    // Map the command into [mMin, mMax] rather than into [0, mMax] and lifting
    // whatever came out too small up to mMin afterwards. That floor mapped every
    // command below it onto the *same* output: with the defaults (min 62, max
    // 178) the whole lower third of the range collapsed to one constant value,
    // so any fine modulation living in there - kerb texture, small impacts, the
    // detail this motor is supposed to convey - was flattened away before it
    // ever reached the H-bridge. Mapping keeps every command level
    // distinguishable while leaving the minimum force exactly where it was.
    int span   = (int)mMax - (int)mMin;
    int target = (int)mMin + (int)(((long)span * abs(torque - 127)) / 127L);
    target = constrain(target, (int)mMin, (int)mMax);
    target = (int)(target * stallScale);

    // Soft-ramp: separate lastPwm per direction so a direction change
    // resets the ramp to zero - prevents carry-over from the opposite side
    // which caused the MIN_PWM threshold to be bypassed on small angles.
    // The min-PWM check below is now purely a breakaway floor for the ramp-up
    // out of standstill (the ramp starts at 0 and would otherwise spend several
    // ticks below the motor's stiction threshold, whining without moving).
    // Steady-state output can no longer land under mMin - target is mapped into
    // [mMin, mMax] above - so it no longer quantises the low end of the range.
    static int lastPwmL = 0, lastPwmR = 0;

    int rampStep = g_cfg.softRampStep;
    if (torque > 127) {
      lastPwmL = 0;
      int f = lastPwmR + constrain(target - lastPwmR, -rampStep, rampStep);
      lastPwmR = f;
      if (f > 0 && f < g_cfg.motorMinPwmRight) f = g_cfg.motorMinPwmRight;
      moveMotorToRight(f);
    } else {
      lastPwmR = 0;
      int f = lastPwmL + constrain(target - lastPwmL, -rampStep, rampStep);
      lastPwmL = f;
      if (f > 0 && f < g_cfg.motorMinPwmLeft) f = g_cfg.motorMinPwmLeft;
      moveMotorToLeft(f);
    }
  } else {
    stopMotor();
    stallLastDir = 0;
    stallScale   = 1.0f;
  }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.setTimeout(5);
  // 38400 baud here only sets this Box's own RX (of the Wheel's button/version
  // packets) - this Box's TX to the Wheel bypasses SoftwareSerial entirely via
  // isrSafeTxWrite(), which is hardcoded to 9600 baud independent of this call.
  // See the "RX from Wheel" comment above for why 38400 (not 9600) matters here.
  link.begin(38400);

  // Load settings from EEPROM before hardware init
  loadConfig();
  applyConfig();
  loadPinConfig();

  if (sim_setup == BOX_FULL)   setupMotor();
  if (sim_setup != BOX_BUDGET) zeroOffset = myEnc.read();

  configurePins();
  // Handbrake pin is not remappable (see HANDBRAKE_PIN declaration above),
  // so its pinMode stays here rather than in configurePins().
  if (HANDBRAKE_ENABLED) pinMode(HANDBRAKE_PIN, INPUT_PULLUP);

  // ── Warte auf Wheel-Version, dann auf PC-PING ────────────────────────────
  // Phase 1: Wait up to 4s for wheel version packet [0xAE][maj][min][pat][CRC]
  //          Wheel sends this every 2s from its loop(), so we'll get it quickly.
  {
    unsigned long deadline = millis() + 4000;
    // Distinguishes "nothing at all arrived on the Wheel link" from "something
    // arrived, but never formed a valid version packet" - the latter almost
    // certainly means a Wheel is physically connected and powered but running
    // firmware that predates this version-packet protocol (e.g. v2), not a
    // cable/power problem. wheelFwVersion stays "unknown" (see declaration)
    // for the truly-silent case; set to "incompatible" below for the other.
    bool anyLinkByteSeen = false;
    while (millis() < deadline) {
      drainLinkRx();
      if (rxAvailableLink() > 0) anyLinkByteSeen = true;
      if (rxAvailableLink() >= 5) {
        uint8_t first = (uint8_t)rxPeekLink();
        if (first == SYNC_WHEEL_VER) {
          rxReadLink();  // consume 0xAE
          uint8_t maj = rxReadLink();
          uint8_t mn  = rxReadLink();
          uint8_t pat = rxReadLink();
          uint8_t crc = rxReadLink();
          uint8_t d[3] = {maj, mn, pat};
          if (crc == crc8(d, 3)) {
            snprintf(wheelFwVersion, sizeof(wheelFwVersion), "ver. %d.%d.%d", maj, mn, pat);
            break;  // got valid version – proceed to PING wait
          }
        } else {
          rxReadLink();  // discard unexpected byte, try again
        }
      }
    }
    if (strcmp(wheelFwVersion, "unknown") == 0 && anyLinkByteSeen) {
      strncpy(wheelFwVersion, "incompatible", sizeof(wheelFwVersion) - 1);
      wheelFwVersion[sizeof(wheelFwVersion) - 1] = '\0';
    }
  }

  // Phase 2: Wait up to 3s for PC PING (now we have wheel version ready)
  {
    char pingBuf[8]; uint8_t pingIdx = 0;
    unsigned long deadline = millis() + 3000;
    while (millis() < deadline) {
      if (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n') {
          pingBuf[pingIdx] = '\0'; pingIdx = 0;
          if (strncmp(pingBuf, "PING", 4) == 0) {
            Serial.println("OK"); Serial.flush();
            Serial.print("VER:BOX:"); Serial.print(FW_VERSION);
            Serial.print(":WHEEL:"); Serial.print(wheelFwVersion);
            Serial.print("\n"); Serial.flush();
            pcConnected = true;
            break;
          }
        } else if (c != '\r') {
          if (pingIdx < 7) pingBuf[pingIdx++] = c;
        }
      }
    }
  }
  lastPcDataTime    = millis();
  lastPcByteTime    = millis();
  lastMotorRefresh  = millis();
  lastDisplayUpdate = millis();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── 1. READ WHEEL DATA ────────────────────────────────────────────────────
  // Two packet types from wheel over the ISR-safe RX link (drained into our
  // own ring buffer by drainLinkRx() so the parser below can peek ahead, see
  // the Timer0 compare-match receiver above):
  //   [0xAE][maj][min][pat][CRC8]   → version packet (every 2s)
  //   [btnHi][btnLo][rst][CRC8]     → button packet (every loop)
  // pendingBtnPacket: set when a valid button packet arrives; cleared in step 7
  // after responding.  btnRxMs records when it arrived so step 7 can enforce a
  // 2 ms guard before TX — this lets any trailing version packet (sent right
  // after the button packet every 2 s, taking 1.3 ms) fully arrive before we
  // switch the half-duplex link to TX, eliminating half-duplex collisions.
  static bool          pendingBtnPacket = false;
  static unsigned long btnRxMs          = 0;
  drainLinkRx();
  if (rxAvailableLink() >= PKT_WHEEL2BOX_LEN) {
    uint8_t first = (uint8_t)rxPeekLink();
    if (first == SYNC_WHEEL_VER && rxAvailableLink() >= 5) {
      // Version packet
      rxReadLink();  // consume sync byte 0xAE
      uint8_t maj = rxReadLink();
      uint8_t mn  = rxReadLink();
      uint8_t pat = rxReadLink();
      uint8_t crc = rxReadLink();
      uint8_t d[3] = {maj, mn, pat};
      if (crc == crc8(d, 3)) {
        char newVer[16];
        snprintf(newVer, sizeof(newVer), "ver. %d.%d.%d", maj, mn, pat);
        if (strcmp(newVer, wheelFwVersion) != 0) {
          strncpy(wheelFwVersion, newVer, sizeof(wheelFwVersion) - 1);
          wheelFwVersion[sizeof(wheelFwVersion) - 1] = '\0';
          if (pcConnected) {
            Serial.print("VER:BOX:"); Serial.print(FW_VERSION);
            Serial.print(":WHEEL:"); Serial.print(wheelFwVersion);
            Serial.print("\n"); Serial.flush();
          }
        }
      }
    } else if (first != SYNC_WHEEL_VER) {
      // Button packet - has no sync byte, so peek all 4 bytes and only consume
      // them once the CRC confirms correct alignment. On mismatch, drop just
      // the first byte and re-check next iteration instead of consuming all 4:
      // a single dropped/corrupted byte would otherwise permanently misalign
      // this stream (every following 4-byte window would fail CRC forever,
      // since there is no sync byte here to resync against - this is what
      // made the wheel reset button "die" without a full resync path).
      uint8_t h = rxPeekOffsetLink(0);
      uint8_t l = rxPeekOffsetLink(1);
      uint8_t r = rxPeekOffsetLink(2);
      uint8_t c = rxPeekOffsetLink(3);
      uint8_t d[3] = { h, l, r };
      if (c == crc8(d, 3)) {
        rxReadLink(); rxReadLink(); rxReadLink(); rxReadLink();  // consume the full packet
        wheelBtnHigh = h;
        wheelBtnLow  = l;
        static uint8_t lastWheelReset = 0;
        if (r && !lastWheelReset) {
          zeroOffset = myEnc.read();
          ACC_OFFSET = analogRead(ACC_PIN);
          BRK_OFFSET = analogRead(BRK_PIN);
          if (CLUTCH_ENABLED) CLUTCH_OFFSET = analogRead(CLUTCH_PIN);
          // The angle jumps straight to ~0 deg this same loop() pass. Without
          // resetting these, the step-2 spike filter (see below) sees that as
          // a >22 deg "spike", freezes lastRawDegrees at the stale pre-reset
          // angle, and never accepts a new reading again - every subsequent
          // sample is still >22 deg from that frozen value, so it stays stuck
          // forever (until steering happens to swing back within 22 deg of
          // the frozen value). Resetting both here makes the filter treat the
          // post-reset angle as a fresh start instead of an outlier.
          lastRawDegrees  = 0.0f;
          smoothedDegrees = 0.0f;
        }
        lastWheelReset = r;
        wheelReset = r;
        pendingBtnPacket = true;
        btnRxMs          = millis();
      } else {
        rxReadLink();  // resync: drop only the misaligned first byte
      }
    }
  }

  // ── 2. READ SENSORS ───────────────────────────────────────────────────────
  // Overflow guard: re-zero if encoder drifts beyond 2x the mechanical limit.
  long rawCount = myEnc.read();
  if (abs(rawCount - zeroOffset) > maxTicks * 2L) zeroOffset = rawCount;
  long ticks   = constrain(rawCount - zeroOffset, -maxTicks, maxTicks);
  // INVERT_STEERING flips the sign here, at the earliest possible point -
  // every consumer of the angle (local offline spring below, the value sent
  // to the PC which drives its FFB/ViGEm axis, and the value sent to the
  // Wheel's display) derives from rawDeg/smoothedDegrees. Needed for units
  // where the encoder was wired/soldered in reverse. Since both FFB paths
  // (PC-side computeTorque and the local angleResistanceTorque() below)
  // derive their direction from this same angle, this alone also corrects
  // FFB direction for the common case where FFB is simply "wrong the same
  // way steering is". INVERT_FFB (see applyMotor() below) is a separate,
  // independent flip for the case where only the motor/H-bridge itself is
  // wired backwards - the two are orthogonal hardware faults and can be
  // combined if both are present.
  float rawDeg = (ticks / 2400.0f) * 360.0f * (INVERT_STEERING ? 1.0f : -1.0f) + degreeOffset;

  // Threshold raised to 22 deg - avoids filtering fast counter-steering moves.
  bool spike = (lastRawDegrees != 0.0f && abs(rawDeg - lastRawDegrees) > 22.0f);
  if (!spike) lastRawDegrees = rawDeg;
  float filtered = spike ? lastRawDegrees : rawDeg;
  if (smoothedDegrees == 0.0f) smoothedDegrees = filtered;
  else smoothedDegrees = filtered * 0.3f + smoothedDegrees * 0.7f;

  static float sAcc = 0.0f, sBrk = 0.0f;
  int rA = analogRead(ACC_PIN), rB = analogRead(BRK_PIN);
  if (sAcc == 0.0f) sAcc = rA; else sAcc = rA * 0.5f + sAcc * 0.5f;
  if (sBrk == 0.0f) sBrk = rB; else sBrk = rB * 0.5f + sBrk * 0.5f;
  int acc = constrain(map(abs((int)sAcc - ACC_OFFSET), 0, ACC_INPUT_MAX, 0, 255), 0, 255);
  int brk = constrain(map(abs((int)sBrk - BRK_OFFSET), 0, BRK_INPUT_MAX, 0, 255), 0, 255);
  acc = (acc < ACC_DEADZONE || ONLY_WHEEL) ? 0 : acc;
  brk = (brk < BRK_DEADZONE || ONLY_WHEEL) ? 0 : brk;

  bool handbrakePressed = HANDBRAKE_ENABLED && digitalRead(HANDBRAKE_PIN) == LOW;
  uint8_t shifterGear    = SHIFTER_ENABLED ? readShifterGear() : 0;
  int clutch = CLUTCH_ENABLED
             ? constrain(map(abs(analogRead(CLUTCH_PIN) - CLUTCH_OFFSET), 0, CLUTCH_INPUT_MAX, 0, 255), 0, 255)
             : 0;

  if (DEBUG) return;

  // ── 3. SEND SENSOR PACKET TO PC (10 Bytes, CRC-8) ────────────────────────
  // Only send after successful PING handshake so the autodetect buffer
  // contains only "OK\n" and nothing else during port scanning.
  if (pcConnected) {
    int16_t angleInt = (int16_t)(smoothedDegrees * 10.0f);
    uint8_t pkt[PKT_BOX2PC_LEN];
    pkt[0] = SYNC_BOX2PC;
    pkt[1] = (uint8_t)((angleInt >> 8) & 0xFF);
    pkt[2] = (uint8_t)(angleInt & 0xFF);
    pkt[3] = (uint8_t)acc;
    pkt[4] = (uint8_t)brk;
    pkt[5] = wheelBtnHigh;
    pkt[6] = wheelBtnLow;
    // extra: bit0=wheelReset bit1=handbrake bits2-4=shifterGear bit5=pcLinkStale
    // (bits 6-7 still free). bit5 polarity is deliberately "1 = stale, 0 = ok"
    // so firmware predating this bit - which sends 0 there - reads as a healthy
    // link on a newer PC app instead of raising a permanent false alarm.
    pkt[7] = (wheelReset ? 0x01 : 0x00) | (handbrakePressed ? 0x02 : 0x00)
           | ((shifterGear & 0x07) << 2) | (pcLinkStale ? 0x20 : 0x00);
    pkt[8] = (uint8_t)clutch;
    pkt[9] = crc8(pkt + 1, 8);
    Serial.write(pkt, PKT_BOX2PC_LEN);
  }

  // ── 4. READ PC REPLY (non-blocking) ──────────────────────────────────────
  // Binary packets plus the text PING handshake (for a PC that connects
  // while the box is already in the loop, without a DTR reset).
  {
    static char  textBuf[8]; static byte textIdx = 0;
    bool pcMsgReady = false;

    // Any inbound byte - correctly framed or not - proves the PC process is
    // still open and still writing to the port. Checked before the parser runs
    // so it also covers bytes that the parser goes on to discard as garbage,
    // which is exactly the case this is meant to survive. Deliberately does NOT
    // set pcConnected: that stays owned by the PING handshake and by a valid
    // packet, so a bare "PING\n" during the PC's port scan can never make us
    // start streaming binary packets before we have answered "OK" (which would
    // drown the handshake the scanner is listening for).
    if (Serial.available()) lastPcByteTime = now;

    while (Serial.available()) {
      uint8_t b = (uint8_t)Serial.peek();

      // A packet already being received always wins over a byte that merely
      // happens to match another packet type's sync value - checked first and
      // unconditionally (no byte-value test), before any "start a new packet"
      // branch gets a chance to run. Otherwise a payload byte equal to 0xBB/
      // 0xBC/0xBD (torque, rumble, gear, speed, ... all range over 0-255) could
      // hijack the parser mid-packet into a bogus new packet, dropping the one
      // in progress (CODE_REVIEW.md 1.3).
      if (cfgRxSync) {
        // 0xBC: Spring config packet, already in progress
        Serial.read();
        cfgRxBuf[cfgRxIdx++] = b;
        if (cfgRxIdx >= PKT_CFG_LEN) {
          cfgRxSync = false; cfgRxIdx = 0;
          if (crc8(cfgRxBuf + 1, 5) == cfgRxBuf[6]) {
            springStartAngle = cfgRxBuf[1];
            springFullAngle  = cfgRxBuf[2];
            springStrength   = cfgRxBuf[3];
            pedalRumbleThr   = cfgRxBuf[4];
            pedalRumbleStr   = cfgRxBuf[5];
          }
        }
      } else if (bsRxSync) {
        // Box-settings packet (0xBD), already in progress
        Serial.read();
        bsRxBuf[bsRxIdx++] = b;
        if (bsRxIdx >= PKT_BOX_SETTINGS_LEN) {
          bsRxSync = false; bsRxIdx = 0;
          if (crc8(bsRxBuf + 1, 6) == bsRxBuf[7]) {
            g_cfg.simSetup         = bsRxBuf[1];
            g_cfg.flags            = bsRxBuf[2];
            g_cfg.motorMaxPwm      = bsRxBuf[3];
            g_cfg.motorMinPwmLeft  = bsRxBuf[4];
            g_cfg.motorMinPwmRight = bsRxBuf[5];
            g_cfg.softRampStep     = bsRxBuf[6];
            saveConfig();
            applyConfig();
          }
        }
      } else if (prRxSync) {
        // Pin-config packet (0xBE), already in progress
        Serial.read();
        prRxBuf[prRxIdx++] = b;
        if (prRxIdx >= PKT_PIN_CONFIG_LEN) {
          prRxSync = false; prRxIdx = 0;
          if (crc8(prRxBuf + 1, 7) == prRxBuf[8]) {
            g_pins.accPin      = prRxBuf[1];
            g_pins.brkPin      = prRxBuf[2];
            g_pins.vibPin      = prRxBuf[3];
            g_pins.vib2Pin     = prRxBuf[4];
            g_pins.clutchPin   = prRxBuf[5];
            g_pins.shifterXPin = prRxBuf[6];
            g_pins.shifterYPin = prRxBuf[7];
            savePinConfig();
            configurePins();
          }
        }
      } else if (pcRxSync || b == SYNC_PC2BOX) {
        // 0xBB: normal FFB packet, already in progress or starting now
        if (readPcPacket()) { pcMsgReady = true; break; }
        else break;
      } else if (b == SYNC_PC2BOX_CFG) {
        // 0xBC: start of a new spring config packet
        Serial.read();
        cfgRxBuf[0] = b; cfgRxIdx = 1; cfgRxSync = true;
      } else if (b == SYNC_BOX_SETTINGS) {
        // start of a new box-settings packet
        Serial.read();
        bsRxBuf[0] = b; bsRxIdx = 1; bsRxSync = true;
      } else if (b == SYNC_PIN_CONFIG) {
        // start of a new pin-config packet
        Serial.read();
        prRxBuf[0] = b; prRxIdx = 1; prRxSync = true;
      } else if (!pcRxSync) {
        // Text byte – PING handshake (PC connecting after box already in loop)
        Serial.read();
        char c = (char)b;
        if (c == '\n') {
          textBuf[textIdx] = '\0'; textIdx = 0;
          if (strncmp(textBuf, "PING", 4) == 0) {
            Serial.println("OK"); Serial.flush();
            Serial.print("VER:BOX:"); Serial.print(FW_VERSION);
            Serial.print(":WHEEL:"); Serial.print(wheelFwVersion);
            Serial.print("\n"); Serial.flush();
            pcConnected    = true;
            pcLinkStale    = false;
            lastPcDataTime = now;
          }
        } else if (c != '\r') {
          if (textIdx < 7) textBuf[textIdx++] = c;
        }
      } else {
        // Unknown byte – discard
        Serial.read();
      }
    }

    if (pcMsgReady) {
      uint8_t torqueVal = pcRxBuf[1];
      uint8_t rumbleVal = pcRxBuf[2];
      uint8_t packed    = pcRxBuf[3];
      bool gameActive   = (packed & 0x01) != 0;
      bool blinkFlag    = (packed & 0x02) != 0;
      uint8_t rpmPct    = (packed >> 2) * 4;

      lastPcDataTime = now;
      pcConnected    = true;
      pcLinkStale    = false;

      // Clamp the per-update step to +-60 instead of dropping the packet
      // outright: a discarded update left the target beyond the threshold
      // forever (every following packet carries the same target), so
      // lastTorque would freeze on the old value until the PC value drifted
      // back into range on its own. Clamping lets it ramp there instead.
      int steppedTarget = lastTorque + constrain((int)torqueVal - lastTorque, -60, 60);
      // Low-pass filter to smooth torque changes and reduce motor judder/gear
      // noise. Alpha is derived from the *measured* packet interval instead of
      // being a fixed 0.4 per packet, so the filter keeps the same bandwidth
      // whatever rate the PC sends at - see TORQUE_TAU_MS for what the fixed
      // alpha silently did when the PC's send rate changed.
      //   alpha = dt / (tau + dt)   (integer form, no float/exp on the AVR)
      unsigned long dtMs = now - lastTorqueMs;
      lastTorqueMs = now;
      if (dtMs < 1)  dtMs = 1;    // two packets inside one millisecond
      if (dtMs > 50) dtMs = 50;   // long gap (stall/reconnect): snap, don't creep
      int num = (int)dtMs;
      int den = num + TORQUE_TAU_MS;
      // Numerator stays small: the step above bounds the difference to +-60, so
      // 60 * 50 = 3000 fits an int comfortably.
      lastTorque += ((steppedTarget - lastTorque) * num) / den;
      lastRumble    = (int)rumbleVal;
      gameFFBActive = gameActive;
      disp_rpmPct   = (int)rpmPct;
      disp_blink    = blinkFlag ? 1 : 0;

      // Gear byte: 0=R, 1=N, 2=1st, 3=2nd, ...
      uint8_t gearByte = pcRxBuf[4];
      if      (gearByte == 0) strcpy(disp_gear, "R");
      else if (gearByte == 1) strcpy(disp_gear, "N");
      else { itoa(gearByte - 1, disp_gear, 10); }

      disp_speed = (int)pcRxBuf[5];  // km/h, see CODE_REVIEW.md 1.1
    }
  }

  // ── 5. MOTOR FFB (15ms timer, runs independently) ────────────────────────
  if (now - lastMotorRefresh >= MOTOR_REFRESH_MS) {
    lastMotorRefresh = now;
    bool pcAlive = pcConnected && (now - lastPcDataTime < PC_TIMEOUT_MS);
    // Use PC torque when connected (PC sends local spring torque when no game active).
    // Only fall back to local spring when PC is truly offline.
    int torque = pcAlive ? lastTorque : angleResistanceTorque(smoothedDegrees);
    applyMotor(torque);

    if (PEDALS_VIBRATION_ENABLED) {
      bool pedalOn = (lastRumble >= (int)pedalRumbleThr);
      // Scale vibration: pedalRumbleStr 100=full on/off, lower=PWM dimming.
      // Only the target duty is set here (15ms motor tick) - the waveform
      // itself is serviced every loop() pass below, see 4.7.
      swPwmDuty = pedalOn ? (uint8_t)((255UL * pedalRumbleStr) / 100) : 0;
    }
  }

  // Soft-PWM (pedal rumble motors, A2/A3) has a 20ms period - servicing it only
  // from the 15ms motor tick above re-evaluates the on/off edge too coarsely
  // (aliasing), making the intensity barely adjustable in practice. Called
  // unconditionally every loop() pass instead; near-free when duty is 0
  // (vibration disabled/off).
  updateSoftPwm();

  // ── 6. PC TIMEOUT ────────────────────────────────────────────────────────
  // Split into two independent levels, see the lastPcDataTime/lastPcByteTime
  // comment at the top of this file.

  // 6a. Packet stall: no valid FFB packet for a while, but the PC is still
  // writing to the port. FFB has already fallen back to the local spring curve
  // in step 5 (pcAlive uses lastPcDataTime), so all that is left here is to
  // neutralise the display state and raise the flag the PC reads back via
  // bit5. Deliberately does NOT reset the spring parameters: those only ever
  // arrive via an explicit 0xBC packet (on connect or on a settings save), so
  // clearing them over a transient stall would silently drop the user's tuning
  // until the next save with nothing to restore it.
  if (pcConnected && (now - lastPcDataTime > PC_TIMEOUT_MS)
                  && (now - lastPcByteTime <= PC_TIMEOUT_MS)) {
    if (!pcLinkStale) {
      pcLinkStale   = true;
      gameFFBActive = false;
      lastTorque    = 127;
      lastRumble    = 0;
      strcpy(disp_gear, "N");
    }
  }

  // 6b. Real disconnect: nothing at all has arrived, so the PC process is gone
  // (closed, crashed, or unplugged). Full reset to standalone defaults, and
  // stop streaming sensor packets - the silence is what lets the PC's next
  // autoDetect() port scan see a clean "OK\n" handshake instead of a flood of
  // 0xAA packets it cannot parse (which would leave it unable to reconnect at
  // all; the DTR reset on open() does not reliably rescue this, see the
  // autoDetect notes on ports that were already enumerated).
  if (pcConnected && (now - lastPcByteTime > PC_TIMEOUT_MS)) {
    pcConnected      = false;
    pcLinkStale      = false;
    gameFFBActive    = false;
    lastTorque       = 127;
    lastRumble       = 0;
    strcpy(disp_gear, "N");
    // Reset to safe defaults on PC disconnect.
    springStartAngle = 5;
    springFullAngle  = 90;
    springStrength   = 100;
    stopMotor();
  }

  // ── 7. SEND DISPLAY DATA TO WHEEL ────────────────────────────────────────
  // Synchronous protocol: respond to each button packet after a 6 ms guard.
  // Why the guard: the Wheel occasionally sends a 5-byte version packet
  // immediately after the 4-byte button packet (every 2 s).  At the Wheel's
  // 38400-baud TX rate the combined 9-byte TX takes ~2.3 ms.  The 6 ms guard
  // (measured from when the button packet's last byte was detected) ensures
  // the version packet is always fully received before we switch to TX,
  // eliminating half-duplex collisions (comfortable margin even accounting
  // for occasional SoftwareSerial RX jitter).
  // Fallback timer at 73 ms (not a multiple of the Wheel's ~20 ms cycle)
  // keeps the OLED alive if no button packets arrive and prevents phase lock.
  bool syncReady = pendingBtnPacket && ((unsigned long)(millis() - btnRxMs) >= 6);
  if (syncReady || (now - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL)) {
    pendingBtnPacket = false;
    lastDisplayUpdate = now;
    float ang = smoothedDegrees - degreeOffset;
    int16_t angInt = (int16_t)(ang * 10.0f);
    uint8_t gearCode = 1;
    if      (disp_gear[0] == 'R') gearCode = 0;
    else if (disp_gear[0] == 'N') gearCode = 1;
    else { int g = atoi(disp_gear); if (g >= 1 && g <= 10) gearCode = (uint8_t)(g + 1); }
    uint8_t wpkt[PKT_BOX2WHEEL_LEN];
    wpkt[0] = SYNC_BOX2WHEEL;
    wpkt[1] = (uint8_t)((angInt >> 8) & 0xFF);
    wpkt[2] = (uint8_t)(angInt & 0xFF);
    wpkt[3] = (uint8_t)acc;
    wpkt[4] = (uint8_t)brk;
    wpkt[5] = (uint8_t)constrain(disp_rpmPct, 0, 100);
    wpkt[6] = (uint8_t)constrain(disp_speed,  0, 255);
    wpkt[7] = (uint8_t)((gearCode << 1) | (disp_blink ? 0x01 : 0x00));
    wpkt[8] = crc8(wpkt + 1, 7);
    isrSafeTxWrite(wpkt, PKT_BOX2WHEEL_LEN);
  }
}
