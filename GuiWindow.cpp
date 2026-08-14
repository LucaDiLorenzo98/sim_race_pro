// GuiWindow.cpp  –  Win32 GUI: window, painting, telemetry display, calibration dialog.
// UTF-8 encoding required (Unicode button symbols in xboxButtonName).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _USE_MATH_DEFINES

#include "SimRacePro.h"
#include "resource.h"
#include "SimRaceProDefs.h"
#include <cmath>
#include <algorithm>
#include <sstream>

// ── Layout constants ───────────────────────────────────────────────────────────
#define IMG_W  550
#define IMG_H  347
#define LP_X   10
#define LP_Y   10
#define LP_W   (IMG_W + 20)
#define LP_H   (IMG_H + 30)
#define TP_X   LP_X
#define TP_Y   (LP_Y + LP_H + 8)
#define TP_W   LP_W
#define TP_H   255
#define SA_X   LP_X
#define SA_Y   (TP_Y + TP_H + 8)
#define SA_W   LP_W
#define SA_H   140
#define MAIN_W (LP_X + LP_W + 26)
#define MAIN_H (SA_Y + SA_H + 48)
#define CON_W  500   // Breite des externen Konsolenfensters
#define CON_H  400   // Höhe des externen Konsolenfensters

// Virtual SSD1306 overlay on the wheel image
#define DISP_X 239
#define DISP_Y  52
#define DISP_W  72
#define DISP_H  36

// Control IDs
#define ID_LOG_BOX       200
#define ID_BTN_RESET_MAP 201
#define ID_BTN_GAMEPAD   205
#define ID_BTN_CONSOLE   206
#define ID_BTN_MANUAL    207
#define ID_BTN_DEBUG     208
#define ID_CONN_LABEL    204
#define ID_GAME_LABEL    203
#define ID_BTNLABEL_BASE 300
#define ID_BTN_FFB       209
#define ID_BTN_WHEEL_HW  210
#define ID_CHK_AUTOSTART 211
#define ID_CHK_START_MIN 212

// Manual dialog tab IDs
#define ID_TAB_MANUAL    400

// ── Button overlay positions (center x,y relative to image origin) ─────────────
struct BtnInfo { int cx, cy; bool square; };
static const BtnInfo BTN_POS[16] = {
    {117,38,false},{164,32,false},{384,32,false},{431,38,false},
    {221,86,true}, {221,51,true}, {328,51,true}, {328,86,true},
    {135,160,false},{178,127,false},{371,127,false},{414,160,false},
    {102,214,false},{238,251,false},{311,251,false},{448,214,false},
};
#define BTN_ROUND_D 24
#define BTN_SQ_SIDE 20

// Wheel-reset button (red button bottom center of wheel.bmp). Deliberately NOT
// part of BTN_POS: its function is fixed (re-zero, arrives as extra bit0, not
// as a button bit), so it is excluded from the wiring wizard, the calibration
// wizard and click-reassignment by construction. Center + color measured from
// wheel.bmp (inner red circle ~25px diameter, RGB(170,69,59)).
#define RESET_BTN_CX   274
#define RESET_BTN_CY   213
#define RESET_BTN_RED  RGB(170,69,59)

// ── Max steering angle slider (below the mini steering-angle dial) ────────────
// Discrete "degrees of rotation" (lock-to-lock) steps the stepped slider snaps
// to. Upper bound matches the Box firmware's mechanical encoder limit
// (maxTicks=3000 -> +-450 deg, see sim_race_pro_box_script.ino). Typical game
// values fall inside this range: karts/oval ~180-360 deg, F1/formula ~360 deg,
// GT3/road cars ~540-900 deg, rally/trucks ~900 deg.
static const int STEER_ANGLE_STEPS[] = {180,270,360,450,540,630,720,810,900};
#define STEER_ANGLE_STEPS_N (int)(sizeof(STEER_ANGLE_STEPS)/sizeof(STEER_ANGLE_STEPS[0]))

// ── Colors ─────────────────────────────────────────────────────────────────────
static COLORREF COL_BG       = RGB(30,30,35);
static COLORREF COL_PANEL    = RGB(40,40,48);
static COLORREF COL_ACCENT   = RGB(0,170,255);
static COLORREF COL_GREEN    = RGB(50,200,80);
static COLORREF COL_RED      = RGB(220,60,60);
static COLORREF COL_YELLOW   = RGB(255,200,0);
static COLORREF COL_TEXT     = RGB(220,220,220);
static COLORREF COL_SUBTEXT  = RGB(140,140,150);

// RPM_GREEN/YELLOW/RED/BLINK now come from SimRacePro.h, shared with
// SimRacePro.cpp's blink decision (see CODE_REVIEW.md 1.9).

// ── Module-level state ─────────────────────────────────────────────────────────
static GuiState*          g_state        = nullptr;
static HINSTANCE          g_hInst        = nullptr;
static HFONT              g_fontNormal   = nullptr, g_fontBold    = nullptr,
                          g_fontSmall    = nullptr, g_fontLog     = nullptr,
                          g_fontBtn      = nullptr, g_fontBtnLarge= nullptr;
static HBRUSH             g_bgBrush      = nullptr, g_panelBrush  = nullptr,
                          g_logBrush     = nullptr;
static HBITMAP            g_wheelImage   = nullptr;
static HICON              g_hAppIcon     = nullptr;
static bool               g_consoleVisible = false;
static bool               g_trayAdded    = false;
static bool               g_startMinimized = false;  // loaded in CreateGuiWindow, read by WM_CREATE and ShowWindow
static NOTIFYICONDATAA    g_nid          = {};
static HWND               g_hwndLog      = nullptr;
static HWND               g_hwndBtnLabels[16] = {};
static HWND               g_hwndConnLabel = nullptr, g_hwndGameLabel = nullptr;
static HWND               g_hwndResetBtn = nullptr,  g_hwndGamepadBtn = nullptr;
static HWND               g_hwndConsoleBtn = nullptr, g_hwndCalibDlg = nullptr;
static HWND               g_hwndManualBtn  = nullptr;
static HWND               g_hwndDebugBtn   = nullptr;
static bool               g_ffbDebugOn     = false;     // shared between WM_DRAWITEM and WM_COMMAND
// cached GDI brush objects (created once, freed in WM_DESTROY)
static HBRUSH             g_brGreen=nullptr, g_brYellow=nullptr, g_brRed=nullptr,
                          g_brBlue=nullptr,  g_brDark=nullptr,   g_brPanel=nullptr;
static HWND               g_hwndConsoleDlg = nullptr;   // separates Konsolenfenster
static HWND               g_hwndFfbDlg     = nullptr;   // Settings window
static HWND               g_hwndFfbPanel   = nullptr;   // FFB tab panel
static HWND               g_hwndBoxPanel   = nullptr;   // Box settings tab panel
static HWND               g_hwndPinCfgPanel= nullptr;   // Pin configuration window
static HWND               g_hwndFfbBtn     = nullptr;
static bool               g_btnActive[16] = {};
static HWND               g_hwndWiringDlg  = nullptr;   // wiring setup progress dialog
static int                g_wiringHighlight = -1;       // overlay position highlighted by the wiring wizard, -1 = none
static HWND               g_hwndMaxAngleLbl = nullptr, g_hwndMaxAngleSld = nullptr;  // Max Steering Angle slider (Telemetry panel)

// ── Helpers ────────────────────────────────────────────────────────────────────
static const char* xboxButtonName(USHORT v) {
    switch (v) {
    case 0x1000: return "A";          case 0x2000: return "B";
    case 0x4000: return "X";          case 0x8000: return "Y";
    case 0x0001: return "\xE2\x96\xB2"; // ▲ Up
    case 0x0002: return "\xE2\x96\xBC"; // ▼ Down
    case 0x0004: return "\xE2\x97\x80"; // ◀ Left
    case 0x0008: return "\xE2\x96\xB6"; // ▶ Right
    case 0x0010: return "\xE2\x89\xA1"; // ≡ Start
    case 0x0020: return "\xE2\xAE\xBA"; // ⮺ Back
    case 0x0100: return "LB";           case 0x0200: return "RB";
    case 0x0040: return "L3";           case 0x0080: return "R3";
    case 0x0400: return "\xE2\xA6\xBB"; // ⦻ Guide
    default:     return "?";
    }
}

// All assignable Xbox buttons - shared by the reassignment dropdown (order and
// names match the calibration wizard's list in SimRacePro.cpp).
static const struct { const char* name; USHORT val; } XBOX_BUTTONS[] = {
    {"A",XBTN_A},{"B",XBTN_B},{"X",XBTN_X},{"Y",XBTN_Y},
    {"DPad Up",XBTN_UP},{"DPad Down",XBTN_DOWN},{"DPad Left",XBTN_LEFT},{"DPad Right",XBTN_RIGHT},
    {"Left Shoulder (LB)",XBTN_LSHOULDER},{"Right Shoulder (RB)",XBTN_RSHOULDER},
    {"Left Stick (L3)",XBTN_LTHUMB},{"Right Stick (R3)",XBTN_RTHUMB},
    {"Start",XBTN_START},{"Back",XBTN_BACK},{"Guide",XBTN_GUIDE}
};
static const int XBOX_BUTTONS_N = (int)(sizeof(XBOX_BUTTONS)/sizeof(XBOX_BUTTONS[0]));

// Snapshot of the wiring layout (visual position -> physical button bit).
static void GetPosToPhysCopy(int out[16]) {
    std::lock_guard<std::mutex> lk(buttonMapMutex);
    for (int i = 0; i < 16; i++) out[i] = g_posToPhys[i];
}

// Nearest STEER_ANGLE_STEPS index for a given degree value - used to snap a
// persisted/default value onto the stepped slider's discrete positions.
static int FindNearestAngleStepIndex(float deg) {
    int bestIdx = 0; float bestDiff = 1e9f;
    for (int i = 0; i < STEER_ANGLE_STEPS_N; i++) {
        float diff = fabsf(deg - (float)STEER_ANGLE_STEPS[i]);
        if (diff < bestDiff) { bestDiff = diff; bestIdx = i; }
    }
    return bestIdx;
}
static void UpdateMaxAngleLabel(int idx) {
    char buf[48]; sprintf_s(buf, "Max Steering Angle: %d deg", STEER_ANGLE_STEPS[idx]);
    SetWindowTextA(g_hwndMaxAngleLbl, buf);
}

// The trackbar reserves padding above/below its (thin) channel to make room
// for the tall default thumb glyph. That padding isn't part of any
// NM_CUSTOMDRAW item (TBCD_CHANNEL/TICS/THUMB), so filling it from
// CDDS_PREPAINT doesn't stick - it keeps showing the window's base
// background instead of the panel color. Subclassing WM_ERASEBKGND is the
// reliable way to recolor it.
static LRESULT CALLBACK MaxAngleSldSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) {
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(COL_PANEL);
        FillRect((HDC)wp, &rc, br);
        DeleteObject(br);
        return 1;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// Hit-test a main-window client coordinate against the button overlays on the
// wheel image; returns the overlay position 0-15 or -1.
static int HitTestBtnOverlay(int mx, int my) {
    const int imgOX = LP_X + 10, imgOY = LP_Y + 24;
    for (int i = 0; i < 16; i++) {
        const BtnInfo& b = BTN_POS[i];
        int dx = mx - (imgOX + b.cx), dy = my - (imgOY + b.cy);
        if (b.square) { int h = BTN_SQ_SIDE / 2; if (abs(dx) <= h && abs(dy) <= h) return i; }
        else          { int r = BTN_ROUND_D / 2; if (dx*dx + dy*dy <= r*r)         return i; }
    }
    return -1;
}

// Load wheel.bmp from embedded RCDATA resource and scale to target size.
static HBITMAP LoadWheelBitmapFromResource(HINSTANCE hInst, int w, int h) {
    HRSRC   hRes  = FindResourceA(hInst, MAKEINTRESOURCEA(IDR_WHEEL_BMP), MAKEINTRESOURCEA(10));  // 10 = RT_RCDATA
    if (!hRes) return nullptr;
    HGLOBAL hGlob = LoadResource(hInst, hRes); if (!hGlob) return nullptr;
    LPVOID  pData = LockResource(hGlob);
    DWORD   size  = SizeofResource(hInst, hRes);
    if (size < sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)) return nullptr;
    auto* fh = (BITMAPFILEHEADER*)pData;
    auto* bi = (BITMAPINFO*)((BYTE*)pData + sizeof(BITMAPFILEHEADER));
    BYTE* bits = (BYTE*)pData + fh->bfOffBits;
    HDC hdcScr = GetDC(nullptr);
    HBITMAP hSrc = CreateDIBitmap(hdcScr, &bi->bmiHeader, CBM_INIT, bits, bi, DIB_RGB_COLORS);
    HBITMAP hDst = CreateCompatibleBitmap(hdcScr, w, h);
    HDC hdcS = CreateCompatibleDC(hdcScr), hdcD = CreateCompatibleDC(hdcScr);
    auto hOS = (HBITMAP)SelectObject(hdcS, hSrc), hOD = (HBITMAP)SelectObject(hdcD, hDst);
    SetStretchBltMode(hdcD, HALFTONE);
    StretchBlt(hdcD, 0,0, w,h, hdcS, 0,0, bi->bmiHeader.biWidth, abs(bi->bmiHeader.biHeight), SRCCOPY);
    SelectObject(hdcS, hOS); SelectObject(hdcD, hOD);
    DeleteDC(hdcS); DeleteDC(hdcD); DeleteObject(hSrc); ReleaseDC(nullptr, hdcScr);
    return hDst;
}

// ── Auxiliary window positioning ────────────────────────────────────────────────
// Console/Manual/FFB/Box-HW windows: if the user has moved a window before, reopen
// it at that exact spot (persisted in windows.ini). Otherwise default to just right
// of the main window, like Show Console already did.
static void ComputeAuxWindowPos(HWND parent, const char* key, int w, int h, int& outX, int& outY) {
    if (g_cfg && g_cfg->loadWindowPos(key, outX, outY)) {
        RECT wa{}; SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        if (outX < wa.left - w + 60)  outX = wa.left;
        if (outX > wa.right - 60)     outX = wa.right - w;
        if (outY < wa.top)            outY = wa.top;
        if (outY > wa.bottom - 60)    outY = wa.bottom - h;
        return;
    }
    RECT wr{}; GetWindowRect(parent, &wr);
    outX = wr.right + 8;
    outY = wr.top;
}

// Called from WM_EXITSIZEMOVE of an aux window to persist its new position.
static void SaveAuxWindowPos(HWND hwnd, const char* key) {
    if (!g_cfg) return;
    RECT wr{}; GetWindowRect(hwnd, &wr);
    g_cfg->saveWindowPos(key, wr.left, wr.top);
}

// ── Manual / User Guide popup ──────────────────────────────────────────────────
// Architecture: custom-painted scrollable child window ("ManualPanel").
// Content is defined as a list of typed rows (heading, body, bullet, link, sep).
// Everything is drawn with DrawTextW so Unicode works correctly.
// Links open via ShellExecuteW on left-click.

enum RowType { ROW_TITLE, ROW_HEADING, ROW_BODY, ROW_BULLET, ROW_LINK, ROW_SEP, ROW_CODE, ROW_TABLE };

struct ManualRow {
    RowType     type;
    const wchar_t* text;
    const wchar_t* url;
};

