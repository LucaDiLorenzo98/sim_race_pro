#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _USE_MATH_DEFINES

#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include "resource.h"
#include "SimRaceProDefs.h"
#include <TlHelp32.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <cstdint>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <sstream>
#include <map>
#include <deque>
#include <cmath>
#include <functional>
#include <cstdio>

#include "src/r3e.h"
#include "src/SharedMemory.h"
#include "src/ACC.h"
#pragma warning(push)
#pragma warning(disable: 6001)  // uninitialized memory in ViGEm external headers
#pragma warning(disable: 6302)  // char* passed as LPWSTR in ViGEm
#include "src/ViGEmClient.h"
#pragma warning(pop)
#include "src/SCSSharedMemory.h"
#include "src/irsdk_minimal.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "ViGEmClient.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")

// ── Constants ─────────────────────────────────────────────────────────────────
#define BAUD_RATE                   CBR_115200
#define DISCONNECT_TIMEOUT_MS       150   // UDP pause detection: F1@60Hz = 16ms/pkt, 150ms = ~9 missed pkts
// Wheel-serial stall detection: independent of writePacket() succeeding (see
// runBackend's FFB send block) - a stalled Box→PC stream is otherwise
// invisible on the PC side since WriteFile to a still-open COM handle
// "succeeds" even when nothing is listening.
// 500ms was tried first and was too tight in practice - it fired on normal
// SoftwareSerial/Timer-ISR jitter on the Box, causing spurious reconnect
// cycling every few seconds even at idle. The Box's own PC_TIMEOUT_MS is
// 3000ms for exactly the same jitter source (see pitfalls memory #12 - it
// used to be 1000ms and had to be raised for the same reason). Kept safely
// below that so the PC starts reconnecting slightly before the Box gives up
// and resets FFB to neutral on its own.
// This is the SOFT threshold: past it, the GUI shows "disconnected" but the
// COM handle is deliberately left open (see WHEEL_HARD_RECONNECT_TIMEOUT_MS).
#define WHEEL_DISCONNECT_TIMEOUT_MS 2500
// Hard threshold: only past THIS point do we close() and rescan COM ports.
// Confirmed by directly observing the Box reboot at the moment of a
// reconnect: closing/reopening the port re-asserts DTR, which fires the
// Arduino's auto-reset circuit. Doing that for every transient stall turned
// a harmless multi-second hiccup into a full Arduino reboot-and-rescan cycle
// - worse than the stall itself. Most stalls clear up on their own well
// within this window if the PC just keeps talking on the same handle (which
// it does - see the FFB send block). Only escalate once it's been stalled
// far longer than any known jitter source explains, i.e. likely a real
// unplug/failure where a rescan is actually warranted.
#define WHEEL_HARD_RECONNECT_TIMEOUT_MS 8000
// A single failed writePacket() (WriteFile returning false) used to escalate
// straight to the disruptive close()+rescan+DTR-reboot above, with no
// debounce. A momentary USB/driver hiccup (write timeout under COMMTIMEOUTS,
// ~17ms for a 7-byte packet) is enough to trigger that on its own even
// though the Box is fine and the handle recovers on the very next FFB tick.
// Require this many *consecutive* write failures before treating the handle
// as actually dead - reached quickly enough (~40ms) for a genuine
// unplug/failure, but ignores single transient blips.
// NOTE: this is a count, but the property that matters is the *time* it
// covers, so it has to track FFB_TX_INTERVAL_MS. It was 4 while the FFB tick
// was 10ms (40ms); at the 4ms tick the same 40ms window needs 10.
#define WRITE_FAIL_STREAK_THRESHOLD 10

// ── FFB loop timing ──────────────────────────────────────────────────────────
// How often the FFB torque is computed and sent to the Box, and how often the
// shared-memory readers are polled.
//
// These are not free-choice numbers: the Box smooths the incoming torque with
// a low-pass filter, so the send interval directly sets how much kerb/impact
// detail survives the trip to the motor. Until v3.1.1 the FFB was sent from
// inside "if (readPacket())" - i.e. at the Box's own packet rate, roughly
// 1kHz - and the Box's filter ran at that rate with it. Decoupling the send
// onto a 10ms timer (the fix for the mutual-stall deadlock, see the comment at
// the send site) cut that to 100Hz and, without anyone touching the filter
// constant, dropped its corner frequency from ~80Hz to ~8Hz: a 20Hz kerb
// texture arrived at 40% amplitude instead of 97%. That is exactly the band
// kerbs and impacts live in, and it is why the wheel went numb.
//
// 4ms restores the bandwidth while keeping the decoupled send that fixed the
// deadlock. The Box's filter is time-based now (see TORQUE_TAU_MS in the Box
// sketch), so it no longer silently re-tunes itself when this value changes -
// but WRITE_FAIL_STREAK_THRESHOLD above still has to be kept in step.
#define FFB_TX_INTERVAL_MS          4
// Shared-memory poll interval. Must not be slower than the FFB tick, or the
// FFB would be recomputed from telemetry it has already seen (no new detail,
// just repeated samples). The readers only memcpy a struct, so 250Hz is cheap.
#define TELEMETRY_POLL_INTERVAL_MS  4
#define RECONFIG_HOLD_TIME_MS       3000
#define GAME_DETECTION_INTERVAL_MS  1000
// IDR_WHEEL_BMP comes from resource.h (included above) - see CODE_REVIEW.md 2.5
#define WM_TRAYICON                 (WM_APP + 50)
#define ID_TRAY_OPEN                9001
#define ID_TRAY_EXIT                9002

// RPM LED / shift-light thresholds, relative to maxRpm - single source of
// truth shared by the backend (FFB blink decision) and the GUI overlay so
// the two can't drift apart (see CODE_REVIEW.md 1.9). Reference values from
// a Porsche 922 redline (6600/7400/8200/8400 of 8880 RPM). The Wheel
// firmware's RPM_LED_1/2/3_THRESHOLD (74/83/92, integer percent) must be
// kept in sync with these manually - it can't include this header.
static constexpr float RPM_GREEN  = 6600.0f / 8880.0f;  // 74.3% - green LEDs on
static constexpr float RPM_YELLOW = 7400.0f / 8880.0f;  // 83.3% - yellow LEDs on
static constexpr float RPM_RED    = 8200.0f / 8880.0f;  // 92.3% - red LEDs on
static constexpr float RPM_BLINK  = 8400.0f / 8880.0f;  // 94.6% - blink / shift-light

// WM_APP messages (backend → GUI), listed in ascending offset order
#define WM_APP_CONN_STATUS   (WM_APP + 1)
#define WM_APP_GAME_CHANGE   (WM_APP + 2)
#define WM_APP_BTNMAP_UPDATE (WM_APP + 3)
#define WM_APP_INPUT_UPDATE  (WM_APP + 4)
#define WM_APP_LOG           (WM_APP + 5)
#define WM_APP_CALIB_NEXT    (WM_APP + 10)
#define WM_APP_CALIB_MAPPED  (WM_APP + 11)
#define WM_APP_CALIB_DONE    (WM_APP + 12)
#define WM_APP_ASK_CALIB     (WM_APP + 13)
#define WM_APP_HW_DIALOG     (WM_APP + 20)
#define WM_APP_UPDATE_AVAIL  (WM_APP + 22)  // GitHub update available (LPARAM = new std::string*)
#define WM_APP_FW_MISMATCH   (WM_APP + 23)  // firmware mismatch detected (LPARAM = FirmwareInfo*)
#define WM_APP_FW_UPDATE_OK  (WM_APP + 24)  // firmware update completed
#define WM_APP_BOX_SETTINGS  (WM_APP + 25)  // box settings changed - resend to box
#define WM_APP_WELCOME       (WM_APP + 26)  // first-run welcome notice (reminds user to flash Arduinos manually)
#define WM_APP_WHEEL_MISSING (WM_APP + 27)  // Box connected but never heard from the Wheel (not a firmware mismatch)
#define WM_APP_WIRING_NEXT   (WM_APP + 28)  // wiring setup: highlight next button position
#define WM_APP_WIRING_DONE   (WM_APP + 29)  // wiring setup finished/aborted - clear highlight
#define WM_APP_ASK_WIRING    (WM_APP + 30)  // ask user to start the wiring setup wizard
#define WM_APP_CFG_RESET     (WM_APP + 31)  // app version changed - configs were auto-reset (LPARAM = new std::string* old version)

// Xbox button bitmask values
enum XButtons {
    XBTN_A         = 0x1000, XBTN_B         = 0x2000, XBTN_X       = 0x4000, XBTN_Y     = 0x8000,
    XBTN_UP        = 0x0001, XBTN_DOWN       = 0x0002, XBTN_LEFT    = 0x0004, XBTN_RIGHT = 0x0008,
    XBTN_START     = 0x0010, XBTN_BACK       = 0x0020,
    XBTN_LSHOULDER = 0x0100, XBTN_RSHOULDER  = 0x0200,
    XBTN_LTHUMB    = 0x0040, XBTN_RTHUMB     = 0x0080, XBTN_GUIDE   = 0x0400
};

// ── Telemetry data ─────────────────────────────────────────────────────────────
// -- FirmwareInfo: passed as LPARAM with WM_APP_FW_MISMATCH ---------------
// Heap-allocated by the backend; WndProc owns it and must delete after use.
struct FirmwareInfo {
    std::string boxVer, wheelVer;           // detected versions
    std::string expectedBox, expectedWheel; // required versions
    std::string comPort;                    // COM port box was found on
    bool        boxMismatch   = false;
    bool        wheelMismatch = false;
};

struct TelemetryData {
    std::string gameName        = "None";
    int         f1Year          = 0;     // F1 packet format year (2018-2024), 0 for non-F1
    float       speed           = 0.0f;
    int         rpm             = 0;
    int         maxRpm          = 0;     // live per-car max RPM from telemetry (0 = unknown)
    int         gear            = 0;
    float       throttle        = 0.0f;  // 0-255
    float       brake           = 0.0f;  // 0-255
    float       g_lat           = 0.0f;  // lateral g-force (g-units)
    bool        absActive       = false;
    bool        tcActive        = false;
    bool        gamePaused      = false;  // true = game paused/menu → FFB neutral
    bool        shiftLight      = false;  // game-native shift/blink indicator
    float       suspensionTravel[4]   = { 0 };
    // Per-wheel vibration signal, in m/s of suspension movement for every reader
    // that has it (R3E native, ACC/AC differentiated from suspensionTravel in the
    // reader). Drives both the rumble motors and the wheel texture force, so a
    // reader that leaves this at 0 has no surface feel at all.
    float       suspensionVelocity[4] = { 0 };
    // ACC/AC only: max |wheelSlip| across the four wheels. Kept separate from
    // suspensionVelocity because it is a *slide/lockup* indicator in its own
    // unit domain, not suspension movement - the two used to share the field,
    // which meant kerbs (a suspension event) had to be inferred from slip and
    // effectively never registered. 0 for every other game.
    float       wheelSlipMax          = 0.0f;
};