// =============================================================================
// TAB 1 – Overview / Quick Start
// =============================================================================
static const ManualRow TAB_GETTING_STARTED[] = {
    { ROW_TITLE,   L"SimRacePro Custom Driver  -  Quick Start Guide", nullptr},
    { ROW_SEP,     nullptr, nullptr },

    { ROW_BODY,    L"Note: once the Arduinos are flashed with the SimRacePro firmware, the original script from LucaDilo no longer works with your wheel - to go back, flash the Arduinos with the original sketches again.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Step 1  -  Install ViGEmBus Driver", nullptr },
    { ROW_BODY,    L"SimRacePro uses the ViGEmBus virtual gamepad driver. Install it before first use.", nullptr },
    { ROW_LINK,    L"ViGEmBus Driver - Latest Release (GitHub)", L"https://github.com/nefarius/ViGEmBus/releases/latest" },
    { ROW_BODY,    L"Download and run the installer, then restart your PC if prompted.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Step 2  -  Install CH340 Driver (if needed)", nullptr },
    { ROW_BODY,    L"The Arduino Nano uses a CH340 USB-to-Serial chip. If Windows does not recognize the Nano automatically, install the driver:", nullptr },
    { ROW_LINK,    L"CH340 Driver Download (WCH official)", L"https://www.wch-ic.com/downloads/CH341SER_EXE.html" },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Step 3  -  Flash Firmware via the Arduino IDE", nullptr },
    { ROW_BODY,    L"Flash both Arduino Nano boards manually before first connecting:", nullptr },
    { ROW_BULLET,  L"Install the Arduino IDE 2.x and select board 'Arduino Nano' (or your exact variant).", nullptr },
    { ROW_BODY,    L"Install the required libraries via Sketch > Include Library > Manage Libraries (Library Manager):", nullptr },
    { ROW_BULLET,  L"\"Encoder\" by Paul Stoffregen  -  needed for the Base sketch.", nullptr },
    { ROW_BULLET,  L"\"Adafruit GFX Library\"  -  needed for the Wheel sketch (OLED display).", nullptr },
    { ROW_BULLET,  L"\"Adafruit SSD1306\"  -  needed for the Wheel sketch (OLED display).", nullptr },
    { ROW_BODY,    L"Wire, EEPROM and SoftwareSerial are part of the Arduino core and do not need to be installed separately.", nullptr },
    { ROW_BULLET,  L"Open \"Arduino files\\sim_race_pro_box_script\\sim_race_pro_box_script.ino\", connect the Base via USB, select its COM port, click Upload.", nullptr },
    { ROW_BULLET,  L"Open \"Arduino files\\sim_race_pro_wheel_script\\sim_race_pro_wheel_script.ino\", connect the Wheel Arduino via USB, select its COM port, click Upload.", nullptr },
    { ROW_BULLET,  L"Reconnect the Wheel Arduino to the steering wheel and the Base to this PC, then launch SimRacePro.", nullptr },
    { ROW_BODY,    L"If SimRacePro detects a firmware version mismatch on connect, it will show which board's firmware is outdated - repeat the upload steps above for that board.", nullptr },
    { ROW_LINK,    L"Arduino IDE 2.x Download", L"https://www.arduino.cc/en/software" },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Step 4  -  Configure Box Hardware Settings", nullptr },
    { ROW_BODY,    L"Before first use, open Box HW (main window) and configure which optional hardware is installed: pedal rumble, handbrake, clutch pedal, shifter. Click Save - settings are stored in EEPROM on the Base and survive power cycles.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Step 5  -  Connect Hardware and Launch", nullptr },
    { ROW_BODY,    L"Connect the Base to your PC via USB. Run SimRacePro Custom.exe. The tray icon appears. On first launch two wizards run:", nullptr },
    { ROW_BULLET,  L"Button Wiring Setup  -  each button position on the wheel image lights up yellow in turn. Press the matching physical button on your wheel (or 'Skip this button' for positions that are not wired). This tells the software which physical button sits where.", nullptr },
    { ROW_BULLET,  L"Button Calibration  -  assigns an Xbox gamepad function (A, B, X, Y, DPad, Start, ...) to each wheel button.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Step 6  -  Start a Game", nullptr },
    { ROW_BODY,    L"SimRacePro auto-detects all supported games. The 'Game:' label turns blue when a game is active. Force Feedback and RPM LEDs activate automatically.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Button Mapping and the Reset Button", nullptr },
    { ROW_BULLET,  L"Remap anytime: click a button overlay on the wheel image in the main window and pick a new Xbox function from the dropdown. If the function is already used elsewhere, you can move it.", nullptr },
    { ROW_BULLET,  L"The red WHEEL RESET button (bottom centre of the wheel) has a fixed function and cannot be remapped: pressing it re-zeros the steering centre and re-calibrates the pedal rest positions. It lights up red in the main window while pressed.", nullptr },
    { ROW_BULLET,  L"'Reset all Settings' (main window) clears button mapping, button wiring, hardware settings and FFB settings - the wizards run again on the next start.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Max Steering Angle", nullptr },
    { ROW_BODY,    L"The stepped slider below the small steering-angle dial (main window, Telemetry panel) sets how many total degrees of physical wheel rotation (lock-to-lock) map to the game's full steering lock in either direction. Steps run from 180 to 900 degrees in 90-degree increments - 900 is the hardware's mechanical rotation limit.", nullptr },
    { ROW_BULLET,  L"180-270 deg  -  karts, some oval/short-track cars.", nullptr },
    { ROW_BULLET,  L"360 deg  -  F1/open-wheel/formula cars (this was the fixed default before this slider existed).", nullptr },
    { ROW_BULLET,  L"540 deg  -  GT3/GT4/touring cars.", nullptr },
    { ROW_BULLET,  L"900 deg  -  rally, trucks (ETS2/ATS), and road cars with a realistic lock-to-lock.", nullptr },
    { ROW_BULLET,  L"Takes effect immediately, no restart needed - the value is saved as soon as you release the slider.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Automatic Settings Reset After an Update", nullptr },
    { ROW_BODY,    L"When SimRacePro starts for the first time after being updated from a different version, it automatically resets all settings (same as 'Reset all Settings') and runs the setup wizards again. This guarantees a clean configuration that matches the new version. Remember to also flash the matching firmware onto both Arduinos (see Step 3).", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Supported Games", nullptr },
    { ROW_BULLET,  L"Forza Motorsport (2023)  /  Forza Horizon 4, 5 & 6", nullptr },
    { ROW_BULLET,  L"DiRT 2  /  DiRT Rally 1 & 2.0  /  DiRT 3  /  DiRT 4  /  DiRT 5", nullptr },
    { ROW_BULLET,  L"Euro Truck Simulator 2  /  American Truck Simulator", nullptr },
    { ROW_BULLET,  L"BeamNG.drive", nullptr },
    { ROW_BULLET,  L"rFactor 2  /  Le Mans Ultimate", nullptr },
    { ROW_BULLET,  L"iRacing", nullptr },
    { ROW_BULLET,  L"GRID Autosport", nullptr },
    { ROW_BULLET,  L"Assetto Corsa  /  Assetto Corsa Competizione", nullptr },
    { ROW_BULLET,  L"EA Sports WRC  /  WRC Generations", nullptr },
    { ROW_BULLET,  L"RaceRoom Racing Experience", nullptr },
    { ROW_BULLET,  L"Automobilista 2  /  Project CARS 3", nullptr },
    { ROW_BULLET,  L"F1 2018 - F1 25  (Codemasters / EA)", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Requirements", nullptr },
    { ROW_BULLET,  L"Windows 10 / 11  (x64)", nullptr },
    { ROW_BULLET,  L"USB 2.0 or higher", nullptr },
    { ROW_BULLET,  L"ViGEmBus Driver  (see Step 1)", nullptr },
    { ROW_BULLET,  L"CH340 Driver  (see Step 2, usually installed automatically)", nullptr },
    { ROW_BULLET,  L"Arduino IDE 2.x  (to flash the Base and Wheel firmware, see Step 3)", nullptr },
    { ROW_BODY,    L"", nullptr },
};

// =============================================================================
// TAB 2 – Box Hardware Settings
// =============================================================================
static const ManualRow TAB_BOX_SETTINGS[] = {
    { ROW_TITLE,   L"Box Hardware Settings", nullptr },
    { ROW_BODY,    L"Open via the 'Box HW' button in the main window. Settings are sent to the Base Arduino on connect and stored in EEPROM - they survive power cycles and firmware updates.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Hardware Setup  (Sim Setup Tier)", nullptr },
    { ROW_BULLET,  L"BOX_FULL   - Encoder + FFB Motor installed. Full force feedback, steering angle tracking.", nullptr },
    { ROW_BULLET,  L"BOX_MEDIUM - Encoder installed, no motor. Steering angle displayed on wheel, no FFB.", nullptr },
    { ROW_BULLET,  L"BOX_BUDGET - No encoder, no motor. Buttons and pedals only.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Optional Hardware", nullptr },
    { ROW_BULLET,  L"Pedal Vibration Motors  -  Enable if rumble motors are wired to pins A2/A3 on the Base. Used for ABS and surface feedback on the pedals.", nullptr },
    { ROW_BULLET,  L"Handbrake (Pin 4)  -  Enable if a digital handbrake button is wired to pin 4 on the Base.", nullptr },
    { ROW_BULLET,  L"3rd Pedal / Clutch  -  Enable if a clutch pedal is wired to pin A4 on the Base. Maps the analogue input to the gamepad clutch axis.", nullptr },
    { ROW_BULLET,  L"Manual Shifter  -  Enable if an H-pattern shifter's X/Y sensor is wired to pins A7/A6 on the Base. Simulates a '1'-'6' key press on gear change (for games with a direct gear-select keybind), since the virtual gamepad has no gear-lever axis.", nullptr },
    { ROW_BULLET,  L"Wheel Only (ignore pedals)  -  Disable all pedal inputs. Useful when using a separate pedal set via USB.", nullptr },
    { ROW_BULLET,  L"Invert Steering  -  Flips steering direction for units where the encoder was wired/soldered in reverse. Corrects the angle at the source on the Base - in-game steering, the wheel's own display, and force feedback direction are all corrected together, since they all derive from this one angle value.", nullptr },
    { ROW_BULLET,  L"Invert Force Feedback  -  Flips only the motor's torque output direction, independent of Invert Steering. Use this instead when the encoder/steering direction is already correct but the motor pushes the wrong way (e.g. the H-bridge/motor leads are wired in reverse). Combine both if the unit has both faults.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Motor Settings", nullptr },
    { ROW_BULLET,  L"Max PWM  (50 - 255)  -  Maximum motor PWM output. 178 = 70% = approx. 7A at 12V with a BTS7960. Lower to reduce current draw and heat. Default: 178.", nullptr },
    { ROW_BULLET,  L"Min PWM Left / Right  (0 - 100)  -  Minimum PWM applied when the motor is active. Compensates for motor stiction so small torque values still move the wheel. Calibrate separately per direction if one side feels sluggish. Default: 62.", nullptr },
    { ROW_BULLET,  L"Ramp Step  (1 - 50)  -  Maximum PWM change per motor update cycle (~15 ms). Limits jerk on sudden FFB changes. Lower = smoother, Higher = more responsive. Default: 12.", nullptr },
    { ROW_BULLET,  L"Stall Protection  -  Gradually reduces motor power if the wheel is held against a stop for an extended time. Prevents the BTS7960 from cutting out due to overcurrent. Default: OFF.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Save & Send", nullptr },
    { ROW_BODY,    L"Click 'Save & Send to Box' to apply changes. Settings are transmitted to the Base via the 0xBD protocol packet and saved to EEPROM immediately. The Base applies them without a reboot.", nullptr },
    { ROW_BODY,    L"Settings are also sent automatically each time the software connects to the Base.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Pin Configuration", nullptr },
    { ROW_BODY,    L"Click 'Pin Configuration...' to remap 7 peripheral pins in software if a wiring mistake was made during the build - no re-soldering needed. Covers throttle, brake, both pedal rumble motors, clutch, and shifter X/Y.", nullptr },
    { ROW_BODY,    L"The encoder, motor and Base-to-Wheel link pins are not remappable (they are tied to interrupt/timer hardware requirements), and the handbrake pin stays fixed since it is the only candidate on a non-analogue pin.", nullptr },
    { ROW_BODY,    L"Each dropdown only offers pins valid for that field (analogue-only for sensors that need analogRead, digital-capable for the rumble motors). Assigning a pin already used elsewhere prompts to overwrite - the other field is then cleared and must be reassigned before saving. Saving is blocked until every field has a unique pin.", nullptr },
    { ROW_BODY,    L"Like the other Box settings, the pin layout is transmitted via its own protocol packet, stored in EEPROM on the Base, and applied automatically without a reboot - as well as automatically re-applied on every reconnect.", nullptr },
    { ROW_BODY,    L"", nullptr },
};

// =============================================================================
// TAB 3 – FFB Settings
// =============================================================================
// FFB support table
// Verified directly against each reader's update()/loop() in SimRacePro.cpp
// (which telemetry fields it actually writes) rather than by feature intent -
// several cells below were wrong before this pass (ACC TC Torque, AMS2/iRacing/
// BeamNG/rF2 Surface, BeamNG Lateral-G and G-Spike) because computeTorque/
// computeRumble derive every effect from telemetry fields, and not every
// reader populates every field even when the game could theoretically supply it.
//
// The 'Pedal Vib' column is derived, not independent: the Base drives the pedal
// motors from the single rumble byte the PC sends (lastRumble >= pedalRumbleThr
// in the Box firmware), and that byte is computeRumble()'s output - the sum of
// Surface + ABS Rumble + G-Spike. So a game gets pedal vibration exactly when at
// least one of those three columns is a tick; there is no separate pedal source.
//
// The G-Spike column depends on GSPIKE_REF_* (SimRacePro.h): RaceRoom's lateral
// value is a +-1 steering force and ETS2/ATS trucks peak near 0.4 g, so before
// that normalisation both could never cross the 1.5 g threshold and their ticks
// here were wrong - ETS2 in particular had no working rumble source at all,
// which is why it showed no pedal vibration in testing.

static const wchar_t* g_ffbTableData =
    L"Game\tSpring\tLateral-G\tSurface\tABS Torque\tTC Torque\tABS Rumble\tG-Spike\tPedal Vib\n"
    L"ACC\t\u2713\t\u2713\t\u2713\t\u2713\t\u2713\t\u2713\t\u2713\t\u2713\n"
    L"RaceRoom\t\u2713\t\u2713\t\u2713\t\u2713\t\u2713\t\u2713\t\u2713\t\u2713\n"
    L"AMS2\t\u2713\t\u2713\t\u2014\t\u2713\t\u2713\t\u2713\t\u2713\t\u2713\n"
    L"iRacing\t\u2713\t\u2713\t\u2014\t\u2014\t\u2014\t\u2014\t\u2713\t\u2713\n"
    L"Assetto Corsa\t\u2713\t\u2713\t\u2713\t\u2014\t\u2014\t\u2014\t\u2713\t\u2713\n"
    L"DiRT / GRID\t\u2713\t\u2713\t\u2713\t\u2014\t\u2014\t\u2014\t\u2713\t\u2713\n"
    L"Forza\t\u2713\t\u2713\t\u2713\t\u2014\t\u2014\t\u2014\t\u2713\t\u2713\n"
    L"BeamNG\t\u2713\t\u2014\t\u2014\t\u2014\t\u2014\t\u2014\t\u2014\t\u2014\n"
    L"rF2 / LMU\t\u2713\t\u2713\t\u2014\t\u2014\t\u2014\t\u2014\t\u2713\t\u2713\n"
    L"ETS2 / ATS\t\u2713\t\u2713\t\u2014\t\u2014\t\u2014\t\u2014\t\u2713\t\u2713\n"
    L"EA WRC\t\u2713\t\u2713\t\u2014\t\u2014\t\u2014\t\u2014\t\u2713\t\u2713\n"
    L"F1 2018-25\t\u2713\t\u2014\t\u2014\t\u2014\t\u2014\t\u2014\t\u2014\t\u2014";

static const ManualRow TAB_FFB_SETTINGS[] = {
    { ROW_TITLE,   L"Force Feedback  -  Settings Reference", nullptr },
    { ROW_SEP,     nullptr, nullptr },
    { ROW_HEADING, L"FFB Feature Support by Game", nullptr },
    { ROW_BODY,    L"Spring is always active (uses the wheel's own angle sensor, no telemetry needed). Every other effect depends on the game actually providing that data - the table shows what each supported game feeds in:", nullptr },
    { ROW_SEP,     nullptr, nullptr },
    { ROW_TABLE,   g_ffbTableData, nullptr },
    { ROW_BODY,    L"'Pedal Vib' is not a separate telemetry channel: the pedal motors run off the same rumble signal as the wheel (Surface + ABS Rumble + G-Spike added together). A game therefore drives the pedals whenever at least one of those three columns is ticked.", nullptr },
    { ROW_BODY,    L"The G-Spike threshold is normalised per game, so the same slider position means the same severity of event everywhere - it is not compared against a raw number that means something different in each title. A truck in ETS2 / ATS reaches its threshold at around 0.3 g and a hard swerve at 0.5 g, exactly as a race car does at 1.5 g and a kerb strike at 2.5 g.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Steering FFB", nullptr },
    { ROW_BULLET,  L"Spring Strength  (0.0 - 1.5)  -  How hard the wheel pulls back to centre. 1.0 = normal. 0 = no centering force. Sent to the Base hardware on connect.", nullptr },
    { ROW_BULLET,  L"Start Angle  (0 - 90 deg)  -  Deadzone around centre. No force below this angle. Lower = more sensitive near centre. Default: 5 deg.", nullptr },
    { ROW_BULLET,  L"Full Force Angle  (10 - 180 deg)  -  Wheel angle at which maximum spring force is reached. Lower = forces build up faster. Default: 90 deg.", nullptr },
    { ROW_BULLET,  L"Lateral-G Strength  (0.0 - 1.5)  -  Simulates tyre self-aligning torque in corners. Only active with a supported game running.", nullptr },
    { ROW_BULLET,  L"Spring Linearity  (0.0 - 2.0)  -  Shape of the spring force curve. 0.0 = linear. 1.0 = logarithmic (default). 2.0 = progressive.", nullptr },
    { ROW_BULLET,  L"ABS Torque Strength  (0.0 - 1.5)  -  Pulsing kickback into the steering when ABS intervenes. Requires game ABS data (ACC, iRacing, AMS2, RaceRoom).", nullptr },
    { ROW_BULLET,  L"TC Torque Strength  (0.0 - 1.5)  -  Light pulse opposite to oversteer when TC cuts power. Requires TC data (ACC, AMS2, RaceRoom).", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Wheel Vibration", nullptr },
    { ROW_BULLET,  L"Overall Intensity  (0.0 - 1.5)  -  Master scale for all vibration effects. Does not affect the steering spring.", nullptr },
    { ROW_BULLET,  L"Surface Threshold  (0.0 - 1.0)  -  Filters small suspension movements. Raise to reduce road noise on smooth tarmac. Default: 0.15.", nullptr },
    { ROW_BULLET,  L"ABS Strength  (0.0 - 1.5)  -  Intensity of the wheel ABS pulse. Set to 0 to disable. Default: 1.0.", nullptr },
    { ROW_BULLET,  L"G-Spike Threshold  (0.5 - 3.0 g)  -  Lateral-G level that triggers a spike event (kerbs, collisions). Default: 1.5 g. The scale refers to a race car; RaceRoom and ETS2 / ATS are normalised onto it automatically, so the same setting fits every game.", nullptr },
    { ROW_BULLET,  L"G-Spike Strength  (0.0 - 1.5)  -  Intensity of the spike rumble. Default: 1.0.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Pedal Vibration", nullptr },
    { ROW_BODY,    L"Requires 'Pedal Vibration Motors' enabled in Box Hardware Settings and motors wired to pins A2/A3.", nullptr },
    { ROW_BODY,    L"How it is calculated: the software builds one rumble value (0 - 255) per FFB frame from Surface + ABS Rumble + G-Spike, scaled by 'Overall Intensity', and sends it to the Base. The Base switches both pedal motors on whenever that value reaches the Threshold below, at a duty cycle set by Intensity. Both pedals always vibrate together - there is no separate throttle/brake signal.", nullptr },
    { ROW_BODY,    L"Supported games: ACC, RaceRoom, AMS2, iRacing, Assetto Corsa, DiRT / GRID, Forza, rF2 / LMU, ETS2 / ATS, EA WRC. Not supported: BeamNG and F1 2018-25 - their telemetry feeds no Surface, ABS or Lateral-G data, so the rumble value stays at zero.", nullptr },
    { ROW_BODY,    L"In ETS2 / ATS the G-Spike is the only rumble source, so the pedals stay quiet during normal driving and only respond to hard cornering and swerving - that is intended, a truck simply does not generate the constant surface and ABS data a race car does.", nullptr },
    { ROW_BULLET,  L"Intensity  (0.0 - 1.5)  -  PWM duty of the pedal motors. 1.0 = motors run continuously at full power while the threshold is exceeded, lower values dim them. Values above 1.0 add no further power - keep it at 1.0 or below.", nullptr },
    { ROW_BULLET,  L"Threshold  (0 - 255)  -  Minimum rumble value before pedals activate. Prevents constant buzzing. Default: 60.", nullptr },
    { ROW_BULLET,  L"Note: 'Surface Threshold', 'ABS Strength' and the G-Spike settings under Wheel Vibration also shape the pedal signal, since it is the same value. Setting 'Overall Intensity' to 0 silences the pedals as well.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Tips", nullptr },
    { ROW_BULLET,  L"'Reset to Defaults' restores all sliders to factory values (confirmation required).", nullptr },
    { ROW_BULLET,  L"Changes take effect immediately - no restart needed.", nullptr },
    { ROW_BULLET,  L"'Save' writes settings to disk and sends spring parameters live to the Base.", nullptr },
    { ROW_BODY,    L"", nullptr },
};

// =============================================================================
// TAB 4+ – Game-specific tabs (unchanged content, updated formatting)
// =============================================================================
static const ManualRow TAB_FORZA[] = {
    { ROW_TITLE,   L"Forza Motorsport / Horizon 4, 5 & 6", nullptr },
    { ROW_BODY,    L"Protocol: UDP (port 5300, Dash format)  |  No plugin required", nullptr },
    { ROW_SEP,     nullptr, nullptr },
    { ROW_HEADING, L"Required In-Game Setup", nullptr },
    { ROW_BULLET,  L"Settings  >  HUD and Gameplay  >  UDP Race Telemetry", nullptr },
    { ROW_BULLET,  L"Data Out:         ON", nullptr },
    { ROW_BULLET,  L"Data Out IP:      127.0.0.1", nullptr },
    { ROW_BULLET,  L"Data Out Port:    5300", nullptr },
    { ROW_BULLET,  L"Data Out Format:  Dash", nullptr },
    { ROW_SEP,     nullptr, nullptr },
    { ROW_HEADING, L"Detected Executables", nullptr },
    { ROW_BULLET,  L"Forza Motorsport 2023:   ForzaMotorsport.exe", nullptr },
    { ROW_BULLET,  L"Forza Horizon 6:         ForzaHorizon6.exe", nullptr },
    { ROW_BULLET,  L"Forza Horizon 5:         ForzaHorizon5.exe", nullptr },
    { ROW_BULLET,  L"Forza Horizon 4:         ForzaHorizon4.exe", nullptr },
    { ROW_SEP,     nullptr, nullptr },
    { ROW_HEADING, L"Reference", nullptr },
    { ROW_LINK,    L"Forza UDP Telemetry Documentation", L"https://support.forzamotorsport.net/hc/en-us/articles/21742934024211" },
};

static const ManualRow TAB_DIRT[] = {
    { ROW_TITLE,   L"DiRT Series  /  GRID Autosport", nullptr },
    { ROW_BODY,    L"Protocol: UDP extradata=3  |  No plugin required", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"DiRT Rally 1  /  DiRT Rally 2.0  /  DiRT 4", nullptr },
    { ROW_BODY,    L"Edit hardware_settings_config.xml and set port to 20777:", nullptr },
    { ROW_CODE,    L"DiRT Rally 2.0:  %USERPROFILE%\\Documents\\My Games\\DiRT Rally 2.0\\hardwaresettings\\hardware_settings_config.xml", nullptr },
    { ROW_CODE,    L"DiRT Rally 1:    %USERPROFILE%\\Documents\\My Games\\DiRT Rally\\hardwaresettings\\hardware_settings_config.xml", nullptr },
    { ROW_CODE,    L"DiRT 4:          %USERPROFILE%\\Documents\\My Games\\DiRT 4\\hardwaresettings\\hardware_settings_config.xml", nullptr },
    { ROW_CODE,    L"<udp enabled=\"true\" extradata=\"3\" ip=\"127.0.0.1\" port=\"20777\" delay=\"1\" />", nullptr },
    { ROW_BULLET,  L"Executables:  dirtrally2.exe  |  dirtrally.exe  |  dirt4.exe", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"DiRT 2  (2009)", nullptr },
    { ROW_CODE,    L"DiRT 2:  %USERPROFILE%\\Documents\\My Games\\DiRT 2\\hardwaresettings\\hardware_settings_config.xml", nullptr },
    { ROW_CODE,    L"<udp enabled=\"true\" extradata=\"3\" ip=\"127.0.0.1\" port=\"20777\" delay=\"1\" />", nullptr },
    { ROW_BULLET,  L"Executables:  dirt2.exe  |  dirt2_game.exe", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"DiRT 3  /  DiRT 3 Complete Edition", nullptr },
    { ROW_CODE,    L"DiRT 3:  %USERPROFILE%\\Documents\\My Games\\DiRT3\\hardwaresettings\\hardware_settings_config.xml", nullptr },
    { ROW_CODE,    L"<motion enabled=\"true\" ip=\"127.0.0.1\" port=\"20777\" delay=\"1\" extradata=\"3\" />", nullptr },
    { ROW_BULLET,  L"Note: DiRT 3 Complete Edition has broken UDP - requires the community fix.", nullptr },
    { ROW_BULLET,  L"Executables:  dirt3game.exe  |  dirt3.exe", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"GRID Autosport", nullptr },
    { ROW_CODE,    L"%USERPROFILE%\\Documents\\My Games\\GRID Autosport\\hardwaresettings\\hardware_settings_config.xml", nullptr },
    { ROW_CODE,    L"<motion enabled=\"true\" ip=\"127.0.0.1\" port=\"20777\" delay=\"1\" extradata=\"3\" />", nullptr },
    { ROW_BULLET,  L"Executables:  GRID_Autosport.exe  |  GRIDAutosport_avx.exe", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Reference", nullptr },
    { ROW_LINK,    L"DiRT Rally UDP format (community doc)", L"https://docs.google.com/spreadsheets/d/1UTgeE7vbnGIzDPFnkHCZjKvnBq5xPCGBFoJWFLCCWQg" },
};

static const ManualRow TAB_ETS[] = {
    { ROW_TITLE,   L"Euro Truck Simulator 2  /  American Truck Simulator", nullptr },
    { ROW_BODY,    L"Protocol: Shared Memory via SCS SDK Plugin  |  Plugin REQUIRED", nullptr },
    { ROW_SEP,     nullptr, nullptr },
    { ROW_HEADING, L"Plugin Installation", nullptr },
    { ROW_BODY,    L"1.  Download the latest scs-telemetry.dll:", nullptr },
    { ROW_LINK,    L"scs-sdk-plugin Releases (GitHub)", L"https://github.com/RenCloud/scs-sdk-plugin/releases" },
    { ROW_BODY,    L"2.  Copy the DLL into the game plugins folder:", nullptr },
    { ROW_CODE,    L"ETS2:  ...\\Euro Truck Simulator 2\\bin\\win_x64\\plugins\\scs-telemetry.dll", nullptr },
    { ROW_CODE,    L"ATS:   ...\\American Truck Simulator\\bin\\win_x64\\plugins\\scs-telemetry.dll", nullptr },
    { ROW_BODY,    L"3.  Launch the game. The plugin activates automatically.", nullptr },
    { ROW_SEP,     nullptr, nullptr },
    { ROW_HEADING, L"Reference", nullptr },
    { ROW_LINK,    L"RenCloud scs-sdk-plugin (GitHub)", L"https://github.com/RenCloud/scs-sdk-plugin" },
};

static const ManualRow TAB_BEAMNG[] = {
    { ROW_TITLE,   L"BeamNG.drive", nullptr },
    { ROW_BODY,    L"Protocol: UDP OutGauge (port 4444)  |  No plugin required", nullptr },
    { ROW_SEP,     nullptr, nullptr },
    { ROW_HEADING, L"Required In-Game Setup", nullptr },
    { ROW_BULLET,  L"Options  >  Other  >  Protocols  >  OutGauge", nullptr },
    { ROW_BULLET,  L"Enable OutGauge", nullptr },
    { ROW_BULLET,  L"IP:    127.0.0.1", nullptr },
    { ROW_BULLET,  L"Port:  4444", nullptr },
    { ROW_BULLET,  L"Click Apply", nullptr },
    { ROW_SEP,     nullptr, nullptr },
    { ROW_HEADING, L"Reference", nullptr },
    { ROW_LINK,    L"BeamNG OutGauge Documentation", L"https://documentation.beamng.com/modding/outgauge/" },
};

static const ManualRow TAB_RF2[] = {
    { ROW_TITLE,   L"rFactor 2  /  Le Mans Ultimate", nullptr },
    { ROW_BODY,    L"Protocol: Shared Memory via rF2 SharedMemory Plugin  |  Plugin REQUIRED", nullptr },
    { ROW_SEP,     nullptr, nullptr },
    { ROW_HEADING, L"Plugin Installation", nullptr },
    { ROW_BODY,    L"1.  Download the plugin:", nullptr },
    { ROW_LINK,    L"rF2 Shared Memory Tools (Overtake.gg)", L"https://www.overtake.gg/downloads/rf2-shared-memory-tools-for-developers.19334" },
    { ROW_BODY,    L"2.  Copy the DLL into the game plugins folder:", nullptr },
    { ROW_CODE,    L"rFactor 2:        ...\\rFactor 2\\Bin64\\Plugins\\rF2SharedMemoryMapPlugin64.dll", nullptr },
    { ROW_CODE,    L"Le Mans Ultimate: ...\\Le Mans Ultimate\\Bin64\\Plugins\\rF2SharedMemoryMapPlugin64.dll", nullptr },
    { ROW_BODY,    L"3.  Launch the game. SimRacePro detects the shared memory automatically.", nullptr },
    { ROW_SEP,     nullptr, nullptr },
    { ROW_HEADING, L"Reference", nullptr },
    { ROW_LINK,    L"rF2SharedMemoryMapPlugin (GitHub)", L"https://github.com/TheIronWolfModding/rF2SharedMemoryMapPlugin" },
};

static const ManualRow TAB_F1[] = {
    { ROW_TITLE,   L"F1 2018 - F1 25  (Codemasters / EA)", nullptr },
    { ROW_BODY,    L"Protocol: UDP (port 20777)  |  No plugin required", nullptr },
    { ROW_BODY,    L"Supported: F1 2018, 2019, 2020, 2021, F1 22, F1 23, F1 24, F1 25.", nullptr },
    { ROW_SEP,     nullptr, nullptr },
    { ROW_HEADING, L"Required In-Game Setup", nullptr },
    { ROW_BULLET,  L"Game Options  >  Settings  >  Telemetry Settings", nullptr },
    { ROW_BULLET,  L"UDP Telemetry:  ON", nullptr },
    { ROW_BULLET,  L"UDP IP:         127.0.0.1  (Broadcast Mode: OFF)", nullptr },
    { ROW_BULLET,  L"UDP Port:       20777", nullptr },
    { ROW_BULLET,  L"UDP Send Rate:  60 Hz", nullptr },
    { ROW_SEP,     nullptr, nullptr },
    { ROW_HEADING, L"Detected Executables", nullptr },
    { ROW_BULLET,  L"F1 25: F1_25.exe  |  F1 24: F1_24.exe  |  F1 23: F1_23.exe  |  F1 22: F1_22.exe", nullptr },
    { ROW_BULLET,  L"F1 2021: F1_2021.exe  |  F1 2020: F1_2020.exe  |  F1 2019: F1_2019.exe  |  F1 2018: F1_2018.exe", nullptr },
    { ROW_SEP,     nullptr, nullptr },
    { ROW_HEADING, L"Reference", nullptr },
    { ROW_LINK,    L"F1 UDP Specification (EA Answers)", L"https://answers.ea.com/t5/General-Discussion/F1-23-UDP-Specification/td-p/12317769" },
};

static const ManualRow TAB_OTHER[] = {
    { ROW_TITLE,   L"Auto-Detected Games", nullptr },
    { ROW_BODY,    L"The following games work out of the box via Shared Memory. Launch the game and SimRacePro detects it automatically.", nullptr },
    { ROW_BODY,    L"Shift-light LEDs blink at 97% of max RPM the same way for every supported game - this is computed centrally, not read from a game-native shift-light flag.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"iRacing", nullptr },
    { ROW_BODY,    L"Shared Memory (iRacing SDK built-in). Must be in an active session (on track).", nullptr },
    { ROW_LINK,    L"iRacing Developer Portal", L"https://developer.iracing.com/" },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Assetto Corsa  /  ACC", nullptr },
    { ROW_BODY,    L"Shared Memory (Local\\acpmf_*). Both AC (acs.exe) and ACC (AC2-Win64-Shipping.exe) detected automatically.", nullptr },
    { ROW_LINK,    L"ACC Shared Memory (Kunos official)", L"https://github.com/acc-devs/acc-sharedmemory-doc" },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"EA Sports WRC  /  WRC Generations", nullptr },
    { ROW_BODY,    L"Shared Memory - detected automatically. Note: Throttle/Brake not available in WRC shared memory.", nullptr },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"RaceRoom Racing Experience", nullptr },
    { ROW_BODY,    L"Shared Memory ($R3E$). Detected via RRRE.exe / RRRE64.exe.", nullptr },
    { ROW_LINK,    L"RaceRoom Spectator SDK", L"https://github.com/sector3studios/r3e-spectator-overlay" },
    { ROW_SEP,     nullptr, nullptr },

    { ROW_HEADING, L"Automobilista 2  /  Project CARS 3", nullptr },
    { ROW_BODY,    L"Shared Memory ($pcars2$). Detected via ams2.exe / ams2avx.exe / pcars3.exe / pcars2.exe (Project CARS 2 uses the same shared-memory format but isn't a tested title here).", nullptr },
    { ROW_BODY,    L"", nullptr },
};

struct TabDef { const wchar_t* title; const ManualRow* rows; int count; };
static const TabDef MANUAL_TABS[] = {
    { L"Overview",                TAB_GETTING_STARTED, (int)(sizeof(TAB_GETTING_STARTED)/sizeof(ManualRow)) },
    { L"Box\nHardware\nSettings", TAB_BOX_SETTINGS,    (int)(sizeof(TAB_BOX_SETTINGS)/sizeof(ManualRow))    },
    { L"Force\nFeedback\nSettings",TAB_FFB_SETTINGS,   (int)(sizeof(TAB_FFB_SETTINGS)/sizeof(ManualRow))    },
    { L"Forza",                   TAB_FORZA,           (int)(sizeof(TAB_FORZA)/sizeof(ManualRow))           },
    { L"DiRT / GRID",             TAB_DIRT,            (int)(sizeof(TAB_DIRT)/sizeof(ManualRow))            },
    { L"ETS2 / ATS",              TAB_ETS,             (int)(sizeof(TAB_ETS)/sizeof(ManualRow))             },
    { L"BeamNG",                  TAB_BEAMNG,          (int)(sizeof(TAB_BEAMNG)/sizeof(ManualRow))          },
    { L"rF2 / LMU",               TAB_RF2,             (int)(sizeof(TAB_RF2)/sizeof(ManualRow))             },
    { L"F1",                      TAB_F1,              (int)(sizeof(TAB_F1)/sizeof(ManualRow))              },
    { L"Others",                  TAB_OTHER,           (int)(sizeof(TAB_OTHER)/sizeof(ManualRow))           },
};
static const int MANUAL_TAB_COUNT = (int)(sizeof(MANUAL_TABS)/sizeof(TabDef));


// ── Manual panel state ────────────────────────────────────────────────────────
static HWND g_hwndManualDlg   = nullptr;
static HWND g_hwndManualPanel = nullptr;   // scrollable content child
static int  g_manualActiveTab  = 0;
static int  g_manualScrollY    = 0;        // current vertical scroll offset (pixels)
static const int TAB_H         = 56;       // tab bar height in pixels (tall enough for 3-line labels)
static int  g_manualContentH   = 0;        // total content height (pixels)

// Fonts for the panel (created once, destroyed on close)
static HFONT g_mfTitle   = nullptr;
static HFONT g_mfHeading = nullptr;
static HFONT g_mfBody    = nullptr;
static HFONT g_mfCode    = nullptr;
static HBRUSH g_mfBg     = nullptr;

// Measured row rects for hit-testing links (rebuilt on paint/resize)
struct LinkRect { RECT rc{}; const wchar_t* url = nullptr; std::wstring copyText; };
static std::vector<LinkRect> g_manualLinks;

// Layout constants
#define MPL_MARGIN   18    // left/right margin
#define MPL_INDENT   30    // bullet/code extra indent
#define MPL_GAP_SM    6    // small vertical gap
#define MPL_GAP_MD   12    // medium gap (after heading)
#define MPL_GAP_LG   18    // large gap (separator)
#define MPL_SEP_H     1    // separator line height

// ── Measure + draw content ────────────────────────────────────────────────────
// Pass drawMode=false  → only measure total height (g_manualContentH)
// Pass drawMode=true   → also draw; scrollY already subtracted from y
static void ManualLayoutContent(HDC hdc, int panelW, bool drawMode, int scrollY) {
    if (drawMode) g_manualLinks.clear();

    int y = MPL_GAP_MD;

    auto drawRow = [&](const ManualRow& row) {
        int x     = MPL_MARGIN;
        int textW = panelW - MPL_MARGIN * 2;
        HFONT fnt = g_mfBody;
        COLORREF col = RGB(210,215,225);
        bool isLink  = false;
        int extraIndent = 0;

        switch (row.type) {
        case ROW_TITLE:
            fnt = g_mfTitle; col = RGB(0,170,255); break;
        case ROW_HEADING:
            fnt = g_mfHeading; col = RGB(230,230,240); break;
        case ROW_BODY:
            fnt = g_mfBody; col = RGB(200,205,215); break;
        case ROW_BULLET:
            fnt = g_mfBody; col = RGB(200,205,215); extraIndent = MPL_INDENT; break;
        case ROW_LINK:
            fnt = g_mfBody; col = RGB(0,170,255); isLink = true; break;
        case ROW_CODE:
            fnt = g_mfCode; col = RGB(140,200,140); extraIndent = MPL_INDENT; break;
        case ROW_TABLE: {
            // Parse tab/newline-separated table data and render as a grid
            // First row = header, remaining rows = data
            if (!row.text) { y += 8; return; }
            std::wstring data(row.text);

            // Split into rows
            std::vector<std::wstring> rows;
            std::wstring cur;
            for (wchar_t ch : data) {
                if (ch == L'\n') { rows.push_back(cur); cur.clear(); }
                else cur += ch;
            }
            if (!cur.empty()) rows.push_back(cur);
            if (rows.empty()) { y += 8; return; }

            // Split header to get column count
            auto splitTab = [](const std::wstring& s) {
                std::vector<std::wstring> cols;
                std::wstring c;
                for (wchar_t ch : s) {
                    if (ch == L'\t') { cols.push_back(c); c.clear(); }
                    else c += ch;
                }
                cols.push_back(c);
                return cols;
            };

            auto headers = splitTab(rows[0]);
            int nCols = (int)headers.size();
            if (nCols < 1) { y += 8; return; }

            int tblX   = x + MPL_INDENT;
            int tblW   = panelW - MPL_MARGIN * 2 - MPL_INDENT * 2;
            int col0W  = 110;  // game name column
            int colW   = (nCols > 1) ? (tblW - col0W) / (nCols - 1) : tblW;
            int rowH   = 22;
            int hdrH   = 24;

            HFONT fntHdr = CreateFontW(13,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            HFONT fntCell = CreateFontW(12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");

            int drawY = y - scrollY;

            if (drawMode) {
                SetBkMode(hdc, TRANSPARENT);

                // Header row background
                HBRUSH hdrBg = CreateSolidBrush(RGB(35,45,65));
                RECT hdrR = { tblX, drawY, tblX+tblW, drawY+hdrH };
                FillRect(hdc, &hdrR, hdrBg);
                DeleteObject(hdrBg);

                // Draw header cells
                SelectObject(hdc, fntHdr);
                SetTextColor(hdc, RGB(180,210,255));
                for (int c = 0; c < nCols; c++) {
                    // Columns tile the area right of col0 exactly - no extra
                    // offset, or the rightmost cell draws past the table border
                    // once the column count grows (9 columns at 900px width).
                    int cx = tblX + (c == 0 ? 6 : col0W + (c-1)*colW);
                    int cw = (c == 0) ? col0W : colW;
                    RECT cr = { cx, drawY+4, cx+cw-4, drawY+hdrH-2 };
                    UINT align = (c == 0) ? DT_LEFT : DT_CENTER;
                    DrawTextW(hdc, headers[c].c_str(), -1, &cr, align|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
                }

                // Data rows
                for (int r = 1; r < (int)rows.size(); r++) {
                    auto cells = splitTab(rows[r]);
                    int ry = drawY + hdrH + (r-1)*rowH;
                    // Alternating row background
                    COLORREF bg = (r % 2 == 0) ? RGB(28,30,38) : RGB(33,36,46);
                    HBRUSH rowBg = CreateSolidBrush(bg);
                    RECT rowR = { tblX, ry, tblX+tblW, ry+rowH };
                    FillRect(hdc, &rowR, rowBg);
                    DeleteObject(rowBg);

                    for (int c = 0; c < nCols && c < (int)cells.size(); c++) {
                        bool isCheck = (cells[c] == L"\u2713");
                        bool isDash  = (cells[c] == L"\u2014");
                        COLORREF tcol = c == 0 ? RGB(220,220,225)
                                      : isCheck ? RGB(100,220,130)
                                      : isDash  ? RGB(80,85,100)
                                      : RGB(200,205,215);
                        SelectObject(hdc, c == 0 ? fntHdr : fntCell);
                        SetTextColor(hdc, tcol);
                        int cx = tblX + (c == 0 ? 6 : col0W + (c-1)*colW);
                        int cw = (c == 0) ? col0W - 6 : colW;
                        RECT cr = { cx, ry+2, cx+cw, ry+rowH-2 };
                        UINT align = (c == 0) ? DT_LEFT : DT_CENTER;
                        DrawTextW(hdc, cells[c].c_str(), -1, &cr, align|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
                    }
                }

                // Table border
                HPEN borderPen = CreatePen(PS_SOLID,1,RGB(60,70,100));
                SelectObject(hdc, borderPen);
                SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, tblX, drawY, tblX+tblW, drawY+hdrH+(int)(rows.size()-1)*rowH);
                DeleteObject(borderPen);
            }

            DeleteObject(fntHdr);
            DeleteObject(fntCell);

            y += hdrH + (int)(rows.size()-1)*rowH + MPL_GAP_MD;
            return;
        }
        case ROW_SEP:
            y += MPL_GAP_LG;
            if (drawMode) {
                HPEN p = CreatePen(PS_SOLID,1,RGB(50,52,62));
                SelectObject(hdc,p); SetBkMode(hdc,TRANSPARENT);
                MoveToEx(hdc, x, y - scrollY, nullptr);
                LineTo(hdc, panelW - MPL_MARGIN, y - scrollY);
                DeleteObject(p);
            }
            y += MPL_GAP_LG;
            return;
        }

        x     += extraIndent;
        textW -= extraIndent;

        SelectObject(hdc, fnt);

        // For bullets prepend a dot
        std::wstring display;
        if (row.type == ROW_BULLET)  display = std::wstring(L"\x2022  ") + row.text;
        else if (row.type == ROW_LINK) display = std::wstring(L"\x25B6  ") + row.text;
        else                           display = row.text ? row.text : L"";

        RECT measure = { x, 0, x + textW, 0 };
        DrawTextW(hdc, display.c_str(), -1, &measure, DT_LEFT|DT_WORDBREAK|DT_CALCRECT|DT_NOPREFIX);
        int rowH = measure.bottom - measure.top;

        if (drawMode) {
            RECT draw = { x, y - scrollY, x + textW, y - scrollY + rowH + 2 };

            if (isLink) {
                // Underline for links
                HFONT fntU = CreateFontW(14,0,0,0,FW_NORMAL,0,1,0,DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
                SelectObject(hdc, fntU);
                SetTextColor(hdc, col);
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, display.c_str(), -1, &draw, DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
                DeleteObject(fntU);
                // Register hit rect (absolute y for hit testing)
                LinkRect lr;
                lr.rc = { x, y, x + textW, y + rowH };
                lr.url = row.url;
                g_manualLinks.push_back(lr);
            } else {
                SelectObject(hdc, fnt);
                SetTextColor(hdc, col);
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, display.c_str(), -1, &draw, DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);

                // ROW_CODE: draw a small copy button on the right
                if (row.type == ROW_CODE) {
                    const int BTN_W = 34, BTN_H = 16;
                    int bx = panelW - MPL_MARGIN - BTN_W;
                    int by = y - scrollY;
                    RECT btnR = { bx, by, bx + BTN_W, by + BTN_H };
                    HBRUSH btnBg = CreateSolidBrush(RGB(45,52,68));
                    FillRect(hdc, &btnR, btnBg);
                    DeleteObject(btnBg);
                    HPEN btnPen = CreatePen(PS_SOLID,1,RGB(80,90,120));
                    SelectObject(hdc, btnPen);
                    SelectObject(hdc, GetStockObject(NULL_BRUSH));
                    Rectangle(hdc, bx, by, bx+BTN_W, by+BTN_H);
                    DeleteObject(btnPen);
                    HFONT btnFont = CreateFontW(11,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
                    SelectObject(hdc, btnFont);
                    SetTextColor(hdc, RGB(160,200,160));
                    SetBkMode(hdc, TRANSPARENT);
                    DrawTextW(hdc, L"Copy", -1, &btnR, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                    DeleteObject(btnFont);
                    SelectObject(hdc, fnt);
                    LinkRect clr;
                    clr.rc  = { bx, y, bx + BTN_W, y + BTN_H };
                    clr.url = L"action:copy";
                    clr.copyText = row.text ? row.text : L"";
                    g_manualLinks.push_back(clr);
                }
            }

            // Heading underline accent
            if (row.type == ROW_HEADING) {
                HPEN ap = CreatePen(PS_SOLID,1,RGB(0,120,180));
                SelectObject(hdc,ap);
                MoveToEx(hdc, MPL_MARGIN, y - scrollY + rowH + 3, nullptr);
                LineTo(hdc, panelW - MPL_MARGIN, y - scrollY + rowH + 3);
                DeleteObject(ap);
                y += 4;
            }
        }

        // Title gets extra space below
        int gapAfter = (row.type == ROW_TITLE)   ? MPL_GAP_LG  :
                       (row.type == ROW_HEADING)  ? MPL_GAP_MD  :
                       (row.type == ROW_CODE)     ? MPL_GAP_SM  : MPL_GAP_SM;

        y += rowH + gapAfter;
    };

    const TabDef& tab = MANUAL_TABS[g_manualActiveTab];
    for (int i = 0; i < tab.count; i++) drawRow(tab.rows[i]);

    y += MPL_GAP_LG;
    g_manualContentH = y;
}

// Forward declarations – defined further below
static void OpenFfbSettingsWindow(HWND parent, ConfigManager* cfg);
static void OpenBoxSettingsWindow(HWND parent, ConfigManager* cfg);
static LRESULT CALLBACK BoxSettingsProc(HWND, UINT, WPARAM, LPARAM);
static void OpenPinConfigWindow(HWND parent, ConfigManager* cfg);
static LRESULT CALLBACK PinConfigProc(HWND, UINT, WPARAM, LPARAM);
static void OpenManualDialogOnTab(HWND hParent, int tabIndex);

// ── Panel WndProc ─────────────────────────────────────────────────────────────
static LRESULT CALLBACK ManualPanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdcReal = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        // Double-buffer
        HDC hdc = CreateCompatibleDC(hdcReal);
        HBITMAP bmp = CreateCompatibleBitmap(hdcReal, cr.right, cr.bottom);
        HBITMAP old = (HBITMAP)SelectObject(hdc, bmp);
        FillRect(hdc, &cr, g_mfBg);

        ManualLayoutContent(hdc, cr.right, true, g_manualScrollY);

        BitBlt(hdcReal,0,0,cr.right,cr.bottom,hdc,0,0,SRCCOPY);
        SelectObject(hdc,old); DeleteObject(bmp); DeleteDC(hdc);
        EndPaint(hwnd,&ps);
        break;
    }
    case WM_SIZE: {
        // Remeasure content height with dummy DC
        HDC hdc = GetDC(hwnd);
        SelectObject(hdc, g_mfBody);
        ManualLayoutContent(hdc, LOWORD(lp), false, 0);
        ReleaseDC(hwnd,hdc);
        // Update scrollbar
        SCROLLINFO si={sizeof(si), SIF_RANGE|SIF_PAGE};
        si.nMin=0; si.nMax=g_manualContentH; si.nPage=HIWORD(lp);
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        // Clamp scroll
        SCROLLINFO si2={sizeof(si2), SIF_POS}; GetScrollInfo(hwnd,SB_VERT,&si2);
        g_manualScrollY=si2.nPos;
        InvalidateRect(hwnd,nullptr,FALSE);
        break;
    }
    case WM_VSCROLL: {
        SCROLLINFO si={sizeof(si),SIF_ALL}; GetScrollInfo(hwnd,SB_VERT,&si);
        int oldPos=si.nPos;
        switch(LOWORD(wp)){
        case SB_TOP:         si.nPos=si.nMin; break;
        case SB_BOTTOM:      si.nPos=si.nMax; break;
        case SB_LINEUP:      si.nPos-=20;     break;
        case SB_LINEDOWN:    si.nPos+=20;     break;
        case SB_PAGEUP:      si.nPos-=si.nPage; break;
        case SB_PAGEDOWN:    si.nPos+=si.nPage; break;
        case SB_THUMBTRACK:  si.nPos=si.nTrackPos; break;
        }
        si.nPos=std::max(si.nMin, std::min(si.nPos,(int)(si.nMax-(int)si.nPage)));
        SetScrollInfo(hwnd,SB_VERT,&si,TRUE);
        GetScrollInfo(hwnd,SB_VERT,&si);
        if(si.nPos!=oldPos){ g_manualScrollY=si.nPos; InvalidateRect(hwnd,nullptr,FALSE); }
        break;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        SCROLLINFO si={sizeof(si),SIF_ALL}; GetScrollInfo(hwnd,SB_VERT,&si);
        si.nPos -= delta/4;
        si.nPos=std::max(si.nMin, std::min(si.nPos,(int)(si.nMax-(int)si.nPage)));
        SetScrollInfo(hwnd,SB_VERT,&si,TRUE);
        GetScrollInfo(hwnd,SB_VERT,&si);
        g_manualScrollY=si.nPos; InvalidateRect(hwnd,nullptr,FALSE);
        break;
    }
    case WM_LBUTTONDOWN: {
        int mx=LOWORD(lp), my=(int)(short)HIWORD(lp);
        int absY = my + g_manualScrollY;
        for (auto& lr : g_manualLinks) {
            if (mx>=lr.rc.left && mx<=lr.rc.right && absY>=lr.rc.top && absY<=lr.rc.bottom) {
                if (wcsncmp(lr.url, L"action:", 7) == 0) {
                    if (wcscmp(lr.url, L"action:ffb_settings") == 0) {
                        HWND topDlg = GetParent(GetParent(hwnd));
                        OpenManualDialogOnTab(topDlg ? topDlg : hwnd, 1);
                    } else if (wcscmp(lr.url, L"action:copy") == 0 && !lr.copyText.empty()) {
                        // Copy code text to clipboard
                        size_t bytes = (lr.copyText.size() + 1) * sizeof(wchar_t);
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                        if (hMem) {
                            void* pMem = GlobalLock(hMem);
                            if (pMem) { memcpy(pMem, lr.copyText.c_str(), bytes); GlobalUnlock(hMem); }
                            OpenClipboard(hwnd);
                            EmptyClipboard();
                            SetClipboardData(CF_UNICODETEXT, hMem);
                            CloseClipboard();
                        }
                    }
                } else {
                    ShellExecuteW(nullptr, L"open", lr.url, nullptr, nullptr, SW_SHOWNORMAL);
                }
                break;
            }
        }
        break;
    }
    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd,&pt);
        int absY = pt.y + g_manualScrollY;
        for (auto& lr : g_manualLinks) {
            if (pt.x>=lr.rc.left && pt.x<=lr.rc.right && absY>=lr.rc.top && absY<=lr.rc.bottom) {
                SetCursor(LoadCursor(nullptr,IDC_HAND)); return TRUE;
            }
        }
        SetCursor(LoadCursor(nullptr,IDC_ARROW)); return TRUE;
    }
    case WM_ERASEBKGND: return 1;
    default: return DefWindowProcA(hwnd,msg,wp,lp);
    }
    return 0;
}

// ── Dialog WndProc ────────────────────────────────────────────────────────────
static LRESULT CALLBACK ManualDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static HFONT  hFontTab  = nullptr;
    static HBRUSH hBgBrush  = nullptr;
    static HBRUSH hTabBrush = nullptr;
    static HBRUSH hActBrush = nullptr;


    switch (msg) {
    case WM_CREATE: {
        hFontTab  = CreateFontW(13,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        hBgBrush  = CreateSolidBrush(RGB(18,18,24));
        hTabBrush = CreateSolidBrush(RGB(32,32,42));
        hActBrush = CreateSolidBrush(RGB(0,130,200));
        g_mfTitle   = CreateFontW(20,0,0,0,FW_BOLD,  0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_mfHeading = CreateFontW(14,0,0,0,FW_BOLD,  0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_mfBody    = CreateFontW(13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_mfCode    = CreateFontW(12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,  L"Consolas");
        g_mfBg      = CreateSolidBrush(RGB(18,18,24));

        // Register panel class once
        static bool panelReg=false;
        if(!panelReg){
            WNDCLASSEXA pc={}; pc.cbSize=sizeof(pc); pc.lpfnWndProc=ManualPanelProc;
            pc.hInstance=GetModuleHandleA(nullptr); pc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
            pc.lpszClassName="SimRaceProManualPanel"; pc.hCursor=LoadCursor(nullptr,IDC_ARROW);
            RegisterClassExA(&pc); panelReg=true;
        }
        RECT cr; GetClientRect(hwnd,&cr);
        g_manualScrollY=0;
        g_hwndManualPanel=CreateWindowExA(0,"SimRaceProManualPanel","",
            WS_CHILD|WS_VISIBLE|WS_VSCROLL,
            0,TAB_H,cr.right,cr.bottom-TAB_H,hwnd,nullptr,GetModuleHandleA(nullptr),nullptr);
        break;
    }
    case WM_SIZE: {
        int W=LOWORD(lp), H=HIWORD(lp);
        if(g_hwndManualPanel && IsWindow(g_hwndManualPanel))
            SetWindowPos(g_hwndManualPanel,nullptr,0,TAB_H,W,H-TAB_H,SWP_NOZORDER);
        RECT tabRc={0,0,W,TAB_H}; InvalidateRect(hwnd,&tabRc,TRUE);
        break;
    }
    case WM_ERASEBKGND: {
        HDC hdc=(HDC)wp; RECT rc; GetClientRect(hwnd,&rc);
        FillRect(hdc,&rc,hBgBrush); return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc=BeginPaint(hwnd,&ps);
        RECT cr; GetClientRect(hwnd,&cr);
        RECT tabBar={0,0,cr.right,TAB_H};
        FillRect(hdc,&tabBar,hBgBrush);
        // Bottom accent line
        HPEN linePen=CreatePen(PS_SOLID,2,RGB(0,150,220));
        SelectObject(hdc,linePen); MoveToEx(hdc,0,TAB_H-1,nullptr); LineTo(hdc,cr.right,TAB_H-1);
        DeleteObject(linePen);
        SelectObject(hdc,hFontTab); SetBkMode(hdc,TRANSPARENT);
        int eachW=cr.right/MANUAL_TAB_COUNT;
        for(int i=0;i<MANUAL_TAB_COUNT;i++){
            int tx=i*eachW, tw=(i==MANUAL_TAB_COUNT-1)?(cr.right-tx):eachW;
            RECT tabR={tx,0,tx+tw,TAB_H-2};
            bool active=(i==g_manualActiveTab);
            FillRect(hdc,&tabR,active?hActBrush:hTabBrush);
            if(i>0){
                HPEN div=CreatePen(PS_SOLID,1,RGB(45,45,58));
                SelectObject(hdc,div); MoveToEx(hdc,tx,2,nullptr); LineTo(hdc,tx,TAB_H-4);
                DeleteObject(div);
            }
            SetTextColor(hdc,active?RGB(255,255,255):RGB(155,158,170));
            RECT tr={tx+2,0,tx+tw-2,TAB_H-2};
            DrawTextW(hdc,MANUAL_TABS[i].title,-1,&tr,DT_CENTER|DT_VCENTER|DT_WORDBREAK);
        }
        EndPaint(hwnd,&ps);
        break;
    }
    case WM_LBUTTONDOWN: {
        int mx=LOWORD(lp), my=HIWORD(lp);
        if(my<TAB_H){
            RECT cr; GetClientRect(hwnd,&cr);
            int eachW=cr.right/MANUAL_TAB_COUNT;
            int clicked=mx/eachW;
            if(clicked>=MANUAL_TAB_COUNT) clicked=MANUAL_TAB_COUNT-1;
            if(clicked!=g_manualActiveTab){
                g_manualActiveTab=clicked;
                g_manualScrollY=0;
                // Reset scrollbar and redraw panel
                if(g_hwndManualPanel && IsWindow(g_hwndManualPanel)){
                    SCROLLINFO si={sizeof(si),SIF_POS}; si.nPos=0;
                    SetScrollInfo(g_hwndManualPanel,SB_VERT,&si,TRUE);
                    SendMessage(g_hwndManualPanel,WM_SIZE,0,
                        MAKELPARAM(0,0)); // triggers remeasure
                    RECT pr; GetClientRect(g_hwndManualPanel,&pr);
                    SendMessage(g_hwndManualPanel,WM_SIZE,0,
                        MAKELPARAM(pr.right,pr.bottom));
                    InvalidateRect(g_hwndManualPanel,nullptr,FALSE);
                }
                RECT tabRc={0,0,10000,TAB_H}; InvalidateRect(hwnd,&tabRc,FALSE);
            }
        }
        break;
    }
    case WM_EXITSIZEMOVE:
        SaveAuxWindowPos(hwnd, "manual"); break;
    case WM_KEYDOWN:
        if(wp==VK_ESCAPE){ DestroyWindow(hwnd); g_hwndManualDlg=nullptr; } break;
    case WM_CLOSE:
        DestroyWindow(hwnd); g_hwndManualDlg=nullptr; break;
    case WM_DESTROY:
        g_hwndManualPanel=nullptr;
        if(hFontTab)  { DeleteObject(hFontTab);  hFontTab=nullptr;  }
        if(hBgBrush)  { DeleteObject(hBgBrush);  hBgBrush=nullptr;  }
        if(hTabBrush) { DeleteObject(hTabBrush); hTabBrush=nullptr; }
        if(hActBrush) { DeleteObject(hActBrush); hActBrush=nullptr; }
        if(g_mfTitle)   { DeleteObject(g_mfTitle);   g_mfTitle=nullptr;   }
        if(g_mfHeading) { DeleteObject(g_mfHeading); g_mfHeading=nullptr; }
        if(g_mfBody)    { DeleteObject(g_mfBody);    g_mfBody=nullptr;    }
        if(g_mfCode)    { DeleteObject(g_mfCode);    g_mfCode=nullptr;    }
        if(g_mfBg)      { DeleteObject(g_mfBg);      g_mfBg=nullptr;      }
        break;
    default: return DefWindowProcA(hwnd,msg,wp,lp);
    }
    return 0;
}

static void OpenManualDialog(HWND hParent) {
    if(g_hwndManualDlg && IsWindow(g_hwndManualDlg)){ SetForegroundWindow(g_hwndManualDlg); return; }
    g_manualActiveTab=0; g_manualScrollY=0;
    static bool classReg=false;
    if(!classReg){
        WNDCLASSEXA wc={}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=ManualDlgProc;
        wc.hInstance=GetModuleHandleA(nullptr); wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName="SimRaceProManual"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        RegisterClassExA(&wc); classReg=true;
    }
    int mx, my; ComputeAuxWindowPos(hParent, "manual", 900, 580, mx, my);
    g_hwndManualDlg=CreateWindowExA(
        WS_EX_DLGMODALFRAME,"SimRaceProManual","SimRacePro Custom Driver Software - Manual",
        WS_POPUP|WS_VISIBLE|WS_CAPTION|WS_SYSMENU|WS_SIZEBOX,
        mx,my,900,580,
        hParent,nullptr,GetModuleHandleA(nullptr),nullptr);
    ShowWindow(g_hwndManualDlg,SW_SHOW);
    SetForegroundWindow(g_hwndManualDlg);
}

// Open manual and immediately switch to a specific tab
static void OpenManualDialogOnTab(HWND hParent, int tabIndex) {
    bool alreadyOpen = g_hwndManualDlg && IsWindow(g_hwndManualDlg);
    if (!alreadyOpen) {
        // Open fresh – but set tab BEFORE OpenManualDialog resets it
        // We bypass the reset by calling the creation code directly here
        g_manualScrollY = 0;
        static bool classReg2 = false;
        if (!classReg2) {
            WNDCLASSEXA wc={}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=ManualDlgProc;
            wc.hInstance=GetModuleHandleA(nullptr); wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
            wc.lpszClassName="SimRaceProManual"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
            RegisterClassExA(&wc); classReg2=true;
        }
        g_manualActiveTab = tabIndex;
        int mx, my; ComputeAuxWindowPos(hParent, "manual", 900, 580, mx, my);
        g_hwndManualDlg=CreateWindowExA(
            WS_EX_DLGMODALFRAME,"SimRaceProManual","SimRacePro Custom Driver Software - Manual",
            WS_POPUP|WS_VISIBLE|WS_CAPTION|WS_SYSMENU|WS_SIZEBOX,
            mx,my,900,580,
            hParent,nullptr,GetModuleHandleA(nullptr),nullptr);
        ShowWindow(g_hwndManualDlg,SW_SHOW);
    } else {
        // Already open – switch tab and refresh
        g_manualActiveTab = tabIndex;
        g_manualScrollY   = 0;
        if (g_hwndManualPanel && IsWindow(g_hwndManualPanel)) {
            SCROLLINFO si={sizeof(si),SIF_POS}; si.nPos=0;
            SetScrollInfo(g_hwndManualPanel,SB_VERT,&si,TRUE);
            RECT pr; GetClientRect(g_hwndManualPanel,&pr);
            SendMessage(g_hwndManualPanel,WM_SIZE,0,MAKELPARAM(pr.right,pr.bottom));
            InvalidateRect(g_hwndManualPanel,nullptr,TRUE);
        }
        InvalidateRect(g_hwndManualDlg,nullptr,TRUE);
    }
    SetForegroundWindow(g_hwndManualDlg);
}
// ── Calibration popup window ───────────────────────────────────────────────────
static LRESULT CALLBACK CalibDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateWindowExA(0,"STATIC","Press the button now...",WS_CHILD|WS_VISIBLE|SS_CENTER,20,20,340,30,hwnd,nullptr,nullptr,nullptr);
        CreateWindowExA(0,"STATIC","",WS_CHILD|WS_VISIBLE|SS_CENTER,20,60,340,40,hwnd,(HMENU)1001,nullptr,nullptr);
        CreateWindowExA(0,"BUTTON","Cancel (ESC)",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,130,120,120,30,hwnd,(HMENU)IDCANCEL,nullptr,nullptr);
        break;
    case WM_APP_CALIB_NEXT: {
        std::string txt; { std::lock_guard<std::mutex> lk(g_state->mtx); txt = "[ " + g_state->calib_current_button + " ]  (" + std::to_string(g_state->calib_step + 1) + "/" + std::to_string(g_state->calib_total) + ")"; }
        SetDlgItemTextA(hwnd, 1001, txt.c_str());
        { std::lock_guard<std::mutex> lk(g_state->mtx); g_state->calib_waiting = false; }
        break;
    }
    case WM_APP_CALIB_DONE:   DestroyWindow(hwnd); g_hwndCalibDlg = nullptr; break;
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) { g_state->calib_aborted = true; { std::lock_guard<std::mutex> lk(g_state->mtx); g_state->calib_waiting = false; } DestroyWindow(hwnd); g_hwndCalibDlg = nullptr; } break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)        { g_state->calib_aborted = true; { std::lock_guard<std::mutex> lk(g_state->mtx); g_state->calib_waiting = false; } DestroyWindow(hwnd); g_hwndCalibDlg = nullptr; } break;
    default: return DefWindowProcA(hwnd, msg, wp, lp);
    }
    return 0;
}

// ── Wiring setup popup window ──────────────────────────────────────────────────
// Companion dialog to the wiring wizard (runWiringSetup): shows progress and
// offers Skip (position without a physical button) / Cancel. The actual
// "which button to press" cue is the yellow highlight on the wheel image.
#define ID_WIRING_SKIP 1002
static LRESULT CALLBACK WiringDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateWindowExA(0,"STATIC","Press the YELLOW highlighted button\non the wheel image now...",WS_CHILD|WS_VISIBLE|SS_CENTER,20,15,340,36,hwnd,nullptr,nullptr,nullptr);
        CreateWindowExA(0,"STATIC","",WS_CHILD|WS_VISIBLE|SS_CENTER,20,58,340,26,hwnd,(HMENU)1001,nullptr,nullptr);
        CreateWindowExA(0,"BUTTON","Skip this button",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,55,95,130,30,hwnd,(HMENU)ID_WIRING_SKIP,nullptr,nullptr);
        CreateWindowExA(0,"BUTTON","Cancel (ESC)",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,195,95,130,30,hwnd,(HMENU)IDCANCEL,nullptr,nullptr);
        break;
    case WM_APP_WIRING_NEXT: {
        std::string txt; { std::lock_guard<std::mutex> lk(g_state->mtx); txt = "Button  " + std::to_string(g_state->wiring_pos + 1) + " / " + std::to_string(g_state->wiring_total); }
        SetDlgItemTextA(hwnd, 1001, txt.c_str());
        { std::lock_guard<std::mutex> lk(g_state->mtx); g_state->wiring_waiting = false; }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_WIRING_SKIP) { g_state->wiring_skip = true; }
        if (LOWORD(wp) == IDCANCEL) { g_state->wiring_aborted = true; { std::lock_guard<std::mutex> lk(g_state->mtx); g_state->wiring_waiting = false; } DestroyWindow(hwnd); g_hwndWiringDlg = nullptr; } break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)        { g_state->wiring_aborted = true; { std::lock_guard<std::mutex> lk(g_state->mtx); g_state->wiring_waiting = false; } DestroyWindow(hwnd); g_hwndWiringDlg = nullptr; } break;
    default: return DefWindowProcA(hwnd, msg, wp, lp);
    }
    return 0;
}