// ── Globals ────────────────────────────────────────────────────────────────────
extern std::map<int, USHORT>    buttonMap;
extern std::mutex               buttonMapMutex; // guards buttonMap AND g_posToPhys (GUI writes vs. backend reads)
// Wiring layout: visual button position (index into the GUI's BTN_POS overlay
// array, 0-15) -> physical GPIO button bit (0-15) as sent by the Wheel.
// -1 = position not wired to any physical button. Default is identity (pos i
// -> bit i) so installs without a layout.ini behave exactly as before.
// Persisted in layout.ini, set up by the wiring wizard before button mapping.
extern int                      g_posToPhys[16];
extern TelemetryData            currentTelemetry;
extern std::mutex               telemetryMutex;
extern std::atomic<bool>        dataReceivedThisFrame;
extern std::atomic<float>       lastTorque;   // atomic; 0-255, center=127, used by GUI

float clamp(float val, float min, float max);
BOOL  isProcessRunning(const wchar_t* name);

// Wird in main.cpp gesetzt wenn die EXE mit -debug gestartet wurde.
// Steuert die Sichtbarkeit des FFB-Debug-Buttons in der GUI.
extern bool g_debugMode;

// ── GUI shared state ───────────────────────────────────────────────────────────
struct GuiState {
    HWND hwnd = nullptr;
    std::atomic<bool> running{ false };

    std::atomic<bool> connected{ false };
    // Box→PC extra bit5: the Box is still streaming to us, but our PC→Box FFB
    // packets have not been reaching it for >PC_TIMEOUT_MS, so it has fallen
    // back to its local spring curve. Deliberately separate from `connected`:
    // the link is up and steering/pedals/buttons are fine, only the downlink is
    // degraded, so reporting it as a disconnect would be wrong. Always false
    // against firmware older than 3.1.7, which sends 0 in that bit.
    std::atomic<bool> pc_link_stale{ false };
    std::string       com_port;
    std::string       active_game;

    float steering  = 0.0f;  // degrees
    float throttle  = 0.0f;  // 0..1
    float brake     = 0.0f;  // 0..1
    float clutch    = 0.0f;  // 0..1
    int   handbrake = 0;
    // GetTickCount64() of the last wheel-reset press (Box→PC extra bit0).
    // The GUI keeps the RESET pill lit for a short hold-off after release so
    // a brief tap doesn't vanish between ~60Hz repaints. 0 = never pressed.
    std::atomic<ULONGLONG> wheel_reset_tick{ 0 };

    std::vector<int>       active_buttons;
    std::map<int, USHORT>  buttonMap;

    std::atomic<bool> has_clutch    { false };
    std::atomic<bool> has_handbrake { false };
    std::atomic<bool> has_shifter   { false };
    std::atomic<bool> hw_dialog_done{ false };
    std::atomic<bool> welcome_done{ false };        // welcome notice acknowledged
    std::atomic<bool> cfg_reset_done{ false };      // version-change auto-reset notice acknowledged
    std::atomic<bool> resend_spring_cfg { false };  // set when FFB settings are saved
    // Set by WndProc when the firmware mismatch notice is dismissed.
    // Backend waits on this before retrying the wheel scan.
    std::atomic<bool> fw_update_done{ false };
    // Set by GUI when box settings are saved - backend re-sends 0xBD packet.
    std::atomic<bool> resend_box_settings{ false };
    // Set by GUI when the pin configuration is saved - backend re-sends 0xBE packet.
    std::atomic<bool> resend_pin_config{ false };
    std::atomic<bool> ffb_debug_on      { false };  // gesetzt wenn Debug-Button aktiv
    // Set by the backend right before it posts WM_CLOSE to itself (e.g. the
    // Start+Back reconfig-hold trigger). Tells WndProc's WM_CLOSE handler to
    // skip the "Do you want to exit?" prompt - that prompt is only for the
    // user directly closing the app (X button / Alt+F4 / tray Exit).
    std::atomic<bool> auto_close_requested{ false };

    // Calibration state
    std::string       calib_current_button;
    int               calib_step  = 0;
    int               calib_total = 15;
    bool              calib_waiting = false;
    bool              calib_done    = false;
    std::atomic<bool> calib_aborted        { false };
    std::atomic<bool> start_calib_requested{ false };
    int               calib_last_pressed_idx = -1;

    // Wiring setup state (position -> physical button assignment wizard).
    // Runs before the Xbox button mapping wizard on first setup; the GUI
    // highlights wiring_pos on the wheel image and the user presses the
    // matching physical button.
    int               wiring_pos     = -1;    // currently highlighted position, -1 = wizard idle
    int               wiring_total   = 16;
    bool              wiring_waiting = false; // handshake: backend waits until GUI showed the step
    std::atomic<bool> wiring_aborted         { false };
    std::atomic<bool> wiring_skip            { false }; // skip current position (no physical button)
    std::atomic<bool> start_wiring_requested { false };

    std::deque<std::string> log_lines;
    std::mutex              mtx;

    void log(const std::string& msg) {
        std::lock_guard<std::mutex> lk(mtx);
        log_lines.push_back(msg);
        if (log_lines.size() > 100) log_lines.pop_front();
        if (hwnd) PostMessage(hwnd, WM_APP_LOG, 0, 0);
    }
};

// ── Language manager ───────────────────────────────────────────────────────────
class LanguageManager {
    std::map<std::string, std::string> strings;
public:
    LanguageManager();
    std::string get(const std::string& key) const;
};
extern LanguageManager* lang;

// Forward declaration – FfbSettings wird weiter unten vollständig definiert
struct FfbSettings;

// ── Serial port ────────────────────────────────────────────────────────────────
struct BoxSettings;
struct BoxPinConfig;

class SerialPort {
    HANDLE  hSerial        = INVALID_HANDLE_VALUE;
    // readPacket state as members (was static locals → not reentrant)
    uint8_t rxBuf[128]     = {};
    int     rxLen          = 0;
    bool    rxSynced       = false;
public:
    bool        open(const char* portName, int baudRate);
    std::string autoDetect(int baudRate, std::string& outBoxVer, std::string& outWheelVer);
    bool        readPacket(uint8_t* buf9);
    bool        writePacket(uint8_t torque, uint8_t rumble, uint8_t rpmPct, bool blink, bool gameActive, int8_t gear = 0, uint8_t speed = 0);
    bool        sendSpringConfig(const FfbSettings& s);
    bool        sendBoxSettings(const BoxSettings& s);
    bool        sendPinConfig(const BoxPinConfig& p);
    bool        writeLine(const std::string& line);
    void        purgeRx();
    void        close();
};

// ── Telemetry readers ──────────────────────────────────────────────────────────
class F1Reader {
    SOCKET      sock             = INVALID_SOCKET;
    bool        running          = false;
    std::thread thread;
    std::chrono::steady_clock::time_point lastDataTime;
    bool        externallyActive = false;

#pragma pack(push, 1)
    struct PacketHeader {
        uint16_t m_packetFormat; uint8_t m_gameMajorVersion; uint8_t m_gameMinorVersion;
        uint8_t  m_packetVersion; uint8_t m_packetId; uint64_t m_sessionUID; float m_sessionTime;
        uint32_t m_frameIdentifier; uint8_t m_playerCarIndex; uint8_t m_secondaryPlayerCarIndex;
    };
    struct CarTelemetryData {
        uint16_t m_speed; float m_throttle; float m_steer; float m_brake;
        uint8_t  m_clutch; int8_t m_gear; uint16_t m_engineRPM;
    };
#pragma pack(pop)
    void loop();
public:
    F1Reader();
    void setActive(bool active);
    bool isActive() const;
    void start();
    void stop();
    bool checkTimeout();
};

class AMS2Reader {
    HANDLE       hMapFile        = nullptr;
    SharedMemory* pView          = nullptr;
    SharedMemory  localCopy      = {};
    bool          externallyActive = false;
public:
    void setActive(bool active);
    bool isActive() const;
    void update();
};

class R3EReader {
    HANDLE hMapFile          = nullptr;
    double lastSimTime       = -1.0;
    std::chrono::steady_clock::time_point lastDataTime;
    bool   externallyActive  = false;
public:
    void setActive(bool active);
    bool isActive() const;
    void update();
};

class ACCReader {
    HANDLE hPhysics          = nullptr;
    HANDLE hGraphics         = nullptr;
    HANDLE hStatic           = nullptr;
    bool   externallyActive  = false;
public:
    void setActive(bool active);
    bool isActive() const;
    void update();
};

// Forza Motorsport / Horizon (UDP Dash format, port 5300)
class ForzaReader {
    SOCKET sock;
    bool   running          = false;
    std::thread thread;
    std::chrono::steady_clock::time_point lastDataTime;
    bool   externallyActive = false;