// ── Exit confirmation popup ──────────────────────────────────────────────────
// Yes = exit the app, No = cancel (stay open), Minimize = hide to tray instead
// of closing. Unlike CalibDlgProc/WiringDlgProc above (which are modeless and
// driven by a background hardware-wizard thread via *_waiting flags), this
// needs an answer synchronously before WM_CLOSE decides whether to proceed -
// so it runs its own small nested message loop (the same technique the stock
// MessageBoxA already uses internally throughout this codebase), blocking the
// caller until a choice is made.
#define ID_EXITCONFIRM_MINIMIZE 1003
enum ExitConfirmResult { EXITCONFIRM_CANCEL, EXITCONFIRM_EXIT, EXITCONFIRM_MINIMIZE };
static ExitConfirmResult g_exitConfirmResult = EXITCONFIRM_CANCEL;

static LRESULT CALLBACK ExitConfirmDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static HBRUSH s_bgBrush = nullptr;
    switch (msg) {
    case WM_CREATE:
        if (!s_bgBrush) s_bgBrush = CreateSolidBrush(COL_BG);
        CreateWindowExA(0,"STATIC","Do you really want to exit SimRacePro Custom Driver Software?",
            WS_CHILD|WS_VISIBLE|SS_CENTER,15,15,330,40,hwnd,nullptr,nullptr,nullptr);
        CreateWindowExA(0,"BUTTON","Yes",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
            25,70,90,30,hwnd,(HMENU)IDYES,nullptr,nullptr);
        CreateWindowExA(0,"BUTTON","No",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
            135,70,90,30,hwnd,(HMENU)IDNO,nullptr,nullptr);
        CreateWindowExA(0,"BUTTON","Minimize",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
            245,70,90,30,hwnd,(HMENU)ID_EXITCONFIRM_MINIMIZE,nullptr,nullptr);
        break;
    case WM_ERASEBKGND: {
        HDC hdc=(HDC)wp; RECT rc; GetClientRect(hwnd,&rc); FillRect(hdc,&rc,s_bgBrush); return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc=(HDC)wp; SetTextColor(hdc,COL_TEXT); SetBkMode(hdc,TRANSPARENT); return (LRESULT)s_bgBrush;
    }
    case WM_DRAWITEM: {
        // Owner-drawn rounded buttons, same palette as the main window's action
        // buttons (Reset=red, Manual=blue, default=neutral grey) so this popup
        // reads as part of the same app instead of a stock Win32 dialog.
        auto* di=(DRAWITEMSTRUCT*)lp;
        if (di->CtlType!=ODT_BUTTON) break;
        HDC hdc=di->hDC; RECT rc=di->rcItem;
        bool pressed=(di->itemState & ODS_SELECTED)!=0;
        bool isYes = (di->CtlID==IDYES);
        bool isNo  = (di->CtlID==IDNO);
        bool isMin = (di->CtlID==ID_EXITCONFIRM_MINIMIZE);
        COLORREF bgCol = isYes ? (pressed?RGB(28,100,55):RGB(35,120,65))   :
                          isNo  ? (pressed?RGB(120,45,45):RGB(150,55,55))   :
                          isMin ? (pressed?RGB(0,95,155):RGB(0,115,180))    :
                                  (pressed?RGB(34,35,46):RGB(44,46,60));
        COLORREF bdCol = isYes ? RGB(45,170,85) : isNo ? RGB(200,90,90) : isMin ? RGB(0,150,220) : RGB(68,70,88);
        FillRect(hdc,&rc,s_bgBrush);
        HBRUSH bgBr=CreateSolidBrush(bgCol); SelectObject(hdc,bgBr); SelectObject(hdc,(HPEN)GetStockObject(NULL_PEN));
        RoundRect(hdc,rc.left,rc.top,rc.right,rc.bottom,6,6); DeleteObject(bgBr);
        HPEN bp=CreatePen(PS_SOLID,1,bdCol); SelectObject(hdc,bp); SelectObject(hdc,(HBRUSH)GetStockObject(NULL_BRUSH));
        RoundRect(hdc,rc.left,rc.top,rc.right-1,rc.bottom-1,6,6); DeleteObject(bp);
        char raw[32]={}; GetWindowTextA(di->hwndItem,raw,sizeof(raw));
        SetBkMode(hdc,TRANSPARENT); SetTextColor(hdc, pressed?RGB(190,195,205):RGB(220,224,235));
        SelectObject(hdc,g_fontNormal);
        RECT tr={rc.left+4,rc.top+2,rc.right-4,rc.bottom-2};
        DrawTextA(hdc,raw,-1,&tr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp)==IDYES)                        { g_exitConfirmResult=EXITCONFIRM_EXIT;     DestroyWindow(hwnd); }
        else if (LOWORD(wp)==IDNO)                     { g_exitConfirmResult=EXITCONFIRM_CANCEL;   DestroyWindow(hwnd); }
        else if (LOWORD(wp)==ID_EXITCONFIRM_MINIMIZE)  { g_exitConfirmResult=EXITCONFIRM_MINIMIZE; DestroyWindow(hwnd); }
        break;
    case WM_KEYDOWN:
        if (wp==VK_ESCAPE) { g_exitConfirmResult=EXITCONFIRM_CANCEL; DestroyWindow(hwnd); }
        break;
    case WM_CLOSE: g_exitConfirmResult=EXITCONFIRM_CANCEL; DestroyWindow(hwnd); return 0;
    default: return DefWindowProcA(hwnd, msg, wp, lp);
    }
    return 0;
}