    // Forza "Dash" packet layout (shared by FM7, FM2023, FH4, FH5)
    // First 232 bytes = Sled, extended with Dash fields
#pragma pack(push, 1)
    struct ForzaDash {
        // ── Sled (232 bytes) ─────────────────────────────────────────
        int32_t  isRaceOn;
        uint32_t timestampMS;
        float    engineMaxRpm;
        float    engineIdleRpm;
        float    currentEngineRpm;
        float    accelX, accelY, accelZ;
        float    velX,   velY,   velZ;
        float    angVelX, angVelY, angVelZ;
        float    yaw, pitch, roll;
        float    normSuspTravelFL, normSuspTravelFR, normSuspTravelRL, normSuspTravelRR;
        float    tireSlipRatioFL, tireSlipRatioFR, tireSlipRatioRL, tireSlipRatioRR;
        float    wheelRotSpeedFL, wheelRotSpeedFR, wheelRotSpeedRL, wheelRotSpeedRR;
        int32_t  wheelOnRumbleStripFL, wheelOnRumbleStripFR, wheelOnRumbleStripRL, wheelOnRumbleStripRR;
        float    wheelInPuddleFL, wheelInPuddleFR, wheelInPuddleRL, wheelInPuddleRR;
        float    surfaceRumbleFL, surfaceRumbleFR, surfaceRumbleRL, surfaceRumbleRR;
        float    tireSlipAngleFL, tireSlipAngleFR, tireSlipAngleRL, tireSlipAngleRR;
        float    tireCombSlipFL, tireCombSlipFR, tireCombSlipRL, tireCombSlipRR;
        float    normDrivingLine;
        float    normAiBreakDiff;
        float    speed;        // m/s
        float    power;
        float    torque;
        float    tireTempFL, tireTempFR, tireTempRL, tireTempRR;
        float    boost;
        float    fuel;
        float    distanceTraveled;
        float    bestLap, lastLap, currentLap, currentRaceTime;
        uint16_t lapNumber;
        uint8_t  racePosition;
        // ── Dash extras (added after Sled) ──────────────────────────
        uint8_t  accel;        // 0-255
        uint8_t  brake;        // 0-255
        uint8_t  clutch;
        uint8_t  handbrake;
        uint8_t  gear;         // 0=R, 1=N, 2=1st ...
        int8_t   steer;
        int8_t   normalDriving;
        int8_t   normalAiBreak;
    };
#pragma pack(pop)

    void loop();
public:
    ForzaReader();
    void setActive(bool active);
    bool isActive() const;
    void start();
    void stop();
    bool checkTimeout();
};

// Assetto Corsa (original, not ACC) – uses same shared memory as ACC but
// with different naming: "acpmf_physics" etc. under non-Local prefix on older versions.
// In practice, modern AC uses the same "Local\acpmf_*" names as ACC.
// We reuse ACCReader logic with a different gameName.
class ACOriginalReader {
    HANDLE hPhysics  = nullptr;
    HANDLE hGraphics = nullptr;
    HANDLE hStatic   = nullptr;
    bool   externallyActive = false;
public:
    void setActive(bool active);
    bool isActive() const;
    void update();
};

// iRacing – shared memory with name-based variable lookup
class IRacingReader {
    HANDLE hMap              = nullptr;
    char*  pSharedMem        = nullptr;
    double lastSimTime       = -1.0;
    bool   externallyActive  = false;

    // Cached variable offsets (set on first open)
    int offRPM         = -1;
    int offSpeed       = -1;
    int offGear        = -1;
    int offThrottle    = -1;
    int offBrake       = -1;
    int offLatAccel    = -1;
    int offShiftLight  = -1;
    bool offsetsCached  = false;

    void cacheOffsets(const irsdk_header* hdr);
    template<typename T> T getVar(const irsdk_header* hdr, int offset) const;
public:
    void setActive(bool active);
    bool isActive() const;
    void update();
};

// DiRT Rally 1 & 2.0 – UDP extradata=3 on port 20778 (different from F1's 20777)
// Requires user to edit hardware_settings_config.xml
class DirtRallyReader {
    SOCKET sock = INVALID_SOCKET;
    bool   running          = false;
    std::thread thread;
    std::chrono::steady_clock::time_point lastDataTime;
    bool   externallyActive = false;

#pragma pack(push, 1)
    struct DirtPacket {               // extradata=3, 264 bytes
        float time;
        float lapTime;
        float lapDistance;
        float totalDistance;
        float posX, posY, posZ;
        float speed;                  // m/s
        float velX, velY, velZ;
        float rollX, rollY, rollZ;    // right vector
        float pitchX, pitchY, pitchZ; // up vector (cross = forward)
        float suspPosBL, suspPosBR, suspPosFR, suspPosFL;
        float suspVelBL, suspVelBR, suspVelFR, suspVelFL;
        float wheelVelBL, wheelVelBR, wheelVelFR, wheelVelFL;
        float throttle;
        float steer;
        float brake;
        float clutch;
        float gear;           // float: 0=N, 1=1st, ..., 10=R
        float gForceLat;
        float gForceLon;
        float lap;
        float engineRate;     // rpm / 10 (!)
        float nativeMaxRPM;   // NOT always present, extradata=3 adds it at offset 152
        float idle;
        float maxRPM;         // rpm
        // more extradata=3 fields beyond here (ignored)
        float _pad[20];
    };
#pragma pack(pop)

    void loop();
public:
    DirtRallyReader();
    void setActive(bool active);
    bool isActive() const;
    void start();
    void stop();
    bool checkTimeout();
};


// ── GRID Autosport (UDP extradata=3, port 20777) ──────────────────────────────
// User must edit: Documents\My Games\GRID Autosport\hardwaresettings\hardware_settings_config.xml
//   <motion enabled="true" ip="127.0.0.1" port="20777" delay="1" />
// Packet format: identical to DiRT Rally extradata=3 (same Codemasters engine)
class GridAutosportReader {
    SOCKET sock = INVALID_SOCKET;
    bool   running          = false;
    std::thread thread;
    std::chrono::steady_clock::time_point lastDataTime;
    bool   externallyActive = false;
    void loop();
public:
    GridAutosportReader();
    void setActive(bool active);
    bool isActive() const;
    void start();
    void stop();
    bool checkTimeout();
};

// EA WRC / WRC Generations – shared memory
class WRCReader {
    HANDLE hMap              = nullptr;
    bool   externallyActive  = false;

#pragma pack(push, 1)
    struct WrcTelemetry {
        uint32_t sequenceNumber;   // odd while game is writing
        uint32_t version;
        // Version 1:
        int32_t  gear;             // Neutral=1, First=2, ...  Reverse=0
        float    velocity[3];      // left, up, forward  m/s
        float    acceleration[3];  // left, up, forward  m/s^2
        int32_t  engineIdleRpm;
        int32_t  engineMaxRpm;
        int32_t  engineRpm;
        float    suspensionTravel[4];   // FL, RL, RR, FR
        float    suspensionPosition[4]; // FL, RL, RR, FR
        float    unknown[4];
    };
#pragma pack(pop)

public:
    void setActive(bool active);
    bool isActive() const;
    void update();
};

// ETS2 / ATS – requires scs-sdk-plugin DLL installed by user
class ETSReader {
    HANDLE      hMap             = nullptr;
    bool        externallyActive = false;
    uint64_t    lastTimestamp    = 0;
public:
    void setActive(bool active);
    bool isActive() const;
    void update();
};

// BeamNG.drive – OutGauge UDP on port 4444
class BeamNGReader {
    SOCKET sock;
    bool   running          = false;
    std::thread thread;
    std::chrono::steady_clock::time_point lastDataTime;
    bool   externallyActive = false;

#pragma pack(push, 1)
    struct OutGaugePacket {
        uint32_t time;          // ms
        char     car[4];        // "beam"
        uint16_t flags;
        char     gear;          // 0=R, 1=N, 2=1st, ...
        char     plid;
        float    speed;         // m/s
        float    rpm;
        float    turbo;
        float    engTemp;
        float    fuel;
        float    oilPressure;
        float    oilTemp;
        uint32_t dashLights;
        uint32_t showLights;
        float    throttle;      // 0-1
        float    brake;         // 0-1
        float    clutch;        // 0-1
        char     display1[16];
        char     display2[16];
        int32_t  id;
    };
#pragma pack(pop)

    void loop();
public:
    BeamNGReader();
    void setActive(bool active);
    bool isActive() const;
    void start();
    void stop();
    bool checkTimeout();
};

// rFactor 2 / Le Mans Ultimate – requires rF2SharedMemoryMapPlugin
class RF2Reader {
    HANDLE hTelemetry        = nullptr;
    bool   externallyActive  = false;
    uint64_t lastVersion     = 0;

    // Minimal layout of rF2 shared memory telemetry buffer header
    struct RF2Header {
        uint32_t mVersionUpdateBegin;
        uint32_t mVersionUpdateEnd;
        int32_t  mNumVehicles;
        int32_t  _pad;
    };
    // Vehicle telemetry (very abbreviated – only fields we need)
    // Full struct is ~4KB, we only read the first player vehicle
    struct RF2VehicleTelemetry {
        double   mElapsedTime;          // session time
        double   mLapStartET;
        char     mVehicleName[64];
        char     mTrackName[64];
        double   mPos[3];               // world position
        double   mLocalVel[3];          // local velocity m/s
        double   mLocalAccel[3];        // local accel m/s^2 ← g_lat source (index 0 = lateral)
        double   mOri[3][3];            // orientation matrix
        double   mLocalRot[3];
        double   mLocalRotAccel[3];
        int32_t  mGear;                 // -1=reverse, 0=neutral, 1=first
        float    mEngineRPM;
        float    mEngineWaterTemp;
        float    mFuelLevel;
        float    mEstimatedMaxGear;
        float    mThrottle;             // 0-1
        float    mUnfilteredThrottle;
        float    mBrake;                // 0-1
        float    mUnfilteredBrake;
        float    mClutch;
        float    mUnfilteredClutch;
        float    mSteerInputValue;
        // wheel data follows (4 wheels × ~200 bytes) – too large to fully declare here
        // We only access fields up to mBrake so this truncated struct is fine
    };

public:
    void setActive(bool active);
    bool isActive() const;
    void update();
};

// ── Virtual Xbox360 gamepad (ViGEm) ───────────────────────────────────────────
class VirtualGamepad {
    PVIGEM_CLIENT client      = nullptr;
    PVIGEM_TARGET target      = nullptr;
    XUSB_REPORT   report      = {};
    bool          initialized = false;
public:
    VirtualGamepad();
    ~VirtualGamepad();
    bool isInitialized() const { return initialized; }
    void update(float steering, float throttle, float brake,
                float clutch, bool hasClutch,
                int handbrake, bool hasHandbrake,
                const std::vector<int>& buttons,
                const std::string& gameName = "",
                int f1Year = 0,
                float steerHalfAngleDeg = 180.0f);
};

// ── FFB Settings (einstellbar per GUI, persistent in ffb.ini) ─────────────────
// Named constants for per-game G-force scale factors
constexpr float GLAT_SCALE_RACEROOM   = 80.0f;   // steering_force_percentage ±1 normalized
constexpr float GLAT_SCALE_DIRT_GRID  = 45.0f;   // DiRT Rally, EA WRC, GRID Autosport
constexpr float GLAT_SCALE_TRUCKS     = 20.0f;   // ETS2/ATS (low lateral g in trucks)
constexpr float GLAT_SCALE_DEFAULT    = 35.0f;   // BeamNG, iRacing, rF2, AC, ACC, AMS2, F1