// Blocks the calling (GUI) thread until Yes/No/Minimize is chosen. Closing the
// popup via Escape or its own [X] counts as No (cancel), same as a plain
// MessageBox's default-safe behaviour.
static ExitConfirmResult ShowExitConfirmDialog(HWND parent) {
    static bool s_classReg = false;
    if (!s_classReg) {
        WNDCLASSEXA wc={}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=ExitConfirmDlgProc;
        wc.hInstance=GetModuleHandleA(nullptr); wc.hbrBackground=CreateSolidBrush(COL_BG);
        wc.lpszClassName="SimRaceProExitConfirm"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        RegisterClassExA(&wc); s_classReg=true;
    }
    g_exitConfirmResult = EXITCONFIRM_CANCEL;
    int dw=360, dh=140;
    int dx=(GetSystemMetrics(SM_CXSCREEN)-dw)/2, dy=(GetSystemMetrics(SM_CYSCREEN)-dh)/2;
    HWND hDlg = CreateWindowExA(WS_EX_DLGMODALFRAME,"SimRaceProExitConfirm",
        "SimRacePro Custom Driver Software - Exit",
        WS_POPUP|WS_VISIBLE|WS_CAPTION, dx,dy,dw,dh, parent,nullptr,GetModuleHandleA(nullptr),nullptr);
    if (!hDlg) return EXITCONFIRM_CANCEL;
    if (g_fontNormal) {
        EnumChildWindows(hDlg, [](HWND hc, LPARAM lp) -> BOOL {
            SendMessage(hc, WM_SETFONT, (WPARAM)lp, TRUE);
            return TRUE;
        }, (LPARAM)g_fontNormal);
    }
    EnableWindow(parent, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    SetForegroundWindow(hDlg);
    MSG msg;
    while (IsWindow(hDlg) && GetMessageA(&msg, nullptr, 0, 0)) {
        if (msg.hwnd == hDlg || IsChild(hDlg, msg.hwnd)) {
            if (!IsDialogMessageA(hDlg, &msg)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        } else {
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return g_exitConfirmResult;
}

// ── Draw helpers ───────────────────────────────────────────────────────────────
static void DrawSteeringWheel(HDC hdc, int cx, int cy, int r, float angleDeg) {
    HBRUSH bg = CreateSolidBrush(COL_PANEL); HPEN ap = CreatePen(PS_SOLID,2,COL_ACCENT);
    SelectObject(hdc,bg); SelectObject(hdc,ap); Ellipse(hdc,cx-r,cy-r,cx+r,cy+r); DeleteObject(bg);
    HPEN rim = CreatePen(PS_SOLID,8,RGB(80,80,90)); SelectObject(hdc,rim); SelectObject(hdc,GetStockObject(NULL_BRUSH));
    Ellipse(hdc,cx-r+8,cy-r+8,cx+r-8,cy+r-8); DeleteObject(rim);
    int ir = r-20;
    HPEN thin = CreatePen(PS_SOLID,1,RGB(80,80,95)); SelectObject(hdc,thin); MoveToEx(hdc,cx,cy-8,nullptr); LineTo(hdc,cx,cy-ir); DeleteObject(thin);
    HPEN thick = CreatePen(PS_SOLID,3,RGB(190,190,210)); SelectObject(hdc,thick); MoveToEx(hdc,cx,cy-(ir+2),nullptr); LineTo(hdc,cx,cy-(r-9)); DeleteObject(thick);
    double rad = (angleDeg-90.0)*M_PI/180.0;
    for (int s=0;s<3;s++) {
        double a = rad+s*(2.0*M_PI/3.0);
        int x1=cx+(int)(cos(a)*8),y1=cy+(int)(sin(a)*8),x2=cx+(int)(cos(a)*ir),y2=cy+(int)(sin(a)*ir);
        HPEN sp = (s==0) ? CreatePen(PS_SOLID,5,COL_ACCENT) : CreatePen(PS_SOLID,3,RGB(100,100,110));
        SelectObject(hdc,sp); MoveToEx(hdc,x1,y1,nullptr); LineTo(hdc,x2,y2); DeleteObject(sp);
    }
    HBRUSH hub = CreateSolidBrush(COL_ACCENT); SelectObject(hdc,hub); SelectObject(hdc,ap);
    Ellipse(hdc,cx-8,cy-8,cx+8,cy+8); DeleteObject(hub); DeleteObject(ap);
    char buf[32]; sprintf_s(buf,"%.1f deg",angleDeg);
    SetTextColor(hdc,COL_ACCENT); SetBkMode(hdc,TRANSPARENT);
    RECT tr={cx-50,cy+r+5,cx+50,cy+r+25}; DrawTextA(hdc,buf,-1,&tr,DT_CENTER|DT_SINGLELINE);
}

static void DrawBar(HDC hdc, int x, int y, int w, int h, float val, COLORREF col, const char* label) {
    HBRUSH bg = CreateSolidBrush(RGB(55,55,65)); SelectObject(hdc,bg); SelectObject(hdc,(HPEN)GetStockObject(NULL_PEN));
    Rectangle(hdc,x,y,x+w,y+h); DeleteObject(bg);
    int fw=(int)(val*(w-2));
    if (fw>0) { HBRUSH fb=CreateSolidBrush(col); SelectObject(hdc,fb); Rectangle(hdc,x+1,y+1,x+1+fw,y+h-1); DeleteObject(fb); }
    char buf[32]; sprintf_s(buf,"%s  %d%%",label,(int)(val*100));
    SetTextColor(hdc,COL_TEXT); SetBkMode(hdc,TRANSPARENT);
    RECT tr={x+4,y,x+w-4,y+h}; DrawTextA(hdc,buf,-1,&tr,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
}

// Virtual SSD1306 display overlay (mirrors the Arduino sketch layout)
static void DrawSSD1306Overlay(HDC hdc, int ox, int oy, float brk, float thr, float rpmPct, const char* gear, int speed, float steerDeg) {
    ox += DISP_X; oy += DISP_Y;
    COLORREF bg=RGB(10,12,14), pix=RGB(49,222,241);
    HBRUSH bgBr=CreateSolidBrush(bg); SelectObject(hdc,bgBr); SelectObject(hdc,(HPEN)GetStockObject(NULL_PEN));
    Rectangle(hdc,ox,oy,ox+DISP_W,oy+DISP_H); DeleteObject(bgBr);
    HPEN brd=CreatePen(PS_SOLID,1,RGB(45,50,58)); SelectObject(hdc,brd); SelectObject(hdc,(HBRUSH)GetStockObject(NULL_BRUSH));
    Rectangle(hdc,ox,oy,ox+DISP_W,oy+DISP_H); DeleteObject(brd);
    SetBkMode(hdc,TRANSPARENT);
    HFONT fSm   = CreateFontA(9,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI");
    HFONT fGear = CreateFontA(24,0,0,0,FW_BOLD,  0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI");
    HFONT fSpd  = CreateFontA(12,0,0,0,FW_BOLD,  0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI");
    SelectObject(hdc,fSm); SetTextColor(hdc,pix);
    char bb[8],tb[8]; sprintf_s(bb,"B:%d",(int)(brk*100)); sprintf_s(tb,"T:%d",(int)(thr*100));
    { RECT r={ox+1,oy+1,ox+35,oy+9};            DrawTextA(hdc,bb,-1,&r,DT_LEFT|DT_TOP|DT_SINGLELINE); }
    { RECT r={ox+36,oy+1,ox+DISP_W-1,oy+9};     DrawTextA(hdc,tb,-1,&r,DT_RIGHT|DT_TOP|DT_SINGLELINE); }
    // RPM bar
    { HBRUSH tr=CreateSolidBrush(bg); SelectObject(hdc,tr); SelectObject(hdc,(HPEN)GetStockObject(NULL_PEN)); Rectangle(hdc,ox+1,oy+10,ox+DISP_W-1,oy+13); DeleteObject(tr); }
    int bw=(int)(rpmPct*(DISP_W-2));
    if (bw>0) { HBRUSH bb2=(rpmPct<RPM_GREEN)?g_brGreen:(rpmPct<RPM_YELLOW)?g_brYellow:g_brRed; SelectObject(hdc,bb2); SelectObject(hdc,(HPEN)GetStockObject(NULL_PEN)); Rectangle(hdc,ox+1,oy+10,ox+1+bw,oy+13); }  // cached brush
    // Gear
    SelectObject(hdc,fGear); SetTextColor(hdc,(gear[0]=='R'&&gear[1]=='\0')?RGB(220,80,80):pix);
    { RECT r={ox,oy+6,ox+DISP_W,oy+35}; DrawTextA(hdc,gear,-1,&r,DT_CENTER|DT_TOP|DT_SINGLELINE); }
    // Speed + kmh + angle
    SelectObject(hdc,fSpd); SetTextColor(hdc,pix);
    char sb[8]; sprintf_s(sb,"%d",speed);
    { RECT r={ox+1,oy+24,ox+21,oy+36}; DrawTextA(hdc,sb,-1,&r,DT_RIGHT|DT_BOTTOM|DT_SINGLELINE); }
    SelectObject(hdc,fSm); SetTextColor(hdc,pix);
    { RECT r={ox+22,oy+24,ox+40,oy+36}; DrawTextA(hdc,"kmh",-1,&r,DT_LEFT|DT_BOTTOM|DT_SINGLELINE); }
    char ab[10]; sprintf_s(ab,"A:%d",(int)steerDeg);
    { RECT r={ox+40,oy+24,ox+DISP_W-1,oy+36}; DrawTextA(hdc,ab,-1,&r,DT_RIGHT|DT_BOTTOM|DT_SINGLELINE); }
    DeleteObject(fSm); DeleteObject(fGear); DeleteObject(fSpd);
}

// Draw mapped button label overlays on top of the wheel image.
static void DrawBtnOverlays(HDC hdc, int imgOX, int imgOY) {
    SelectObject(hdc, g_fontBtn);
    for (int i = 0; i < 16; i++) {
        bool active = g_btnActive[i];
        bool wiring = (g_wiringHighlight == i);  // wiring wizard: "press this one now"
        const BtnInfo& b = BTN_POS[i];
        int sx = imgOX + b.cx, sy = imgOY + b.cy;
        COLORREF fill = wiring ? COL_YELLOW : active ? COL_ACCENT : RGB(90,90,90);
        HPEN  pen = CreatePen(PS_SOLID, wiring ? 2 : 1, wiring ? RGB(255,255,255) : RGB(0,0,0));
        HBRUSH br = CreateSolidBrush(fill);
        SelectObject(hdc,pen); SelectObject(hdc,br);
        RECT tr;
        if (b.square) { int h=BTN_SQ_SIDE/2; Rectangle(hdc,sx-h,sy-h,sx+h+1,sy+h+1); tr={sx-h,sy-h,sx+h+1,sy+h+1}; }
        else          { int r=BTN_ROUND_D/2;  Ellipse(hdc,sx-r,sy-r,sx+r+1,sy+r+1);   tr={sx-r,sy-r,sx+r+1,sy+r+1}; }
        DeleteObject(pen); DeleteObject(br);
        SetBkMode(hdc,TRANSPARENT); SetTextColor(hdc, wiring ? RGB(30,30,30) : RGB(220,220,220));
        char buf[32]= "-"; if (g_hwndBtnLabels[i]) GetWindowTextA(g_hwndBtnLabels[i],buf,sizeof(buf));
        int wlen = MultiByteToWideChar(CP_UTF8,0,buf,-1,nullptr,0);
        if (wlen>0) {
            std::wstring wb(wlen,0); MultiByteToWideChar(CP_UTF8,0,buf,-1,&wb[0],wlen);
            bool big = (wb[0]==0x29BB||wb[0]==0x2EBA||wb[0]==0x2261);
            if (big) SelectObject(hdc,g_fontBtnLarge);
            DrawTextW(hdc,wb.c_str(),-1,&tr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            if (big) SelectObject(hdc,g_fontBtn);
        }
    }

    // Fixed wheel-reset button: same red as the bmp underneath when idle, glows
    // bright red while held (+800ms hold-off so a tap survives ~60Hz repaints,
    // see GuiState::wheel_reset_tick). See RESET_BTN_* for why it has no BTN_POS
    // entry and therefore no label, no wiring/mapping and no click handling.
    {
        bool lit = (GetTickCount64() - g_state->wheel_reset_tick.load()) < 800;
        int sx = imgOX + RESET_BTN_CX, sy = imgOY + RESET_BTN_CY, r = BTN_ROUND_D/2;
        if (lit) {  // glow ring on top of the silver bezel around the button
            HPEN gp = CreatePen(PS_SOLID, 3, RGB(255,130,115));
            SelectObject(hdc, gp); SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Ellipse(hdc, sx-r-4, sy-r-4, sx+r+5, sy+r+5);
            DeleteObject(gp);
        }
        HPEN  pen = CreatePen(PS_SOLID, 1, RGB(0,0,0));
        HBRUSH br = CreateSolidBrush(lit ? RGB(255,70,55) : RESET_BTN_RED);
        SelectObject(hdc, pen); SelectObject(hdc, br);
        Ellipse(hdc, sx-r, sy-r, sx+r+1, sy+r+1);
        DeleteObject(pen); DeleteObject(br);
    }
}

// ── Systray ────────────────────────────────────────────────────────────────────
static void AddTrayIcon(HWND hwnd) {
    if (g_trayAdded) return;
    g_nid.cbSize=sizeof(NOTIFYICONDATAA); g_nid.hWnd=hwnd; g_nid.uID=1;
    g_nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP; g_nid.uCallbackMessage=WM_TRAYICON; g_nid.hIcon=g_hAppIcon;
    char title[128]={}; GetWindowTextA(hwnd,title,sizeof(title)); strcpy_s(g_nid.szTip,title);
    Shell_NotifyIconA(NIM_ADD,&g_nid); g_trayAdded=true;
}
static void RemoveTrayIcon() { if (!g_trayAdded) return; Shell_NotifyIconA(NIM_DELETE,&g_nid); g_trayAdded=false; }
static void ShowMainWindow(HWND hwnd) { ShowWindow(hwnd,SW_RESTORE); SetForegroundWindow(hwnd); }

// ── Autostart with Windows (HKCU Run key) ───────────────────────────────────────
static const char* AUTOSTART_VALUE_NAME = "SimRacePro";
static bool IsAutostartEnabled() {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                       0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) return false;
    bool exists = (RegQueryValueExA(hk, AUTOSTART_VALUE_NAME, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS);
    RegCloseKey(hk);
    return exists;
}
static void SetAutostartEnabled(bool enable) {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                       0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS) return;
    if (enable) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string quoted = "\"" + std::string(exePath) + "\"";
        RegSetValueExA(hk, AUTOSTART_VALUE_NAME, 0, REG_SZ, (const BYTE*)quoted.c_str(), (DWORD)quoted.size() + 1);
    } else {
        RegDeleteValueA(hk, AUTOSTART_VALUE_NAME);
    }
    RegCloseKey(hk);
}
static void ApplyConsoleVisibility(HWND hwnd) {
    if (g_consoleVisible) {
        if (!g_hwndConsoleDlg || !IsWindow(g_hwndConsoleDlg)) {
            // Neues Top-Level-Konsolenfenster erstellen
            int cx, cy; ComputeAuxWindowPos(hwnd, "console", CON_W, CON_H, cx, cy);
            g_hwndConsoleDlg = CreateWindowExA(
                WS_EX_TOOLWINDOW,
                "SimRaceProConsole", "SimRacePro Custom Driver Software - Console",
                WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
                cx, cy, CON_W, CON_H,
                nullptr, nullptr, g_hInst, nullptr);
            if (g_hwndConsoleDlg) {
                // Log-Edit ins neue Fenster verschieben
                SetParent(g_hwndLog, g_hwndConsoleDlg);
                RECT cr; GetClientRect(g_hwndConsoleDlg, &cr);
                SetWindowPos(g_hwndLog, nullptr, 0, 0, cr.right, cr.bottom,
                             SWP_NOZORDER | SWP_SHOWWINDOW);
                ShowWindow(g_hwndConsoleDlg, SW_SHOW);
            }
        } else {
            ShowWindow(g_hwndConsoleDlg, SW_RESTORE);
            SetForegroundWindow(g_hwndConsoleDlg);
        }
    } else {
        if (g_hwndConsoleDlg && IsWindow(g_hwndConsoleDlg)) {
            ShowWindow(g_hwndConsoleDlg, SW_HIDE);
        }
    }
    SetWindowTextA(g_hwndConsoleBtn, g_consoleVisible ? "Hide\nConsole" : "Show\nConsole");
    InvalidateRect(g_hwndConsoleBtn, nullptr, TRUE);
}


// ── FFB Settings Fenster ────────────────────────────────────────────────────────
// Slider-IDs
#define ID_SLD_SPRING    501
#define ID_SLD_GLAT      502
#define ID_SLD_WRUM_STR  503
#define ID_SLD_WRUM_THR  504
#define ID_SLD_PRUM_STR  505
#define ID_SLD_PRUM_THR  506
#define ID_SLD_ABS_TRQ   507
#define ID_SLD_TC_TRQ    508
#define ID_SLD_SPRING_LIN 509
#define ID_BTN_FFB_SAVE  510
#define ID_BTN_FFB_HELP  511
#define ID_BTN_FFB_RESET 512
#define ID_BTN_BOX_SAVE  520
#define ID_CHK_PEDALRUMBLE 521
#define ID_CHK_HANDBRAKE   522
#define ID_CHK_CLUTCH      523
#define ID_CHK_SHIFTER     524
#define ID_CHK_ONLYWHEEL   525
#define ID_CHK_STALL       526
#define ID_CMB_SIMSETUP    527
#define ID_SLD_MOTORMAX    528
#define ID_SLD_MINPWML     529
#define ID_SLD_MINPWMR     530
#define ID_SLD_RAMPSTEP    531
#define ID_LBL_MOTORMAX    533
#define ID_LBL_MINPWML     534
#define ID_LBL_MINPWMR     535
#define ID_LBL_RAMPSTEP    536
#define ID_BTN_BOX_HELP    537
#define ID_CHK_INVERTSTEER 538
#define ID_CHK_INVERTFFB   539
#define ID_BTN_PINCFG_OPEN 540

// Pin Configuration window - control IDs
#define ID_CMB_PIN_ACC      550
#define ID_CMB_PIN_BRK      551
#define ID_CMB_PIN_VIB      552
#define ID_CMB_PIN_VIB2     553
#define ID_CMB_PIN_CLUTCH   554
#define ID_CMB_PIN_SHIFT_X  555
#define ID_CMB_PIN_SHIFT_Y  556
#define ID_BTN_PINCFG_SAVE  557
#define ID_BTN_PINCFG_RESET 558
#define ID_BTN_PINCFG_HELP  559

static HWND g_sldSpring=nullptr, g_sldGlat=nullptr;
static HWND g_sldSpringStart=nullptr, g_sldSpringFull=nullptr;
static HWND g_lblSpringStart=nullptr, g_lblSpringFull=nullptr;
static HWND g_sldWRumStr=nullptr, g_sldWRumThr=nullptr;
static HWND g_sldAbsStr=nullptr,  g_sldGSpikeThr=nullptr, g_sldGSpikeStr=nullptr, g_sldAbsTrq=nullptr, g_sldTcTrq=nullptr, g_sldSpringLin=nullptr;
static HWND g_lblAbsStr=nullptr,  g_lblGSpikeThr=nullptr, g_lblGSpikeStr=nullptr, g_lblAbsTrq=nullptr, g_lblTcTrq=nullptr, g_lblSpringLin=nullptr;
static HWND g_sldPRumStr=nullptr, g_sldPRumThr=nullptr;
static HWND g_lblSpring=nullptr, g_lblGlat=nullptr;
static HWND g_lblWRumStr=nullptr, g_lblWRumThr=nullptr;
static HWND g_lblPRumStr=nullptr, g_lblPRumThr=nullptr;
static ConfigManager* g_ffbCfgMgr = nullptr;

// Hilfsfunktion: Slider-Wert → Label aktualisieren
static void UpdateFfbLabel(HWND lbl, HWND sld, float scale, const char* suffix) {
    int pos = (int)SendMessage(sld, TBM_GETPOS, 0, 0);
    float val = pos / scale;
    char buf[32]; sprintf_s(buf, "%.2f %s", val, suffix);
    SetWindowTextA(lbl, buf);
}

static LRESULT CALLBACK FfbSettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Statische Brushes fuer dunkles Theme (einmalig erstellt)
    static HBRUSH s_bgBrush    = nullptr;
    static HBRUSH s_panelBrush = nullptr;
    static HBRUSH s_editBrush  = nullptr;
    if (!s_bgBrush) {
        s_bgBrush    = CreateSolidBrush(RGB(30,30,35));
        s_panelBrush = CreateSolidBrush(RGB(40,40,48));
        s_editBrush  = CreateSolidBrush(RGB(50,52,62));
    }

    switch (msg) {
    case WM_CREATE: {
        // Dunklen Hintergrund setzen
        SetClassLongPtrA(hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)s_bgBrush);

        // Fonts - Wide-Char fuer korrekte Umlaute
        HFONT fntNorm = CreateFontW(14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        HFONT fntBold = CreateFontW(14,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");

        // Sektion-Header (Wide fuer Umlaute/Sonderzeichen)
        // ── 2-column layout constants ─────────────────────────────────────────
        // Col 1 (Steering FFB + Buttons): x=12, width=280
        // Col 2 (Wheel + Pedal Vibration): x=308, width=280
        // Row height: 48px per slider, 32px section header
        const int C1X = 12,  CW = 272;
        const int C2X = 304, C2W = 272;
        const int SLD_W = 200, LBL_W = 52, LBL_OFF = 208;
        const int ROW_H = 48, SEC_H = 36;
        const int INNER_PAD = 10;

        // Accent colours for section headers
        HBRUSH brAccent1 = CreateSolidBrush(RGB(0,100,160));   // blue  – Steering
        HBRUSH brAccent2 = CreateSolidBrush(RGB(0,120,80));    // green – Vibration

        // Helper: section header with coloured left-bar
        auto addSection = [&](int x, int y, int w, const wchar_t* title, HBRUSH accentBr) {
            // Accent bar
            HWND bar = CreateWindowExA(0,"STATIC","",WS_CHILD|WS_VISIBLE|SS_OWNERDRAW,
                x,y+2,4,22,hwnd,(HMENU)(INT_PTR)accentBr,g_hInst,nullptr);
            (void)bar;
            // Section label
            HWND h = CreateWindowExW(0,L"STATIC",title,WS_CHILD|WS_VISIBLE,
                x+8,y,w-8,22,hwnd,nullptr,g_hInst,nullptr);
            SendMessage(h,WM_SETFONT,(WPARAM)fntBold,TRUE);
        };

        // Helper: slider row (label top, slider + value below)
        auto addRow = [&](int x, int y, int w, const wchar_t* title,
                          HWND& sld, HWND& lbl, int minV, int maxV, int initV) {
            // Title label
            HWND h = CreateWindowExW(0,L"STATIC",title,WS_CHILD|WS_VISIBLE,
                x,y,w-LBL_W-8,16,hwnd,nullptr,g_hInst,nullptr);
            SendMessage(h,WM_SETFONT,(WPARAM)fntNorm,TRUE);
            // Value label (right-aligned)
            lbl = CreateWindowExA(0,"STATIC","",WS_CHILD|WS_VISIBLE|SS_RIGHT,
                x+w-LBL_W,y,LBL_W,16,hwnd,nullptr,g_hInst,nullptr);
            SendMessage(lbl,WM_SETFONT,(WPARAM)fntBold,TRUE);
            // Slider
            sld = CreateWindowExA(0,TRACKBAR_CLASSA,"",
                WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_NOTICKS,
                x,y+18,w,20,hwnd,nullptr,g_hInst,nullptr);
            SendMessage(sld,TBM_SETRANGE,TRUE,MAKELPARAM(minV,maxV));
            SendMessage(sld,TBM_SETPOS,TRUE,initV);
        };

        // Snapshot once under the lock - avoids reading each field of the
        // live, GUI/backend-shared g_ffbSettings unsynchronized (see 4.5).
        FfbSettings ffb; { std::lock_guard<std::mutex> lk(g_ffbSettingsMutex); ffb = g_ffbSettings; }

        // ── Column 1: Steering FFB ─────────────────────────────────────────────
        int y1 = INNER_PAD;
        addSection(C1X, y1, CW, L"Steering FFB", brAccent1); y1 += SEC_H;
        addRow(C1X, y1, CW, L"Spring Strength",    g_sldSpring,      g_lblSpring,      0,150,(int)(ffb.springStrength*100));   y1+=ROW_H;
        addRow(C1X, y1, CW, L"Start Angle (deg)",  g_sldSpringStart, g_lblSpringStart, 0,90, (int)(ffb.springStartAngle));      y1+=ROW_H;
        addRow(C1X, y1, CW, L"Full Force Angle",   g_sldSpringFull,  g_lblSpringFull,  10,180,(int)(ffb.springFullAngle));      y1+=ROW_H;
        addRow(C1X, y1, CW, L"Spring Linearity",   g_sldSpringLin,   g_lblSpringLin,   0,200,(int)(ffb.springLinearity*100));   y1+=ROW_H;
        addRow(C1X, y1, CW, L"Lateral-G Strength", g_sldGlat,        g_lblGlat,        0,150,(int)(ffb.gLatStrength*100));      y1+=ROW_H;
        addRow(C1X, y1, CW, L"ABS Torque Strength",g_sldAbsTrq,      g_lblAbsTrq,      0,150,(int)(ffb.absTorqueStr*100));      y1+=ROW_H;
        addRow(C1X, y1, CW, L"TC Torque Strength", g_sldTcTrq,       g_lblTcTrq,       0,150,(int)(ffb.tcTorqueStr*100));       y1+=ROW_H+8;

        // Buttons below col 1
        int btnY = y1;
        HWND btnSave = CreateWindowExA(0,"BUTTON","Save",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            C1X,btnY,80,28,hwnd,(HMENU)ID_BTN_FFB_SAVE,g_hInst,nullptr);
        SendMessage(btnSave,WM_SETFONT,(WPARAM)fntNorm,TRUE);
        HWND btnHelp = CreateWindowExA(0,"BUTTON","? Help",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            C1X+88,btnY,80,28,hwnd,(HMENU)ID_BTN_FFB_HELP,g_hInst,nullptr);
        SendMessage(btnHelp,WM_SETFONT,(WPARAM)fntNorm,TRUE);
        HWND btnReset = CreateWindowExA(0,"BUTTON","Reset",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            C1X+176,btnY,CW-176,28,hwnd,(HMENU)ID_BTN_FFB_RESET,g_hInst,nullptr);
        SendMessage(btnReset,WM_SETFONT,(WPARAM)fntNorm,TRUE);

        // ── Column 2: Wheel Vibration ──────────────────────────────────────────
        int y2 = INNER_PAD;
        addSection(C2X, y2, C2W, L"Wheel Vibration", brAccent2); y2 += SEC_H;
        addRow(C2X, y2, C2W, L"Overall Intensity",  g_sldWRumStr,   g_lblWRumStr,   0,150,(int)(ffb.wheelRumbleStr*100)); y2+=ROW_H;
        addRow(C2X, y2, C2W, L"Surface Threshold",  g_sldWRumThr,   g_lblWRumThr,   0,100,(int)(ffb.wheelRumbleThr*100)); y2+=ROW_H;
        addRow(C2X, y2, C2W, L"ABS Strength",       g_sldAbsStr,    g_lblAbsStr,    0,150,(int)(ffb.absStrength*100));    y2+=ROW_H;
        addRow(C2X, y2, C2W, L"G-Spike Threshold",  g_sldGSpikeThr, g_lblGSpikeThr, 50,300,(int)(ffb.gSpikeThreshold*100)); y2+=ROW_H;
        addRow(C2X, y2, C2W, L"G-Spike Strength",   g_sldGSpikeStr, g_lblGSpikeStr, 0,150,(int)(ffb.gSpikeStrength*100));   y2+=ROW_H+16;

        addSection(C2X, y2, C2W, L"Pedal Vibration", brAccent2); y2 += SEC_H;
        addRow(C2X, y2, C2W, L"Intensity",  g_sldPRumStr, g_lblPRumStr, 0,150,(int)(ffb.pedalRumbleStr*100)); y2+=ROW_H;
        addRow(C2X, y2, C2W, L"Threshold",  g_sldPRumThr, g_lblPRumThr, 0,255,(int)(ffb.pedalRumbleThr));     y2+=ROW_H;

        // ── Init all value labels ──────────────────────────────────────────────
        UpdateFfbLabel(g_lblSpring,      g_sldSpring,      100.0f, "");
        UpdateFfbLabel(g_lblSpringStart, g_sldSpringStart, 1.0f,   "");
        UpdateFfbLabel(g_lblSpringFull,  g_sldSpringFull,  1.0f,   "");
        UpdateFfbLabel(g_lblSpringLin,   g_sldSpringLin,   100.0f, "");
        UpdateFfbLabel(g_lblGlat,        g_sldGlat,        100.0f, "");
        UpdateFfbLabel(g_lblAbsTrq,      g_sldAbsTrq,      100.0f, "");
        UpdateFfbLabel(g_lblTcTrq,       g_sldTcTrq,       100.0f, "");
        UpdateFfbLabel(g_lblWRumStr,     g_sldWRumStr,     100.0f, "");
        UpdateFfbLabel(g_lblWRumThr,     g_sldWRumThr,     100.0f, "");
        UpdateFfbLabel(g_lblAbsStr,      g_sldAbsStr,      100.0f, "");
        UpdateFfbLabel(g_lblGSpikeThr,   g_sldGSpikeThr,   100.0f, "");
        UpdateFfbLabel(g_lblGSpikeStr,   g_sldGSpikeStr,   100.0f, "");
        UpdateFfbLabel(g_lblPRumStr,     g_sldPRumStr,     100.0f, "");
        UpdateFfbLabel(g_lblPRumThr,     g_sldPRumThr,     1.0f,   "");
        break;
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, s_bgBrush);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, RGB(220,220,220));
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)s_bgBrush;
    }
    case WM_CTLCOLORBTN: {
        // "Save"-Button: Standard-Theme reicht, aber Hintergrund anpassen
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)s_panelBrush;
    }
    case WM_HSCROLL: {
        auto upd = [&](HWND sld, HWND lbl, float sc) {
            if ((HWND)lp == sld) UpdateFfbLabel(lbl, sld, sc, "");
        };
        upd(g_sldSpring,      g_lblSpring,      100.0f);
        upd(g_sldSpringStart, g_lblSpringStart, 1.0f);
        upd(g_sldSpringFull,  g_lblSpringFull,  1.0f);
        upd(g_sldGlat,        g_lblGlat,        100.0f);
        upd(g_sldSpringLin,   g_lblSpringLin,   100.0f);
        upd(g_sldAbsTrq,      g_lblAbsTrq,      100.0f);
        upd(g_sldTcTrq,       g_lblTcTrq,       100.0f);
        upd(g_sldWRumStr,   g_lblWRumStr,   100.0f);
        upd(g_sldWRumThr,   g_lblWRumThr,   100.0f);
        upd(g_sldAbsStr,    g_lblAbsStr,    100.0f);
        upd(g_sldGSpikeThr, g_lblGSpikeThr, 100.0f);
        upd(g_sldGSpikeStr, g_lblGSpikeStr, 100.0f);
        upd(g_sldPRumStr, g_lblPRumStr, 100.0f);
        upd(g_sldPRumThr, g_lblPRumThr, 1.0f);
        {
            std::lock_guard<std::mutex> lk(g_ffbSettingsMutex);
            g_ffbSettings.springStrength    = SendMessage(g_sldSpring,      TBM_GETPOS,0,0) / 100.0f;
            g_ffbSettings.springStartAngle  = (float)SendMessage(g_sldSpringStart, TBM_GETPOS,0,0);
            g_ffbSettings.springFullAngle   = (float)SendMessage(g_sldSpringFull,  TBM_GETPOS,0,0);
            // Plausibility: Full Force Angle must be greater than Start Angle.
            // Clamp silently to avoid inverting the spring curve in computeTorque.
            if (g_ffbSettings.springFullAngle <= g_ffbSettings.springStartAngle)
                g_ffbSettings.springFullAngle = g_ffbSettings.springStartAngle + 5.0f;
            g_ffbSettings.gLatStrength      = SendMessage(g_sldGlat,   TBM_GETPOS,0,0)  / 100.0f;
            g_ffbSettings.absTorqueStr      = SendMessage(g_sldAbsTrq,    TBM_GETPOS,0,0) / 100.0f;
            g_ffbSettings.tcTorqueStr       = SendMessage(g_sldTcTrq,     TBM_GETPOS,0,0) / 100.0f;
            g_ffbSettings.springLinearity   = SendMessage(g_sldSpringLin, TBM_GETPOS,0,0) / 100.0f;
            g_ffbSettings.wheelRumbleStr  = SendMessage(g_sldWRumStr,   TBM_GETPOS,0,0) / 100.0f;
            g_ffbSettings.wheelRumbleThr  = SendMessage(g_sldWRumThr,   TBM_GETPOS,0,0) / 100.0f;
            g_ffbSettings.absStrength     = SendMessage(g_sldAbsStr,    TBM_GETPOS,0,0) / 100.0f;
            g_ffbSettings.gSpikeThreshold = SendMessage(g_sldGSpikeThr, TBM_GETPOS,0,0) / 100.0f;
            g_ffbSettings.gSpikeStrength  = SendMessage(g_sldGSpikeStr, TBM_GETPOS,0,0) / 100.0f;
            g_ffbSettings.pedalRumbleStr  = SendMessage(g_sldPRumStr,TBM_GETPOS,0,0)  / 100.0f;
            g_ffbSettings.pedalRumbleThr  = (float)SendMessage(g_sldPRumThr,TBM_GETPOS,0,0);
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN_FFB_SAVE && g_ffbCfgMgr) {
            { std::lock_guard<std::mutex> lk(g_ffbSettingsMutex); g_ffbCfgMgr->saveFfbSettings(g_ffbSettings); }
            if (g_state) g_state->resend_spring_cfg = true;
            MessageBoxA(hwnd, "FFB settings saved.", "SimRacePro Custom Driver Software", MB_OK|MB_ICONINFORMATION);
        }
        if (LOWORD(wp) == ID_BTN_FFB_RESET) {
            if (MessageBoxA(hwnd,
                "Reset all FFB settings to their default values?",
                "SimRacePro Custom Driver Software - Reset FFB",
                MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                SendMessage(g_sldSpring,      TBM_SETPOS, TRUE, 100);
                SendMessage(g_sldSpringStart, TBM_SETPOS, TRUE,   5);
                SendMessage(g_sldSpringFull,  TBM_SETPOS, TRUE,  90);
                SendMessage(g_sldGlat,        TBM_SETPOS, TRUE, 100);
                SendMessage(g_sldSpringLin,   TBM_SETPOS, TRUE, 100);
                SendMessage(g_sldAbsTrq,      TBM_SETPOS, TRUE,  50);
                SendMessage(g_sldTcTrq,       TBM_SETPOS, TRUE,  40);
                SendMessage(g_sldWRumStr,     TBM_SETPOS, TRUE, 100);
                SendMessage(g_sldWRumThr,     TBM_SETPOS, TRUE,  15);
                SendMessage(g_sldAbsStr,      TBM_SETPOS, TRUE, 100);
                SendMessage(g_sldGSpikeThr,   TBM_SETPOS, TRUE, 150);
                SendMessage(g_sldGSpikeStr,   TBM_SETPOS, TRUE, 100);
                SendMessage(g_sldPRumStr,     TBM_SETPOS, TRUE, 100);
                SendMessage(g_sldPRumThr,     TBM_SETPOS, TRUE,  60);
                UpdateFfbLabel(g_lblSpring,      g_sldSpring,      100.0f, "");
                UpdateFfbLabel(g_lblSpringStart, g_sldSpringStart,   1.0f, "");
                UpdateFfbLabel(g_lblSpringFull,  g_sldSpringFull,    1.0f, "");
                UpdateFfbLabel(g_lblGlat,        g_sldGlat,        100.0f, "");
                UpdateFfbLabel(g_lblSpringLin,   g_sldSpringLin,   100.0f, "");
                UpdateFfbLabel(g_lblAbsTrq,      g_sldAbsTrq,      100.0f, "");
                UpdateFfbLabel(g_lblTcTrq,       g_sldTcTrq,       100.0f, "");
                UpdateFfbLabel(g_lblWRumStr,     g_sldWRumStr,     100.0f, "");
                UpdateFfbLabel(g_lblWRumThr,     g_sldWRumThr,     100.0f, "");
                UpdateFfbLabel(g_lblAbsStr,      g_sldAbsStr,      100.0f, "");
                UpdateFfbLabel(g_lblGSpikeThr,   g_sldGSpikeThr,   100.0f, "");
                UpdateFfbLabel(g_lblGSpikeStr,   g_sldGSpikeStr,   100.0f, "");
                UpdateFfbLabel(g_lblPRumStr,     g_sldPRumStr,     100.0f, "");
                UpdateFfbLabel(g_lblPRumThr,     g_sldPRumThr,       1.0f, "");
                { std::lock_guard<std::mutex> lk(g_ffbSettingsMutex); g_ffbSettings = FfbSettings{}; }
                if (g_state) g_state->resend_spring_cfg = true;
            }
        }
        if (LOWORD(wp) == ID_BTN_FFB_HELP && g_state && g_state->hwnd) {
            OpenManualDialogOnTab(g_state->hwnd, 1);
        }
        break;
    case WM_EXITSIZEMOVE:
        SaveAuxWindowPos(hwnd, "ffb");
        break;
    case WM_CLOSE:
        g_hwndBoxPanel = nullptr;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ── Box Settings Panel ───────────────────────────────────────────────────────
static LRESULT CALLBACK BoxSettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        int y = 10;
        // Hardware tier
        CreateWindowExA(0,"STATIC","Hardware Setup:",WS_CHILD|WS_VISIBLE,
            10,y,150,20,hwnd,nullptr,g_hInst,nullptr); y+=22;
        HWND hCmb=CreateWindowExA(0,"COMBOBOX","",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,
            10,y,200,80,hwnd,(HMENU)ID_CMB_SIMSETUP,g_hInst,nullptr);
        SendMessageA(hCmb,CB_ADDSTRING,0,(LPARAM)"BOX_FULL  (Encoder + Motor)");
        SendMessageA(hCmb,CB_ADDSTRING,0,(LPARAM)"BOX_MEDIUM (Encoder, no Motor)");
        SendMessageA(hCmb,CB_ADDSTRING,0,(LPARAM)"BOX_BUDGET (no Encoder, no Motor)");
        SendMessageA(hCmb,CB_SETCURSEL,g_boxSettings.simSetup,0); y+=30;
        // Hardware flags
        CreateWindowExA(0,"STATIC","Optional Hardware:",WS_CHILD|WS_VISIBLE,
            10,y,200,20,hwnd,nullptr,g_hInst,nullptr); y+=22;
        HWND hChk;
        hChk=CreateWindowExA(0,"BUTTON","Pedal Vibration Motors",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            10,y,220,20,hwnd,(HMENU)ID_CHK_PEDALRUMBLE,g_hInst,nullptr);
        SendMessage(hChk,BM_SETCHECK,g_boxSettings.hasPedalRumble?BST_CHECKED:BST_UNCHECKED,0); y+=24;
        hChk=CreateWindowExA(0,"BUTTON","Handbrake (Pin 4)",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            10,y,220,20,hwnd,(HMENU)ID_CHK_HANDBRAKE,g_hInst,nullptr);
        SendMessage(hChk,BM_SETCHECK,g_boxSettings.hasHandbrake?BST_CHECKED:BST_UNCHECKED,0); y+=24;
        hChk=CreateWindowExA(0,"BUTTON","3rd Pedal / Clutch",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            10,y,220,20,hwnd,(HMENU)ID_CHK_CLUTCH,g_hInst,nullptr);
        SendMessage(hChk,BM_SETCHECK,g_boxSettings.hasClutch?BST_CHECKED:BST_UNCHECKED,0); y+=24;
        hChk=CreateWindowExA(0,"BUTTON","Manual Shifter (H-Pattern)",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            10,y,220,20,hwnd,(HMENU)ID_CHK_SHIFTER,g_hInst,nullptr);
        SendMessage(hChk,BM_SETCHECK,g_boxSettings.hasShifter?BST_CHECKED:BST_UNCHECKED,0); y+=24;
        hChk=CreateWindowExA(0,"BUTTON","Wheel Only (ignore pedals)",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            10,y,220,20,hwnd,(HMENU)ID_CHK_ONLYWHEEL,g_hInst,nullptr);
        SendMessage(hChk,BM_SETCHECK,g_boxSettings.onlyWheel?BST_CHECKED:BST_UNCHECKED,0); y+=24;
        hChk=CreateWindowExA(0,"BUTTON","Invert Steering",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            10,y,220,20,hwnd,(HMENU)ID_CHK_INVERTSTEER,g_hInst,nullptr);
        SendMessage(hChk,BM_SETCHECK,g_boxSettings.invertSteering?BST_CHECKED:BST_UNCHECKED,0); y+=24;
        hChk=CreateWindowExA(0,"BUTTON","Invert Force Feedback",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            10,y,220,20,hwnd,(HMENU)ID_CHK_INVERTFFB,g_hInst,nullptr);
        SendMessage(hChk,BM_SETCHECK,g_boxSettings.invertForceFeedback?BST_CHECKED:BST_UNCHECKED,0); y+=30;
        // Motor settings
        CreateWindowExA(0,"STATIC","Motor Settings:",WS_CHILD|WS_VISIBLE,
            10,y,200,20,hwnd,nullptr,g_hInst,nullptr); y+=22;
        auto makeSlider = [&](int id, int lblId, int lo, int hi, int val, const char* lbl) {
            char buf[32]; sprintf_s(buf,"%s: %d",lbl,val);
            CreateWindowExA(0,"STATIC",buf,WS_CHILD|WS_VISIBLE,10,y,200,18,hwnd,(HMENU)(UINT_PTR)lblId,g_hInst,nullptr);
            HWND hs=CreateWindowExA(0,TRACKBAR_CLASSA,"",WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_NOTICKS,
                210,y,260,20,hwnd,(HMENU)(UINT_PTR)id,g_hInst,nullptr);
            SendMessage(hs,TBM_SETRANGE,TRUE,MAKELONG(lo,hi));
            SendMessage(hs,TBM_SETPOS,TRUE,val); y+=26;
        };
        makeSlider(ID_SLD_MOTORMAX, ID_LBL_MOTORMAX, 50,255,g_boxSettings.motorMaxPwm,  "Max PWM");
        makeSlider(ID_SLD_MINPWML,  ID_LBL_MINPWML,  0,100, g_boxSettings.motorMinPwmLeft,"Min PWM Left");
        makeSlider(ID_SLD_MINPWMR,  ID_LBL_MINPWMR,  0,100, g_boxSettings.motorMinPwmRight,"Min PWM Right");
        makeSlider(ID_SLD_RAMPSTEP, ID_LBL_RAMPSTEP, 1,50,  g_boxSettings.softRampStep,  "Ramp Step");
        hChk=CreateWindowExA(0,"BUTTON","Stall Protection",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            10,y,220,20,hwnd,(HMENU)ID_CHK_STALL,g_hInst,nullptr);
        SendMessage(hChk,BM_SETCHECK,g_boxSettings.stallProtection?BST_CHECKED:BST_UNCHECKED,0); y+=30;
        CreateWindowExA(0,"BUTTON","Save & Send to Box",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            10,y,160,28,hwnd,(HMENU)ID_BTN_BOX_SAVE,g_hInst,nullptr);
        CreateWindowExA(0,"BUTTON","? Help",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            180,y,80,28,hwnd,(HMENU)ID_BTN_BOX_HELP,g_hInst,nullptr);
        y += 34;
        CreateWindowExA(0,"BUTTON","Pin Configuration...",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            10,y,250,28,hwnd,(HMENU)ID_BTN_PINCFG_OPEN,g_hInst,nullptr);

        // Apply app font (Segoe UI 15) to all child controls
        if (g_fontNormal) {
            EnumChildWindows(hwnd, [](HWND hc, LPARAM lp) -> BOOL {
                SendMessage(hc, WM_SETFONT, (WPARAM)lp, TRUE);
                return TRUE;
            }, (LPARAM)g_fontNormal);
        }
        break;
    }
    case WM_HSCROLL: {
        // Update slider labels
        auto upd = [&](int sldId, int lblId, const char* lbl) {
            HWND hs = GetDlgItem(hwnd, sldId);
            int v = (int)SendMessage(hs, TBM_GETPOS, 0, 0);
            char buf[32]; sprintf_s(buf, "%s: %d", lbl, v);
            SetDlgItemTextA(hwnd, lblId, buf);
        };
        upd(ID_SLD_MOTORMAX, ID_LBL_MOTORMAX, "Max PWM");
        upd(ID_SLD_MINPWML,  ID_LBL_MINPWML,  "Min PWM Left");
        upd(ID_SLD_MINPWMR,  ID_LBL_MINPWMR,  "Min PWM Right");
        upd(ID_SLD_RAMPSTEP, ID_LBL_RAMPSTEP, "Ramp Step");
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN_BOX_HELP) {
            // Open Manual on Box Hardware Settings tab (index 1)
            OpenManualDialogOnTab(g_state ? g_state->hwnd : nullptr, 1);
            break;
        }
        if (LOWORD(wp) == ID_BTN_PINCFG_OPEN) {
            OpenPinConfigWindow(hwnd, g_ffbCfgMgr);
            break;
        }
        if (LOWORD(wp) == ID_BTN_BOX_SAVE && g_state && g_ffbCfgMgr) {
            g_boxSettings.simSetup    = (uint8_t)SendDlgItemMessageA(hwnd,ID_CMB_SIMSETUP,CB_GETCURSEL,0,0);
            g_boxSettings.hasPedalRumble = SendDlgItemMessage(hwnd,ID_CHK_PEDALRUMBLE,BM_GETCHECK,0,0)==BST_CHECKED;
            g_boxSettings.hasHandbrake   = SendDlgItemMessage(hwnd,ID_CHK_HANDBRAKE,  BM_GETCHECK,0,0)==BST_CHECKED;
            g_boxSettings.hasClutch      = SendDlgItemMessage(hwnd,ID_CHK_CLUTCH,     BM_GETCHECK,0,0)==BST_CHECKED;
            g_boxSettings.hasShifter     = SendDlgItemMessage(hwnd,ID_CHK_SHIFTER,    BM_GETCHECK,0,0)==BST_CHECKED;
            g_boxSettings.onlyWheel      = SendDlgItemMessage(hwnd,ID_CHK_ONLYWHEEL,  BM_GETCHECK,0,0)==BST_CHECKED;
            g_boxSettings.invertSteering = SendDlgItemMessage(hwnd,ID_CHK_INVERTSTEER,BM_GETCHECK,0,0)==BST_CHECKED;
            g_boxSettings.invertForceFeedback = SendDlgItemMessage(hwnd,ID_CHK_INVERTFFB,BM_GETCHECK,0,0)==BST_CHECKED;
            g_boxSettings.stallProtection= SendDlgItemMessage(hwnd,ID_CHK_STALL,      BM_GETCHECK,0,0)==BST_CHECKED;
            g_boxSettings.motorMaxPwm    = (uint8_t)SendDlgItemMessage(hwnd,ID_SLD_MOTORMAX,TBM_GETPOS,0,0);
            g_boxSettings.motorMinPwmLeft= (uint8_t)SendDlgItemMessage(hwnd,ID_SLD_MINPWML, TBM_GETPOS,0,0);
            g_boxSettings.motorMinPwmRight=(uint8_t)SendDlgItemMessage(hwnd,ID_SLD_MINPWMR, TBM_GETPOS,0,0);
            g_boxSettings.softRampStep   = (uint8_t)SendDlgItemMessage(hwnd,ID_SLD_RAMPSTEP,TBM_GETPOS,0,0);
            // Update GuiState optional hardware flags
            if (g_state) {
                g_state->has_clutch    = g_boxSettings.hasClutch;
                g_state->has_handbrake = g_boxSettings.hasHandbrake;
                g_state->has_shifter   = g_boxSettings.hasShifter;
            }
            g_ffbCfgMgr->saveBoxSettings(g_boxSettings);
            if (g_state) g_state->resend_box_settings = true;
            MessageBoxA(hwnd, "Box settings saved and sent to Box.\n"
                "Settings are stored in EEPROM on the Box and\n"
                "will be applied automatically on next power-on.",
                "SimRacePro - Box Settings", MB_OK|MB_ICONINFORMATION);
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        SetTextColor((HDC)wp, RGB(220,220,220));
        SetBkColor((HDC)wp, RGB(30,30,35));
        static HBRUSH hBgBr = CreateSolidBrush(RGB(30,30,35));
        return (LRESULT)hBgBr;
    }
    case WM_EXITSIZEMOVE:
        SaveAuxWindowPos(hwnd, "boxhw");
        break;
    case WM_CLOSE:
        g_hwndBoxPanel = nullptr;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void OpenFfbSettingsWindow(HWND parent, ConfigManager* cfg) {
    g_ffbCfgMgr = cfg;
    if (!g_hwndFfbDlg || !IsWindow(g_hwndFfbDlg)) {
        // Fensterklasse registrieren
        WNDCLASSEXA wc = {};
        wc.cbSize=sizeof(wc); wc.lpfnWndProc=FfbSettingsProc;
        wc.hInstance=g_hInst; wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName="SimRaceProFfbSettings"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        RegisterClassExA(&wc);

        int ffbW = 600, ffbH = 470;
        int cx, cy; ComputeAuxWindowPos(parent, "ffb", ffbW, ffbH, cx, cy);
        g_hwndFfbDlg = CreateWindowExA(
            WS_EX_TOOLWINDOW,
            "SimRaceProFfbSettings", "SimRacePro Custom Driver Software - Settings",
            WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
            cx, cy, ffbW, ffbH,
            nullptr, nullptr, g_hInst, nullptr);

    }
    if (g_hwndFfbDlg) {
        ShowWindow(g_hwndFfbDlg, SW_SHOW);
        SetForegroundWindow(g_hwndFfbDlg);
    }
}

// ── WndProc ────────────────────────────────────────────────────────────────────
static void OpenBoxSettingsWindow(HWND parent, ConfigManager* cfg) {
    g_ffbCfgMgr = cfg;
    if (!g_hwndBoxPanel || !IsWindow(g_hwndBoxPanel)) {
        static bool s_boxReg = false;
        if (!s_boxReg) {
            WNDCLASSEXA wc = {};
            wc.cbSize=sizeof(wc); wc.lpfnWndProc=BoxSettingsProc;
            wc.hInstance=g_hInst; wc.hbrBackground=CreateSolidBrush(RGB(30,30,35));
            wc.lpszClassName="SimRaceProBoxSettings"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
            RegisterClassExA(&wc);
            s_boxReg = true;
        }
        int bW = 500, bH = 565;
        int cx, cy; ComputeAuxWindowPos(parent, "boxhw", bW, bH, cx, cy);
        g_hwndBoxPanel = CreateWindowExA(
            WS_EX_TOOLWINDOW,
            "SimRaceProBoxSettings", "SimRacePro - Box Hardware Settings",
            WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
            cx, cy, bW, bH,
            nullptr, nullptr, g_hInst, nullptr);
    }
    if (g_hwndBoxPanel) {
        ShowWindow(g_hwndBoxPanel, SW_SHOW);
        SetForegroundWindow(g_hwndBoxPanel);
    }
}

// ── Pin Configuration window ────────────────────────────────────────────────
// Lets the user remap the Box's 7 non-critical peripheral pins (throttle,
// brake, 2x pedal rumble, clutch, shifter X/Y) to correct wiring mistakes in
// software instead of re-soldering. Deliberately excludes the encoder, motor
// and Box<->Wheel link pins (hardware-constrained, see PinConfig in the box
// firmware) and the handbrake pin (only candidate on a non-analog pin, not
// worth the complexity - see plan notes).
struct PinChoice { const char* label; uint8_t value; };
#define PINCFG_SENTINEL 0xFF
static const PinChoice PINCFG_ANALOG[8] = {
    {"A0",14},{"A1",15},{"A2",16},{"A3",17},{"A4",18},{"A5",19},{"A6",20},{"A7",21}
};
// D4 is deliberately absent - permanently reserved by the hardcoded handbrake pin.
static const PinChoice PINCFG_DIGITAL[11] = {
    {"D7",7},{"D12",12},{"D13",13},
    {"A0",14},{"A1",15},{"A2",16},{"A3",17},{"A4",18},{"A5",19},{"A6",20},{"A7",21}
};
struct PinFieldDef { const char* label; int comboId; bool analog; uint8_t defaultVal; };
// Order matches BoxPinConfig's field order AND the ID_CMB_PIN_* ID sequence
// (fieldIdx = comboId - ID_CMB_PIN_ACC relies on this).
static const PinFieldDef PINCFG_FIELDS[7] = {
    { "Throttle Pedal",       ID_CMB_PIN_ACC,     true,  14 },
    { "Brake Pedal",          ID_CMB_PIN_BRK,     true,  15 },
    { "Pedal Rumble Motor 1", ID_CMB_PIN_VIB,     false, 16 },
    { "Pedal Rumble Motor 2", ID_CMB_PIN_VIB2,    false, 17 },
    { "Clutch Pedal",         ID_CMB_PIN_CLUTCH,  true,  18 },
    { "Shifter X-Axis",       ID_CMB_PIN_SHIFT_X, true,  21 },
    { "Shifter Y-Axis",       ID_CMB_PIN_SHIFT_Y, true,  20 },
};
// Tracks each combo's last accepted (non-conflicting) selection index, so a
// declined overwrite ("No") can revert the just-changed combo. Single-instance
// window (like BoxSettingsProc), reset fresh in WM_CREATE on every (re)open.
static int g_pinPrevSel[7] = { 0,0,0,0,0,0,0 };

static std::string PinLabelFor(uint8_t val) {
    for (const auto& c : PINCFG_DIGITAL) if (c.value == val) return c.label;
    return "?";
}

static LRESULT CALLBACK PinConfigProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        int y = 10;
        CreateWindowExA(0,"STATIC","Remap peripheral pins to fix wiring mistakes without re-soldering.",
            WS_CHILD|WS_VISIBLE,10,y,450,20,hwnd,nullptr,g_hInst,nullptr); y+=28;

        uint8_t curVals[7] = { g_boxPinConfig.accPin, g_boxPinConfig.brkPin, g_boxPinConfig.vibPin,
                                g_boxPinConfig.vib2Pin, g_boxPinConfig.clutchPin,
                                g_boxPinConfig.shifterXPin, g_boxPinConfig.shifterYPin };
        for (int i = 0; i < 7; i++) {
            const PinFieldDef& fd = PINCFG_FIELDS[i];
            char lbl[64]; sprintf_s(lbl, "%s (%s):", fd.label, fd.analog ? "Analog" : "Digital");
            CreateWindowExA(0,"STATIC",lbl,WS_CHILD|WS_VISIBLE,
                10,y,230,20,hwnd,nullptr,g_hInst,nullptr);
            HWND hCmb = CreateWindowExA(0,"COMBOBOX","",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
                250,y,120,150,hwnd,(HMENU)(UINT_PTR)fd.comboId,g_hInst,nullptr);
            const PinChoice* list = fd.analog ? PINCFG_ANALOG : PINCFG_DIGITAL;
            int n = fd.analog ? 8 : 11;
            int selIdx = 0;
            for (int j = 0; j < n; j++) {
                int idx = (int)SendMessageA(hCmb, CB_ADDSTRING, 0, (LPARAM)list[j].label);
                SendMessageA(hCmb, CB_SETITEMDATA, idx, (LPARAM)list[j].value);
                if (list[j].value == curVals[i]) selIdx = idx;
            }
            SendMessageA(hCmb, CB_SETCURSEL, selIdx, 0);
            g_pinPrevSel[i] = selIdx;
            y += 30;
        }

        y += 10;
        CreateWindowExA(0,"BUTTON","Save & Send to Box",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            10,y,160,28,hwnd,(HMENU)ID_BTN_PINCFG_SAVE,g_hInst,nullptr);
        CreateWindowExA(0,"BUTTON","Reset to Defaults",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            180,y,140,28,hwnd,(HMENU)ID_BTN_PINCFG_RESET,g_hInst,nullptr);
        CreateWindowExA(0,"BUTTON","? Help",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            330,y,80,28,hwnd,(HMENU)ID_BTN_PINCFG_HELP,g_hInst,nullptr);

        if (g_fontNormal) {
            EnumChildWindows(hwnd, [](HWND hc, LPARAM lp) -> BOOL {
                SendMessage(hc, WM_SETFONT, (WPARAM)lp, TRUE);
                return TRUE;
            }, (LPARAM)g_fontNormal);
        }
        break;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == CBN_SELCHANGE) {
            int changedId = LOWORD(wp);
            if (changedId >= ID_CMB_PIN_ACC && changedId <= ID_CMB_PIN_SHIFT_Y) {
                int fieldIdx = changedId - ID_CMB_PIN_ACC;
                HWND hCmbChanged = GetDlgItem(hwnd, changedId);
                int newSel = (int)SendMessageA(hCmbChanged, CB_GETCURSEL, 0, 0);
                uint8_t newVal = (uint8_t)SendMessageA(hCmbChanged, CB_GETITEMDATA, newSel, 0);

                int conflictField = -1;
                if (newVal != PINCFG_SENTINEL) {
                    for (int i = 0; i < 7; i++) {
                        if (i == fieldIdx) continue;
                        HWND hOther = GetDlgItem(hwnd, PINCFG_FIELDS[i].comboId);
                        int otherSel = (int)SendMessageA(hOther, CB_GETCURSEL, 0, 0);
                        uint8_t otherVal = (uint8_t)SendMessageA(hOther, CB_GETITEMDATA, otherSel, 0);
                        if (otherVal == newVal) { conflictField = i; break; }
                    }
                }

                if (conflictField >= 0) {
                    std::string msg = "'" + PinLabelFor(newVal) + "' is already assigned to '" +
                        PINCFG_FIELDS[conflictField].label + "'.\n\nOverwrite? '" +
                        PINCFG_FIELDS[conflictField].label + "' will be cleared.";
                    if (MessageBoxA(hwnd, msg.c_str(), "SimRacePro Custom Driver Software - Pin Configuration",
                                    MB_YESNO|MB_ICONQUESTION) == IDYES) {
                        HWND hOther = GetDlgItem(hwnd, PINCFG_FIELDS[conflictField].comboId);
                        int cnt = (int)SendMessageA(hOther, CB_GETCOUNT, 0, 0);
                        int unassignedIdx = -1;
                        for (int k = 0; k < cnt; k++) {
                            if ((uint8_t)SendMessageA(hOther, CB_GETITEMDATA, k, 0) == PINCFG_SENTINEL) { unassignedIdx = k; break; }
                        }
                        if (unassignedIdx < 0) {
                            unassignedIdx = (int)SendMessageA(hOther, CB_ADDSTRING, 0, (LPARAM)"- not assigned -");
                            SendMessageA(hOther, CB_SETITEMDATA, unassignedIdx, (LPARAM)PINCFG_SENTINEL);
                        }
                        SendMessageA(hOther, CB_SETCURSEL, unassignedIdx, 0);
                        g_pinPrevSel[conflictField] = unassignedIdx;
                        g_pinPrevSel[fieldIdx] = newSel;
                    } else {
                        SendMessageA(hCmbChanged, CB_SETCURSEL, g_pinPrevSel[fieldIdx], 0);
                    }
                } else {
                    g_pinPrevSel[fieldIdx] = newSel;
                }
            }
            break;
        }
        if (LOWORD(wp) == ID_BTN_PINCFG_HELP) {
            OpenManualDialogOnTab(g_state ? g_state->hwnd : nullptr, 1);
            break;
        }
        if (LOWORD(wp) == ID_BTN_PINCFG_RESET) {
            BoxPinConfig def; // default-constructed = factory defaults
            uint8_t defVals[7] = { def.accPin, def.brkPin, def.vibPin, def.vib2Pin,
                                    def.clutchPin, def.shifterXPin, def.shifterYPin };
            for (int i = 0; i < 7; i++) {
                HWND hCmb = GetDlgItem(hwnd, PINCFG_FIELDS[i].comboId);
                int cnt = (int)SendMessageA(hCmb, CB_GETCOUNT, 0, 0);
                for (int k = 0; k < cnt; k++) {
                    if ((uint8_t)SendMessageA(hCmb, CB_GETITEMDATA, k, 0) == defVals[i]) {
                        SendMessageA(hCmb, CB_SETCURSEL, k, 0);
                        g_pinPrevSel[i] = k;
                        break;
                    }
                }
            }
            break;
        }
        if (LOWORD(wp) == ID_BTN_PINCFG_SAVE && g_state && g_ffbCfgMgr) {
            uint8_t vals[7];
            std::vector<std::string> missing;
            for (int i = 0; i < 7; i++) {
                HWND hCmb = GetDlgItem(hwnd, PINCFG_FIELDS[i].comboId);
                int sel = (int)SendMessageA(hCmb, CB_GETCURSEL, 0, 0);
                vals[i] = (sel == CB_ERR) ? PINCFG_SENTINEL : (uint8_t)SendMessageA(hCmb, CB_GETITEMDATA, sel, 0);
                if (vals[i] == PINCFG_SENTINEL) missing.push_back(PINCFG_FIELDS[i].label);
            }
            if (!missing.empty()) {
                std::string msg = "The following pins are not assigned:\n\n";
                for (auto& m : missing) msg += "- " + m + "\n";
                msg += "\nAssign a pin to every field before saving.";
                MessageBoxA(hwnd, msg.c_str(), "SimRacePro Custom Driver Software - Pin Configuration", MB_OK|MB_ICONWARNING);
                break;
            }
            // Defensive: the live conflict check above should already prevent this.
            bool dup = false;
            for (int i = 0; i < 7 && !dup; i++)
                for (int j = i+1; j < 7 && !dup; j++)
                    if (vals[i] == vals[j]) dup = true;
            if (dup) {
                MessageBoxA(hwnd, "Two fields are assigned to the same pin. Please fix the conflict before saving.",
                    "SimRacePro Custom Driver Software - Pin Configuration", MB_OK|MB_ICONWARNING);
                break;
            }
            g_boxPinConfig.accPin       = vals[0];
            g_boxPinConfig.brkPin       = vals[1];
            g_boxPinConfig.vibPin       = vals[2];
            g_boxPinConfig.vib2Pin      = vals[3];
            g_boxPinConfig.clutchPin    = vals[4];
            g_boxPinConfig.shifterXPin  = vals[5];
            g_boxPinConfig.shifterYPin  = vals[6];
            g_ffbCfgMgr->saveBoxPinConfig(g_boxPinConfig);
            g_state->resend_pin_config = true;
            MessageBoxA(hwnd, "Pin configuration saved and sent to Box.\n"
                "Settings are stored in EEPROM on the Box and\n"
                "will be applied automatically on next power-on.",
                "SimRacePro - Pin Configuration", MB_OK|MB_ICONINFORMATION);
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        SetTextColor((HDC)wp, RGB(220,220,220));
        SetBkColor((HDC)wp, RGB(30,30,35));
        static HBRUSH hBgBr = CreateSolidBrush(RGB(30,30,35));
        return (LRESULT)hBgBr;
    }
    case WM_EXITSIZEMOVE:
        SaveAuxWindowPos(hwnd, "pinconfig");
        break;
    case WM_CLOSE:
        g_hwndPinCfgPanel = nullptr;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void OpenPinConfigWindow(HWND parent, ConfigManager* cfg) {
    g_ffbCfgMgr = cfg;
    if (!g_hwndPinCfgPanel || !IsWindow(g_hwndPinCfgPanel)) {
        static bool s_pinCfgReg = false;
        if (!s_pinCfgReg) {
            WNDCLASSEXA wc = {};
            wc.cbSize=sizeof(wc); wc.lpfnWndProc=PinConfigProc;
            wc.hInstance=g_hInst; wc.hbrBackground=CreateSolidBrush(RGB(30,30,35));
            wc.lpszClassName="SimRaceProPinConfig"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
            RegisterClassExA(&wc);
            s_pinCfgReg = true;
        }
        int pW = 480, pH = 380;
        int cx, cy; ComputeAuxWindowPos(parent, "pinconfig", pW, pH, cx, cy);
        g_hwndPinCfgPanel = CreateWindowExA(
            WS_EX_TOOLWINDOW,
            "SimRaceProPinConfig", "SimRacePro - Pin Configuration",
            WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
            cx, cy, pW, pH,
            nullptr, nullptr, g_hInst, nullptr);
    }
    if (g_hwndPinCfgPanel) {
        ShowWindow(g_hwndPinCfgPanel, SW_SHOW);
        SetForegroundWindow(g_hwndPinCfgPanel);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        g_state->hwnd = hwnd;
        g_hAppIcon = (HICON)LoadImageA(g_hInst,MAKEINTRESOURCEA(IDI_APP_ICON),IMAGE_ICON,0,0,LR_DEFAULTSIZE|LR_SHARED);
        if (!g_hAppIcon) g_hAppIcon = LoadIcon(nullptr,IDI_APPLICATION);
        SendMessageA(hwnd,WM_SETICON,ICON_SMALL,(LPARAM)g_hAppIcon);
        SendMessageA(hwnd,WM_SETICON,ICON_BIG,  (LPARAM)g_hAppIcon);
        AddTrayIcon(hwnd);

        g_fontNormal   = CreateFontA(15,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI");
        g_fontBold     = CreateFontA(15,0,0,0,FW_BOLD,  0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI");
        g_fontSmall    = CreateFontA(12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI");
        g_fontLog      = CreateFontA(16,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Consolas");
        g_fontBtn      = CreateFontW(14,0,0,0,FW_BOLD,  0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI Symbol");
        g_fontBtnLarge = CreateFontW(22,0,0,0,FW_BOLD,  0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI Symbol");

        g_bgBrush    = CreateSolidBrush(COL_BG);
        g_panelBrush = CreateSolidBrush(COL_PANEL);
        // GDI brush cache
        g_brGreen  = CreateSolidBrush(RGB(50,200,80));
        g_brYellow = CreateSolidBrush(RGB(255,200,0));
        g_brRed    = CreateSolidBrush(RGB(220,60,60));
        g_brBlue   = CreateSolidBrush(RGB(0,120,200));
        g_brDark   = CreateSolidBrush(RGB(25,27,35));
        g_brPanel  = CreateSolidBrush(RGB(35,37,47));
        g_logBrush   = CreateSolidBrush(RGB(20,20,25));

        g_wheelImage = LoadWheelBitmapFromResource(g_hInst, IMG_W, IMG_H);
        if (!g_wheelImage) g_wheelImage = (HBITMAP)LoadImageA(nullptr,"wheel.bmp",IMAGE_BITMAP,IMG_W,IMG_H,LR_LOADFROMFILE|LR_CREATEDIBSECTION);

        // Hidden static controls store button label text; drawn manually as overlays
        for (int i=0;i<16;i++) g_hwndBtnLabels[i]=CreateWindowExA(0,"STATIC","-",WS_CHILD,0,0,1,1,hwnd,(HMENU)(INT_PTR)(ID_BTNLABEL_BASE+i),nullptr,nullptr);

        g_hwndConnLabel  = CreateWindowExA(0,"STATIC","Wheel: Searching...",WS_CHILD|WS_VISIBLE|SS_LEFT,SA_X+10,SA_Y+10,SA_W-20,22,hwnd,(HMENU)ID_CONN_LABEL,nullptr,nullptr);
        g_hwndGameLabel  = CreateWindowExA(0,"STATIC","Game: None",         WS_CHILD|WS_VISIBLE|SS_LEFT,SA_X+10,SA_Y+38,SA_W-20,22,hwnd,(HMENU)ID_GAME_LABEL,nullptr,nullptr);
        SendMessage(g_hwndConnLabel,WM_SETFONT,(WPARAM)g_fontBold,TRUE);
        SendMessage(g_hwndGameLabel,WM_SETFONT,(WPARAM)g_fontBold,TRUE);

        // Max Steering Angle - stepped slider directly below the mini
        // steering-angle dial (DrawSteeringWheel, drawn in WM_PAINT at
        // TP_X+65,TP_Y+110 r=65 with its "xx.x deg" readout ending around
        // TP_Y+200) - see g_maxSteerAngleDeg / STEER_ANGLE_STEPS above.
        {
            int idx = FindNearestAngleStepIndex(g_maxSteerAngleDeg.load());
            g_hwndMaxAngleLbl = CreateWindowExA(0,"STATIC","",WS_CHILD|WS_VISIBLE|SS_CENTER,
                TP_X+8,TP_Y+202,130,12,hwnd,nullptr,g_hInst,nullptr);
            SendMessage(g_hwndMaxAngleLbl,WM_SETFONT,(WPARAM)g_fontSmall,TRUE);
            UpdateMaxAngleLabel(idx);
            // TBS_NOTICKS (not AUTOTICKS): the control's own reserved tick
            // band below the channel turned out to be a fixed-color strip
            // our custom draw couldn't reliably recolor (it kept showing the
            // window's base background instead of the panel color). Ticks
            // are drawn by us directly on the channel (TBCD_CHANNEL below)
            // instead, so no such band is needed - keeps the control just
            // tall enough for the channel + thumb.
            g_hwndMaxAngleSld = CreateWindowExA(0,TRACKBAR_CLASSA,"",WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_NOTICKS,
                TP_X+8,TP_Y+215,130,24,hwnd,nullptr,g_hInst,nullptr);
            SendMessage(g_hwndMaxAngleSld,TBM_SETRANGE,TRUE,MAKELPARAM(0,STEER_ANGLE_STEPS_N-1));
            SendMessage(g_hwndMaxAngleSld,TBM_SETTICFREQ,1,0);
            SendMessage(g_hwndMaxAngleSld,TBM_SETPOS,TRUE,idx);
            // Suppress the dotted keyboard-focus rectangle the control would
            // otherwise draw around itself - looks out of place among the
            // rest of the custom-drawn dark UI.
            SendMessage(g_hwndMaxAngleSld,WM_CHANGEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEFOCUS),0);
            SetWindowSubclass(g_hwndMaxAngleSld, MaxAngleSldSubclassProc, 1, 0);
        }

        // Autostart / start-minimized checkboxes: owner-drawn (no background fill,
        // unlike a plain BS_AUTOCHECKBOX without a visual-styles manifest - see
        // CODE_REVIEW notes), checkbox glyph to the right of the label, right edge
        // aligned with the right edge of the Manual button below. Same rows as the
        // (left-aligned) connection/game status labels above - kept narrow (not
        // full row width) so they don't overlap/hide that status text on the left.
        {
            const int gap  = 5;
            const int bw   = (SA_W - 20 - gap * 5) / 6;
            const int rightEdge = SA_X + 10 + 5 * (bw + gap) + bw;  // = ID_BTN_MANUAL's right edge
            const int chkW = 230;  // wide enough for "Autostart with Windows" + glyph
            CreateWindowExA(0,"BUTTON","Autostart with Windows",
                WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
                rightEdge-chkW,SA_Y+10,chkW,22,hwnd,(HMENU)ID_CHK_AUTOSTART,nullptr,nullptr);
            CreateWindowExA(0,"BUTTON","Start minimized",
                WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
                rightEdge-chkW,SA_Y+38,chkW,22,hwnd,(HMENU)ID_CHK_START_MIN,nullptr,nullptr);
        }

        // 6 buttons, three-line labels
        {
            int gap = 5;
            int bw  = (SA_W - 20 - gap * 5) / 6;
            int bh  = 54;  // tall enough for 3 text lines
            int by  = SA_Y + 70;
            g_hwndGamepadBtn = CreateWindowExA(0,"BUTTON","Windows\nGamepad\nSettings",    WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, SA_X+10+0*(bw+gap),by,bw,bh,hwnd,(HMENU)ID_BTN_GAMEPAD,  nullptr,nullptr);
            g_hwndFfbBtn     = CreateWindowExA(0,"BUTTON","Force\nFeedback\nSettings",    WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, SA_X+10+1*(bw+gap),by,bw,bh,hwnd,(HMENU)ID_BTN_FFB,     nullptr,nullptr);
            CreateWindowExA(0,"BUTTON","Wheel\nHardware\nSettings",                       WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, SA_X+10+2*(bw+gap),by,bw,bh,hwnd,(HMENU)ID_BTN_WHEEL_HW,nullptr,nullptr);
            g_hwndResetBtn   = CreateWindowExA(0,"BUTTON","Reset\nall\nSettings",          WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, SA_X+10+3*(bw+gap),by,bw,bh,hwnd,(HMENU)ID_BTN_RESET_MAP,nullptr,nullptr);
            g_hwndConsoleBtn = CreateWindowExA(0,"BUTTON","Show\nConsole",                 WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, SA_X+10+4*(bw+gap),by,bw,bh,hwnd,(HMENU)ID_BTN_CONSOLE,  nullptr,nullptr);
            g_hwndManualBtn  = CreateWindowExA(0,"BUTTON","Manual",                        WS_CHILD|WS_VISIBLE|BS_OWNERDRAW, SA_X+10+5*(bw+gap),by,bw,bh,hwnd,(HMENU)ID_BTN_MANUAL,   nullptr,nullptr);
            // Debug button (hidden unless -debug flag)
            g_hwndDebugBtn   = CreateWindowExA(0,"BUTTON","FFB\nDebug",
                g_debugMode ? (WS_CHILD|WS_VISIBLE|BS_OWNERDRAW) : (WS_CHILD|BS_OWNERDRAW),
                SA_X+10+4*(bw+gap),by+bh+5,bw,bh,hwnd,(HMENU)ID_BTN_DEBUG,nullptr,nullptr);
        }

        // Log-Edit wird als Child von hwnd erstellt; beim Öffnen des Konsolenfensters
        // wird es per SetParent() dorthin verschoben.
        g_hwndLog = CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","",
            WS_CHILD|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            0,0,CON_W,CON_H,hwnd,(HMENU)ID_LOG_BOX,nullptr,nullptr);
        SendMessage(g_hwndLog,WM_SETFONT,(WPARAM)g_fontLog,TRUE);

        // Konsolen-Fensterklasse registrieren (einfaches Fenster, kein eigener WndProc nötig)
        {
            WNDCLASSEXA wcc = {};
            wcc.cbSize = sizeof(wcc);
            wcc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
                if (m == WM_SIZE && g_hwndLog) {
                    RECT cr; GetClientRect(h, &cr);
                    SetWindowPos(g_hwndLog,nullptr,0,0,cr.right,cr.bottom,SWP_NOZORDER);
                }
                if (m == WM_CTLCOLOREDIT || m == WM_CTLCOLORSTATIC) {
                    // Weisse Schrift auf dunklem Hintergrund fuer das Log-Edit
                    HDC hdc = (HDC)w;
                    SetTextColor(hdc, RGB(220,220,220));
                    SetBkColor(hdc, RGB(20,20,25));
                    static HBRUSH s_logBrush = CreateSolidBrush(RGB(20,20,25));
                    return (LRESULT)s_logBrush;
                }
                if (m == WM_CLOSE) {
                    ShowWindow(h, SW_HIDE);
                    if (g_hwndConsoleBtn) {
                        g_consoleVisible = false;
                        SetWindowTextA(g_hwndConsoleBtn, "Show\nConsole");
                        InvalidateRect(g_hwndConsoleBtn,nullptr,TRUE);
                    }
                    return 0;
                }
                if (m == WM_EXITSIZEMOVE) { SaveAuxWindowPos(h, "console"); return 0; }
                return DefWindowProcA(h,m,w,l);
            };
            wcc.hInstance     = g_hInst;
            wcc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
            wcc.lpszClassName = "SimRaceProConsole";
            wcc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassExA(&wcc);
        }
        break;
    }

    case WM_ERASEBKGND: return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdcReal = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        HDC hdc = CreateCompatibleDC(hdcReal);
        HBITMAP hBmp = CreateCompatibleBitmap(hdcReal, cr.right, cr.bottom);
        HBITMAP hOld = (HBITMAP)SelectObject(hdc, hBmp);
        FillRect(hdc, &cr, g_bgBrush);

        // ── Left panel: button mapping image ─────────────────────────────────
        RECT lpR={LP_X,LP_Y,LP_X+LP_W,LP_Y+LP_H}; FillRect(hdc,&lpR,g_panelBrush);
        SetBkMode(hdc,TRANSPARENT); SelectObject(hdc,g_fontBold); SetTextColor(hdc,COL_ACCENT);
        RECT lpT={LP_X+10,LP_Y+4,LP_X+LP_W-10,LP_Y+20}; DrawTextA(hdc,"BUTTON MAPPING",-1,&lpT,DT_LEFT|DT_SINGLELINE);
        { HPEN dp=CreatePen(PS_SOLID,1,COL_ACCENT); SelectObject(hdc,dp); MoveToEx(hdc,LP_X+10,LP_Y+22,nullptr); LineTo(hdc,LP_X+LP_W-10,LP_Y+22); DeleteObject(dp); }

        int imgOX=LP_X+10, imgOY=LP_Y+24;
        if (g_wheelImage) { HDC hdcI=CreateCompatibleDC(hdc); auto hOI=(HBITMAP)SelectObject(hdcI,g_wheelImage); BitBlt(hdc,imgOX,imgOY,IMG_W,IMG_H,hdcI,0,0,SRCCOPY); SelectObject(hdcI,hOI); DeleteDC(hdcI); }
        else { RECT imgR={imgOX,imgOY,imgOX+IMG_W,imgOY+IMG_H}; HBRUSH fb=CreateSolidBrush(RGB(50,50,60)); FillRect(hdc,&imgR,fb); DeleteObject(fb); SetTextColor(hdc,COL_SUBTEXT); DrawTextA(hdc,"wheel.bmp not found",-1,&imgR,DT_CENTER|DT_VCENTER|DT_SINGLELINE); }
        DrawBtnOverlays(hdc, imgOX, imgOY);

        // SSD1306 overlay
        { float dThr,dBrk,dSteer; { std::lock_guard<std::mutex> lk(g_state->mtx); dThr=g_state->throttle; dBrk=g_state->brake; dSteer=g_state->steering; }
          TelemetryData td; { std::lock_guard<std::mutex> lk(telemetryMutex); td=currentTelemetry; }
          int dmr=(td.maxRpm>0)?td.maxRpm:9000;
          float rp=std::min((float)td.rpm/(float)dmr,1.0f);
          char dg[4]; if(td.gear==0)strcpy_s(dg,"N");else if(td.gear==-1)strcpy_s(dg,"R");else sprintf_s(dg,"%d",td.gear);
          DrawSSD1306Overlay(hdc,imgOX,imgOY,dBrk,dThr,rp,dg,(int)td.speed,dSteer); }

        // ── Telemetry panel ───────────────────────────────────────────────────
        RECT tpR={TP_X,TP_Y,TP_X+TP_W,TP_Y+TP_H}; FillRect(hdc,&tpR,g_panelBrush);
        SelectObject(hdc,g_fontBold); SetTextColor(hdc,COL_ACCENT);
        RECT tpT={TP_X+10,TP_Y+6,TP_X+TP_W-10,TP_Y+24}; DrawTextA(hdc,"TELEMETRY INPUTS + OUTPUTS",-1,&tpT,DT_LEFT|DT_SINGLELINE);
        { HPEN tp=CreatePen(PS_SOLID,1,COL_ACCENT); SelectObject(hdc,tp); MoveToEx(hdc,TP_X+10,TP_Y+26,nullptr); LineTo(hdc,TP_X+TP_W-10,TP_Y+26); DeleteObject(tp); }

        // cx=TP_X+73 centers the dial over the Max Steering Angle slider below it
        // (slider spans TP_X+8..TP_X+138, center TP_X+73) instead of flush against
        // the panel's left edge.
        { float steer; { std::lock_guard<std::mutex> lk(g_state->mtx); steer=g_state->steering; } DrawSteeringWheel(hdc,TP_X+73,TP_Y+110,65,steer); }

        float thr,brk,clt; int hb; bool hasClutch,hasHb;
        { std::lock_guard<std::mutex> lk(g_state->mtx); thr=g_state->throttle; brk=g_state->brake; clt=g_state->clutch; hb=g_state->handbrake; hasClutch=g_state->has_clutch; hasHb=g_state->has_handbrake; }
        int bx=TP_X+145, by=TP_Y+36;
        DrawBar(hdc,bx,by,TP_W-155,26,thr,COL_GREEN,"Throttle");
        DrawBar(hdc,bx,by+32,TP_W-155,26,brk,COL_RED,"Brake");
        if (hasClutch) DrawBar(hdc,bx,by+64,TP_W-155,26,clt,COL_YELLOW,"Clutch");
        if (hasHb) {
            int hbx=bx+(TP_W-155)-90, hby=by+(hasClutch?96:64);
            HBRUSH hbBr=CreateSolidBrush(hb?COL_RED:RGB(55,55,65)); SelectObject(hdc,hbBr); SelectObject(hdc,(HPEN)GetStockObject(NULL_PEN));
            RoundRect(hdc,hbx,hby,hbx+85,hby+22,6,6); DeleteObject(hbBr);
            SetTextColor(hdc,hb?RGB(255,255,255):COL_SUBTEXT);
            RECT hbr={hbx,hby,hbx+85,hby+22}; DrawTextA(hdc,hb?"HANDBRAKE":"handbrake",-1,&hbr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }

        // Divider + "GAME DATA" label
        { HPEN div=CreatePen(PS_SOLID,1,RGB(60,60,72)); SelectObject(hdc,div); MoveToEx(hdc,TP_X+145,TP_Y+158,nullptr); LineTo(hdc,TP_X+TP_W-10,TP_Y+158); DeleteObject(div); }
        SelectObject(hdc,g_fontSmall); SetTextColor(hdc,COL_SUBTEXT);
        { RECT sub={TP_X+145,TP_Y+161,TP_X+TP_W-145,TP_Y+177}; DrawTextA(hdc,"GAME DATA",-1,&sub,DT_LEFT|DT_SINGLELINE); }

        TelemetryData t; { std::lock_guard<std::mutex> lk(telemetryMutex); t=currentTelemetry; }

        // RPM bar
        { int mr=(t.maxRpm>0)?t.maxRpm:9000;
          float rp=std::min((float)t.rpm/(float)mr,1.0f);
          int rbx=TP_X+145,rby=TP_Y+178,rbw=TP_W-155,rbh=14;
          HBRUSH rbb=CreateSolidBrush(RGB(40,40,50)); SelectObject(hdc,rbb); SelectObject(hdc,(HPEN)GetStockObject(NULL_PEN));
          RoundRect(hdc,rbx,rby,rbx+rbw,rby+rbh,4,4); DeleteObject(rbb);
          int fw=(int)(rp*(rbw-2));
          if (fw>0) { HBRUSH rfb=(rp<RPM_GREEN)?g_brGreen:(rp<RPM_YELLOW)?g_brYellow:g_brRed; SelectObject(hdc,rfb); RoundRect(hdc,rbx+1,rby+1,rbx+1+fw,rby+rbh-1,3,3); }  // cached brush

          // Telemetry cells (RPM, Speed, Gear, FFB, ABS, TC)
          int cx0=TP_X+153, cy0=TP_Y+196, cw=74, ch=52, gap=6;
          bool gameActive=(t.gameName!="None");
          char gearStr[4];
          if(t.gear==0)strcpy_s(gearStr,"N");else if(t.gear==-1)strcpy_s(gearStr,"R");else sprintf_s(gearStr,"%d",t.gear);
          // Normalize to the hardware FFB cap (FFB_MAX_STRENGTH=0.70)
          // so that the maximum output shows as 100% in the GUI.
          float ffbPct=((lastTorque-127.0f)/127.0f)*100.0f;
          char ffbBuf[16]; sprintf_s(ffbBuf,"%+.0f%%",ffbPct);
          char rpmBuf[16]; sprintf_s(rpmBuf,"%d",t.rpm);
          char spdBuf[16]; sprintf_s(spdBuf,"%.0f",t.speed);

          auto drawCell=[&](int x,int y,int w2,int h2,const char* lbl,const char* val,COLORREF vc=COL_TEXT){
              HBRUSH cb=CreateSolidBrush(RGB(22,22,28)); SelectObject(hdc,cb); SelectObject(hdc,(HPEN)GetStockObject(NULL_PEN)); RoundRect(hdc,x,y,x+w2,y+h2,5,5); DeleteObject(cb);
              SetBkMode(hdc,TRANSPARENT); SelectObject(hdc,g_fontSmall); SetTextColor(hdc,COL_SUBTEXT);
              RECT lr={x+4,y+3,x+w2-4,y+16}; DrawTextA(hdc,lbl,-1,&lr,DT_CENTER|DT_SINGLELINE);
              SelectObject(hdc,g_fontBold); SetTextColor(hdc,vc);
              RECT vr={x+2,y+14,x+w2-2,y+h2-3}; DrawTextA(hdc,val,-1,&vr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
          };
          drawCell(cx0+0*(cw+gap),cy0,cw,ch,"RPM",       gameActive?rpmBuf:"--",COL_ACCENT);
          drawCell(cx0+1*(cw+gap),cy0,cw,ch,"SPEED km/h", gameActive?spdBuf:"--",COL_TEXT);
          drawCell(cx0+2*(cw+gap),cy0,cw,ch,"GEAR",       gameActive?gearStr:"--",(strcmp(gearStr,"R")==0)?COL_RED:COL_TEXT);
          drawCell(cx0+3*(cw+gap),cy0,cw,ch,"FFB",        gameActive?ffbBuf:"--",(ffbPct>0)?COL_GREEN:(ffbPct<0?COL_RED:COL_SUBTEXT));
          // ABS / TC flags
          int fx=cx0+4*(cw+gap),fy=cy0; int fw2=(cw-3)/2;
          auto drawFlag=[&](const char* lbl,bool on,int x,int y,int w2,int h2){
              HBRUSH fb=CreateSolidBrush(on?COL_YELLOW:RGB(22,22,28)); SelectObject(hdc,fb); SelectObject(hdc,(HPEN)GetStockObject(NULL_PEN)); RoundRect(hdc,x,y,x+w2,y+h2,4,4); DeleteObject(fb);
              HPEN bp=CreatePen(PS_SOLID,1,on?COL_YELLOW:RGB(60,60,72)); SelectObject(hdc,bp); SelectObject(hdc,(HBRUSH)GetStockObject(NULL_BRUSH)); RoundRect(hdc,x,y,x+w2,y+h2,4,4); DeleteObject(bp);
              SetBkMode(hdc,TRANSPARENT); SelectObject(hdc,g_fontSmall); SetTextColor(hdc,on?RGB(20,20,20):COL_SUBTEXT);
              RECT lr={x,y,x+w2,y+h2}; DrawTextA(hdc,lbl,-1,&lr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
          };
          drawFlag("ABS",t.absActive,fx,fy,fw2,ch);
          drawFlag("TC", t.tcActive, fx+fw2+3,fy,fw2,ch);
        }

        // ── Status area ───────────────────────────────────────────────────────
        RECT saR={SA_X,SA_Y,SA_X+SA_W,SA_Y+SA_H}; FillRect(hdc,&saR,g_panelBrush);
        { HPEN sad=CreatePen(PS_SOLID,1,COL_ACCENT); SelectObject(hdc,sad); MoveToEx(hdc,SA_X+10,SA_Y+2,nullptr); LineTo(hdc,SA_X+SA_W-10,SA_Y+2); DeleteObject(sad); }

        BitBlt(hdcReal,0,0,cr.right,cr.bottom,hdc,0,0,SRCCOPY);
        SelectObject(hdc,hOld); DeleteObject(hBmp); DeleteDC(hdc);
        EndPaint(hwnd,&ps);
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc=(HDC)wp; HWND ctrl=(HWND)lp; SetBkMode(hdc,TRANSPARENT);
        if (ctrl==g_hwndConnLabel) { SetTextColor(hdc,g_state->connected.load()?COL_GREEN:COL_RED); return (LRESULT)g_panelBrush; }
        if (ctrl==g_hwndGameLabel) { std::string gm; { std::lock_guard<std::mutex> lk(g_state->mtx); gm=g_state->active_game; } SetTextColor(hdc,gm.empty()?COL_SUBTEXT:COL_ACCENT); return (LRESULT)g_panelBrush; }
        if (ctrl==g_hwndMaxAngleLbl) { SetTextColor(hdc,COL_TEXT); return (LRESULT)g_panelBrush; }
        if (ctrl==g_hwndLog)       { SetTextColor(hdc,COL_TEXT); SetBkColor(hdc,RGB(20,20,25)); return (LRESULT)g_logBrush; }
        return (LRESULT)g_bgBrush;
    }

    // ── Backend messages ──────────────────────────────────────────────────────
    case WM_APP_CONN_STATUS: {
        bool ok=g_state->connected.load(); std::string port; { std::lock_guard<std::mutex> lk(g_state->mtx); port=g_state->com_port; }
        SetWindowTextA(g_hwndConnLabel,ok?("Wheel Connected  ["+port+"]").c_str():"Searching for Wheel...");
        InvalidateRect(hwnd,nullptr,FALSE); break;
    }
    case WM_APP_GAME_CHANGE: {
        std::string gm; { std::lock_guard<std::mutex> lk(g_state->mtx); gm=g_state->active_game; }
        SetWindowTextA(g_hwndGameLabel,gm.empty()?"Game: None":("Game: "+gm).c_str());
        InvalidateRect(hwnd,nullptr,FALSE); break;
    }
    case WM_APP_BTNMAP_UPDATE: {
        std::map<int,USHORT> bm; { std::lock_guard<std::mutex> lk(g_state->mtx); bm=g_state->buttonMap; }
        // buttonMap is keyed by physical button bit - translate through the
        // wiring layout so each overlay position shows its own assignment.
        int p2p[16]; GetPosToPhysCopy(p2p);
        for (int i=0;i<16;i++) {
            const char* txt = "-";
            if (p2p[i] >= 0) { auto it=bm.find(p2p[i]); if (it!=bm.end()) txt = xboxButtonName(it->second); }
            SetWindowTextA(g_hwndBtnLabels[i], txt);
        }
        RECT r={LP_X,LP_Y,LP_X+LP_W,LP_Y+LP_H}; InvalidateRect(hwnd,&r,FALSE); break;
    }
    case WM_APP_INPUT_UPDATE: {
        bool physActive[16] = {};
        { std::lock_guard<std::mutex> lk(g_state->mtx); for (int idx:g_state->active_buttons) if (idx>=0&&idx<16) physActive[idx]=true; }
        int p2p[16]; GetPosToPhysCopy(p2p);
        for (int i=0;i<16;i++) g_btnActive[i] = (p2p[i]>=0 && physActive[p2p[i]]);
        { RECT r={TP_X,TP_Y,TP_X+TP_W,TP_Y+TP_H}; InvalidateRect(hwnd,&r,FALSE); }
        { RECT r={LP_X,LP_Y,LP_X+LP_W,LP_Y+LP_H}; InvalidateRect(hwnd,&r,FALSE); }
        break;
    }
    case WM_APP_CALIB_MAPPED: {
        int idx=(int)wp; USHORT xbv=(USHORT)lp;   // idx = physical button bit
        int p2p[16]; GetPosToPhysCopy(p2p);
        for (int p=0;p<16;p++) if (p2p[p]==idx) { SetWindowTextA(g_hwndBtnLabels[p],xboxButtonName(xbv)); RECT r={LP_X,LP_Y,LP_X+LP_W,LP_Y+LP_H}; InvalidateRect(hwnd,&r,FALSE); break; }
        break;
    }
    case WM_APP_LOG: {
        std::deque<std::string> lines; { std::lock_guard<std::mutex> lk(g_state->mtx); lines=g_state->log_lines; }
        // incremental append
        if (lines.empty()) break;
        std::string narrow = lines.back() + "\r\n";
        int wn = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, nullptr, 0);
        std::wstring wide(wn > 1 ? wn - 1 : 0, 0);
        if (wn > 1) MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, &wide[0], wn);
        SendMessage(g_hwndLog, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageW(g_hwndLog, EM_REPLACESEL, FALSE, (LPARAM)wide.c_str());
        SendMessage(g_hwndLog, WM_VSCROLL, SB_BOTTOM, 0); break;
    }
    case WM_APP_CALIB_NEXT: if (g_hwndCalibDlg&&IsWindow(g_hwndCalibDlg)) SendMessage(g_hwndCalibDlg,WM_APP_CALIB_NEXT,0,0); break;
    case WM_APP_CALIB_DONE: if (g_hwndCalibDlg&&IsWindow(g_hwndCalibDlg)) SendMessage(g_hwndCalibDlg,WM_APP_CALIB_DONE,0,0); break;

    case WM_APP_WIRING_NEXT: {
        { std::lock_guard<std::mutex> lk(g_state->mtx); g_wiringHighlight = g_state->wiring_pos; }
        if (g_hwndWiringDlg&&IsWindow(g_hwndWiringDlg)) SendMessage(g_hwndWiringDlg,WM_APP_WIRING_NEXT,0,0);
        else { std::lock_guard<std::mutex> lk(g_state->mtx); g_state->wiring_waiting = false; }  // dialog gone - don't stall the backend
        { RECT r={LP_X,LP_Y,LP_X+LP_W,LP_Y+LP_H}; InvalidateRect(hwnd,&r,FALSE); }
        break;
    }
    case WM_APP_WIRING_DONE: {
        g_wiringHighlight = -1;
        if (g_hwndWiringDlg&&IsWindow(g_hwndWiringDlg)) { DestroyWindow(g_hwndWiringDlg); g_hwndWiringDlg=nullptr; }
        { RECT r={LP_X,LP_Y,LP_X+LP_W,LP_Y+LP_H}; InvalidateRect(hwnd,&r,FALSE); }
        break;
    }
    case WM_APP_ASK_WIRING: {
        // Ownerless for the same reason as WM_APP_ASK_CALIB (see pitfalls #16).
        int r=MessageBoxA(nullptr,
            "Wheel button wiring setup\n\n"
            "Because the GPIO wiring can differ between builds, the software first\n"
            "needs to learn which physical wheel button sits at which position.\n\n"
            "After you press OK, one button after another lights up YELLOW on the\n"
            "wheel image in the main window - press the matching button on your\n"
            "wheel each time. Use \"Skip this button\" for positions your wheel\n"
            "does not have.\n\n"
            "Press OK to begin, or Cancel to keep the default wiring.",
            "SimRacePro Custom Driver Software - Button Wiring Setup",MB_OKCANCEL|MB_ICONINFORMATION);
        if (r==IDOK) {
            ShowMainWindow(hwnd);  // the yellow highlight lives on the main window
            static bool s_wiringClassReg = false;
            if (!s_wiringClassReg) {
                WNDCLASSEXA wc={}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=WiringDlgProc; wc.hInstance=GetModuleHandleA(nullptr); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszClassName="SimRaceProWiring";
                RegisterClassExA(&wc); s_wiringClassReg = true;
            }
            // Next to the main window (not centered) so it never covers the wheel image.
            RECT wr{}; GetWindowRect(hwnd,&wr);
            RECT wa{}; SystemParametersInfoA(SPI_GETWORKAREA,0,&wa,0);
            int dx = (wr.right+8+380 <= wa.right) ? wr.right+8 : (wr.left-388 >= wa.left ? wr.left-388 : wa.right-388);
            g_hwndWiringDlg=CreateWindowExA(WS_EX_DLGMODALFRAME,"SimRaceProWiring","SimRacePro Custom Driver Software - Button Wiring",WS_POPUP|WS_VISIBLE|WS_CAPTION,dx,wr.top+60,380,175,hwnd,nullptr,GetModuleHandleA(nullptr),nullptr);
            ShowWindow(g_hwndWiringDlg,SW_SHOW);
            g_state->start_wiring_requested=true;
        } else { g_state->wiring_aborted=true; g_state->start_wiring_requested=true; }
        break;
    }

    case WM_APP_ASK_CALIB: {
        // No owner window: same reasoning as WM_APP_WELCOME/WM_APP_FW_MISMATCH -
        // this fires automatically on startup and shouldn't block the main
        // window (e.g. the Manual button) while the user decides.
        int r=MessageBoxA(nullptr,"No button mapping found.\n\nStart the button calibration wizard?\n\nPress OK to begin, or Cancel to skip.","SimRacePro Custom Driver Software - Button Setup",MB_OKCANCEL|MB_ICONINFORMATION);
        if (r==IDOK) {
            WNDCLASSEXA wc={}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=CalibDlgProc; wc.hInstance=GetModuleHandleA(nullptr); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); wc.lpszClassName="SimRaceProCalib";
            static bool s_calibClassReg = false;
            if (!s_calibClassReg) { RegisterClassExA(&wc); s_calibClassReg = true; }
            g_hwndCalibDlg=CreateWindowExA(WS_EX_DLGMODALFRAME,"SimRaceProCalib","SimRacePro Custom Driver Software - Button Calibration",WS_POPUP|WS_VISIBLE|WS_CAPTION,(GetSystemMetrics(SM_CXSCREEN)-380)/2,(GetSystemMetrics(SM_CYSCREEN)-180)/2,380,180,hwnd,nullptr,GetModuleHandleA(nullptr),nullptr);
            ShowWindow(g_hwndCalibDlg,SW_SHOW);
            g_state->start_calib_requested=true;
        } else { g_state->calib_aborted=true; g_state->start_calib_requested=true; }
        break;
    }
    case WM_APP_HW_DIALOG: {
        // Hardware config is now managed via Box Hardware Settings window.
        // Read values directly from g_boxSettings (loaded from EEPROM/disk on start).
        g_state->has_clutch    = g_boxSettings.hasClutch;
        g_state->has_handbrake = g_boxSettings.hasHandbrake;
        g_state->has_shifter   = g_boxSettings.hasShifter;
        g_state->hw_dialog_done = true;
        // Show Box Hardware Settings window if this is first run (no config saved yet)
        if (g_cfg && !g_cfg->hasOptionalHardwareConfig())
            OpenBoxSettingsWindow(hwnd, g_cfg);
        break;
    }

    case WM_APP_WELCOME: {
        // ── Welcome Notice ───────────────────────────────────────────────────
        // Shown on first run when no config exists (fresh install).
        // Firmware is no longer flashed by the software itself - remind the
        // user to flash both Arduinos manually beforehand.
        // No owner window: passing hwnd here would disable the main window
        // (and all its child controls, e.g. the Manual button) for as long as
        // this box is shown. The user should still be able to open the Manual
        // while reading this notice, so this is deliberately ownerless
        // (same reasoning as WM_APP_FW_MISMATCH below).
        MessageBoxA(nullptr,
            "Welcome to SimRacePro!\r\n\r\n"
            "This appears to be a fresh installation.\r\n\r\n"
            "Before continuing, make sure both the BASE and WHEEL Arduino\r\n"
            "are flashed with the SimRacePro firmware (" EXPECTED_BOX_FW_VERSION ").\r\n"
            "Flashing is done manually via the Arduino IDE:\r\n\r\n"
            "  1. Install required libraries (Encoder, Adafruit GFX,\r\n"
            "     Adafruit SSD1306) via Library Manager\r\n"
            "  2. Open the sketch in \"Arduino files\\sim_race_pro_box_script\"\r\n"
            "     (Base) resp. \"Arduino files\\sim_race_pro_wheel_script\" (Wheel)\r\n"
            "  3. Select the correct board and COM port\r\n"
            "  4. Click Upload\r\n\r\n"
            "See the Manual (\"Getting Started\" tab) for the full step-by-step guide.\r\n\r\n"
            "Click OK once both Arduinos are flashed to continue.",
            "SimRacePro - First Run Setup",
            MB_OK | MB_ICONINFORMATION);

        if (g_state) g_state->welcome_done = true;
        break;
    }

    case WM_APP_FW_MISMATCH: {
        // Backend detected a firmware version mismatch.
        // lp = heap-allocated FirmwareInfo* - we own it.
        FirmwareInfo* fi = reinterpret_cast<FirmwareInfo*>(lp);
        if (!fi) break;

        // Firmware is no longer flashed by the software itself - just inform
        // the user which board needs a manual reflash via the Arduino IDE.
        std::string info = "A firmware update is required before the software can be used.\n\n";
        if (fi->boxMismatch)
            info += "Box firmware:   " + fi->boxVer + "  (required: " + fi->expectedBox + ")\n";
        if (fi->wheelMismatch)
            info += "Wheel firmware: " + fi->wheelVer + "  (required: " + fi->expectedWheel + ")\n";
        info += "\nPlease reflash the affected board(s) manually via the Arduino IDE\n"
                "(see Manual -> \"Getting Started\" for instructions), then reconnect.\n"
                "The software will keep retrying the connection automatically.";

        // No owner window: passing hwnd here would disable the main window
        // (and all its child controls, e.g. the Manual button) for as long as
        // this box is shown. The user should still be able to open the Manual
        // while firmware is out of date, so this is deliberately ownerless.
        MessageBoxA(nullptr, info.c_str(),
            "SimRacePro - Firmware Update Required",
            MB_OK | MB_ICONWARNING);

        delete fi;
        if (g_state) g_state->fw_update_done = true;
        break;
    }

    case WM_APP_WHEEL_MISSING: {
        // Backend connected to the Box fine, but the Box itself never heard
        // back from the Wheel - not a firmware mismatch, so no update wizard,
        // just a heads-up. Steering angle, pedals and FFB keep working (they
        // run through the Box's own encoder); only wheel buttons are affected.
        std::string info = "The Box is connected, but hasn't received any response from the Wheel.\n\n"
                            "Please check that the Wheel's USB/serial cable is properly connected and the Wheel is powered on.\n\n"
                            "Steering, pedals and force feedback will keep working, but wheel buttons will not respond until the Wheel is connected.";

        // No owner window: same reasoning as WM_APP_WELCOME/WM_APP_FW_MISMATCH -
        // this fires automatically from the backend thread and shouldn't block
        // the main window while the user goes to check the cable.
        MessageBoxA(nullptr, info.c_str(),
            "SimRacePro - Wheel Not Responding",
            MB_OK | MB_ICONWARNING);
        break;
    }

    case WM_APP_CFG_RESET: {
        // Backend found configs older than the current config format
        // (CONFIG_SCHEMA_VERSION - see the schema check at the top of
        // runBackend) and wiped them. Does NOT fire on every version bump, only
        // when the on-disk format actually changed. lp = heap std::string* with
        // the old version ("" = pre-3.1.4, no marker yet).
        std::string* oldVer = reinterpret_cast<std::string*>(lp);
        std::string from = oldVer->empty() ? std::string("an older version")
                                           : ("version " + *oldVer);
        delete oldVer;
        std::string info = "SimRacePro was updated from " + from + " to v" VER_STRING ".\n\n"
            "The configuration format changed in this update, so the old settings could no\n"
            "longer be read and have been reset: button mapping, button wiring, hardware\n"
            "settings and FFB settings.\n\n"
            "The setup wizards will now guide you through a clean configuration.\n"
            "Also make sure both Arduinos are flashed with the matching firmware (see Manual).";
        // No owner window: same reasoning as WM_APP_WELCOME/WM_APP_FW_MISMATCH -
        // fires automatically from the backend thread on startup.
        MessageBoxA(nullptr, info.c_str(),
            "SimRacePro Custom Driver Software - Updated",
            MB_OK | MB_ICONINFORMATION);
        if (g_state) g_state->cfg_reset_done = true;  // backend waits on this
        break;
    }

    case WM_APP_UPDATE_AVAIL: {
        // lp = heap-allocated std::string* with new version string
        std::string* newVer = reinterpret_cast<std::string*>(lp);
        std::string msg = "A new version of SimRacePro Custom Driver Software is available!\n\n"
                          "Current version:  v" VER_STRING "\n"
                          "Available version: v" + *newVer + "\n\n"
                          "Would you like to open the download page?";
        delete newVer;
        // No owner window: same reasoning as WM_APP_WELCOME/WM_APP_FW_MISMATCH -
        // this fires automatically (update check) and shouldn't block the main
        // window (e.g. the Manual button) while the user decides.
        if (MessageBoxA(nullptr, msg.c_str(),
                "SimRacePro Custom Driver Software - Update Available",
                MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            ShellExecuteA(hwnd, "open", UPDATE_RELEASE_URL, nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    }

    case WM_DRAWITEM: {
        auto* di = (DRAWITEMSTRUCT*)lp;
        if (di->CtlType != ODT_BUTTON) break;
        HDC hdc  = di->hDC;
        RECT rc  = di->rcItem;

        if (di->CtlID == ID_CHK_AUTOSTART || di->CtlID == ID_CHK_START_MIN) {
            // Owner-drawn buttons still get their background erased to the
            // system's default (light) button-face color before WM_DRAWITEM
            // fires - fill with the panel color ourselves so it blends in
            // instead of showing a stray light box behind the control.
            FillRect(hdc, &rc, g_panelBrush);
            // Checkbox glyph to the right of its label, glyph flush with rc.right
            // (aligned with the Manual button's right edge - see WM_CREATE).
            // BM_SETCHECK/BM_GETCHECK don't track state for BS_OWNERDRAW buttons,
            // so the checked state is derived straight from its actual source
            // instead: the registry key for autostart, g_startMinimized otherwise.
            bool checked = (di->CtlID == ID_CHK_AUTOSTART) ? IsAutostartEnabled() : g_startMinimized;
            char raw[64]={}; GetWindowTextA(di->hwndItem, raw, sizeof(raw));

            const int boxSize = 16;
            RECT boxRc  = { rc.right-boxSize, rc.top+(rc.bottom-rc.top-boxSize)/2, rc.right, rc.top+(rc.bottom-rc.top-boxSize)/2+boxSize };
            RECT textRc = { rc.left, rc.top, boxRc.left-6, rc.bottom };

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, COL_TEXT);
            SelectObject(hdc, g_fontNormal);
            DrawTextA(hdc, raw, -1, &textRc, DT_RIGHT|DT_VCENTER|DT_SINGLELINE);

            DWORD dfcState = DFCS_BUTTONCHECK | (checked ? DFCS_CHECKED : 0) | ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0);
            DrawFrameControl(hdc, &boxRc, DFC_BUTTON, dfcState);
            break;
        }

        bool pressed  = (di->itemState & ODS_SELECTED) != 0;
        bool isManual = (di->CtlID == ID_BTN_MANUAL);
        bool isConsole= (di->CtlID == ID_BTN_CONSOLE);
        bool isDebug  = (di->CtlID == ID_BTN_DEBUG);
        bool isFfb    = (di->CtlID == ID_BTN_FFB);
        bool isBoxHW  = (di->CtlID == ID_BTN_WHEEL_HW);
        bool isReset  = (di->CtlID == ID_BTN_RESET_MAP);

        // Background fill
        COLORREF bgCol = isManual  ? (pressed ? RGB(0, 95,155)  : RGB(0,115,180))  :
                         isConsole ? (pressed ? RGB(28,100,55)   : RGB(35,120,65))  :
                         isFfb    ? (pressed ? RGB(90,30,120)   : RGB(110,40,150)) :
                         isBoxHW  ? (pressed ? RGB(120,70,20)   : RGB(150,90,30))  :
                         isReset  ? (pressed ? RGB(120,45,45)   : RGB(150,55,55))  :
                         isDebug   ? (g_ffbDebugOn ? (pressed ? RGB(160,80,0) : RGB(190,100,0))
                                                   : (pressed ? RGB(34,35,46) : RGB(44,46,60))) :
                                     (pressed ? RGB(34, 35, 46)  : RGB(44, 46, 60));
        COLORREF bdCol = isManual  ? RGB(0,150,220) :
                         isConsole ? RGB(45,170,85) :
                         isFfb    ? RGB(160,70,200) :
                         isBoxHW  ? RGB(210,140,50) :
                         isReset  ? RGB(200,90,90)  :
                         isDebug   ? (g_ffbDebugOn ? RGB(255,140,0) : RGB(68,70,88)) :
                                     RGB(68, 70, 88);
        // Owner-drawn buttons get erased to the system's default (light) button-face
        // color before WM_DRAWITEM fires; the RoundRect below doesn't cover the
        // rect's corners, so that default color would otherwise show through as a
        // thin border around each button. Fill with the panel color first so the
        // corners blend in instead.
        FillRect(hdc, &rc, g_panelBrush);
        HBRUSH bgBr = CreateSolidBrush(bgCol);
        SelectObject(hdc, bgBr);
        SelectObject(hdc, (HPEN)GetStockObject(NULL_PEN));
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
        DeleteObject(bgBr);

        // Border
        HPEN bp = CreatePen(PS_SOLID, 1, bdCol);
        SelectObject(hdc, bp);
        SelectObject(hdc, (HBRUSH)GetStockObject(NULL_BRUSH));
        RoundRect(hdc, rc.left, rc.top, rc.right-1, rc.bottom-1, 6, 6);
        DeleteObject(bp);

        // Text: replace all \n with \r\n so DrawTextA wraps correctly (handles 3-line labels)
        char raw[128]={}; GetWindowTextA(di->hwndItem, raw, sizeof(raw));
        std::string txt(raw);
        for (size_t i = 0; i < txt.size(); i++) {
            if (txt[i] == '\n' && (i == 0 || txt[i-1] != '\r')) {
                txt.insert(i, 1, '\r'); i++;
            }
        }

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, pressed ? RGB(190,195,205) : RGB(220,224,235));
        SelectObject(hdc, g_fontNormal);

        // Shrink rect slightly so text doesn't touch border
        RECT tr = { rc.left+4, rc.top+2, rc.right-4, rc.bottom-2 };
        DrawTextA(hdc, txt.c_str(), -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_EDITCONTROL);
        break;
    }

    // Custom-draw the Max Steering Angle trackbar so it blends into the dark
    // panel instead of the classic light-gray channel/tick look: dark channel,
    // white step ticks. Thumb is left to the default renderer.
    case WM_NOTIFY: {
        LPNMHDR nmhdr = (LPNMHDR)lp;
        if (nmhdr->hwndFrom == g_hwndMaxAngleSld && nmhdr->code == NM_CUSTOMDRAW) {
            LPNMCUSTOMDRAW nmcd = (LPNMCUSTOMDRAW)lp;
            if (nmcd->dwDrawStage == CDDS_PREPAINT) {
                // rc here is the whole control's client rect, incl. the
                // padding around the channel/tics that CDDS_ITEMPREPAINT
                // never gets asked to paint - fill it so no default-colored
                // strip is left showing around our custom channel/ticks.
                HBRUSH panelBr = CreateSolidBrush(COL_PANEL);
                FillRect(nmcd->hdc, &nmcd->rc, panelBr);
                DeleteObject(panelBr);
                return CDRF_NOTIFYITEMDRAW;
            }
            if (nmcd->dwDrawStage == CDDS_ITEMPREPAINT) {
                if (nmcd->dwItemSpec == TBCD_CHANNEL) {
                    HBRUSH br = CreateSolidBrush(RGB(22,22,28));
                    FillRect(nmcd->hdc, &nmcd->rc, br);
                    DeleteObject(br);
                    // Draw the step ticks directly on the channel itself
                    // (rather than relying on the separate TBCD_TICS pass
                    // below, whose rect turned out to be unreliable without
                    // a themed/manifested trackbar - it either collapses to
                    // zero height or gets painted over) so they're always
                    // visible at each stop the thumb will snap to.
                    int lo = (int)SendMessage(g_hwndMaxAngleSld, TBM_GETRANGEMIN, 0, 0);
                    int hi = (int)SendMessage(g_hwndMaxAngleSld, TBM_GETRANGEMAX, 0, 0);
                    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255,255,255));
                    HPEN old = (HPEN)SelectObject(nmcd->hdc, pen);
                    for (int i = lo; i <= hi; i++) {
                        int x = nmcd->rc.left + (int)((double)(nmcd->rc.right - nmcd->rc.left) * (i - lo) / (hi - lo));
                        x = std::max((int)nmcd->rc.left + 1, std::min((int)nmcd->rc.right - 1, x));
                        MoveToEx(nmcd->hdc, x, nmcd->rc.top, nullptr);
                        LineTo(nmcd->hdc, x, nmcd->rc.bottom);
                    }
                    SelectObject(nmcd->hdc, old); DeleteObject(pen);
                    return CDRF_SKIPDEFAULT;
                }
                if (nmcd->dwItemSpec == TBCD_TICS) {
                    // Already panel-colored by the CDDS_PREPAINT fill above -
                    // nothing else to draw (see TBCD_CHANNEL branch).
                    return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
            }
        }
        break;
    }

    case WM_HSCROLL: {
        if ((HWND)lp == g_hwndMaxAngleSld) {
            int idx = (int)SendMessage(g_hwndMaxAngleSld, TBM_GETPOS, 0, 0);
            idx = std::max(0, std::min(STEER_ANGLE_STEPS_N - 1, idx));
            g_maxSteerAngleDeg.store((float)STEER_ANGLE_STEPS[idx]);
            UpdateMaxAngleLabel(idx);
            // Persist once the drag settles rather than on every intermediate
            // SB_THUMBTRACK tick - avoids hammering app.ini mid-drag.
            if (LOWORD(wp) != SB_THUMBTRACK && g_cfg) {
                AppSettings as; as.startMinimized = g_startMinimized; as.maxSteerAngleDeg = (float)STEER_ANGLE_STEPS[idx];
                g_cfg->saveAppSettings(as);
            }
        }
        break;
    }

    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case ID_BTN_GAMEPAD: ShellExecuteA(hwnd,"open","control.exe","joy.cpl",nullptr,SW_SHOW); break;
        case ID_BTN_FFB:      if (g_cfg) OpenFfbSettingsWindow(hwnd, g_cfg); break;
        case ID_BTN_WHEEL_HW: if (g_cfg) OpenBoxSettingsWindow(hwnd, g_cfg); break;
        // BS_OWNERDRAW buttons have no built-in check-state tracking (unlike
        // BS_AUTOCHECKBOX) - toggle the real underlying setting and let
        // WM_DRAWITEM re-derive the glyph state from it on repaint.
        case ID_CHK_AUTOSTART:
            SetAutostartEnabled(!IsAutostartEnabled());
            InvalidateRect(GetDlgItem(hwnd, ID_CHK_AUTOSTART), nullptr, TRUE);
            break;
        case ID_CHK_START_MIN: {
            g_startMinimized = !g_startMinimized;
            if (g_cfg) { AppSettings as; as.startMinimized = g_startMinimized; as.maxSteerAngleDeg = g_maxSteerAngleDeg.load(); g_cfg->saveAppSettings(as); }
            InvalidateRect(GetDlgItem(hwnd, ID_CHK_START_MIN), nullptr, TRUE);
            break;
        }
        case ID_BTN_CONSOLE: g_consoleVisible=!g_consoleVisible; ApplyConsoleVisibility(hwnd); break;
        case ID_BTN_MANUAL:  OpenManualDialog(hwnd); break;
        case ID_BTN_DEBUG: {
            g_ffbDebugOn = !g_ffbDebugOn;
            if (g_state) g_state->ffb_debug_on = g_ffbDebugOn;
            SetWindowTextA(g_hwndDebugBtn, g_ffbDebugOn ? "FFB\nDebug ON" : "FFB\nDebug");
            InvalidateRect(g_hwndDebugBtn, nullptr, TRUE);
            if (g_ffbDebugOn) {
                if (!g_consoleVisible) { g_consoleVisible = true; ApplyConsoleVisibility(hwnd); }
                g_state->log("[DBG] Debug logging started - writing to AppData\\SimRacePro\\debug_*.log");
                g_state->log("[DBG] Monitoring: RX gap, TX values, process scan time.");
            } else {
                g_state->log("[DBG] Debug logging stopped.");
            }
            break;
        }
        case ID_BTN_RESET_MAP: {
            if (MessageBoxA(hwnd,"Reset all settings?\n\nThis clears button mapping, button wiring, hardware settings and FFB settings.\nThe wiring and calibration wizards will run on next start.","SimRacePro Custom Driver Software - Reset all Settings",MB_YESNO|MB_ICONWARNING)==IDYES) {
                ConfigManager cfg;
                std::remove(cfg.getConfigPath().c_str());
                std::remove(cfg.getHardwareConfigPath().c_str());
                std::remove(cfg.getFfbConfigPath().c_str());
                std::remove(cfg.getLayoutConfigPath().c_str());
                { std::lock_guard<std::mutex> lk(buttonMapMutex); buttonMap.clear(); for (int i=0;i<16;i++) g_posToPhys[i]=i; }
                { std::lock_guard<std::mutex> lk(g_state->mtx); g_state->buttonMap.clear(); }
                for (int i=0;i<16;i++) SetWindowTextA(g_hwndBtnLabels[i],"-");
                g_state->log("All settings reset. Restart to recalibrate.");
                MessageBoxA(hwnd,"All settings cleared.\nPlease restart SimRacePro to run the wizard.","SimRacePro Custom Driver Software",MB_OK|MB_ICONINFORMATION);
                RECT r={LP_X,LP_Y,LP_X+LP_W,LP_Y+LP_H}; InvalidateRect(hwnd,&r,FALSE);
            } break;
        }
        } break;
    }

    // ── Click-to-reassign: click a wheel button overlay, pick an Xbox button ──
    case WM_LBUTTONDOWN: {
        int mx=(short)LOWORD(lp), my=(short)HIWORD(lp);
        int pos=HitTestBtnOverlay(mx,my);
        if (pos<0) break;
        // Not while a wizard is actively (re)writing the mapping/layout.
        if ((g_hwndCalibDlg&&IsWindow(g_hwndCalibDlg))||(g_hwndWiringDlg&&IsWindow(g_hwndWiringDlg))) break;
        int p2p[16]; GetPosToPhysCopy(p2p);
        int phys=p2p[pos];
        if (phys<0) {
            MessageBoxA(hwnd,"This button position is not wired to a physical wheel button.\n\nUse \"Reset all Settings\" and restart to redo the wiring setup.","SimRacePro Custom Driver Software - Button Mapping",MB_OK|MB_ICONINFORMATION);
            break;
        }
        // Dropdown with every Xbox button; the current assignment is checked.
        USHORT cur=0; bool hasCur=false;
        { std::lock_guard<std::mutex> lk(buttonMapMutex); auto it=buttonMap.find(phys); if (it!=buttonMap.end()){cur=it->second;hasCur=true;} }
        HMENU hm=CreatePopupMenu();
        for (int i=0;i<XBOX_BUTTONS_N;i++)
            AppendMenuA(hm,MF_STRING|((hasCur&&XBOX_BUTTONS[i].val==cur)?MF_CHECKED:0),(UINT_PTR)(i+1),XBOX_BUTTONS[i].name);
        AppendMenuA(hm,MF_SEPARATOR,0,nullptr);
        AppendMenuA(hm,MF_STRING|(!hasCur?MF_CHECKED:0),100,"Not assigned");
        POINT pt={mx,my}; ClientToScreen(hwnd,&pt);
        SetForegroundWindow(hwnd);  // required so the menu closes on an outside click
        int cmd=TrackPopupMenu(hm,TPM_RETURNCMD|TPM_LEFTBUTTON,pt.x,pt.y,0,hwnd,nullptr);
        DestroyMenu(hm);
        if (cmd<=0) break;  // dismissed

        std::map<int,USHORT> bm; { std::lock_guard<std::mutex> lk(buttonMapMutex); bm=buttonMap; }
        std::string logMsg;
        if (cmd==100) {
            if (!hasCur) break;
            bm.erase(phys);
            logMsg = "Button assignment cleared.";
        } else {
            USHORT val=XBOX_BUTTONS[cmd-1].val;
            if (hasCur && val==cur) break;  // unchanged
            // Conflict check: is this Xbox button already on another wheel button?
            int otherPhys=-1; for (auto& kv:bm) if (kv.second==val && kv.first!=phys) { otherPhys=kv.first; break; }
            if (otherPhys>=0) {
                std::string m = std::string("'")+XBOX_BUTTONS[cmd-1].name+"' is already assigned to another wheel button.\n\nOverwrite? The other wheel button will lose its assignment.";
                if (MessageBoxA(hwnd,m.c_str(),"SimRacePro Custom Driver Software - Button Mapping",MB_YESNO|MB_ICONQUESTION)!=IDYES) break;
                bm.erase(otherPhys);
            }
            bm[phys]=val;
            logMsg = "Mapped btn " + std::to_string(phys) + " -> " + XBOX_BUTTONS[cmd-1].name;
        }
        { std::lock_guard<std::mutex> lk(buttonMapMutex); buttonMap=bm; }
        { std::lock_guard<std::mutex> lk(g_state->mtx); g_state->buttonMap=bm; }
        if (g_cfg) g_cfg->saveConfig("",bm);
        g_state->log(logMsg);
        SendMessage(hwnd,WM_APP_BTNMAP_UPDATE,0,0);  // refresh all overlay labels
        break;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp)==HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd,&pt);
            if (HitTestBtnOverlay(pt.x,pt.y)>=0) { SetCursor(LoadCursor(nullptr,IDC_HAND)); return TRUE; }
        }
        return DefWindowProcA(hwnd,msg,wp,lp);
    }

    case WM_SIZE:
        if (wp==SIZE_MINIMIZED) {
            ShowWindow(hwnd,SW_HIDE);
            char title[128]={}; GetWindowTextA(hwnd,title,sizeof(title)); strcpy_s(g_nid.szTip,title);
            if (g_trayAdded) Shell_NotifyIconA(NIM_MODIFY,&g_nid);
        } else InvalidateRect(hwnd,nullptr,FALSE);
        break;

    case WM_TRAYICON:
        if (lp==WM_LBUTTONUP||lp==WM_LBUTTONDBLCLK) { ShowMainWindow(hwnd); }
        else if (lp==WM_RBUTTONUP) {
            HMENU hm=CreatePopupMenu(); char title[128]={}; GetWindowTextA(hwnd,title,sizeof(title));
            AppendMenuA(hm,MF_STRING|MF_GRAYED,0,title); AppendMenuA(hm,MF_SEPARATOR,0,nullptr);
            AppendMenuA(hm,MF_STRING,ID_TRAY_OPEN,"Open"); AppendMenuA(hm,MF_STRING,ID_TRAY_EXIT,"Exit");
            SetForegroundWindow(hwnd); POINT pt; GetCursorPos(&pt);
            int cmd=TrackPopupMenu(hm,TPM_RETURNCMD|TPM_RIGHTBUTTON|TPM_BOTTOMALIGN,pt.x,pt.y,0,hwnd,nullptr);
            DestroyMenu(hm);
            if (cmd==ID_TRAY_OPEN) ShowMainWindow(hwnd); else if (cmd==ID_TRAY_EXIT) PostMessage(hwnd,WM_CLOSE,0,0);
        }
        break;

    case WM_CLOSE: {
        // The backend posts WM_CLOSE itself for the Start+Back reconfig-hold
        // trigger (physical-button reset, not a user "quit" request) - skip
        // the prompt for that case and close immediately.
        if (g_state && g_state->auto_close_requested.exchange(false)) { DestroyWindow(hwnd); return 0; }
        // Otherwise covers both the title-bar [X]/Alt+F4 and the tray icon's
        // "Exit" item (which just posts WM_CLOSE here too) - one confirmation
        // for every user-initiated way to close the app.
        switch (ShowExitConfirmDialog(hwnd)) {
        case EXITCONFIRM_EXIT:     DestroyWindow(hwnd); break;
        case EXITCONFIRM_MINIMIZE: ShowWindow(hwnd, SW_MINIMIZE); break;  // WM_SIZE handler hides it to the tray
        case EXITCONFIRM_CANCEL:   default: break;
        }
        return 0;
    }

    default: return DefWindowProcA(hwnd,msg,wp,lp);

    case WM_DESTROY: {
        // free cached GDI objects
        if (g_brGreen)  { DeleteObject(g_brGreen);  g_brGreen=nullptr; }
        if (g_brYellow) { DeleteObject(g_brYellow); g_brYellow=nullptr; }
        if (g_brRed)    { DeleteObject(g_brRed);    g_brRed=nullptr; }
        if (g_brBlue)   { DeleteObject(g_brBlue);   g_brBlue=nullptr; }
        if (g_brDark)   { DeleteObject(g_brDark);   g_brDark=nullptr; }
        if (g_brPanel)  { DeleteObject(g_brPanel);  g_brPanel=nullptr; }
        RemoveTrayIcon(); g_state->running = false;
        // These are all parentless top-level windows (WS_EX_TOOLWINDOW, parent=
        // nullptr) so Windows doesn't tear them down with the main window on its
        // own - previously only the Manual dialog was cleaned up here, leaving
        // Console/FFB/Box Hardware Settings open until process exit (see 4.9).
        if (g_hwndManualDlg  && IsWindow(g_hwndManualDlg))  { DestroyWindow(g_hwndManualDlg);  g_hwndManualDlg=nullptr; }
        if (g_hwndConsoleDlg && IsWindow(g_hwndConsoleDlg)) { DestroyWindow(g_hwndConsoleDlg); g_hwndConsoleDlg=nullptr; }
        if (g_hwndFfbDlg     && IsWindow(g_hwndFfbDlg))     { DestroyWindow(g_hwndFfbDlg);     g_hwndFfbDlg=nullptr; }
        if (g_hwndBoxPanel   && IsWindow(g_hwndBoxPanel))   { DestroyWindow(g_hwndBoxPanel);   g_hwndBoxPanel=nullptr; }
        if (g_hwndCalibDlg   && IsWindow(g_hwndCalibDlg))   { DestroyWindow(g_hwndCalibDlg);   g_hwndCalibDlg=nullptr; }
        if (g_hwndWiringDlg  && IsWindow(g_hwndWiringDlg))  { DestroyWindow(g_hwndWiringDlg);  g_hwndWiringDlg=nullptr; }
        if(g_fontNormal)   { DeleteObject(g_fontNormal);   g_fontNormal   = nullptr; }
        if(g_fontBold)     { DeleteObject(g_fontBold);     g_fontBold     = nullptr; }
        if(g_fontSmall)    { DeleteObject(g_fontSmall);    g_fontSmall    = nullptr; }
        if(g_fontLog)      { DeleteObject(g_fontLog);      g_fontLog      = nullptr; }
        if(g_fontBtn)      { DeleteObject(g_fontBtn);      g_fontBtn      = nullptr; }
        if(g_fontBtnLarge) { DeleteObject(g_fontBtnLarge); g_fontBtnLarge = nullptr; }
        if(g_bgBrush)      { DeleteObject(g_bgBrush);      g_bgBrush      = nullptr; }
        if(g_panelBrush)   { DeleteObject(g_panelBrush);   g_panelBrush   = nullptr; }
        if(g_logBrush)     { DeleteObject(g_logBrush);     g_logBrush     = nullptr; }
        if(g_wheelImage)   { DeleteObject(g_wheelImage);   g_wheelImage   = nullptr; }
        PostQuitMessage(0); break;
    }
    }
    return 0;
}

// ── CreateGuiWindow ────────────────────────────────────────────────────────────
bool CreateGuiWindow(HINSTANCE hInst, int nCmdShow, GuiState& state) {
    g_state=&state; g_hInst=hInst;
    InitCommonControls();

    // Load "start minimized" before the window (and its WM_CREATE checkbox init)
    // is created - the backend's ConfigManager (g_cfg) isn't constructed yet at
    // this point, so use a throwaway local instance (same on-disk file).
    { ConfigManager tmpCfg; AppSettings as; tmpCfg.loadAppSettings(as); g_startMinimized = as.startMinimized; g_maxSteerAngleDeg.store(as.maxSteerAngleDeg); }

    WNDCLASSEXA wc={}; wc.cbSize=sizeof(wc); wc.style=CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc=WndProc; wc.hInstance=hInst; wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName="SimRaceProWindow"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    RegisterClassExA(&wc);
    int sx=GetSystemMetrics(SM_CXSCREEN), sy=GetSystemMetrics(SM_CYSCREEN);
    // WS_CLIPCHILDREN: WM_PAINT below blits the whole double-buffered client
    // area in one BitBlt. Without clipping, that blit covers the child controls
    // too, and InvalidateRect on the parent also invalidates the children under
    // it - so every WM_APP_INPUT_UPDATE (telemetry rate, i.e. constantly while
    // the Box is connected) repainted the Max Steering Angle slider and its
    // label, making them flicker. Clipping keeps the parent's paint out of the
    // child rects; every visible child paints its own opaque background
    // (WM_CTLCOLORSTATIC / WM_DRAWITEM / the trackbar's custom draw).
    HWND hwnd=CreateWindowExA(0,"SimRaceProWindow","SimRacePro Custom Driver Software v" VER_STRING,
        (WS_OVERLAPPEDWINDOW&~WS_MAXIMIZEBOX&~WS_THICKFRAME)|WS_CLIPCHILDREN,
        (sx-MAIN_W)/2,(sy-MAIN_H)/2,MAIN_W,MAIN_H,nullptr,nullptr,hInst,nullptr);
    if (!hwnd) return false;
    state.hwnd=hwnd;
    ShowWindow(hwnd, g_startMinimized ? SW_HIDE : nCmdShow);
    UpdateWindow(hwnd);
    return true;
}