// Per-game reference level for the G-spike rumble in computeRumble().
// gSpikeThreshold is a slider labelled in g, which silently assumed that every
// reader writes real g AND that the vehicle can pull race-car cornering loads.
// Two readers break that: RaceRoom stores steering_force_percentage (range ±1,
// not g at all) and ETS2/ATS stores real g from vehicles that peak near 0.4 g -
// in both cases |g_lat| could never exceed the 1.5 g default, so the G-spike
// never fired once (and with it, pedal vibration in ETS2 - the only rumble
// source those trucks have).
// Dividing the reader's value by its reference normalises every game into one
// domain, so the slider selects the same *event severity* everywhere instead of
// the same raw number. Each value is "what this game reads during spirited
// cornering" / 1.5, which puts the default threshold exactly there and full
// spike strength at a genuine impact.
constexpr float GSPIKE_REF_RACEROOM   = 0.4f;    // 0.6 steering force → 1.5, max 1.0 → 2.5
constexpr float GSPIKE_REF_TRUCKS     = 0.2f;    // ETS2/ATS: 0.3 g → 1.5, 0.5 g swerve → 2.5
constexpr float GSPIKE_REF_DEFAULT    = 1.0f;    // value already is g - unchanged behaviour

// ── Wheel texture force ──────────────────────────────────────────────────────
// computeRumble() produces a perfectly good surface/impact signal (per-game
// normalised, with the user's Strength/Threshold sliders applied) - but its
// only consumer used to be the rumble byte, which the Box routes exclusively to
// the *pedal* vibration motors. On a rig without those motors (hasPedalRumble
// off, which is the common case) the entire signal went nowhere: the wheel has
// no vibration motor either, and the Box→Wheel packet carries no rumble field.
// Kerbs and impacts therefore had no path to the steering motor at all, and the
// only thing left in the torque was a smooth spring plus lateral g.
//
// So the same signal is now also injected into the torque as an alternating
// force - a texture the driver feels through the rim, in the direction that has
// headroom left (see the injection site in runBackend). Scale is deliberately
// generous: it replaces a previous 15-unit injection that only ran when the
// spring was already above 95% of full lock, i.e. essentially never.
constexpr float TEXTURE_MAX_TORQUE    = 30.0f;   // of 127, before wheelRumbleStr
// Half-period of the alternating texture force. 30ms = ~16.7Hz: high enough to
// read as a kerb rattle rather than a wobble, low enough that the Box's 15ms
// motor tick still resolves it (~4.5 ticks per cycle) and its soft-ramp can
// actually swing between the two levels.
constexpr int   TEXTURE_TOGGLE_MS     = 30;

struct FfbSettings {
    // Steering FFB
    float springStrength   = 1.0f;   // 0.0 – 1.5   overall spring strength
    float springStartAngle = 5.0f;   // 0 – 90°     deadzone: no force below this angle
    float springFullAngle  = 90.0f;  // 10 – 180°   angle at which full force is reached
    float springLinearity  = 1.0f;   // 0.0 – 2.0   spring curve: 0=linear, 1=log (default), 2=very progressive
    float gLatStrength     = 1.0f;   // 0.0 – 1.5   lateral-G force strength
    float absTorqueStr     = 0.5f;   // 0.0 – 1.5   ABS steering torque pulse strength
    float tcTorqueStr      = 0.4f;   // 0.0 – 1.5   TC steering torque pulse strength
    // Wheel Rumble – per source
    float wheelRumbleStr   = 1.0f;   // 0.0 – 1.5   overall wheel rumble intensity
    float wheelRumbleThr   = 0.15f;  // 0.0 – 1.0   surface threshold (suspension/slip)
    float absStrength      = 1.0f;   // 0.0 – 1.5   ABS pulse strength
    float gSpikeThreshold  = 1.5f;   // 0.5 – 3.0g  G-spike trigger level
    float gSpikeStrength   = 1.0f;   // 0.0 – 1.5   G-spike intensity
    // Pedal Vibration
    float pedalRumbleStr   = 1.0f;   // 0.0 – 1.5   pedal vibration intensity
    float pedalRumbleThr   = 60.0f;  // 0 – 255     rumble threshold for pedal activation
};
extern FfbSettings g_ffbSettings;
extern std::mutex  g_ffbSettingsMutex; // guards g_ffbSettings (GUI slider writes vs. backend FFB computation)
// =============================================================================
// BoxSettings  -  hardware configuration sent to the Box Arduino via 0xBD packet
// =============================================================================
// Stored in EEPROM on the Box so settings survive power cycles.
// The PC sends this packet on connect and whenever the user saves settings.
struct BoxSettings {
    // Hardware tier
    uint8_t simSetup            = 0;     // 0=BOX_FULL, 1=BOX_MEDIUM, 2=BOX_BUDGET
    // Optional hardware flags
    bool    hasPedalRumble      = false; // vibration motors on pedals
    bool    hasHandbrake        = false; // handbrake input on pin 4
    bool    hasClutch           = false; // 3rd pedal (clutch)
    bool    hasShifter          = false; // manual shifter (MANUAL_TX)
    bool    onlyWheel           = false; // ignore pedal inputs
    bool    invertSteering      = false; // flip encoder direction (encoder soldered in reverse)
    bool    invertForceFeedback = false; // flip motor torque direction (motor/H-bridge wired in reverse, independent of invertSteering)
    // Motor settings
    uint8_t motorMaxPwm         = 178;   // max PWM (0-255), 178=70%
    uint8_t motorMinPwmLeft     = 62;    // min PWM left direction
    uint8_t motorMinPwmRight    = 62;    // min PWM right direction
    uint8_t softRampStep        = 12;    // max PWM change per cycle
    bool    stallProtection     = false; // stall protection enable
};

extern BoxSettings g_boxSettings;
  // in SimRacePro.cpp definiert

// =============================================================================
// BoxPinConfig  -  remappable peripheral pins sent to the Box Arduino via the
//                  0xBE packet (see Pin Configuration window)
// =============================================================================
// Stored in EEPROM on the Box (separate region from BoxConfig) so a wiring
// mistake made during the build can be corrected in software instead of
// re-soldering. Arduino pin numbers A0-A7 = 14-21 on the Nano - same raw
// values the firmware itself gets from the A0..A7 macros.
// Deliberately excludes the encoder (2/3, interrupt pins - "Invert Steering"
// already covers an A/B swap), the motor (8/9/10/11, tied to the Timer1/
// Timer2 prescaler hack for silent PWM), the Box<->Wheel link (5/6, TX is
// hardcoded register bit-banging), and the handbrake (4 - the only candidate
// on a non-analog pin, not worth remapping given every other non-remappable
// digital pin stays hardcoded too).
struct BoxPinConfig {
    uint8_t accPin       = 14; // A0 - throttle pedal
    uint8_t brkPin       = 15; // A1 - brake pedal
    uint8_t vibPin       = 16; // A2 - pedal rumble motor 1
    uint8_t vib2Pin      = 17; // A3 - pedal rumble motor 2
    uint8_t clutchPin    = 18; // A4 - clutch pedal
    uint8_t shifterXPin  = 21; // A7 - shifter X-axis
    uint8_t shifterYPin  = 20; // A6 - shifter Y-axis
};

extern BoxPinConfig g_boxPinConfig;
  // in SimRacePro.cpp definiert

// =============================================================================
// AppSettings  -  general application preferences (not hardware/FFB related)
// =============================================================================
struct AppSettings {
    bool  startMinimized   = false;  // start hidden in the tray instead of showing the main window
    float maxSteerAngleDeg = 360.0f; // total degrees of rotation (lock-to-lock) mapped to the game's full steering lock
};

// Live mirror of AppSettings::maxSteerAngleDeg - the main window's stepped
// slider (below the mini steering-angle dial) writes this, VirtualGamepad::update
// reads it. Atomic: written by the GUI thread, read by the backend thread.
extern std::atomic<float> g_maxSteerAngleDeg;

class ConfigManager {
    std::string configPath;
    std::string hwConfigPath;
    std::string ffbConfigPath;
    std::string winConfigPath;
    std::string appConfigPath;
    std::string layoutConfigPath;
    std::string verMarkerPath;
public:
    ConfigManager();
    bool        configExists();
    void        saveConfig(const std::string& comPort, const std::map<int, USHORT>& btnMap);
    bool        loadConfig(std::string& outComPort, std::map<int, USHORT>& outBtnMap);
    std::string getConfigPath();
    std::string getHardwareConfigPath();
    std::string getFfbConfigPath();
    std::string getLayoutConfigPath();
    // Wiring layout (visual position -> physical button bit, see g_posToPhys)
    bool        layoutExists();
    void        saveLayout(const int posToPhys[16]);
    bool        loadLayout(int posToPhys[16]);
    bool        hasOptionalHardwareConfig();
    void        saveFfbSettings(const FfbSettings& s);
    void        loadFfbSettings(FfbSettings& s);
    void        saveBoxSettings(const BoxSettings& s);
    void        loadBoxSettings(BoxSettings& s);
    void        saveBoxPinConfig(const BoxPinConfig& p);
    void        loadBoxPinConfig(BoxPinConfig& p);
    void        saveAppSettings(const AppSettings& s);
    void        loadAppSettings(AppSettings& s);
    // Version marker (lastversion.ini): which app version wrote the current
    // configs. Deliberately a separate file - it must survive both "Reset all
    // Settings" AND saveAppSettings() (the GUI writes app.ini from a fresh
    // struct, so a field there would be wiped on every checkbox toggle).
    std::string loadLastRunVersion();                 // "" = no marker (fresh install or pre-3.1.4)
    void        saveLastRunVersion(const std::string& v);
    // Persist/restore top-left window position for a named window (e.g. "console", "manual", "ffb", "boxhw")
    void        saveWindowPos(const std::string& key, int x, int y);
    bool        loadWindowPos(const std::string& key, int& x, int& y);
};
extern ConfigManager* g_cfg;  // gesetzt in runBackend, genutzt von GuiWindow


// ── GUI ────────────────────────────────────────────────────────────────────────
bool CreateGuiWindow(HINSTANCE hInst, int nCmdShow, GuiState& state);

// ── Backend (called from detached thread in WinMain) ──────────────────────────
void runBackend(GuiState& g_gui);
