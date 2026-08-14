// SimRacePro.cpp  –  Backend: globals, language, serial, telemetry readers,
//                    virtual gamepad, config manager, FFB logic, backend thread.
// UTF-8 encoding required.
#include "SimRacePro.h"
#include <initguid.h>
#include <devguid.h>    // GUID_DEVCLASS_PORTS
#include <setupapi.h>

// ── Globals ───────────────────────────────────────────────────────────────────
LanguageManager*        lang = nullptr;
std::map<int, USHORT>   buttonMap;
std::mutex              buttonMapMutex;
// Identity default: behaves exactly like the pre-layout builds until the
// wiring wizard (or layout.ini) says otherwise. Guarded by buttonMapMutex.
int                     g_posToPhys[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
TelemetryData           currentTelemetry;
std::mutex              telemetryMutex;
std::atomic<bool>       dataReceivedThisFrame(false);
std::atomic<float>      lastTorque{ 127.0f };  // atomic for thread-safe cross-thread read
static std::string      g_detectedWheelVer;  // updated when Box sends VER:BOX:...:WHEEL:...
FfbSettings             g_ffbSettings;
std::mutex              g_ffbSettingsMutex;
BoxSettings             g_boxSettings;
BoxPinConfig            g_boxPinConfig;
std::atomic<float>      g_maxSteerAngleDeg{360.0f};
ConfigManager*          g_cfg = nullptr;

float clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// isProcessRunning: nutzt einen geteilten Snapshot der einmal pro Sekunde
// erneuert wird. Vermeidet N separate CreateToolhelp32Snapshot-Aufrufe
// (einer pro Spiel) die den Serial-Thread blockieren wuerden.
static HANDLE   s_procSnap    = INVALID_HANDLE_VALUE;
static PROCESSENTRY32W s_procEntry = {};
static std::vector<std::wstring> s_runningExes;  // cached list for this tick

static void refreshProcessSnapshot() {
    s_runningExes.clear();
    if (s_procSnap != INVALID_HANDLE_VALUE) { CloseHandle(s_procSnap); s_procSnap = INVALID_HANDLE_VALUE; }
    s_procSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s_procSnap == INVALID_HANDLE_VALUE) return;
    s_procEntry.dwSize = sizeof(s_procEntry);
    if (Process32FirstW(s_procSnap, &s_procEntry))
        do { s_runningExes.push_back(s_procEntry.szExeFile); }
        while (Process32NextW(s_procSnap, &s_procEntry));
    CloseHandle(s_procSnap); s_procSnap = INVALID_HANDLE_VALUE;
}

BOOL isProcessRunning(const wchar_t* name) {
    for (const auto& exe : s_runningExes)
        if (_wcsicmp(exe.c_str(), name) == 0) return TRUE;
    return FALSE;
}

// ── LanguageManager ───────────────────────────────────────────────────────────
LanguageManager::LanguageManager() {
    bool de = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN;
    auto a = [&](const char* k, const char* en, const char* deStr) { strings[k] = de ? deStr : en; };
    a("autodetect_start",    "=== AUTO-DETECTION STARTED ===",       "=== AUTO-DETECTION GESTARTET ===");
    a("searching_simracepro","Searching for SimRacePro wheel...",     "Suche nach SimRacePro Lenkrad...");
    a("testing_port",        "Testing ",                              "Teste ");
    a("found",               "FOUND!",                                "GEFUNDEN!");
    a("no_response",         "No response",                           "Keine Antwort");
    a("wheel_detected",      "Wheel successfully detected on ",       "Lenkrad erfolgreich erkannt auf ");
    a("no_wheel_found",      "No wheel found!",                       "Kein Lenkrad gefunden!");
    a("ports_found",         " COM port(s) found",                    " COM-Port(s) gefunden");
}
std::string LanguageManager::get(const std::string& k) const {
    auto it = strings.find(k);
    return it != strings.end() ? it->second : "[?" + k + "]";
}


// ── CRC-8 (Polynom 0x07) ─────────────────────────────────────────────────────
// SYNC NOTE: This table is identical to the one in both Arduino sketches.
// If you ever need to change the polynomial, update all three copies.
static const uint8_t CRC8_TABLE_PC[256] = {
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
static uint8_t crc8_pc(const uint8_t* data, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) crc = CRC8_TABLE_PC[crc ^ data[i]];
    return crc;
}
// ── Debug file logger ─────────────────────────────────────────────────────────
static std::ofstream s_debugFile;

static void debugOpenFile() {
    if (s_debugFile.is_open()) return;
    // Path: %AppData%\SimRacePro\debug_YYYY-MM-DD_HH-MM-SS.log
    PWSTR p = nullptr;
    std::string dir = ".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &p))) {
        int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, NULL, 0, NULL, NULL);
        std::string s(n - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, p, -1, &s[0], n, NULL, NULL);
        CoTaskMemFree(p);
        dir = s + "\\SimRacePro";
        CreateDirectoryA(dir.c_str(), NULL);
    }
    SYSTEMTIME st; GetLocalTime(&st);
    char fname[64];
    sprintf_s(fname, "\\debug_%04d-%02d-%02d_%02d-%02d-%02d.log",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    s_debugFile.open(dir + fname, std::ios::out | std::ios::trunc);
    if (s_debugFile.is_open())
        s_debugFile << "SimRacePro Debug Log " << fname + 1 << "\n"
                    << "Format: [HH:MM:SS.mmm] <message>\n\n";
}

static void debugClose() { if (s_debugFile.is_open()) s_debugFile.close(); }

static std::string debugTs() {
    SYSTEMTIME st; GetLocalTime(&st);
    char buf[20];
    sprintf_s(buf, "[%02d:%02d:%02d.%03d]", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

static void debugLog(const std::string& msg) {
    if (!s_debugFile.is_open()) return;
    s_debugFile << debugTs() << " " << msg << "\n";
    s_debugFile.flush();
}



// Parse a "major.minor.patch" string. Returns false for anything else (empty
// marker files, HTML error pages, ...) so callers can treat it as "unknown".
static bool parseVerTriple(const char* s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    return sscanf_s(s, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3;
}

// ── GitHub Update Check ────────────────────────────────────────────────────────
// version.txt on GitHub should contain just the version string e.g. "3.2.0"
// URLs are defined in SimRaceProDefs.h – update them to match your repository.

static void checkForUpdatesAsync(HWND hwndMain) {
    std::thread([hwndMain]() {
        // Fetch version.txt via WinHTTP (HTTPS)
        HINTERNET hSession = WinHttpOpen(L"SimRacePro/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return;

        HINTERNET hConnect = WinHttpConnect(hSession, UPDATE_CHECK_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return; }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", UPDATE_CHECK_PATH,
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return; }

        bool sent = WinHttpSendRequest(hRequest,
            WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(hRequest, nullptr);

        // A 404/redirect/error still returns a body (usually an HTML error
        // page) that just happens to differ from VER_STRING - without this
        // check that shows up as a bogus "update available" popup with HTML
        // as the version number.
        if (sent) {
            DWORD statusCode = 0, size = sizeof(statusCode);
            if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX)
                || statusCode != 200)
                sent = false;
        }

        std::string body;
        if (sent) {
            char buf[256]; DWORD read = 0;
            while (WinHttpReadData(hRequest, buf, sizeof(buf)-1, &read) && read > 0) {
                buf[read] = '\0';
                body += buf;
            }
        }
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);

        if (body.empty()) return;
        // Trim whitespace/newlines
        while (!body.empty() && (body.back()=='\r'||body.back()=='\n'||body.back()==' ')) body.pop_back();

        // Compare numerically (major.minor.patch) - a plain != would also fire
        // while the local build is AHEAD of the published version.txt (e.g.
        // right after a version bump, before the release is uploaded) and then
        // offer a "new" version that is actually older. Unparseable bodies
        // (error pages etc.) are treated as "no update".
        int remote[3], local[3];
        bool remoteNewer = parseVerTriple(body.c_str(), remote) && parseVerTriple(VER_STRING, local)
            && std::lexicographical_compare(local, local + 3, remote, remote + 3);
        if (remoteNewer) {
            // Allocate a string on the heap – freed in the WndProc handler
            std::string* newVer = new std::string(body);
            // if window already closed, free to avoid leak
            if (!PostMessage(hwndMain, WM_APP_UPDATE_AVAIL, 0, (LPARAM)newVer))
                delete newVer;
        }
    }).detach();
}

// ── SerialPort ────────────────────────────────────────────────────────────────
bool SerialPort::open(const char* portName, int baudRate) {
    hSerial = CreateFileA(portName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hSerial == INVALID_HANDLE_VALUE) return false;
    DCB dcb = {}; dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hSerial, &dcb)) { CloseHandle(hSerial); hSerial = INVALID_HANDLE_VALUE; return false; }
    dcb.BaudRate = baudRate; dcb.ByteSize = 8; dcb.StopBits = ONESTOPBIT; dcb.Parity = NOPARITY;
    dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fRtsControl = RTS_CONTROL_DISABLE;
    if (!SetCommState(hSerial, &dcb)) { CloseHandle(hSerial); hSerial = INVALID_HANDLE_VALUE; return false; }
    // ReadIntervalTimeout=0, ReadTotalTimeoutMultiplier=0, ReadTotalTimeoutConstant=20ms
    // → ReadFile blockiert maximal 20ms wenn keine Daten kommen
    COMMTIMEOUTS to = { 0, 0, 20, 1, 10 };
    SetCommTimeouts(hSerial, &to);
    SetupComm(hSerial, 4096, 4096);
    PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

// enumerateComPorts: liefert die tatsaechlich vorhandenen COM-Ports statt
// COM1-30 blind durchzuprobieren. Primaer via SetupAPI (Ports-Geraeteklasse,
// liefert auch den Friendly-Name), Fallback via Registry SERIALCOMM.
// Ports deren Friendly-Name nach einem USB-Seriell-Adapter aussieht
// (Arduino, CH340, CP210x, FTDI, ...) werden nach vorne sortiert, damit die
// Box moeglichst frueh gefunden wird und fremde Geraete (z.B. Onboard-UARTs)
// erst zuletzt einen PING abbekommen.
struct ComPortInfo { int number; std::string friendlyName; bool likelyUsbSerial; };

static std::vector<ComPortInfo> enumerateComPorts() {
    std::vector<ComPortInfo> ports;
    HDEVINFO devs = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (devs != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA info = {}; info.cbSize = sizeof(info);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &info); i++) {
            // "PortName" aus dem Geraete-Registry-Key (filtert LPTx aus)
            char portName[64] = {};
            HKEY hKey = SetupDiOpenDevRegKey(devs, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (hKey == INVALID_HANDLE_VALUE) continue;
            DWORD type = 0, size = sizeof(portName) - 1;
            LONG rc = RegQueryValueExA(hKey, "PortName", nullptr, &type, (LPBYTE)portName, &size);
            RegCloseKey(hKey);
            if (rc != ERROR_SUCCESS || type != REG_SZ || _strnicmp(portName, "COM", 3) != 0) continue;
            int num = atoi(portName + 3);
            if (num <= 0) continue;
            char friendly[256] = {};
            SetupDiGetDeviceRegistryPropertyA(devs, &info, SPDRP_FRIENDLYNAME, nullptr,
                                              (PBYTE)friendly, sizeof(friendly) - 1, nullptr);
            std::string fn = friendly;
            std::string lower = fn;
            for (auto& c : lower) c = (char)tolower((unsigned char)c);
            static const char* usbHints[] = { "arduino", "ch340", "ch341", "ch910", "cp210", "ftdi", "ft232", "usb" };
            bool usb = false;
            for (const char* h : usbHints)
                if (lower.find(h) != std::string::npos) { usb = true; break; }
            ports.push_back({ num, fn, usb });
        }
        SetupDiDestroyDeviceInfoList(devs);
    }
    // Fallback falls SetupAPI nichts liefert: HKLM\HARDWARE\DEVICEMAP\SERIALCOMM
    // (Werte-Daten sind die Portnamen, z.B. "COM5")
    if (ports.empty()) {
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                          0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            for (DWORD i = 0;; i++) {
                char name[256]; DWORD nameLen = sizeof(name);
                char value[64] = {}; DWORD valLen = sizeof(value) - 1; DWORD type = 0;
                if (RegEnumValueA(hKey, i, name, &nameLen, nullptr, &type,
                                  (LPBYTE)value, &valLen) != ERROR_SUCCESS) break;
                if (type != REG_SZ || _strnicmp(value, "COM", 3) != 0) continue;
                int num = atoi(value + 3);
                // No friendly name available here, so we can't tell USB
                // adapters apart - treat all as likelyUsbSerial so the scan
                // waits out a possible DTR-reset boot instead of skipping
                // the Box after 1.2s (slow scan beats no connection).
                if (num > 0) ports.push_back({ num, "", true });
            }
            RegCloseKey(hKey);
        }
    }
    std::sort(ports.begin(), ports.end(), [](const ComPortInfo& a, const ComPortInfo& b) {
        if (a.likelyUsbSerial != b.likelyUsbSerial) return a.likelyUsbSerial;
        return a.number < b.number;
    });
    return ports;
}

std::string SerialPort::autoDetect(int baudRate, std::string& outBoxVer, std::string& outWheelVer) {
    outBoxVer = "unknown"; outWheelVer = "unknown";
    debugLog(lang->get("autodetect_start"));
    debugLog(lang->get("searching_simracepro"));
    std::vector<ComPortInfo> comPorts = enumerateComPorts();
    debugLog(std::to_string(comPorts.size()) + lang->get("ports_found"));
    for (const ComPortInfo& pi : comPorts) {
        std::string port = "\\\\.\\COM" + std::to_string(pi.number);
        if (!open(port.c_str(), baudRate)) continue;
        debugLog(lang->get("testing_port") + port +
                 (pi.friendlyName.empty() ? "" : " (" + pi.friendlyName + ")"));
        PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
        bool ok = false;
        uint8_t okBuf[128] = {}; DWORD okGot = 0;
        // Send PINGs for up to 10s total to cover:
        //   bootloader (~1.5s) + wheel version wait (max 4s) + PING wait (3s) = ~8.5s
        for (int a = 0; a < 34 && !ok; a++) {
            const char* ping = "PING\n";
            DWORD written;
            WriteFile(hSerial, ping, 5, &written, NULL);
            Sleep(300);
            uint8_t rbuf[256]; DWORD got = 0;
            (void)ReadFile(hSerial, rbuf, sizeof(rbuf), &got, NULL);
            // Scan for 'O','K' sequence anywhere in buffer
            for (DWORD j = 0; j + 1 < got; j++) {
                if (rbuf[j] == 'O' && rbuf[j+1] == 'K') {
                    ok = true;
                    if (got > j + 2) {
                        DWORD copyLen = got - j - 2;
                        if (copyLen > sizeof(okBuf)) copyLen = sizeof(okBuf);
                        memcpy(okBuf, rbuf + j + 2, copyLen);
                        okGot = copyLen;
                    }
                    break;
                }
            }
            // Opening the port asserts DTR, which auto-resets the Box (same
            // mechanism as the close()+open() reboot in the hard-reconnect
            // path). After that reset the Box is legitimately silent for up
            // to ~6s: bootloader (~2s) + setup() Phase 1 waiting for the
            // Wheel version (up to 4s). PINGs sent meanwhile sit in its RX
            // buffer and get answered the moment Phase 2 starts. So a port
            // that looks like a USB-serial adapter gets ~7s of silence
            // before we give up on it; only ports that don't look like an
            // Arduino are skipped after ~1.2s of no response.
            int silentLimit = pi.likelyUsbSerial ? 22 : 3;
            if (!ok && a >= silentLimit && got == 0) break;
        }
        if (ok) {
            // Read version string "VER:BOX:x.x.x:WHEEL:x.x.x\n"
            // May already be in okBuf, or arrive shortly after OK
            uint8_t vbuf[256]; DWORD vgot = 0;
            // Copy what we already have from the OK read
            memcpy(vbuf, okBuf, okGot);
            vgot = okGot;
            // Wait a bit for the VER string to arrive, then read remaining bytes
            Sleep(150);
            DWORD extra = 0;
            if (vgot < sizeof(vbuf) - 1)
                (void)ReadFile(hSerial, vbuf + vgot, (DWORD)(sizeof(vbuf) - vgot - 1), &extra, NULL);
            vgot += extra;
            if (vgot >= sizeof(vbuf)) vgot = sizeof(vbuf) - 1;
            vbuf[vgot] = '\0';
            // Parse "VER:BOX:x.x.x:WHEEL:x.x.x"
            const char* verPtr = strstr((char*)vbuf, "VER:BOX:");
            if (verPtr) {
                verPtr += 8;  // skip "VER:BOX:"
                const char* sep = strstr(verPtr, ":WHEEL:");
                if (sep) {
                    outBoxVer   = std::string(verPtr, sep - verPtr);
                    outWheelVer = std::string(sep + 7);
                    // trim non-printable characters from both ends (catches 0xAA packet bytes)
                    for (auto& s : {&outBoxVer, &outWheelVer}) {
                        while (!s->empty() && (uint8_t)s->front() < 32) s->erase(s->begin());
                        while (!s->empty() && (uint8_t)s->back()  < 32) s->pop_back();
                        // strip any remaining non-printable bytes inside the string
                        s->erase(std::remove_if(s->begin(), s->end(),
                            [](unsigned char c){ return c < 32 || c > 126; }), s->end());
                    }
                }
            }
            PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
            debugLog(lang->get("found"));
            debugLog(lang->get("wheel_detected") + port);
            return port;
        }
        debugLog(lang->get("no_response"));
        close();
    }
    debugLog(lang->get("no_wheel_found"));
    return "";
}

// readPacket: liest ein 10-Byte-Paket [0xAA ... CRC] aus dem RX-Buffer.
// CRC-8 ueber Bytes [1]..[8]. Verwirft Pakete mit falscher CRC.
bool SerialPort::readPacket(uint8_t* buf9) {
    if (hSerial == INVALID_HANDLE_VALUE) return false;
    // using member vars (rxBuf, rxLen, rxSynced) instead of statics
    // → readPacket is now reentrant per SerialPort instance
    auto& synced = rxSynced;

    // Neue Bytes nachladen
    if (rxLen < 128) {
        DWORD got = 0;
        (void)ReadFile(hSerial, rxBuf + rxLen, (DWORD)(128 - rxLen), &got, NULL);
        rxLen += (int)got;
    }

    // Sync-Byte suchen – aber VER-Strings abfangen bevor sie verworfen werden
    if (!synced) {
        int start = 0;
        while (start < rxLen && rxBuf[start] != 0xAA) {
            // Check for VER: string
            if (rxBuf[start] == 'V' && rxLen - start >= 8) {
                // Scan for newline to extract full line
                int end = start + 1;
                while (end < rxLen && rxBuf[end] != '\n') end++;
                if (end < rxLen) {  // full line present
                    char line[64] = {};
                    int  len = (end - start) < 63 ? (end - start) : 63;
                    memcpy(line, rxBuf + start, len);
                    line[len] = '\0';
                    const char* verPtr = strstr(line, "VER:BOX:");
                    if (verPtr) {
                        const char* sep = strstr(verPtr + 8, ":WHEEL:");
                        if (sep) {
                            g_detectedWheelVer = std::string(sep + 7);
                            while (!g_detectedWheelVer.empty() &&
                                   (g_detectedWheelVer.back()=='\r'||g_detectedWheelVer.back()=='\n'))
                                g_detectedWheelVer.pop_back();
                        }
                    }
                    start = end + 1;  // skip past newline
                    continue;
                }
            }
            start++;
        }
        if (start > 0) { memmove(rxBuf, rxBuf + start, rxLen - start); rxLen -= start; }
        if (rxLen == 0) return false;
        synced = true;
    }

    if (rxLen < 10) return false;  // noch nicht genug Bytes

    // CRC pruefen: CRC-8 von Bytes [1]..[8] muss gleich Byte [9] sein
#pragma warning(suppress: 6385)  // rxLen >= 10 proven by guard above
    if (crc8_pc(rxBuf+1, 8) != rxBuf[9]) {
        // Korruptes Paket: sync verloren, nächstes 0xAA suchen
        synced = false;
        memmove(rxBuf, rxBuf + 1, rxLen - 1);
        rxLen--;
        return false;
    }

    // Gültiges Paket: kopieren und Buffer verschieben
    memcpy(buf9, rxBuf, 10);
    memmove(rxBuf, rxBuf + 10, rxLen - 10);
    rxLen  -= 10;
    synced  = (rxLen > 0 && rxBuf[0] == 0xAA);
    return true;
}

// purgeRx: wirft alle gepufferten RX-Daten weg (Windows-Buffer + interner rxBuf).
// Aufruf vor einer Kalibrierungs-Erkennungsschleife, damit keine alten Pakete
// die Erkennung verzögern.
void SerialPort::purgeRx() {
    if (hSerial == INVALID_HANDLE_VALUE) return;
    PurgeComm(hSerial, PURGE_RXCLEAR);
    rxLen     = 0;
    rxSynced  = false;
}

// writePacket: sendet ein 7-Byte-Paket [0xBB torque rumble packed gear speed CRC]
//   packed: bit7-2=rpmPct/4, bit1=blink, bit0=gameActive
//   gear:   0=R, 1=N, 2=1st, 3=2nd, ...
//   speed:  km/h, clamped to 0-255 (Wheel-Display, siehe CODE_REVIEW.md 1.1)
bool SerialPort::writePacket(uint8_t torque, uint8_t rumble, uint8_t rpmPct, bool blink, bool gameActive, int8_t gear, uint8_t speed) {
    if (hSerial == INVALID_HANDLE_VALUE) return false;
    uint8_t packed   = (uint8_t)(((rpmPct / 4) & 0x3F) << 2) | (blink ? 0x02 : 0x00) | (gameActive ? 0x01 : 0x00);
    uint8_t gearByte = (gear < 0) ? 0 : (uint8_t)(gear + 1);  // -1=R→0, 0=N→1, 1→2 ...
    uint8_t d5[5]    = { torque, rumble, packed, gearByte, speed };
    uint8_t pkt[7]   = { 0xBB, torque, rumble, packed, gearByte, speed, crc8_pc(d5, 5) };
    DWORD n;
    return WriteFile(hSerial, pkt, 7, &n, NULL) && n == 7;
}

// sendSpringConfig: sendet Spring-Parameter zur Box
// Protokoll: [0xBC][startAngle][fullAngle][strength][CRC]
//   startAngle : 0-30  (Grad, Deadzone)
//   fullAngle  : 30-255 (Grad, bei dem volle Kraft erreicht wird, /1 = Grad)
//   strength   : 0-200 (springStrength * 100)
//   CRC        : XOR von Bytes [1]..[3]
bool SerialPort::sendSpringConfig(const FfbSettings& s) {
    if (hSerial == INVALID_HANDLE_VALUE) return false;
    uint8_t startA    = (uint8_t)clamp(s.springStartAngle,  0.0f,  90.0f);
    uint8_t fullA     = (uint8_t)clamp(s.springFullAngle,  10.0f, 180.0f);
    uint8_t strV      = (uint8_t)clamp(s.springStrength * 100.0f, 0.0f, 150.0f);
    uint8_t pedalThr  = (uint8_t)clamp(s.pedalRumbleThr, 0.0f, 255.0f);
    uint8_t pedalStr  = (uint8_t)clamp(s.pedalRumbleStr * 100.0f, 0.0f, 150.0f);
    uint8_t d5[5]     = { startA, fullA, strV, pedalThr, pedalStr };
    uint8_t pkt[7]    = { 0xBC, startA, fullA, strV, pedalThr, pedalStr, crc8_pc(d5, 5) };
    DWORD n;
    return WriteFile(hSerial, pkt, 7, &n, NULL) && n == 7;
}

// sendBoxSettings: sends hardware/motor config to Box (8-byte 0xBD packet).
// Format: [0xBD][simSetup][flags][motorMaxPwm][minL][minR][rampStep][CRC8]
//   flags: bit0=pedalRumble bit1=handbrake bit2=clutch
//          bit3=shifter bit4=onlyWheel bit5=stallProtection
//          bit6=invertSteering bit7=invertForceFeedback
bool SerialPort::sendBoxSettings(const BoxSettings& s) {
    if (hSerial == INVALID_HANDLE_VALUE) return false;
    uint8_t flags = 0;
    if (s.hasPedalRumble)       flags |= 0x01;
    if (s.hasHandbrake)         flags |= 0x02;
    if (s.hasClutch)            flags |= 0x04;
    if (s.hasShifter)           flags |= 0x08;
    if (s.onlyWheel)            flags |= 0x10;
    if (s.stallProtection)      flags |= 0x20;
    if (s.invertSteering)       flags |= 0x40;
    if (s.invertForceFeedback)  flags |= 0x80;
    uint8_t d[6] = { s.simSetup, flags, s.motorMaxPwm,
                     s.motorMinPwmLeft, s.motorMinPwmRight, s.softRampStep };
    uint8_t pkt[8] = { 0xBD, d[0], d[1], d[2], d[3], d[4], d[5], crc8_pc(d, 6) };
    DWORD n;
    return WriteFile(hSerial, pkt, 8, &n, NULL) && n == 8;
}

// sendPinConfig: sends remapped peripheral pins to Box (9-byte 0xBE packet).
// Format: [0xBE][acc][brk][vib][vib2][clutch][shifterX][shifterY][CRC8]
bool SerialPort::sendPinConfig(const BoxPinConfig& p) {
    if (hSerial == INVALID_HANDLE_VALUE) return false;
    uint8_t d[7] = { p.accPin, p.brkPin, p.vibPin, p.vib2Pin,
                     p.clutchPin, p.shifterXPin, p.shifterYPin };
    uint8_t pkt[9] = { 0xBE, d[0], d[1], d[2], d[3], d[4], d[5], d[6], crc8_pc(d, 7) };
    DWORD n;
    return WriteFile(hSerial, pkt, 9, &n, NULL) && n == 9;
}

bool SerialPort::writeLine(const std::string& line) {
    if (hSerial == INVALID_HANDLE_VALUE) return false;
    DWORD n;
    if (WriteFile(hSerial, line.c_str(), (DWORD)line.size(), &n, NULL)) { FlushFileBuffers(hSerial); return true; }
    return false;
}

void SerialPort::close() {
    if (hSerial != INVALID_HANDLE_VALUE) { PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR); CloseHandle(hSerial); hSerial = INVALID_HANDLE_VALUE; }
    // PurgeComm above only clears the OS-level driver buffer - rxBuf/rxLen/
    // rxSynced are readPacket()'s own framing state and survived untouched
    // across close()+open() until now. A reconnect could carry stale
    // pre-disconnect bytes into the new connection, mixing them with freshly
    // read bytes in the same buffer (self-healing via the CRC-mismatch
    // resync, but wastes a cycle and risks acting on a bogus 10 leftover
    // bytes right after reconnect). Reset framing state so every reconnect
    // starts from a clean slate.
    rxLen    = 0;
    rxSynced = false;
}

// ── F1Reader ──────────────────────────────────────────────────────────────────
F1Reader::F1Reader() {
    // Socket is created in start() to avoid blocking port 20777 permanently
}
void F1Reader::setActive(bool a)  { externallyActive = a; }
bool F1Reader::isActive()  const  { return externallyActive; }
void F1Reader::start() {
    externallyActive = true;
    if (sock == INVALID_SOCKET) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock != INVALID_SOCKET) {
            int reuse = 1; setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
            sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(20777);
            if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { closesocket(sock); sock = INVALID_SOCKET; return; }
            u_long mode = 1; ioctlsocket(sock, FIONBIO, &mode);
            int buf = 65536; setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&buf, sizeof(buf));
        }
    }
    running = true; thread = std::thread(&F1Reader::loop, this); SetThreadPriority(thread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
}
void F1Reader::stop()  { externallyActive = false; running = false; if (thread.joinable()) thread.join(); if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; } }
bool F1Reader::checkTimeout() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lastDataTime).count() > DISCONNECT_TIMEOUT_MS;
}
void F1Reader::loop() {
    char buf[2048];
    while (running) {
        if (!externallyActive || sock == INVALID_SOCKET) { Sleep(5); continue; }
        // select() avoids busy-wait, drops CPU usage to ~0%%
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv{ 0, 20000 };  // 20ms timeout
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
        {
            int len = recv(sock, buf, sizeof(buf), 0);
            if (len > 0) {
                lastDataTime = std::chrono::steady_clock::now();
                auto* hdr = (PacketHeader*)buf;
                if (hdr->m_packetId == 6) {
                    // Header sizes by format year:
                    // 2018:      21 bytes (no secondaryPlayerCarIndex)
                    // 2019:      23 bytes (no secondaryPlayerCarIndex)
                    // 2020-2022: 24 bytes (adds secondaryPlayerCarIndex)
                    // 2023-2024: 29 bytes (adds extra fields)
                    int hdrSize = (hdr->m_packetFormat >= 2023) ? 29
                                : (hdr->m_packetFormat >= 2020) ? 24
                                : (hdr->m_packetFormat >= 2019) ? 23 : 21;
                    // Real per-car CarTelemetryData size on the wire - NOT
                    // sizeof(local CarTelemetryData), which only mirrors the
                    // first 18 bytes of the real struct that we actually read.
                    // Using sizeof(local struct) as the stride pointed at the
                    // wrong car for every m_playerCarIndex > 0 (see 1.8).
                    // Sizes confirmed against the official Codemasters/EA UDP
                    // specs: 2019 66B (uint16 tyre temps, no revLightsBitValue),
                    // 2020 58B (tyre temps shrink to uint8), 2021+ 60B (adds
                    // m_revLightsBitValue). No official 2018 spec copy is
                    // available; 57B is derived from the 2019 layout by
                    // replacing the three float fields (throttle/steer/brake,
                    // 4B each) with their 2018 uint8/int8/uint8 equivalents
                    // (1B each) - the 2018 field *types* differ too (see 2.6),
                    // which this stride fix alone does not address.
                    int carSize = (hdr->m_packetFormat >= 2021) ? 60
                                : (hdr->m_packetFormat == 2020) ? 58
                                : (hdr->m_packetFormat == 2019) ? 66 : 57;
                    int off = hdrSize + hdr->m_playerCarIndex * carSize;
                    if (off + (int)sizeof(CarTelemetryData) <= len) {
                        auto* car = (CarTelemetryData*)(buf + off);
                        std::lock_guard<std::mutex> lk(telemetryMutex);
                        currentTelemetry.gameName  = "F1";
                        currentTelemetry.f1Year    = hdr->m_packetFormat;
                        currentTelemetry.speed     = (float)car->m_speed;
                        currentTelemetry.rpm       = car->m_engineRPM;
                        currentTelemetry.maxRpm    = 15000;
                        currentTelemetry.gear      = car->m_gear;
                        currentTelemetry.throttle  = car->m_throttle * 255.0f;
                        currentTelemetry.brake     = car->m_brake    * 255.0f;
                        currentTelemetry.g_lat     = 0;
                        dataReceivedThisFrame      = true;
                    }
                }
            }
        }
    }
}

// ── AMS2Reader ────────────────────────────────────────────────────────────────
void AMS2Reader::setActive(bool active) {
    if (!active) { if (pView) { UnmapViewOfFile(pView); pView = nullptr; } if (hMapFile) { CloseHandle(hMapFile); hMapFile = nullptr; } }
    externallyActive = active;
}
bool AMS2Reader::isActive() const { return externallyActive; }
void AMS2Reader::update() {
    if (!externallyActive) return;
    if (!hMapFile) { hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, "$pcars2$"); if (!hMapFile) return; }
    if (!pView)    { pView = (SharedMemory*)MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, sizeof(SharedMemory)); if (!pView) { CloseHandle(hMapFile); hMapFile = nullptr; return; } }

    unsigned int seq = pView->mSequenceNumber;
    if (seq & 1) return;
    memcpy(&localCopy, pView, sizeof(SharedMemory));
    if (localCopy.mSequenceNumber != seq || localCopy.mVersion < 10) return;

    unsigned int gs = localCopy.mGameState;
    if (gs != GAME_INGAME_PLAYING && gs != GAME_INGAME_PAUSED && gs != GAME_INGAME_INMENU_TIME_TICKING) return;
    if (localCopy.mRpm > 25000.0f || localCopy.mSpeed > 278.0f) return;

    std::lock_guard<std::mutex> lk(telemetryMutex);
    currentTelemetry.gameName  = "AMS2";
    currentTelemetry.gamePaused = (gs == GAME_INGAME_PAUSED);
    currentTelemetry.speed     = localCopy.mSpeed * 3.6f;
    currentTelemetry.rpm       = (int)localCopy.mRpm;
    currentTelemetry.maxRpm    = (int)localCopy.mMaxRPM;
    currentTelemetry.gear      = localCopy.mGear;
    currentTelemetry.throttle  = localCopy.mThrottle * 255.0f;
    currentTelemetry.brake     = localCopy.mBrake    * 255.0f;
    currentTelemetry.g_lat     = localCopy.mLocalAcceleration[VEC_X] / 9.81f;
    currentTelemetry.absActive  = (localCopy.mAntiLockActive != 0);
    currentTelemetry.tcActive   = ((localCopy.mCarFlags & CAR_TCS) != 0);
    currentTelemetry.shiftLight = false;  // always computed at 97% maxRpm in serial output
    dataReceivedThisFrame = true;
}

// ── R3EReader ─────────────────────────────────────────────────────────────────
void R3EReader::setActive(bool active) {
    if (!active && hMapFile) { CloseHandle(hMapFile); hMapFile = nullptr; }
    externallyActive = active;
}
bool R3EReader::isActive() const { return externallyActive; }
void R3EReader::update() {
    if (!externallyActive) return;
    if (!hMapFile) { hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, "$R3E"); if (!hMapFile) hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\$R3E"); }
    if (!hMapFile) return;
    r3e_shared* p = (r3e_shared*)MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, 0);
    if (!p) { CloseHandle(hMapFile); hMapFile = nullptr; return; }
    double t = p->player.game_simulation_time;
    if (t != lastSimTime && p->version_major == 3) {
        lastSimTime = t;
        float speed = p->car_speed * 3.6f;
        int   rpm   = (int)(p->engine_rps * 60.0f / (2.0f * 3.14159265f));
        if (rpm < 25000 && speed < 1000.0f) {
            lastDataTime = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(telemetryMutex);
            currentTelemetry.gameName  = "RaceRoom";
            currentTelemetry.speed     = speed;
            currentTelemetry.rpm       = rpm;
            currentTelemetry.maxRpm    = (int)(p->max_engine_rps * 60.0f / (2.0f * 3.14159265f));
            currentTelemetry.gear      = p->gear;
            currentTelemetry.throttle  = p->throttle * 255.0f;
            currentTelemetry.brake     = p->brake     * 255.0f;
            currentTelemetry.g_lat     = -(float)p->player.steering_force_percentage;  // R3E: direct steering force, negate for correct direction
            currentTelemetry.absActive   = (p->aid_settings.abs == 5);
            currentTelemetry.tcActive    = (p->aid_settings.tc  == 5);
            currentTelemetry.shiftLight = false;
            for (int i = 0; i < 4; i++) {
                currentTelemetry.suspensionTravel[i]   = (float)p->player.suspension_deflection[i];
                currentTelemetry.suspensionVelocity[i] = (float)p->player.suspension_velocity[i];
            }
            dataReceivedThisFrame = true;
        }
    } else {
        // simTime unchanged – hold last telemetry for up to 200ms to prevent flicker
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - lastDataTime).count();
        if (age < 200) dataReceivedThisFrame = true;
    }
    UnmapViewOfFile(p);
}

// normalizeGear: unified gear convention -1=R, 0=N, 1=1st, 2=2nd ...
// ACC and Assetto Corsa readers call this to convert their 0=R,1=N,2=1st encoding.
static int normalizeGear(int rawGear, const std::string& gameName);

// ── ACC/AC surface signal ─────────────────────────────────────────────────────
// Both readers used to write wheelSlip into suspensionVelocity[] and let
// computeRumble() treat it as the surface signal. wheelSlip is a slide/lockup
// indicator though - it barely moves when a wheel drops onto a kerb, which is a
// suspension event. suspensionTravel[] (the real thing) was read into the
// telemetry struct and then never used by anything.
//
// This differentiates suspensionTravel into an actual suspension velocity in
// m/s, which puts ACC/AC in the same unit domain as RaceRoom's native
// suspension_velocity and finally gives kerbs a signal path. wheelSlip is kept
// as well, in its own field, so slides/lockups still register.
//
// dt is measured rather than assumed: the poll interval jitters, and dividing
// by a nominal interval would turn that jitter straight into fake spikes.
// Samples with an implausible dt (first call, a long stall, a paused game) are
// skipped instead of differentiated.
// State is passed in rather than held in a local static so ACC and AC Original
// keep separate histories - they share this helper but are different games.
struct SuspDerivState {
    float prevTravel[4] = { 0, 0, 0, 0 };
    float lastVel[4]    = { 0, 0, 0, 0 };
    std::chrono::steady_clock::time_point prevTime = std::chrono::steady_clock::now();
    int   prevPacketId  = -1;
    bool  havePrev      = false;
};
static void deriveSuspensionVelocity(SuspDerivState& st, int packetId,
                                     const float travel[4], float outVel[4]) {
    // We poll faster than the game writes (250Hz against ACC's ~333Hz physics
    // page, and slower still when it stutters), so a good share of polls see the
    // exact same sample twice. Differentiating those would yield a hard 0 for
    // every wheel and punch a hole in the surface signal on that tick - a rasp
    // on top of the texture rather than the texture itself. packetId tells us
    // whether the game actually moved on; if it did not, hold the last velocity
    // and leave prevTime alone so the next real sample is divided by the full
    // elapsed time rather than by the poll interval.
    if (st.havePrev && packetId == st.prevPacketId) {
        for (int i = 0; i < 4; i++) outVel[i] = st.lastVel[i];
        return;
    }

    auto  now = std::chrono::steady_clock::now();
    float dt  = std::chrono::duration_cast<std::chrono::microseconds>(now - st.prevTime).count() / 1e6f;

    // Skip the first sample and anything after a long gap (paused game, menu,
    // alt-tab): the "velocity" across such a gap is meaningless and would fire
    // as one large bogus impact the moment the driver comes back.
    bool usable = st.havePrev && dt > 0.0005f && dt < 0.1f;
    for (int i = 0; i < 4; i++) {
        outVel[i]     = usable ? (travel[i] - st.prevTravel[i]) / dt : 0.0f;
        st.lastVel[i] = outVel[i];
        st.prevTravel[i] = travel[i];
    }
    st.prevTime     = now;
    st.prevPacketId = packetId;
    st.havePrev     = true;
}

// ── ACCReader ─────────────────────────────────────────────────────────────────
void ACCReader::setActive(bool active) {
    if (!active) {
        if (hPhysics)  { CloseHandle(hPhysics);  hPhysics  = nullptr; }
        if (hGraphics) { CloseHandle(hGraphics); hGraphics = nullptr; }
        if (hStatic)   { CloseHandle(hStatic);   hStatic   = nullptr; }
    }
    externallyActive = active;
}
bool ACCReader::isActive() const { return externallyActive; }
void ACCReader::update() {
    if (!externallyActive) return;
    if (!hPhysics)  hPhysics  = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_physics");
    if (!hGraphics) hGraphics = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_graphics");
    if (!hStatic)   hStatic   = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_static");
    if (!hPhysics || !hGraphics || !hStatic) return;

    auto* ph = (SPageFilePhysics*)MapViewOfFile(hPhysics,  FILE_MAP_READ, 0, 0, sizeof(SPageFilePhysics));
    auto* gr = (SPageFileGraphic*)MapViewOfFile(hGraphics, FILE_MAP_READ, 0, 0, sizeof(SPageFileGraphic));
    auto* st = (SPageFileStatic*) MapViewOfFile(hStatic,   FILE_MAP_READ, 0, 0, sizeof(SPageFileStatic));

    if (ph && gr && st && gr->status == AC_LIVE && ph->rpms < 25000 && ph->speedKmh < 1000.0f) {
        std::lock_guard<std::mutex> lk(telemetryMutex);
        currentTelemetry.gameName  = "ACC";
        currentTelemetry.speed     = ph->speedKmh;
        currentTelemetry.rpm       = ph->rpms;
        currentTelemetry.maxRpm    = (ph->currentMaxRpm > 0) ? ph->currentMaxRpm : (st ? st->maxRpm : 0);
        currentTelemetry.gear      = normalizeGear(ph->gear, "ACC");
        currentTelemetry.throttle  = ph->gas   * 255.0f;
        currentTelemetry.brake     = ph->brake  * 255.0f;
        currentTelemetry.g_lat     = ph->accG[1];
        currentTelemetry.absActive  = (ph->absInAction != 0);
        currentTelemetry.tcActive   = (ph->tcinAction  != 0);
        currentTelemetry.shiftLight = false;  // always computed at 97% maxRpm in serial output
        float slipMax = 0.0f;
        for (int i = 0; i < 4; i++) {
            currentTelemetry.suspensionTravel[i] = ph->suspensionTravel[i];
            // wheelSlip: <1 = normal grip, >1 = slide/lockup. Kept as its own
            // signal - see wheelSlipMax in TelemetryData.
            slipMax = std::max(slipMax, std::abs(ph->wheelSlip[i]));
        }
        currentTelemetry.wheelSlipMax = slipMax;
        static SuspDerivState accSusp;
        deriveSuspensionVelocity(accSusp, ph->packetId, currentTelemetry.suspensionTravel,
                                                        currentTelemetry.suspensionVelocity);
        dataReceivedThisFrame = true;
    }
    if (ph) UnmapViewOfFile(ph);
    if (gr) UnmapViewOfFile(gr);
    if (st) UnmapViewOfFile(st);
}

// ── ForzaReader ───────────────────────────────────────────────────────────────
// Supports Forza Motorsport 7/2023 and Horizon 4/5 "Dash" UDP format.
// In-game: Settings > Gameplay & HUD > UDP Race Telemetry
//   Data Out: ON  |  Data Out IP: 127.0.0.1  |  Data Out Port: 5300
//   Packet Format: Dash
ForzaReader::ForzaReader() {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(5300);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { closesocket(sock); sock = INVALID_SOCKET; return; }
    u_long mode = 1; ioctlsocket(sock, FIONBIO, &mode);
    int buf = 65536; setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&buf, sizeof(buf));
}
void ForzaReader::setActive(bool a)  { externallyActive = a; }
bool ForzaReader::isActive()  const  { return externallyActive; }
void ForzaReader::start() { running = true; thread = std::thread(&ForzaReader::loop, this); SetThreadPriority(thread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL); }
void ForzaReader::stop()  { running = false; if (thread.joinable()) thread.join(); if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; } }
bool ForzaReader::checkTimeout() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lastDataTime).count() > DISCONNECT_TIMEOUT_MS;
}
void ForzaReader::loop() {
    char buf[512];
    while (running) {
        if (!externallyActive || sock == INVALID_SOCKET) { Sleep(5); continue; }
        // select() avoids busy-wait, drops CPU usage to ~0%%
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv{ 0, 20000 };  // 20ms timeout
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
        {
            int len = recv(sock, buf, sizeof(buf), 0);
            // Accept both Sled (232 bytes) and Dash (232+8=240 bytes) formats
            if (len >= 232) {
                lastDataTime = std::chrono::steady_clock::now();
                auto* p = (ForzaDash*)buf;
                if (p->isRaceOn) {
                    std::lock_guard<std::mutex> lk(telemetryMutex);
                    currentTelemetry.gameName  = "Forza";
                    currentTelemetry.speed     = p->speed * 3.6f;
                    currentTelemetry.rpm       = (int)p->currentEngineRpm;
                    currentTelemetry.maxRpm    = (int)p->engineMaxRpm;
                    // Dash format has gear byte; Sled does not – safe because len≥232
                    if (len >= 240) {
                        int g = (int)(unsigned char)p->gear;
                        // Forza: 0=Reverse, 1=Neutral, 2=1st, 3=2nd ...
                        currentTelemetry.gear = (g == 0) ? -1 : (g == 1) ? 0 : g - 1;
                        currentTelemetry.throttle = p->accel;
                        currentTelemetry.brake    = p->brake;
                    }
                    // Lateral accel from local space: accelX is lateral (left=positive)
                    currentTelemetry.g_lat = p->accelX / 9.81f;
                    // Surface rumble: use tire combined slip values
                    currentTelemetry.suspensionVelocity[0] = p->tireCombSlipFL;
                    currentTelemetry.suspensionVelocity[1] = p->tireCombSlipFR;
                    currentTelemetry.suspensionVelocity[2] = p->tireCombSlipRL;
                    currentTelemetry.suspensionVelocity[3] = p->tireCombSlipRR;
                    // Shift light at 97% of max RPM
                    currentTelemetry.shiftLight = false;  // always computed at 97% maxRpm in serial output
                    dataReceivedThisFrame = true;
                }
            }
        }
    }
}

// ── ACOriginalReader ──────────────────────────────────────────────────────────
// Assetto Corsa (original) uses the same shared memory layout and names as ACC.
// The only difference: AC's graphics page has status AC_LIVE as well.
// We reuse all ACC structs; the only change is the gameName tag.
void ACOriginalReader::setActive(bool active) {
    if (!active) {
        if (hPhysics)  { CloseHandle(hPhysics);  hPhysics  = nullptr; }
        if (hGraphics) { CloseHandle(hGraphics); hGraphics = nullptr; }
        if (hStatic)   { CloseHandle(hStatic);   hStatic   = nullptr; }
    }
    externallyActive = active;
}
bool ACOriginalReader::isActive() const { return externallyActive; }
void ACOriginalReader::update() {
    if (!externallyActive) return;
    if (!hPhysics)  hPhysics  = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_physics");
    if (!hGraphics) hGraphics = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_graphics");
    if (!hStatic)   hStatic   = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_static");
    if (!hPhysics || !hGraphics || !hStatic) return;
    auto* ph = (SPageFilePhysics*)MapViewOfFile(hPhysics,  FILE_MAP_READ, 0, 0, sizeof(SPageFilePhysics));
    auto* gr = (SPageFileGraphic*)MapViewOfFile(hGraphics, FILE_MAP_READ, 0, 0, sizeof(SPageFileGraphic));
    auto* st = (SPageFileStatic*) MapViewOfFile(hStatic,   FILE_MAP_READ, 0, 0, sizeof(SPageFileStatic));
    if (ph && gr && st && gr->status == AC_LIVE && ph->rpms < 25000) {
        std::lock_guard<std::mutex> lk(telemetryMutex);
        currentTelemetry.gameName  = "Assetto Corsa";
        currentTelemetry.speed     = ph->speedKmh;
        currentTelemetry.rpm       = ph->rpms;
        currentTelemetry.maxRpm    = (ph->currentMaxRpm > 0) ? ph->currentMaxRpm : (st ? st->maxRpm : 0);
        currentTelemetry.gear      = normalizeGear(ph->gear, "Assetto Corsa");
        currentTelemetry.throttle  = ph->gas   * 255.0f;
        currentTelemetry.brake     = ph->brake  * 255.0f;
        currentTelemetry.g_lat     = ph->accG[1];
        currentTelemetry.shiftLight = false;  // always computed at 97% maxRpm in serial output
        float slipMax = 0.0f;
        for (int i = 0; i < 4; i++) {
            currentTelemetry.suspensionTravel[i] = ph->suspensionTravel[i];
            slipMax = std::max(slipMax, std::abs(ph->wheelSlip[i]));
        }
        currentTelemetry.wheelSlipMax = slipMax;
        static SuspDerivState acSusp;   // see the ACC reader above
        deriveSuspensionVelocity(acSusp, ph->packetId, currentTelemetry.suspensionTravel,
                                                       currentTelemetry.suspensionVelocity);
        dataReceivedThisFrame = true;
    }
    if (ph) UnmapViewOfFile(ph);
    if (gr) UnmapViewOfFile(gr);
    if (st) UnmapViewOfFile(st);
}

// ── IRacingReader ─────────────────────────────────────────────────────────────
// Shared memory: "Local\IRSDKMemMapFileName"
// Variables are found by name lookup in the variable header array.
void IRacingReader::setActive(bool active) {
    if (!active) {
        if (pSharedMem) { UnmapViewOfFile(pSharedMem); pSharedMem = nullptr; }
        if (hMap)       { CloseHandle(hMap); hMap = nullptr; }
        offsetsCached = false;
    }
    externallyActive = active;
}
bool IRacingReader::isActive() const { return externallyActive; }

void IRacingReader::cacheOffsets(const irsdk_header* hdr) {
    auto* vars = (irsdk_varHeader*)((char*)hdr + hdr->varHeaderOffset);
    for (int i = 0; i < hdr->numVars; i++) {
        std::string n = vars[i].name;
        if (n == "RPM")            offRPM        = vars[i].offset;
        else if (n == "Speed")     offSpeed      = vars[i].offset;
        else if (n == "Gear")      offGear       = vars[i].offset;
        else if (n == "Throttle")  offThrottle   = vars[i].offset;
        else if (n == "Brake")     offBrake      = vars[i].offset;
        else if (n == "LatAccel")  offLatAccel   = vars[i].offset;
        else if (n == "ShiftLight" || n == "RevLightsFlash") offShiftLight = vars[i].offset;
    }
    offsetsCached = true;
}

template<typename T>
T IRacingReader::getVar(const irsdk_header* hdr, int offset) const {
    if (offset < 0) return T{};
    // Pick the most recently updated buffer
    int latestBuf = 0;
    for (int i = 1; i < hdr->numBuf; i++)
        if (hdr->varBuf[i].tickCount > hdr->varBuf[latestBuf].tickCount) latestBuf = i;
    const char* data = (char*)hdr + hdr->varBuf[latestBuf].bufOffset;
    T val; memcpy(&val, data + offset, sizeof(T));
    return val;
}

void IRacingReader::update() {
    if (!externallyActive) return;
    if (!hMap) {
        hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, IRSDK_MEMMAPFILENAME);
        if (!hMap) return;
    }
    if (!pSharedMem) {
        pSharedMem = (char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        if (!pSharedMem) { CloseHandle(hMap); hMap = nullptr; return; }
    }
    auto* hdr = (irsdk_header*)pSharedMem;
    if (!(hdr->status & IRSDK_STCONNECTED)) return;
    if (!offsetsCached) cacheOffsets(hdr);

    float rpm    = getVar<float>(hdr, offRPM);
    float speed  = getVar<float>(hdr, offSpeed);  // m/s
    int   gear   = getVar<int32_t>(hdr, offGear);
    float thr    = getVar<float>(hdr, offThrottle);
    float brk    = getVar<float>(hdr, offBrake);
    float lat    = getVar<float>(hdr, offLatAccel); // m/s² → convert to g

    if (rpm <= 0 || rpm > 25000) return;

    std::lock_guard<std::mutex> lk(telemetryMutex);
    currentTelemetry.gameName  = "iRacing";
    currentTelemetry.rpm       = (int)rpm;
    currentTelemetry.maxRpm    = 0;  // iRacing has no MaxRPM channel; fallback used
    currentTelemetry.speed     = speed * 3.6f;
    currentTelemetry.gear      = gear;
    currentTelemetry.throttle  = thr * 255.0f;
    currentTelemetry.brake     = brk * 255.0f;
    currentTelemetry.g_lat     = lat / 9.81f;
    currentTelemetry.shiftLight = false;  // always computed at 97% maxRpm in serial output
    // iRacing has no per-wheel velocity in standard channels; fall back to g_lat spike
    for (int i = 0; i < 4; i++) currentTelemetry.suspensionVelocity[i] = 0;
    dataReceivedThisFrame = true;
}

// ── DirtRallyReader ───────────────────────────────────────────────────────────
// DiRT series – UDP extradata=3, port 20777 (standard port, no conflict since
// socket is only open while the game is running)
// User must edit: Documents\My Games\<game>\hardwaresettings\hardware_settings_config.xml
//   <udp enabled="true" extradata="3" ip="127.0.0.1" port="20777" delay="1" />
DirtRallyReader::DirtRallyReader() {
    // Socket is created in start() to avoid permanently blocking port 20777
}
void DirtRallyReader::setActive(bool a)  { externallyActive = a; }
bool DirtRallyReader::isActive()  const  { return externallyActive; }
void DirtRallyReader::start() {
    externallyActive = true;
    // Recreate socket if it was closed by a previous stop()
    if (sock == INVALID_SOCKET) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock != INVALID_SOCKET) {
            sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(20777);
            if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { closesocket(sock); sock = INVALID_SOCKET; return; }
            u_long mode = 1; ioctlsocket(sock, FIONBIO, &mode);
        }
    }
    running = true; thread = std::thread(&DirtRallyReader::loop, this);
}
void DirtRallyReader::stop()  { externallyActive = false; running = false; if (thread.joinable()) thread.join(); if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; } }
bool DirtRallyReader::checkTimeout() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lastDataTime).count() > DISCONNECT_TIMEOUT_MS;
}
void DirtRallyReader::loop() {
    char buf[512];
    while (running) {
        if (!externallyActive || sock == INVALID_SOCKET) { Sleep(5); continue; }
        // select() avoids busy-wait, drops CPU usage to ~0%%
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv{ 0, 20000 };  // 20ms timeout
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
        {
            int len = recv(sock, buf, sizeof(buf), 0);
            if (len >= 152) {
                lastDataTime = std::chrono::steady_clock::now();
                auto* p = (DirtPacket*)buf;
                float speed  = p->speed * 3.6f;
                float rpm    = p->engineRate * 10.0f;
                float maxRpm = (len >= 156) ? p->maxRPM : 0;
                if (rpm <= 25000 && speed <= 500) {
                    int gf = (int)(p->gear + 0.5f);
                    int gear = (gf == 0) ? 0 : (gf == 10) ? -1 : gf;
                    std::lock_guard<std::mutex> lk(telemetryMutex);
                    currentTelemetry.gameName  = "DiRT Rally";
                    currentTelemetry.speed     = speed;
                    currentTelemetry.rpm       = (int)rpm;
                    currentTelemetry.maxRpm    = (int)maxRpm;
                    currentTelemetry.gear      = gear;
                    currentTelemetry.throttle  = p->throttle * 255.0f;
                    currentTelemetry.brake     = p->brake    * 255.0f;
                    currentTelemetry.g_lat     = p->gForceLat;
                    currentTelemetry.suspensionVelocity[0] = p->suspVelBL;
                    currentTelemetry.suspensionVelocity[1] = p->suspVelBR;
                    currentTelemetry.suspensionVelocity[2] = p->suspVelFR;
                    currentTelemetry.suspensionVelocity[3] = p->suspVelFL;
                    currentTelemetry.shiftLight = false;
                    dataReceivedThisFrame = true;
                }
            }
        }
    }
}


// ── GridAutosportReader ───────────────────────────────────────────────────────
// GRID Autosport – UDP port 20777, same packet format as DiRT Rally extradata=3
// Config: Documents\My Games\GRID Autosport\hardwaresettings\hardware_settings_config.xml
//   <motion enabled="true" ip="127.0.0.1" port="20777" delay="1" />
GridAutosportReader::GridAutosportReader() {
    // Socket is created in start() to avoid permanently blocking port 20777
    // (F1 series also uses port 20777 and must not be locked out)
}
void GridAutosportReader::setActive(bool a)  { externallyActive = a; }
bool GridAutosportReader::isActive()  const  { return externallyActive; }
void GridAutosportReader::start() {
    externallyActive = true;
    if (sock == INVALID_SOCKET) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock != INVALID_SOCKET) {
            int reuse = 1; setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
            sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(20777);
            if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { closesocket(sock); sock = INVALID_SOCKET; return; }
            u_long mode = 1; ioctlsocket(sock, FIONBIO, &mode);
        }
    }
    running = true; thread = std::thread(&GridAutosportReader::loop, this);
}
void GridAutosportReader::stop()  { externallyActive = false; running = false; if (thread.joinable()) thread.join(); if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; } }
bool GridAutosportReader::checkTimeout() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lastDataTime).count() > DISCONNECT_TIMEOUT_MS;
}
void GridAutosportReader::loop() {
    // Reuse DirtRallyReader packet struct – identical wire format
    struct GridPacket {
        float time, lapTime, lapDistance, totalDistance;
        float posX, posY, posZ;
        float speed;                    // m/s
        float velX, velY, velZ;
        float rollX, rollY, rollZ;
        float pitchX, pitchY, pitchZ;
        float suspPosBL, suspPosBR, suspPosFR, suspPosFL;
        float suspVelBL, suspVelBR, suspVelFR, suspVelFL;
        float wheelVelBL, wheelVelBR, wheelVelFR, wheelVelFL;
        float throttle;
        float steer;
        float brake;
        float clutch;
        float gear;                     // 0=N, 1-8=forward, 10=R
        float gForceLat;
        float gForceLon;
        float lap;
        float engineRate;               // rpm / 10
    };
    char buf[512];
    while (running) {
        if (!externallyActive || sock == INVALID_SOCKET) { Sleep(5); continue; }
        // select() avoids busy-wait, drops CPU usage to ~0%%
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv{ 0, 20000 };  // 20ms timeout
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
        {
            int len = recv(sock, buf, sizeof(buf), 0);
            if (len >= 152) {
                lastDataTime = std::chrono::steady_clock::now();
                auto* p = (GridPacket*)buf;
                float speed = p->speed * 3.6f;
                float rpm   = p->engineRate * 10.0f;
                if (rpm <= 25000 && speed <= 500) {
                    int gf = (int)(p->gear + 0.5f);
                    int gear = (gf == 0) ? 0 : (gf == 10) ? -1 : gf;
                    std::lock_guard<std::mutex> lk(telemetryMutex);
                    currentTelemetry.gameName  = "GRID Autosport";
                    currentTelemetry.speed     = speed;
                    currentTelemetry.rpm       = (int)rpm;
                    currentTelemetry.maxRpm    = 8000;  // GRID Autosport has no maxRPM field; use safe default
                    currentTelemetry.gear      = gear;
                    currentTelemetry.throttle  = p->throttle * 255.0f;
                    currentTelemetry.brake     = p->brake    * 255.0f;
                    currentTelemetry.g_lat     = p->gForceLat;
                    currentTelemetry.suspensionVelocity[0] = p->suspVelBL;
                    currentTelemetry.suspensionVelocity[1] = p->suspVelBR;
                    currentTelemetry.suspensionVelocity[2] = p->suspVelFR;
                    currentTelemetry.suspensionVelocity[3] = p->suspVelFL;
                    currentTelemetry.shiftLight = false;
                    dataReceivedThisFrame = true;
                }
            }
        }
    }
}

// ── WRCReader ─────────────────────────────────────────────────────────────────
// EA SPORTS WRC / WRC Generations – shared memory
// Memory map name: L"Local\WRC-8wSotWzFKAhBlbW10ZJBKaWMdWszbBXg"
static constexpr wchar_t WRC_MMAP_NAME[] = L"Local\\WRC-8wSotWzFKAhBlbW10ZJBKaWMdWszbBXg";

void WRCReader::setActive(bool active) {
    if (!active) { if (hMap) { CloseHandle(hMap); hMap = nullptr; } }
    externallyActive = active;
}
bool WRCReader::isActive() const { return externallyActive; }
void WRCReader::update() {
    if (!externallyActive) return;
    if (!hMap) { hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, WRC_MMAP_NAME); if (!hMap) return; }
    auto* p = (WrcTelemetry*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(WrcTelemetry));
    if (!p) { CloseHandle(hMap); hMap = nullptr; return; }
    // Odd sequence = game is currently writing, skip
    if (p->sequenceNumber & 1) { UnmapViewOfFile(p); return; }
    if (p->version < 1) { UnmapViewOfFile(p); return; }

    float rpm   = (float)p->engineRpm;
    float speed = std::sqrt(p->velocity[0]*p->velocity[0] + p->velocity[2]*p->velocity[2]) * 3.6f;
    // gear: 0=Reverse, 1=Neutral, 2=1st gear, ...
    int gear = (p->gear == 0) ? -1 : (p->gear == 1) ? 0 : p->gear - 1;

    if (rpm > 20000 || speed > 400) { UnmapViewOfFile(p); return; }
    {
        std::lock_guard<std::mutex> lk(telemetryMutex);
        currentTelemetry.gameName  = "EA WRC";
        currentTelemetry.speed     = speed;
        currentTelemetry.rpm       = (int)rpm;
        currentTelemetry.maxRpm    = p->engineMaxRpm;
        currentTelemetry.gear      = gear;
        // acceleration[0] = lateral (left = positive) in m/s²
        currentTelemetry.g_lat     = p->acceleration[0] / 9.81f;
        // Throttle/brake not in WRC shared mem (only motion data), use g_lat fallback for rumble
        for (int i = 0; i < 4; i++) currentTelemetry.suspensionVelocity[i] = 0;
        currentTelemetry.shiftLight = false;  // always computed at 97% maxRpm in serial output
        dataReceivedThisFrame = true;
    }
    UnmapViewOfFile(p);
}

// ── ETSReader ─────────────────────────────────────────────────────────────────
// Euro Truck Simulator 2 / American Truck Simulator
// Requires: scs-sdk-plugin DLL in <game>\bin\win_x64\plugins\
// Download:  https://github.com/RenCloud/scs-sdk-plugin
// Shared memory name: "Local\SCSTelemetry"
void ETSReader::setActive(bool active) {
    if (!active) { if (hMap) { CloseHandle(hMap); hMap = nullptr; } }
    externallyActive = active;
}
bool ETSReader::isActive() const { return externallyActive; }
void ETSReader::update() {
    if (!externallyActive) return;
    if (!hMap) {
        hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\SCSTelemetry");
        if (!hMap) hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, "SCSTelemetry");
        if (!hMap) return;
    }

    auto* p = (SCSGameState*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(SCSGameState));
    if (!p) { CloseHandle(hMap); hMap = nullptr; return; }

    // sdkActive must be true and plugin revision >= 10
    if (!p->sdkActive || p->scs_values.telemetry_plugin_revision < 10) {
        UnmapViewOfFile(p); return;
    }

    float speed  = std::abs(p->speed) * 3.6f;
    float rpm    = p->engineRpm;
    float maxRpm = p->engineRpmMax;

    if (speed > 300.0f || rpm > 10000.0f) { UnmapViewOfFile(p); return; }

    {
        std::lock_guard<std::mutex> lk(telemetryMutex);
        currentTelemetry.gameName  = "ETS2/ATS";
        currentTelemetry.gamePaused = (p->paused != 0);
        currentTelemetry.speed     = speed;
        currentTelemetry.rpm       = (int)rpm;
        currentTelemetry.maxRpm    = (maxRpm > 0) ? (int)maxRpm : 2500;
        currentTelemetry.gear      = p->gearDashboard;
        currentTelemetry.throttle  = p->userThrottle * 255.0f;
        currentTelemetry.brake     = p->userBrake    * 255.0f;
        currentTelemetry.g_lat     = p->accelerationX / 9.81f;
        for (int i = 0; i < 4; i++) currentTelemetry.suspensionVelocity[i] = 0;
        currentTelemetry.shiftLight = false;
        dataReceivedThisFrame = true;
    }
    UnmapViewOfFile(p);
}

// ── BeamNGReader ──────────────────────────────────────────────────────────────
// BeamNG.drive OutGauge UDP – port 4444
// Enable in-game: Options > Other > Protocols > OutGauge UDP Protocol
//   IP: 127.0.0.1  |  Port: 4444
BeamNGReader::BeamNGReader() {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(4444);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { closesocket(sock); sock = INVALID_SOCKET; return; }
    u_long mode = 1; ioctlsocket(sock, FIONBIO, &mode);
}
void BeamNGReader::setActive(bool a)  { externallyActive = a; }
bool BeamNGReader::isActive()  const  { return externallyActive; }
void BeamNGReader::start() { running = true; thread = std::thread(&BeamNGReader::loop, this); }
void BeamNGReader::stop()  { running = false; if (thread.joinable()) thread.join(); if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; } }
bool BeamNGReader::checkTimeout() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lastDataTime).count() > DISCONNECT_TIMEOUT_MS;
}
void BeamNGReader::loop() {
    char buf[512];
    while (running) {
        if (!externallyActive || sock == INVALID_SOCKET) { Sleep(5); continue; }
        // select() avoids busy-wait, drops CPU usage to ~0%%
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv{ 0, 20000 };  // 20ms timeout
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
        {
            int len = recv(sock, buf, sizeof(buf), 0);
            if (len >= (int)sizeof(OutGaugePacket)) {
                lastDataTime = std::chrono::steady_clock::now();
                auto* p = (OutGaugePacket*)buf;
                float speed = p->speed * 3.6f;
                float rpm   = p->rpm;
                if (rpm <= 25000 && speed <= 500) {
                    // OutGauge gear: 0=R, 1=N, 2=1st gear, ...
                    int gear;
                    int gi = (int)(unsigned char)p->gear;
                    if (gi == 0) gear = -1;
                    else if (gi == 1) gear = 0;
                    else gear = gi - 1;

                    {
                        std::lock_guard<std::mutex> lk(telemetryMutex);
                        currentTelemetry.gameName  = "BeamNG";
                        currentTelemetry.speed     = speed;
                        currentTelemetry.rpm       = (int)rpm;
                        currentTelemetry.maxRpm    = 0;  // OutGauge has no maxRPM field
                        currentTelemetry.gear      = gear;
                        currentTelemetry.throttle  = p->throttle * 255.0f;
                        currentTelemetry.brake     = p->brake    * 255.0f;
                        // DL_SHIFT bit (0x1) in showLights = shift light
                        currentTelemetry.shiftLight = false;  // always computed at 97% maxRpm in serial output
                        // No g_lat or per-wheel data in OutGauge – zero out
                        currentTelemetry.g_lat = 0;
                        for (int i = 0; i < 4; i++) currentTelemetry.suspensionVelocity[i] = 0;
                        dataReceivedThisFrame = true;
                    }
                } // end if rpm/speed valid
            }
        }
    }
}

// ── RF2Reader ─────────────────────────────────────────────────────────────────
// rFactor 2 & Le Mans Ultimate – via rF2SharedMemoryMapPlugin
// Plugin: https://www.overtake.gg/downloads/rf2-shared-memory-tools-for-developers.19334/
// Install plugin DLL in: <rF2 install>\Bin64\Plugins\
// Shared memory name: "$rFactor2SMMP_Telemetry$"
void RF2Reader::setActive(bool active) {
    if (!active) { if (hTelemetry) { CloseHandle(hTelemetry); hTelemetry = nullptr; } }
    externallyActive = active;
}
bool RF2Reader::isActive() const { return externallyActive; }
void RF2Reader::update() {
    if (!externallyActive) return;
    if (!hTelemetry) {
        hTelemetry = OpenFileMappingA(FILE_MAP_READ, FALSE, "$rFactor2SMMP_Telemetry$");
        if (!hTelemetry) return;
    }
    // Shared memory starts with RF2Header, then vehicle array
    char* p = (char*)MapViewOfFile(hTelemetry, FILE_MAP_READ, 0, 0, 0);
    if (!p) { CloseHandle(hTelemetry); hTelemetry = nullptr; return; }

    auto* hdr = (RF2Header*)p;
    // Check version consistency
    if (hdr->mVersionUpdateBegin != hdr->mVersionUpdateEnd) { UnmapViewOfFile(p); return; }
    if (hdr->mNumVehicles <= 0) { UnmapViewOfFile(p); return; }

    // Vehicle telemetry array starts right after the header
    auto* veh = (RF2VehicleTelemetry*)(p + sizeof(RF2Header));
    float rpm   = veh->mEngineRPM;
    float speed = (float)std::sqrt(veh->mLocalVel[0]*veh->mLocalVel[0] +
                                   veh->mLocalVel[2]*veh->mLocalVel[2]) * 3.6f;

    if (rpm <= 0 || rpm > 25000) { UnmapViewOfFile(p); return; }

    {
        std::lock_guard<std::mutex> lk(telemetryMutex);
        currentTelemetry.gameName  = "rFactor 2";
        currentTelemetry.speed     = speed;
        currentTelemetry.rpm       = (int)rpm;
        currentTelemetry.maxRpm    = 0; // rF2 has no direct max RPM in telemetry struct; fallback 9000
        currentTelemetry.gear      = veh->mGear;
        currentTelemetry.throttle  = veh->mThrottle * 255.0f;
        currentTelemetry.brake     = veh->mBrake    * 255.0f;
        // mLocalAccel[0] = lateral in m/s²
        currentTelemetry.g_lat     = (float)(veh->mLocalAccel[0] / 9.81);
        for (int i = 0; i < 4; i++) currentTelemetry.suspensionVelocity[i] = 0;
        // No maxRpm in rF2 struct; shiftLight resolved in serial output via fallback
        currentTelemetry.shiftLight = false; // overridden below via maxRpm fallback
        dataReceivedThisFrame = true;
    }
    UnmapViewOfFile(p);
}

// ── VirtualGamepad ────────────────────────────────────────────────────────────
VirtualGamepad::VirtualGamepad() {
    client = vigem_alloc();
    if (!client) return;
    if (!VIGEM_SUCCESS(vigem_connect(client))) { vigem_free(client); return; }
    target = vigem_target_x360_alloc();
    if (!VIGEM_SUCCESS(vigem_target_add(client, target))) { vigem_target_free(target); vigem_free(client); return; }
    memset(&report, 0, sizeof(report));
    initialized = true;
}
VirtualGamepad::~VirtualGamepad() {
    if (initialized) { vigem_target_remove(client, target); vigem_target_free(target); vigem_disconnect(client); vigem_free(client); }
}
void VirtualGamepad::update(float steering, float throttle, float brake,
                             float clutch, bool hasClutch, int handbrake, bool hasHandbrake,
                             const std::vector<int>& buttons, const std::string& gameName,
                             int f1Year, float steerHalfAngleDeg)
{
    if (!initialized) return;
    // AMS2: pre-scale steering to compensate for the game's ~30% stick deadzone
    float steerOut = steering;
    if (gameName == "AMS2") {
        const float DEAD = 6500.0f, USABLE = 32767.0f - DEAD;
        steerOut = (steerOut > 0) ?  DEAD + (steerOut / steerHalfAngleDeg) * USABLE
                                  : -DEAD + (steerOut / steerHalfAngleDeg) * USABLE;
        steerOut = clamp(steerOut, -32767.0f, 32767.0f);
    } else if (gameName == "F1" && (f1Year == 2018 || f1Year == 2019)) {
        // F1 2018 and 2019 have a hardware deadzone of ~35° that cannot be
        // disabled in-game. F1 2020+ has a calibration slider that can be set
        // to 0, so no workaround is needed for those versions.
        // Deadzone: 35° of the configured max angle = 19.4% → 6375 of 32767
        const float DEAD = 6375.0f, USABLE = 32767.0f - DEAD;
        steerOut = (steerOut > 0) ?  DEAD + (steerOut / steerHalfAngleDeg) * USABLE
                                  : -DEAD + (steerOut / steerHalfAngleDeg) * USABLE;
        steerOut = clamp(steerOut, -32767.0f, 32767.0f);
    } else {
        steerOut = (steerOut / steerHalfAngleDeg) * 32767.0f;
    }
    report.wButtons = 0;
    { std::lock_guard<std::mutex> lk(buttonMapMutex);
      for (int idx : buttons) { auto it = buttonMap.find(idx); if (it != buttonMap.end()) report.wButtons |= it->second; } }
    if (hasHandbrake && handbrake) report.wButtons |= XBTN_RTHUMB;
    report.sThumbLX      = (short)steerOut;
    report.bRightTrigger = (BYTE)clamp(throttle, 0.0f, 255.0f);
    report.bLeftTrigger  = (BYTE)clamp(brake,    0.0f, 255.0f);
    if (hasClutch) {
        short cVal = (short)clamp(clutch, 0.0f, 255.0f);
        report.sThumbRY = (short)(((255 - cVal) / 255.0f) * 32767.0f); // inverted: released = full up
    }
    // Dirty check: only call into ViGEm driver when report actually changed.
    // vigem_target_x360_update() has non-trivial kernel overhead on every call.
    static XUSB_REPORT lastReport{};
    if (memcmp(&report, &lastReport, sizeof(XUSB_REPORT)) != 0) {
        VIGEM_ERROR err = vigem_target_x360_update(client, target, report);
        if (!VIGEM_SUCCESS(err)) {
            // ViGEm driver error - attempt to reconnect once.
            vigem_target_remove(client, target);
            vigem_target_free(target);
            target = vigem_target_x360_alloc();
            if (!VIGEM_SUCCESS(vigem_target_add(client, target))) {
                vigem_target_free(target); target = nullptr; initialized = false;
            } else {
                vigem_target_x360_update(client, target, report);
            }
        }
        lastReport = report;
    }
}

// ── ConfigManager ─────────────────────────────────────────────────────────────
static std::string wstrToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
static std::string appDataDir() {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &p))) {
        std::string s = wstrToUtf8(p); CoTaskMemFree(p);
        s += "\\SimRacePro"; CreateDirectoryA(s.c_str(), NULL); return s;
    }
    return ".";
}
ConfigManager::ConfigManager() {
    std::string dir = appDataDir();
    configPath    = dir + "\\config.ini";
    hwConfigPath  = dir + "\\hardware.ini";
    ffbConfigPath = dir + "\\ffb.ini";
    winConfigPath = dir + "\\windows.ini";
    appConfigPath = dir + "\\app.ini";
    layoutConfigPath = dir + "\\layout.ini";
    verMarkerPath = dir + "\\lastversion.ini";
}
bool ConfigManager::configExists()       { return std::ifstream(configPath).good(); }
bool ConfigManager::hasOptionalHardwareConfig() { return std::ifstream(hwConfigPath).good(); }
std::string ConfigManager::getConfigPath()          { return configPath; }
std::string ConfigManager::getHardwareConfigPath()  { return hwConfigPath; }
std::string ConfigManager::getFfbConfigPath()       { return ffbConfigPath; }

void ConfigManager::saveWindowPos(const std::string& key, int x, int y) {
    std::map<std::string, std::pair<int,int>> all;
    {
        std::ifstream f(winConfigPath);
        std::string line;
        while (std::getline(f, line)) {
            size_t eq = line.find('='); if (eq == std::string::npos) continue;
            size_t comma = line.find(',', eq); if (comma == std::string::npos) continue;
            all[line.substr(0, eq)] = { std::stoi(line.substr(eq + 1, comma - eq - 1)), std::stoi(line.substr(comma + 1)) };
        }
    }
    all[key] = { x, y };
    std::ofstream f(winConfigPath); if (!f) return;
    for (auto& kv : all) f << kv.first << "=" << kv.second.first << "," << kv.second.second << "\n";
}
bool ConfigManager::loadWindowPos(const std::string& key, int& x, int& y) {
    std::ifstream f(winConfigPath); if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('='); if (eq == std::string::npos || line.substr(0, eq) != key) continue;
        size_t comma = line.find(',', eq); if (comma == std::string::npos) continue;
        x = std::stoi(line.substr(eq + 1, comma - eq - 1));
        y = std::stoi(line.substr(comma + 1));
        return true;
    }
    return false;
}

void ConfigManager::saveConfig(const std::string&, const std::map<int, USHORT>& m) {
    std::ofstream f(configPath); if (!f) return;
    f << "[Buttons]\n"; for (auto& kv : m) f << kv.first << "=" << kv.second << "\n";
}
bool ConfigManager::loadConfig(std::string& outPort, std::map<int, USHORT>& out) {
    std::ifstream f(configPath); if (!f) return false;
    char line[256];
    while (f.getline(line, 256)) { char* eq = strchr(line, '='); if (!eq) continue; *eq = '\0'; out[atoi(line)] = (USHORT)atoi(eq + 1); }
    return true;
}

std::string ConfigManager::getLayoutConfigPath() { return layoutConfigPath; }
bool ConfigManager::layoutExists() { return std::ifstream(layoutConfigPath).good(); }
void ConfigManager::saveLayout(const int posToPhys[16]) {
    std::ofstream f(layoutConfigPath); if (!f) return;
    f << "[Layout]\n"; for (int p = 0; p < 16; p++) f << p << "=" << posToPhys[p] << "\n";
}
bool ConfigManager::loadLayout(int posToPhys[16]) {
    std::ifstream f(layoutConfigPath); if (!f) return false;
    char line[256];
    while (f.getline(line, 256)) {
        char* eq = strchr(line, '='); if (!eq) continue; *eq = '\0';
        int pos = atoi(line), phys = atoi(eq + 1);
        if (pos >= 0 && pos < 16 && phys >= -1 && phys < 16) posToPhys[pos] = phys;
    }
    return true;
}
// hardware.ini holds both BoxSettings and BoxPinConfig keys. Both savers must
// only replace their own keys and leave the other's lines untouched - a plain
// std::ofstream truncate-and-rewrite (as each used to do independently) would
// wipe out whatever the other one last wrote to the same file.
static void mergeIniKeys(const std::string& path, const std::vector<std::string>& ownKeys,
                          const std::vector<std::string>& newLines) {
    std::vector<std::string> keep;
    {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            bool isOwnKey = false;
            for (const auto& k : ownKeys) {
                if (line.substr(0, k.size()) == k) { isOwnKey = true; break; }
            }
            if (!isOwnKey && !line.empty()) keep.push_back(line);
        }
    }
    std::ofstream f(path); if (!f) return;
    for (const auto& l : keep)     f << l << "\n";
    for (const auto& l : newLines) f << l << "\n";
}

void ConfigManager::saveBoxSettings(const BoxSettings& s) {
    static const std::vector<std::string> keys = {
        "simSetup=", "hasPedalRumble=", "hasHandbrake=", "hasClutch=", "hasShifter=",
        "onlyWheel=", "invertSteering=", "invertFFB=", "motorMaxPwm=", "motorMinPwmLeft=",
        "motorMinPwmR=", "softRampStep=", "stallProtection=" };
    std::vector<std::string> lines = {
        "simSetup="        + std::to_string((int)s.simSetup),
        "hasPedalRumble="  + std::to_string(s.hasPedalRumble),
        "hasHandbrake="    + std::to_string(s.hasHandbrake),
        "hasClutch="       + std::to_string(s.hasClutch),
        "hasShifter="      + std::to_string(s.hasShifter),
        "onlyWheel="       + std::to_string(s.onlyWheel),
        "invertSteering="  + std::to_string(s.invertSteering),
        "invertFFB="       + std::to_string(s.invertForceFeedback),
        "motorMaxPwm="     + std::to_string((int)s.motorMaxPwm),
        "motorMinPwmLeft=" + std::to_string((int)s.motorMinPwmLeft),
        "motorMinPwmR="    + std::to_string((int)s.motorMinPwmRight),
        "softRampStep="    + std::to_string((int)s.softRampStep),
        "stallProtection=" + std::to_string(s.stallProtection) };
    mergeIniKeys(hwConfigPath, keys, lines);
}

void ConfigManager::loadBoxSettings(BoxSettings& s) {
    std::ifstream f(hwConfigPath); if (!f) return;
    std::string line;
    auto ri = [](const std::string& l, const std::string& k, uint8_t& v) {
        if (l.substr(0, k.size()) == k) v = (uint8_t)std::stoi(l.substr(k.size()));
    };
    auto rb = [](const std::string& l, const std::string& k, bool& v) {
        if (l.substr(0, k.size()) == k) v = std::stoi(l.substr(k.size())) != 0;
    };
    while (std::getline(f, line)) {
        uint8_t tmp = 0; bool btmp = false;
        if (line.substr(0,9) == "simSetup=") { tmp = (uint8_t)std::stoi(line.substr(9)); s.simSetup = tmp; }
        rb(line, "hasPedalRumble=",  s.hasPedalRumble);
        rb(line, "hasHandbrake=",    s.hasHandbrake);
        rb(line, "hasClutch=",       s.hasClutch);
        rb(line, "hasShifter=",      s.hasShifter);
        rb(line, "onlyWheel=",       s.onlyWheel);
        rb(line, "invertSteering=",  s.invertSteering);
        rb(line, "invertFFB=",       s.invertForceFeedback);
        ri(line, "motorMaxPwm=",     s.motorMaxPwm);
        ri(line, "motorMinPwmLeft=", s.motorMinPwmLeft);
        ri(line, "motorMinPwmR=",    s.motorMinPwmRight);
        ri(line, "softRampStep=",    s.softRampStep);
        rb(line, "stallProtection=", s.stallProtection);
    }
}

void ConfigManager::saveBoxPinConfig(const BoxPinConfig& p) {
    static const std::vector<std::string> keys = {
        "accPin=", "brkPin=", "vibPin=", "vib2Pin=", "clutchPin=", "shifterXPin=", "shifterYPin=" };
    std::vector<std::string> lines = {
        "accPin="      + std::to_string((int)p.accPin),
        "brkPin="      + std::to_string((int)p.brkPin),
        "vibPin="      + std::to_string((int)p.vibPin),
        "vib2Pin="     + std::to_string((int)p.vib2Pin),
        "clutchPin="   + std::to_string((int)p.clutchPin),
        "shifterXPin=" + std::to_string((int)p.shifterXPin),
        "shifterYPin=" + std::to_string((int)p.shifterYPin) };
    mergeIniKeys(hwConfigPath, keys, lines);
}

void ConfigManager::loadBoxPinConfig(BoxPinConfig& p) {
    std::ifstream f(hwConfigPath); if (!f) return;
    std::string line;
    auto ri = [](const std::string& l, const std::string& k, uint8_t& v) {
        if (l.substr(0, k.size()) == k) v = (uint8_t)std::stoi(l.substr(k.size()));
    };
    while (std::getline(f, line)) {
        ri(line, "accPin=",      p.accPin);
        ri(line, "brkPin=",      p.brkPin);
        ri(line, "vibPin=",      p.vibPin);
        ri(line, "vib2Pin=",     p.vib2Pin);
        ri(line, "clutchPin=",   p.clutchPin);
        ri(line, "shifterXPin=", p.shifterXPin);
        ri(line, "shifterYPin=", p.shifterYPin);
    }
}

void ConfigManager::saveAppSettings(const AppSettings& s) {
    std::ofstream f(appConfigPath); if (!f) return;
    f << "startMinimized=" << s.startMinimized << "\n";
    f << "maxSteerAngleDeg=" << s.maxSteerAngleDeg << "\n";
}
void ConfigManager::loadAppSettings(AppSettings& s) {
    std::ifstream f(appConfigPath); if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.substr(0, 15) == "startMinimized=")       s.startMinimized   = std::stoi(line.substr(15)) != 0;
        else if (line.substr(0, 17) == "maxSteerAngleDeg=") s.maxSteerAngleDeg = std::stof(line.substr(17));
    }
}

std::string ConfigManager::loadLastRunVersion() {
    std::ifstream f(verMarkerPath); if (!f) return "";
    std::string v; std::getline(f, v);
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
    return v;
}
void ConfigManager::saveLastRunVersion(const std::string& v) {
    std::ofstream f(verMarkerPath); if (f) f << v << "\n";
}

void ConfigManager::saveFfbSettings(const FfbSettings& s) {
    std::ofstream f(ffbConfigPath); if (!f) return;
    f << "[FFB]\n";
    f << "springStrength="   << s.springStrength   << "\n";
    f << "springStartAngle=" << s.springStartAngle << "\n";
    f << "springFullAngle="  << s.springFullAngle  << "\n";
    f << "gLatStrength="     << s.gLatStrength     << "\n";
    f << "springLinearity="  << s.springLinearity  << "\n";
    f << "absTorqueStr="     << s.absTorqueStr     << "\n";
    f << "tcTorqueStr="      << s.tcTorqueStr      << "\n";
    f << "wheelRumbleStr="   << s.wheelRumbleStr   << "\n";
    f << "wheelRumbleThr="   << s.wheelRumbleThr   << "\n";
    f << "absStrength="      << s.absStrength      << "\n";
    f << "gSpikeThreshold="  << s.gSpikeThreshold  << "\n";
    f << "gSpikeStrength="   << s.gSpikeStrength   << "\n";
    f << "pedalRumbleStr="   << s.pedalRumbleStr   << "\n";
    f << "pedalRumbleThr="   << s.pedalRumbleThr   << "\n";
}

void ConfigManager::loadFfbSettings(FfbSettings& s) {
    std::ifstream f(ffbConfigPath); if (!f) return;
    char line[128];
    auto readFloat = [](const char* line, const char* key, float& out) {
        const char* p = strstr(line, key);
        if (p) { p += strlen(key); if (*p == '=') out = (float)atof(p + 1); }
    };
    while (f.getline(line, 128)) {
        readFloat(line, "springStrength",   s.springStrength);
        readFloat(line, "springStartAngle", s.springStartAngle);
        readFloat(line, "springFullAngle",  s.springFullAngle);
        readFloat(line, "gLatStrength",     s.gLatStrength);
        readFloat(line, "springLinearity",  s.springLinearity);
        readFloat(line, "absTorqueStr",     s.absTorqueStr);
        readFloat(line, "tcTorqueStr",      s.tcTorqueStr);
        readFloat(line, "wheelRumbleStr",   s.wheelRumbleStr);
        readFloat(line, "wheelRumbleThr",   s.wheelRumbleThr);
        readFloat(line, "absStrength",      s.absStrength);
        readFloat(line, "gSpikeThreshold",  s.gSpikeThreshold);
        readFloat(line, "gSpikeStrength",   s.gSpikeStrength);
        readFloat(line, "pedalRumbleStr",   s.pedalRumbleStr);
        readFloat(line, "pedalRumbleThr",   s.pedalRumbleThr);
    }
}

// ── FFB ───────────────────────────────────────────────────────────────────────
// Serial output format:  rpm;gear;speed;torque;rumble;rpmPct;blink\n
// RPM LED thresholds: RPM_GREEN/YELLOW/RED/BLINK now come from SimRacePro.h,
// shared with GuiWindow.cpp (see CODE_REVIEW.md 1.9).
//   torque : 0-255  (127 = center / no force)
//   rumble : 0-255  motor vibration intensity

// ── computeTorque ─────────────────────────────────────────────────────────────
// ALWAYS called – steering resistance is active even with no game running,
// so the wheel has weight in unsupported titles and menus.
//
// Model:
//   1. Linear spring   – 0% at ≤5°, 100% at ≥100°, pushes wheel back to centre.
//                        Simple and predictable; identical to the proven old model.
//   2. Lateral G-force – added when game telemetry is active.  Simulates tyre
//                        self-aligning torque in corners (wheel wants to go straight).
//
// NOTE: A velocity damper was intentionally removed – it caused the wheel to
// oscillate / strike left-right uncontrollably due to frame-to-frame angle noise.

static float computeTorque(const TelemetryData& t, float steerAngle,
                           float* outSpring = nullptr, float* outGLat = nullptr) {
    // ── SAFETY: Game paused or no telemetry → neutral immediately ────────────
    // Prevents motor from holding a sustained force when UDP stops (pause/menu).
    if (t.gamePaused) {
        if (outSpring) *outSpring = 0.0f;
        if (outGLat)   *outGLat   = 0.0f;
        return 0.0f;  // 0 = neutral (sent as 127 to box)
    }
    // Snapshot once under the lock - the GUI writes individual fields live as
    // FFB sliders move, so reading g_ffbSettings field-by-field without a lock
    // is a formal data race (harmless in practice on x64 for aligned floats,
    // but a local copy costs nothing and removes it outright).
    FfbSettings f; { std::lock_guard<std::mutex> lk(g_ffbSettingsMutex); f = g_ffbSettings; }

    // ── 1. Linear spring ─────────────────────────────────────────────────────
    float absA   = std::abs(steerAngle);
    float spring = 0.0f;
    if (absA > f.springStartAngle) {
        float range = (f.springFullAngle - f.springStartAngle < 1.0f)
                      ? 1.0f : (f.springFullAngle - f.springStartAngle);
        float t01 = clamp((absA - f.springStartAngle) / range, 0.0f, 1.0f);
        // Linearity blend: 0=pure linear, 1=log curve (default), 2=very progressive
        float lin  = t01;                                         // linear
        float logC = log(1.0f + t01 * 9.0f) / log(10.0f);       // logarithmic
        float blend = clamp(f.springLinearity, 0.0f, 2.0f) / 2.0f;
        float curve = lin + (logC - lin) * blend * 2.0f;         // blend 0→1 = lin→log
        if (blend > 0.5f) curve = logC + (logC * (blend - 0.5f) * 2.0f) * 0.3f; // extra progressive
        spring = clamp(curve, 0.0f, 1.0f) * 127.0f;
        if (steerAngle > 0.0f) spring = -spring;
        spring *= f.springStrength;
    }

    // ── 2. Lateral G-force (only with active telemetry) ───────────────────────
    float gLatForce = 0.0f;
    // Skip gLat computation entirely when no game is active or g_lat is zero.
    if (t.gameName != "None" && t.g_lat != 0.0f) {
        float gScale = GLAT_SCALE_DEFAULT;
        if      (t.gameName == "RaceRoom")               gScale = GLAT_SCALE_RACEROOM;
        else if (t.gameName == "DiRT Rally"
              || t.gameName == "EA WRC"
              || t.gameName == "GRID Autosport")          gScale = GLAT_SCALE_DIRT_GRID;
        else if (t.gameName == "ETS2/ATS")               gScale = GLAT_SCALE_TRUCKS;
        // Pure lateral-g force (original): simulates tyre self-aligning torque.
        float pureLat = -t.g_lat * gScale * f.gLatStrength;

        // Combined force (v2.0 style): modulates g_lat by steering angle.
        // At centre (0 deg) the effect is zero; it grows with lock angle.
        // Formula mirrors v2.0: satForce = -wheelAngle * (g_lat * scale) * 0.7
        float angleFactor = clamp(steerAngle / 90.0f, -1.0f, 1.0f);
        float combined    = -angleFactor * t.g_lat * gScale * f.gLatStrength * 0.7f;

        // Blend: 50% pure lateral-g + 50% angle-modulated (adjust to taste).
        gLatForce = clamp(pureLat * 0.5f + combined * 0.5f, -100.0f, 100.0f);
    }

    if (outSpring) *outSpring = spring;
    if (outGLat)   *outGLat   = gLatForce;

    // ── 3. ABS torque pulse ───────────────────────────────────────────────────
    // When ABS is active: add an alternating push against current steer direction.
    // Simulates the pulsing kickback felt in the steering column under hard braking.
    float absTorque = 0.0f;
    if (t.absActive && f.absTorqueStr > 0.0f) {
        static auto absToggle = std::chrono::steady_clock::now();
        static bool absPhase = false;
        auto nowT = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(nowT - absToggle).count() > 80) {
            absToggle = nowT;
            absPhase = !absPhase;
        }
        if (absPhase) {
            float pushDir = (steerAngle >= 0.0f) ? 1.0f : -1.0f;
            absTorque = pushDir * 35.0f * f.absTorqueStr;
        }
    }

    // ── 4. TC torque pulse ────────────────────────────────────────────────────
    // When TC is active: light pulsing in throttle direction to indicate wheelspin.
    float tcTorque = 0.0f;
    if (t.tcActive && f.tcTorqueStr > 0.0f) {
        static auto tcToggle = std::chrono::steady_clock::now();
        static bool tcPhase = false;
        auto nowTc = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(nowTc - tcToggle).count() > 60) {
            tcToggle = nowTc;
            tcPhase = !tcPhase;
        }
        if (tcPhase) {
            // TC intervention: wheel kicks opposite to steering (rear stepping out)
            float pushDir = (steerAngle >= 0.0f) ? -1.0f : 1.0f;
            tcTorque = pushDir * 25.0f * f.tcTorqueStr;
        }
    }

    return clamp(spring + gLatForce + absTorque + tcTorque, -127.0f, 127.0f);
}

// ── computeRumble ─────────────────────────────────────────────────────────────
// Three independent sources, each with its own threshold/strength setting:
//   A) Surface  – suspension velocity / tyre slip (wheelRumbleThr filters road noise)
//   B) ABS      – hard pulse on brake lockup (absStrength scales it)
//   C) G-Spike  – sharp direction change / kerb hit (gSpikeThreshold + gSpikeStrength),
//                 lateral value normalised per game first, see GSPIKE_REF_*
// Overall scale: wheelRumbleStr applied to the sum before clamping.
static float computeRumble(const TelemetryData& t) {
    // Skip all computation when no game is active.
    if (t.gameName == "None") return 0.0f;
    // Snapshot once under the lock - see computeTorque() for why.
    FfbSettings f; { std::lock_guard<std::mutex> lk(g_ffbSettingsMutex); f = g_ffbSettings; }
    float result = 0.0f;

    // ── A. Surface: per-wheel suspension velocity / tyre slip ─────────────────
    float maxV = 0.0f;
    for (int i = 0; i < 4; i++) maxV = std::max(maxV, std::abs(t.suspensionVelocity[i]));
    float r = 0.0f;
    if (maxV > 0.0f) {
        // ACC/AC now deliver real suspension velocity in m/s (differentiated in
        // the reader), so they share RaceRoom's scale instead of the old /8,
        // which was calibrated for wheelSlip and made kerbs unreachable.
        if      (t.gameName == "ACC" || t.gameName == "Assetto Corsa") r = clamp(maxV / 2.0f, 0.0f, 1.0f);
        else if (t.gameName == "RaceRoom")                              r = clamp(maxV / 2.0f, 0.0f, 1.0f);
        else if (t.gameName == "Forza")                                 r = clamp(maxV / 6.0f, 0.0f, 1.0f);
        else if (t.gameName == "DiRT Rally" || t.gameName == "EA WRC"
              || t.gameName == "GRID Autosport")                        r = clamp(maxV / 2.0f, 0.0f, 1.0f);
        else                                                             r = clamp(maxV / 3.0f, 0.0f, 1.0f);
    }
    // Slides/lockups (ACC/AC): kept as a second source in its own unit domain so
    // the switch to suspension velocity above did not silently drop it. Whichever
    // is more severe wins - a kerb taken mid-slide should not read as calmer than
    // either event alone.
    if (t.wheelSlipMax > 0.0f)
        r = std::max(r, clamp(t.wheelSlipMax / 8.0f, 0.0f, 1.0f));
    if (r > 0.0f) {
        const float thr = f.wheelRumbleThr;
        if (r > thr) result += ((r - thr) / (1.0f - thr + 0.001f)) * 255.0f;
    }

    // ── B. ABS pulse ──────────────────────────────────────────────────────────
    if (t.absActive)
        result += 180.0f * f.absStrength;

    // ── C. Lateral G-spike ────────────────────────────────────────────────────
    // Normalise the reader's lateral value into a common domain before applying
    // the threshold - RaceRoom's is a ±1 steering force and ETS2/ATS trucks peak
    // near 0.4 g, so an absolute g threshold was unreachable for both. See
    // GSPIKE_REF_* in SimRacePro.h. Every other game divides by 1.0 (no change).
    float gRef = GSPIKE_REF_DEFAULT;
    if      (t.gameName == "RaceRoom")  gRef = GSPIKE_REF_RACEROOM;
    else if (t.gameName == "ETS2/ATS")  gRef = GSPIKE_REF_TRUCKS;

    float absG = std::abs(t.g_lat) / gRef;
    float gThr = f.gSpikeThreshold;
    if (absG > gThr)
        result += clamp((absG - gThr) / 2.0f, 0.0f, 1.0f) * 120.0f * f.gSpikeStrength;

    return clamp(result * f.wheelRumbleStr, 0.0f, 255.0f);
}

// ── Calibration wizard ────────────────────────────────────────────────────────
static bool runCalibration(SerialPort& serial, std::map<int, USHORT>& btnMap, GuiState& g) {
    static const struct { const char* name; USHORT val; } T[] = {
        {"A",XBTN_A},{"B",XBTN_B},{"X",XBTN_X},{"Y",XBTN_Y},
        {"DPad Right",XBTN_RIGHT},{"DPad Left",XBTN_LEFT},{"DPad Up",XBTN_UP},{"DPad Down",XBTN_DOWN},
        {"Left Stick (L3)",XBTN_LTHUMB},{"Right Stick (R3)",XBTN_RTHUMB},{"Guide",XBTN_GUIDE},
        {"Left Shoulder (LB)",XBTN_LSHOULDER},{"Back",XBTN_BACK},{"Start",XBTN_START},
        {"Right Shoulder (RB)",XBTN_RSHOULDER}
    };
    const int N = (int)(sizeof(T) / sizeof(T[0]));

    for (int ti = 0; ti < N; ti++) {
        { std::lock_guard<std::mutex> lk(g.mtx); g.calib_current_button = T[ti].name; g.calib_step = ti; g.calib_total = N; g.calib_waiting = true; }
        PostMessage(g.hwnd, WM_APP_CALIB_NEXT, 0, 0);
        while (true) {
            if (g.calib_aborted.load()) return false;
            bool w; { std::lock_guard<std::mutex> lk(g.mtx); w = g.calib_waiting; }
            if (!w) break; Sleep(10);
        }
        if (g.calib_aborted.load()) return false;

        // Send heartbeat so Box knows PC is alive and starts emitting 0xAA packets.
        serial.purgeRx();
        serial.writePacket(127, 0, 0, false, false, 0);
        g.log("Calib: press " + std::string(T[ti].name));
        bool det = false;
        auto t0  = std::chrono::steady_clock::now();
        auto tHb = std::chrono::steady_clock::now();
        while (!det) {
            if (g.calib_aborted.load()) return false;
            if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count() > 30) break;
            // Heartbeat every 500 ms to keep Box connected (PC_TIMEOUT_MS = 3 s).
            auto tNow = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(tNow - tHb).count() > 500) {
                tHb = tNow;
                serial.writePacket(127, 0, 0, false, false, 0);
            }
            // Purge on every iteration: Box sends ~12 kB/s but Sleep(2) actually
            // sleeps ~15 ms (Windows 15.625 ms timer granularity), so we drain only
            // ~500 B/s.  The OS 4096-byte buffer fills in <0.5 s and new button-
            // press packets get silently dropped.  Purging each iteration keeps the
            // buffer at zero so the very next ReadFile call returns a fresh packet.
            serial.purgeRx();
            uint8_t pkt[10];
            if (serial.readPacket(pkt)) {
                uint8_t btnLo = pkt[6], btnHi = pkt[5];
                for (int i = 0; i < 8; i++) {
                    if ((btnLo >> i) & 1) {
                        bool used = false; for (auto& kv : btnMap) if (kv.first == i) { used = true; break; }
                        if (!used) { btnMap[i] = T[ti].val; g.log("Mapped btn " + std::to_string(i) + " -> " + T[ti].name); PostMessage(g.hwnd, WM_APP_CALIB_MAPPED, (WPARAM)i, (LPARAM)T[ti].val); det = true; break; }
                    }
                }
                if (!det) for (int i = 0; i < 8; i++) {
                    if ((btnHi >> i) & 1) {
                        int idx = i + 8;
                        bool used = false; for (auto& kv : btnMap) if (kv.first == idx) { used = true; break; }
                        if (!used) { btnMap[idx] = T[ti].val; g.log("Mapped btn " + std::to_string(idx) + " -> " + T[ti].name); PostMessage(g.hwnd, WM_APP_CALIB_MAPPED, (WPARAM)idx, (LPARAM)T[ti].val); det = true; break; }
                    }
                }
            }
            Sleep(2);
        }
        Sleep(300);
    }
    { std::lock_guard<std::mutex> lk(g.mtx); g.calib_waiting = false; g.calib_done = true; }
    PostMessage(g.hwnd, WM_APP_CALIB_DONE, 0, 0);
    return true;
}

// ── Wiring setup wizard ───────────────────────────────────────────────────────
// GPIO wiring differs between builds, so the visual button positions on the
// wheel image don't necessarily match the bit indices the Wheel transmits.
// For each visual position the GUI highlights the button on the wheel image
// and the user presses the matching physical button; the detected bit index is
// recorded in posToPhys (all entries must be preset to -1 by the caller).
// Serial handling mirrors runCalibration (heartbeat + per-iteration purge,
// see pitfalls #13).
static bool runWiringSetup(SerialPort& serial, int posToPhys[16], GuiState& g) {
    const int N = 16;
    for (int pos = 0; pos < N; pos++) {
        { std::lock_guard<std::mutex> lk(g.mtx); g.wiring_pos = pos; g.wiring_total = N; g.wiring_waiting = true; }
        PostMessage(g.hwnd, WM_APP_WIRING_NEXT, 0, 0);
        while (true) {
            if (g.wiring_aborted.load()) { { std::lock_guard<std::mutex> lk(g.mtx); g.wiring_pos = -1; } PostMessage(g.hwnd, WM_APP_WIRING_DONE, 0, 0); return false; }
            bool w; { std::lock_guard<std::mutex> lk(g.mtx); w = g.wiring_waiting; }
            if (!w) break; Sleep(10);
        }

        // Heartbeat so the Box knows the PC is alive and keeps sending 0xAA packets.
        serial.purgeRx();
        serial.writePacket(127, 0, 0, false, false, 0);
        g.log("Wiring: press the highlighted button (" + std::to_string(pos + 1) + "/" + std::to_string(N) + ")");
        bool det = false;
        auto t0  = std::chrono::steady_clock::now();
        auto tHb = std::chrono::steady_clock::now();
        while (!det) {
            if (g.wiring_aborted.load()) { { std::lock_guard<std::mutex> lk(g.mtx); g.wiring_pos = -1; } PostMessage(g.hwnd, WM_APP_WIRING_DONE, 0, 0); return false; }
            if (g.wiring_skip.exchange(false)) { g.log("Wiring: position " + std::to_string(pos + 1) + " skipped."); break; }
            if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count() > 30) { g.log("Wiring: timeout - position " + std::to_string(pos + 1) + " left unassigned."); break; }
            auto tNow = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(tNow - tHb).count() > 500) {
                tHb = tNow;
                serial.writePacket(127, 0, 0, false, false, 0);
            }
            serial.purgeRx();
            uint8_t pkt[10];
            if (serial.readPacket(pkt)) {
                uint16_t bits = ((uint16_t)pkt[5] << 8) | pkt[6];  // btnHi | btnLo
                for (int i = 0; i < 16 && !det; i++) {
                    if (!((bits >> i) & 1)) continue;
                    // Each physical button can only sit at one position - this
                    // also ignores a button still held from the previous step.
                    bool used = false; for (int p = 0; p < pos; p++) if (posToPhys[p] == i) { used = true; break; }
                    if (used) continue;
                    posToPhys[pos] = i;
                    g.log("Wiring: position " + std::to_string(pos + 1) + " -> physical button " + std::to_string(i));
                    det = true;
                }
            }
            Sleep(2);
        }
        Sleep(300);  // debounce before the next position
    }
    { std::lock_guard<std::mutex> lk(g.mtx); g.wiring_pos = -1; g.wiring_waiting = false; }
    PostMessage(g.hwnd, WM_APP_WIRING_DONE, 0, 0);
    return true;
}

// ── Backend thread ────────────────────────────────────────────────────────────
// ── Game registry entry (defined globally so MSVC initializer-list works) ─────
struct GameEntry {
    std::string               name;
    std::vector<std::wstring> exes;        // process names to check (any match = running)
    bool                      running    = false;
    bool                      wasRunning = false;
    std::function<void()>     onStart;    // called once when game is detected
    std::function<void()>     onStop;     // called once when game exits
    // Optional extra check after EXE found (e.g. AC_LIVE for AC/ACC)
    // nullptr = always active once EXE is running
    std::function<bool()>     sessionCheck;
};

static int normalizeGear(int rawGear, const std::string& gameName) {
    if (gameName == "ACC" || gameName == "Assetto Corsa") return rawGear - 1;
    return rawGear;
}

// Manual H-shifter support: the virtual Xbox 360 pad has no "gear lever" axis,
// so a detected gear (from the box's readShifterGear(), see 2.1) is instead
// turned into a momentary '1'-'6' key press - the same "direct gear select"
// key binding approach SimRaceProv2 used (there via Python's keyboard.press(),
// here via SendInput). Games without a native gear-select bind ignore it.
static void sendGearKeyPress(int gear) {
    if (gear < 1 || gear > 6) return;
    WORD vk = (WORD)('0' + gear);  // VK codes for '0'-'9' equal their ASCII value
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = vk; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// runBackend uses a large stack due to Reader objects with shared memory buffers.
// This is intentional – all readers are long-lived and stack allocation is safe here.
#pragma warning(push)
#pragma warning(disable: 6262)
void runBackend(GuiState& g) {
    lang = new LanguageManager();
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    g.log("=== SimRacePro ===");
    checkForUpdatesAsync(g.hwnd);

    ConfigManager cfg;
    g_cfg = &cfg;

    // Config-schema verification: configs written BEFORE the last on-disk
    // format change (CONFIG_SCHEMA_VERSION, see SimRaceProDefs.h) can no longer
    // be interpreted correctly, so they get wiped exactly like "Reset all
    // Settings" does and the user re-runs the wizards on a clean config. Must
    // run BEFORE any config is loaded below.
    //
    // Deliberately NOT "marker != VER_STRING": that reset every user's mapping
    // on every release, including GUI-only/bugfix ones where nothing about the
    // config had changed. The comparison is numeric and one-directional, so a
    // downgrade (marker newer than the running build) also leaves the configs
    // alone - their format is still the current one.
    //
    // A missing marker with existing configs means "updated from a pre-3.1.4
    // version" (older than any schema) and resets; a missing marker without
    // configs is just a fresh install. An unparseable marker is treated like a
    // missing one.
    {
        std::string lastVer = cfg.loadLastRunVersion();
        bool anyCfg = cfg.configExists() || cfg.hasOptionalHardwareConfig()
                   || cfg.layoutExists() || std::ifstream(cfg.getFfbConfigPath()).good();
        int marker[3], schema[3];
        bool cfgOutdated = !parseVerTriple(lastVer.c_str(), marker)
                        || (parseVerTriple(CONFIG_SCHEMA_VERSION, schema)
                            && std::lexicographical_compare(marker, marker + 3, schema, schema + 3));
        if (anyCfg && cfgOutdated) {
            std::remove(cfg.getConfigPath().c_str());
            std::remove(cfg.getHardwareConfigPath().c_str());
            std::remove(cfg.getFfbConfigPath().c_str());
            std::remove(cfg.getLayoutConfigPath().c_str());
            g.log("Config format change detected (" + (lastVer.empty() ? std::string("pre-3.1.4") : lastVer)
                + " -> " VER_STRING ", schema " CONFIG_SCHEMA_VERSION ") - settings reset for a clean setup.");
            // Heap string freed in the WndProc handler (WM_APP_UPDATE_AVAIL pattern).
            std::string* oldVer = new std::string(lastVer);
            g.cfg_reset_done = false;
            if (!PostMessage(g.hwnd, WM_APP_CFG_RESET, 0, (LPARAM)oldVer))
                delete oldVer;  // window already closed
            else
                // Wait for the user to acknowledge before continuing - otherwise
                // the welcome notice (posted right below) opens ON TOP of this
                // dialog and the user reads them in reverse order.
                while (g.running.load() && !g.cfg_reset_done.load()) Sleep(20);
        }
        else if (anyCfg && lastVer != VER_STRING) {
            g.log("Updated (" + lastVer + " -> " VER_STRING ") - config format unchanged, settings kept.");
        }
        // Always the running app version, not the schema version: the marker
        // doubles as the "updated from X" source for the reset dialog.
        cfg.saveLastRunVersion(VER_STRING);
    }

    { std::string dummy; std::lock_guard<std::mutex> lk(buttonMapMutex);
      if (cfg.configExists() && cfg.loadConfig(dummy, buttonMap)) g.log("Config loaded (" + std::to_string(buttonMap.size()) + " buttons)");
      if (cfg.loadLayout(g_posToPhys)) g.log("Wiring layout loaded."); }
    { std::lock_guard<std::mutex> lk(g_ffbSettingsMutex); cfg.loadFfbSettings(g_ffbSettings); }
    cfg.loadBoxSettings(g_boxSettings);
    cfg.loadBoxPinConfig(g_boxPinConfig);
    // Mirror from BoxSettings (single source of truth) - must run after loadBoxSettings()
    g.has_clutch    = g_boxSettings.hasClutch;
    g.has_handbrake = g_boxSettings.hasHandbrake;
    g.has_shifter   = g_boxSettings.hasShifter;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        g.log("[ERROR] WSAStartup failed - UDP telemetry unavailable");
    VirtualGamepad gamepad;
    if (gamepad.isInitialized()) g.log("[Gamepad] Initialized.");
    else g.log("[ERROR] Gamepad init failed - is ViGEmBus installed?");
    SerialPort serial;
    std::string port;

    auto scanForWheel = [&]() -> bool {
        serial.close(); lastTorque = 127.0f;
        g.connected = false; { std::lock_guard<std::mutex> lk(g.mtx); g.com_port = ""; }
        PostMessage(g.hwnd, WM_APP_CONN_STATUS, 0, 0);
        g.log("Scanning COM ports for wheel...");
        std::string boxVer, wheelVer;
        std::string found = serial.autoDetect(BAUD_RATE, boxVer, wheelVer);
        if (!found.empty()) {
            port = found; g.log("Wheel connected on " + port);
            g.log("Box firmware:   " + boxVer);
            // The Box reports 3 distinct states for wheelVer, see its setup()
            // Phase 1: a real "ver. x.y.z" once it validated a version packet,
            // "incompatible" if the link produced bytes but never a valid
            // version packet (Wheel is present/powered but its firmware never
            // sends this packet - i.e. old/wrong firmware), or "unknown" if the
            // link was completely silent for the full 4s wait (Wheel absent,
            // unpowered, or not wired up at all).
            bool wheelSilent      = (wheelVer == "unknown");
            bool wheelIncompatible = (wheelVer == "incompatible");
            g.log("Wheel firmware: " + (wheelSilent ? "no response (not connected?)" :
                                         wheelIncompatible ? "detected, but not speaking the current protocol (old firmware?)" :
                                         wheelVer));
            g.connected = true; { std::lock_guard<std::mutex> lk(g.mtx); g.com_port = port; }
            PostMessage(g.hwnd, WM_APP_CONN_STATUS, 0, 0);
            // Spring-Config 3× senden mit Abstand – Box muss erst im Loop sein
            // Send spring config once - box needs ~60ms after handshake to enter loop.
            Sleep(60);
            serial.sendSpringConfig(g_ffbSettings);
            serial.sendBoxSettings(g_boxSettings);
            serial.sendPinConfig(g_boxPinConfig);
            g_detectedWheelVer = wheelVer;
            // "unknown" (truly silent link) is intentionally kept OUT of the
            // firmware-mismatch check below - it isn't a version problem, so
            // routing it into the reflash dialog would tell the user to flash
            // firmware that has nothing wrong with it. "incompatible" (and any
            // real-but-wrong version string) DOES belong there: something is
            // connected and transmitting, it's just not current firmware.
            bool wheelResponded = !wheelSilent;
            bool boxOk   = (boxVer   == EXPECTED_BOX_FW_VERSION);
            bool wheelOk = wheelResponded && (wheelVer == EXPECTED_WHEEL_FW_VERSION);
            if (!boxOk || (wheelResponded && !wheelOk)) {
                // Post firmware mismatch message - GUI will show update wizard.
                // Do NOT allow normal connection until firmware is updated.
                FirmwareInfo* fi = new FirmwareInfo();
                fi->boxVer        = boxVer;
                fi->wheelVer      = wheelIncompatible ? "unknown (likely outdated firmware)" : wheelVer;
                fi->expectedBox   = EXPECTED_BOX_FW_VERSION;
                fi->expectedWheel = EXPECTED_WHEEL_FW_VERSION;
                fi->comPort       = port;
                fi->boxMismatch   = !boxOk;
                fi->wheelMismatch = wheelResponded && !wheelOk;
                if (!PostMessage(g.hwnd, WM_APP_FW_MISMATCH, 0, (LPARAM)fi))
                    delete fi;  // window already closed
                // Close serial - do not allow FFB until firmware matches.
                serial.close();
                g.connected = false;
                // Block backend until firmware update completes or user cancels.
                while (g.running.load() && !g.fw_update_done.load()) Sleep(200);
                g.fw_update_done = false;
                return false;  // retry scan - will reconnect after flashing
            }
            // Box is fine and reachable, but the Wheel link stayed completely
            // silent - warn once (not on every hard-reconnect retry while it
            // stays missing) and keep going: angle/pedals/FFB run entirely
            // through the Box's own encoder and don't need the Wheel, only
            // wheel buttons do.
            static bool s_wheelMissingWarned = false;
            if (wheelSilent) {
                if (!s_wheelMissingWarned) {
                    s_wheelMissingWarned = true;
                    g.log("Warning: Box connected, but no response from Wheel.");
                    PostMessage(g.hwnd, WM_APP_WHEEL_MISSING, 0, 0);
                }
            } else {
                s_wheelMissingWarned = false;
            }
            return true;
        }
        g.log("Wheel not found - retrying in 3s..."); return false;
    };

    // Step 1: First-run welcome notice (shown when no config exists / fresh install).
    // Reminds the user to flash both Arduinos manually before connecting.
    if (!cfg.hasOptionalHardwareConfig()) {
        g.welcome_done = false;
        PostMessage(g.hwnd, WM_APP_WELCOME, 0, 0);
        while (!g.welcome_done.load() && g.running.load()) Sleep(20);
        if (!g.running.load()) { g_cfg = nullptr; WSACleanup(); delete lang; return; }
    }

    // Step 2: Connect to Box (firmware version check runs inside scanForWheel).
    while (g.running.load() && !scanForWheel()) for (int i = 0; i < 30 && g.running.load(); i++) Sleep(100);
    if (!g.running.load()) { g_cfg = nullptr; WSACleanup(); delete lang; return; }

    // Step 3: Hardware config on first run (after successful connect so Box settings can be sent)
    if (!cfg.hasOptionalHardwareConfig()) {
        g.hw_dialog_done = false;
        PostMessage(g.hwnd, WM_APP_HW_DIALOG, 0, 0);
        while (!g.hw_dialog_done.load() && g.running.load()) Sleep(20);
        // Persist via the single BoxSettings format (same file/writer as the Box
        // Hardware Settings window) so the two no longer disagree - see 1.2.
        cfg.saveBoxSettings(g_boxSettings);
        // Send updated box settings to Box now that hardware is configured
        serial.sendBoxSettings(g_boxSettings);
    }

    // Step 4: Button calibration if no mapping exists
    bool needCalib; { std::lock_guard<std::mutex> lk(buttonMapMutex); needCalib = buttonMap.empty(); }

    // Step 4a: Wiring setup BEFORE the Xbox button mapping. GPIO wiring can
    // differ between builds, so the user first tells the software which
    // physical button sits at which position on the wheel image (the GUI
    // highlights the position to press). Only runs during a fresh setup
    // (no mapping yet) when no layout has been recorded - existing installs
    // keep the identity default and behave as before.
    if (needCalib && !cfg.layoutExists()) {
        g.log("No wiring layout. Starting wiring setup...");
        g.start_wiring_requested = false;
        g.wiring_aborted = false;
        PostMessage(g.hwnd, WM_APP_ASK_WIRING, 0, 0);
        while (g.running.load() && !g.start_wiring_requested.load() && !g.wiring_aborted.load()) Sleep(20);
        if (g.running.load() && !g.wiring_aborted.load()) {
            int newLayout[16]; for (int i = 0; i < 16; i++) newLayout[i] = -1;
            if (runWiringSetup(serial, newLayout, g)) {
                cfg.saveLayout(newLayout);
                { std::lock_guard<std::mutex> lk(buttonMapMutex); for (int i = 0; i < 16; i++) g_posToPhys[i] = newLayout[i]; }
                g.log("Wiring layout saved.");
                PostMessage(g.hwnd, WM_APP_BTNMAP_UPDATE, 0, 0);  // refresh overlay labels with the new layout
            } else g.log("Wiring setup aborted - keeping default wiring.");
        } else if (g.running.load()) g.log("Wiring setup skipped - keeping default wiring.");
    }

    if (needCalib) {
        g.log("No button mapping. Starting wizard...");
        g.start_calib_requested = false;
        PostMessage(g.hwnd, WM_APP_ASK_CALIB, 0, 0);
        while (!g.start_calib_requested.load() && !g.calib_aborted.load()) Sleep(20);
        if (!g.calib_aborted.load()) {
            // Build into a local map - runCalibration mutates it over the whole
            // wizard duration without holding buttonMapMutex, so it must not
            // touch the global buttonMap directly (see 1.5: reset-vs-backend race).
            std::map<int, USHORT> newMap;
            if (runCalibration(serial, newMap, g)) {
                cfg.saveConfig("", newMap);
                { std::lock_guard<std::mutex> lk(buttonMapMutex); buttonMap = std::move(newMap); }
                g.log("Calibration saved.");
            } else g.log("Calibration aborted.");
        }
    }
    std::map<int, USHORT> btnMapCopy; { std::lock_guard<std::mutex> lk(buttonMapMutex); btnMapCopy = buttonMap; }
    { std::lock_guard<std::mutex> lk(g.mtx); g.buttonMap = btnMapCopy; }
    PostMessage(g.hwnd, WM_APP_BTNMAP_UPDATE, 0, 0);

    F1Reader f1; AMS2Reader ams2; R3EReader r3e; ACCReader acc;
    g.log("Ready. Waiting for game...");

    float last_angle = 0, last_throttle = 0, last_brake = 0, last_clutch = 0;
    int   last_handbrake  = 0;
    int   last_shifter_gear = 0;

    // ── New game readers ──────────────────────────────────────────────────────
    ForzaReader       forza;
    ACOriginalReader  acOrig;
    IRacingReader     iracing;
    DirtRallyReader   dirt;
    GridAutosportReader grid;
    WRCReader         wrc;
    ETSReader         ets;
    BeamNGReader      beamng;
    RF2Reader         rf2;

    // ── Unified game registry ─────────────────────────────────────────────────
    // Each entry: display name, EXE list, running flag, was-running flag,
    // onStart lambda (starts listener if needed), onStop lambda.
    // Helper: close all shared-memory handles when a shared-mem reader stops
    auto smStop = [](auto& reader) { reader.setActive(false); };

    std::vector<GameEntry> games = {
        // ── F1 2018–2024 (UDP 20777) ─────────────────────────────────────────
        { "F1",
          { L"F1_25.exe", L"F1_24.exe", L"F1_23.exe", L"F1_22.exe",
            L"F1_2021.exe", L"F1_2021_dx12.exe",
            L"F1_2020.exe", L"F1_2020_dx12.exe",
            L"F1_2019.exe", L"F1_2019_dx12.exe",
            L"F1_2018.exe", L"F1_2018_dx12.exe" },
          false, false,
          [&]{ f1.start(); },
          [&]{ f1.stop();  } },

        // ── AMS2 / Project CARS 3 (shared mem $pcars2$) ──────────────────────
        { "AMS2",
          { L"pcars2.exe", L"pcars3.exe", L"ams2avx.exe", L"ams2.exe" },
          false, false,
          [&]{ ams2.setActive(true);  },
          [&]{ smStop(ams2); } },

        // ── RaceRoom (shared mem $R3E) ────────────────────────────────────────
        { "RaceRoom",
          { L"RRRE.exe", L"RRRE64.exe" },
          false, false,
          [&]{ r3e.setActive(true);  },
          [&]{ smStop(r3e); },
          // sessionCheck: RRRE.exe can be running before shared memory exists -
          // wait for $R3E so detection doesn't flicker (see CODE_REVIEW.md 1.6).
          [&]() -> bool {
              HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, "$R3E");
              if (!h) h = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\$R3E");
              bool smAvail = !!h;
              if (h) CloseHandle(h);
              return smAvail;
          }},

        // ── ACC (shared mem acpmf_*, only when AC_LIVE) ───────────────────────
        { "ACC",
          { L"AC2-Win64-Shipping.exe" },
          false, false,
          [&]{ acc.setActive(true);  },
          [&]{ smStop(acc); },
          // sessionCheck: only count as running when actually on-track (AC_LIVE)
          [&]() -> bool {
              HANDLE hP = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_physics");
              if (!hP) return false;
              HANDLE hG = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_graphics");
              if (!hG) { CloseHandle(hP); return false; }
              auto* gr = (SPageFileGraphic*)MapViewOfFile(hG, FILE_MAP_READ, 0, 0, sizeof(SPageFileGraphic));
              bool live = gr && gr->status == AC_LIVE;
              if (gr) UnmapViewOfFile(gr);
              CloseHandle(hG); CloseHandle(hP);
              return live;
          }},

        // ── Forza Motorsport / Horizon (UDP 5300) ─────────────────────────────
        { "Forza",
          { L"ForzaMotorsport.exe", L"ForzaHorizon6.exe", L"ForzaHorizon5.exe", L"ForzaHorizon4.exe" },
          false, false,
          [&]{ forza.start(); },
          [&]{ forza.stop();  } },

        // ── Assetto Corsa original (shared mem acpmf_*, only when AC_LIVE) ────
        { "Assetto Corsa",
          { L"acs.exe" },
          false, false,
          [&]{ acOrig.setActive(true);  },
          [&]{ smStop(acOrig); },
          [&]() -> bool {
              HANDLE hP = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_physics");
              if (!hP) return false;
              HANDLE hG = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\acpmf_graphics");
              if (!hG) { CloseHandle(hP); return false; }
              auto* gr = (SPageFileGraphic*)MapViewOfFile(hG, FILE_MAP_READ, 0, 0, sizeof(SPageFileGraphic));
              bool live = gr && gr->status == AC_LIVE;
              if (gr) UnmapViewOfFile(gr);
              CloseHandle(hG); CloseHandle(hP);
              return live;
          }},

        // ── iRacing (shared mem iRSDK) ────────────────────────────────────────
        { "iRacing",
          { L"iRacingSim64DX11.exe", L"iRacingSim64.exe" },
          false, false,
          [&]{ iracing.setActive(true);  },
          [&]{ smStop(iracing); } },

        // ── DiRT series (UDP 20777, standard port) ───────────────────────────
        { "DiRT Rally",
          { L"dirtrally2.exe", L"dirtrally.exe", L"dirt4.exe",
            L"dirt3game.exe", L"dirt3.exe", L"dirt2.exe", L"dirt2_game.exe",
            L"drt.exe", L"dirt5.exe" },
          false, false,
          [&]{ dirt.start(); },
          [&]{ dirt.stop();  } },

        // ── GRID Autosport (UDP motion, port 20777) ───────────────────────────
        // Config: Documents\My Games\GRID Autosport\hardwaresettings\hardware_settings_config.xml
        //   <motion enabled="true" ip="127.0.0.1" port="20777" delay="1" />
        { "GRID Autosport",
          { L"GRID_Autosport.exe", L"gridautosport.exe", L"GRIDAutosport_avx.exe" },
          false, false,
          [&]{ grid.start(); },
          [&]{ grid.stop();  } },

        // ── EA WRC / WRC Generations (shared mem) ────────────────────────────
        { "EA WRC",
          { L"WRC.exe", L"WRCGenerations.exe" },
          false, false,
          [&]{ wrc.setActive(true);  },
          [&]{ smStop(wrc); } },

        // ── ETS2 / ATS (shared mem via scs-sdk-plugin) ───────────────────────
        { "ETS2/ATS",
          { L"eurotrucks2.exe", L"Euro Truck Simulator 2.exe", L"amtrucks.exe", L"American Truck Simulator.exe" },
          false, false,
          [&]{ ets.setActive(true);  },
          [&]{ smStop(ets); } },

        // ── BeamNG.drive (UDP OutGauge 4444) ──────────────────────────────────
        { "BeamNG",
          { L"BeamNG.drive.x64.exe", L"BeamNG.Drive.exe" },
          false, false,
          [&]{ beamng.start(); },
          [&]{ beamng.stop();  } },

        // ── rFactor 2 / Le Mans Ultimate (shared mem via plugin) ──────────────
        { "rFactor 2",
          { L"rFactor2.exe", L"LeMansUltimate.exe" },
          false, false,
          [&]{ rf2.setActive(true);  },
          [&]{ smStop(rf2); } },
    };

    bool  wasF1 = false, wasAMS2 = false, wasR3E = false, wasACC = false;
    bool  f1Run = false,  ams2Run = false,  r3eRun = false,  accRun = false;
    bool  wasForza = false, wasAcOrig = false, wasIR = false;
    bool  wasDirt  = false, wasWRC    = false, wasETS = false;
    bool  wasGrid  = false;
    bool  wasBeamNG = false, wasRF2   = false;
    bool  forzaRun = false, acOrigRun = false, irRun = false;
    bool  dirtRun  = false, wrcRun    = false, etsRun = false;
    bool  gridRun  = false;
    bool  beamNGRun = false, rf2Run   = false;
    auto  lastGameCheck   = std::chrono::steady_clock::now();
    auto  reconfigStart   = std::chrono::steady_clock::now();
    bool  reconfigTrig    = false;

    while (g.running.load()) {
        auto now = std::chrono::steady_clock::now();

        // ── Config resend (triggered by a Settings save in the GUI) ───────────
        // The request stays pending until the link is actually up. It used to be
        // consumed with exchange(false) *before* the connected check, so a save
        // that happened to land inside a soft stall (which by design lasts up to
        // WHEEL_DISCONNECT_TIMEOUT_MS and is invisible to the user) was dropped
        // silently - while the GUI had already told the user "saved and sent to
        // Box". The setting then stayed at whatever the Box had in EEPROM until
        // the next reconnect resent it, which reads exactly like "this slider
        // does nothing". Only clear the flag once the packet has really gone out.
        if (g.connected.load()) {
            if (g.resend_spring_cfg.load()) {
                if (serial.sendSpringConfig(g_ffbSettings)) g.resend_spring_cfg = false;
            }
            if (g.resend_box_settings.load()) {
                if (serial.sendBoxSettings(g_boxSettings)) {
                    g.resend_box_settings = false;
                    // Logged unconditionally: this is the one place that proves
                    // what the Box was actually told, which is the first thing
                    // worth knowing when a motor setting appears to have no
                    // effect (the alternative being a hardware limit).
                    char bs[128];
                    sprintf_s(bs, "Box settings sent: maxPwm=%d minPwm=%d/%d ramp=%d stallProt=%d",
                              (int)g_boxSettings.motorMaxPwm, (int)g_boxSettings.motorMinPwmLeft,
                              (int)g_boxSettings.motorMinPwmRight, (int)g_boxSettings.softRampStep,
                              (int)g_boxSettings.stallProtection);
                    g.log(bs);
                }
            }
            if (g.resend_pin_config.load()) {
                if (serial.sendPinConfig(g_boxPinConfig)) g.resend_pin_config = false;
            }
        }

        // ── Game detection every 1 second ────────────────────────────────────
        // Primary detection: EXE running check for EVERY game, every cycle.
        // Listeners (UDP threads / shared-mem readers) are started/stopped
        // dynamically based on this check, saving resources when idle.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastGameCheck).count() > GAME_DETECTION_INTERVAL_MS) {
            lastGameCheck = now;

            // ONE snapshot for all games – avoids N blocking kernel calls per tick
            auto tSnap0 = std::chrono::steady_clock::now();
            refreshProcessSnapshot();
            if (g.ffb_debug_on.load()) {
                long long snapMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - tSnap0).count();
                char sbuf[64]; sprintf_s(sbuf, "SNAP  process_scan=%lldms", snapMs);
                debugLog(sbuf);
            }

            for (auto& ge : games) {
                // Step 1: is the EXE running?
                bool exeFound = false;
                for (const auto& exe : ge.exes) {
                    if (isProcessRunning(exe.c_str())) { exeFound = true; break; }
                }

                // Step 2: optional session-level check (e.g. AC_LIVE)
                bool nowRunning = false;
                if (exeFound) {
                    nowRunning = ge.sessionCheck ? ge.sessionCheck() : true;
                }

                ge.running = nowRunning;

                // Transition: game started
                if (ge.running && !ge.wasRunning) {
                    g.log("[" + ge.name + "] detected");
                    ge.onStart();
                    { std::lock_guard<std::mutex> lk(g.mtx); g.active_game = ge.name; }
                    PostMessage(g.hwnd, WM_APP_GAME_CHANGE, 0, 0);
                }
                // Transition: game stopped
                else if (!ge.running && ge.wasRunning) {
                    if (ge.name == "ETS2/ATS") debugLog("ETS2: process no longer found - lost detection");
                    g.log("[" + ge.name + "] closed");
                    ge.onStop();
                    { std::lock_guard<std::mutex> lk(g.mtx); g.active_game = ""; }
                    PostMessage(g.hwnd, WM_APP_GAME_CHANGE, 0, 0);
                }

                ge.wasRunning = ge.running;
            }

            // Rebuild legacy run-flags for telemetry update section below
            auto flag = [&](const std::string& n) -> bool {
                for (auto& ge : games) if (ge.name == n) return ge.running;
                return false;
            };
            f1Run      = flag("F1");
            ams2Run    = flag("AMS2");
            r3eRun     = flag("RaceRoom");
            accRun     = flag("ACC");
            forzaRun   = flag("Forza");
            acOrigRun  = flag("Assetto Corsa");
            irRun      = flag("iRacing");
            dirtRun    = flag("DiRT Rally");
            gridRun    = flag("GRID Autosport");
            wrcRun     = flag("EA WRC");
            etsRun     = flag("ETS2/ATS");
            beamNGRun  = flag("BeamNG");
            rf2Run     = flag("rFactor 2");
        }

        // ── Telemetry update ──────────────────────────────────────────────────
        // This loop is otherwise unthrottled (spins as fast as ReadFile allows),
        // so without a gate the shared-memory readers below would each copy
        // their full SharedMemory struct thousands of times per second for no
        // benefit. Gated to TELEMETRY_POLL_INTERVAL_MS - the whole block (including the
        // dataReceivedThisFrame bookkeeping) is skipped together on off-ticks,
        // since dataReceivedThisFrame is only meaningful for iterations where
        // update()/checkTimeout() actually ran; skipping just the update() calls
        // while still evaluating "no data received" every iteration would
        // otherwise spuriously reset telemetry to neutral on every gated tick.
        static auto s_lastTelemetryUpdate = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastTelemetryUpdate).count() >= TELEMETRY_POLL_INTERVAL_MS) {
        s_lastTelemetryUpdate = now;
        dataReceivedThisFrame = false;
        if (ams2Run) ams2.update();
        if (r3eRun)  r3e.update();
        if (accRun)  acc.update();
        // Shared-memory crash guard: if a SM game is active but gamePaused
        // has been true for >5 s, the game likely crashed. Force neutral.
        {
            static auto smPauseStart = std::chrono::steady_clock::now();
            bool smActive = ams2Run || r3eRun || accRun || wrcRun || etsRun || rf2Run;
            std::lock_guard<std::mutex> lk(telemetryMutex);
            if (smActive && currentTelemetry.gamePaused) {
                auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - smPauseStart).count();
                if (held > 5000) { currentTelemetry = TelemetryData{}; }
            } else {
                smPauseStart = std::chrono::steady_clock::now();
            }
        }
        if (acOrigRun) acOrig.update();
        if (irRun)   iracing.update();
        if (wrcRun)  wrc.update();
        if (etsRun)  ets.update();
        if (rf2Run)  rf2.update();
        bool udpTimeout = false;
        if (f1Run    && f1.checkTimeout())    { udpTimeout = true; dataReceivedThisFrame = false; }
        else if (f1Run)                         dataReceivedThisFrame = true;
        if (forzaRun  && forza.checkTimeout()) { udpTimeout = true; }
        else if (forzaRun)                      dataReceivedThisFrame = true;
        if (dirtRun   && dirt.checkTimeout())  { udpTimeout = true; }
        else if (dirtRun)                       dataReceivedThisFrame = true;
        if (gridRun   && grid.checkTimeout())  { udpTimeout = true; }
        else if (gridRun)                       dataReceivedThisFrame = true;
        if (beamNGRun && beamng.checkTimeout()){ udpTimeout = true; }
        else if (beamNGRun)                     dataReceivedThisFrame = true;

        if (!dataReceivedThisFrame) {
            std::lock_guard<std::mutex> lk(telemetryMutex);
            if (udpTimeout && currentTelemetry.gameName != "None") {
                // UDP stream stopped (pause/menu): keep game name but set paused
                // so FFB neutralises immediately without losing spring config
                currentTelemetry.gamePaused = true;
                currentTelemetry.g_lat      = 0.0f;
                currentTelemetry.absActive  = false;
                currentTelemetry.tcActive   = false;
            } else {
                currentTelemetry = TelemetryData();
            }
        } else {
            // Data received — ensure paused flag is cleared for UDP readers
            // (shared memory readers set it themselves)
            std::lock_guard<std::mutex> lk(telemetryMutex);
            if (currentTelemetry.gameName != "None" &&
                currentTelemetry.gameName != "AMS2" &&
                currentTelemetry.gameName != "ETS2/ATS") {
                currentTelemetry.gamePaused = false;
            }
        }
        }  // end 8ms telemetry-update gate

        // ── Serial: Binärprotokoll ────────────────────────────────────────────
        // Box → PC: [0xAA][angleHi][angleLo][acc][brk][btnHi][btnLo][extra][clutch][CRC8]
        // PC → Box: [0xBB][torque][rumble][packed][gear][speed][CRC8]
        {
            bool dbg = g.ffb_debug_on.load();
            if (dbg && !s_debugFile.is_open()) debugOpenFile();
            if (!dbg && s_debugFile.is_open()) debugClose();
        }
        static auto s_lastPktTime = std::chrono::steady_clock::now();
        // Last time a *valid* Box→PC packet was received - tracked independently
        // of the FFB send below. See WHEEL_DISCONNECT_TIMEOUT_MS for why.
        static auto s_lastValidRx = std::chrono::steady_clock::now();
        uint8_t pkt[10];
        if (serial.readPacket(pkt)) {
            s_lastValidRx = std::chrono::steady_clock::now();
            // Recovered from a soft stall (see the FFB send block below) - the
            // handle was kept open throughout, so this is just the status
            // flipping back, no reconnect/rescan was needed.
            if (!g.connected.load()) {
                g.connected = true;
                // Soft-stall recovery - deliberately not logged to the visible
                // console. These blips self-heal within WHEEL_HARD_RECONNECT_
                // TIMEOUT_MS and are harmless by design (see WHEEL_DISCONNECT_
                // TIMEOUT_MS); showing them to a normal user just reads as "is
                // something wrong?" for no actionable reason. Still visible via
                // FFB Debug ("RX gap=...ms" already covers this) for anyone
                // actually troubleshooting a flaky link.
                if (g.ffb_debug_on.load()) g.log("Wheel responding again.");
                PostMessage(g.hwnd, WM_APP_CONN_STATUS, 0, 0);
            }
            if (g.ffb_debug_on.load()) {
                auto nowPkt = std::chrono::steady_clock::now();
                long long gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowPkt - s_lastPktTime).count();
                s_lastPktTime = nowPkt;
                int16_t ar = (int16_t)((pkt[1] << 8) | pkt[2]);
                char dbuf[160];
                sprintf_s(dbuf, "RX  gap=%lldms  angle=%.1f  acc=%d  brk=%d  btnHi=0x%02X  btnLo=0x%02X  extra=0x%02X  clutch=%d",
                    gapMs, ar / 10.0f, pkt[3], pkt[4], pkt[5], pkt[6], pkt[7], pkt[8]);
                debugLog(dbuf);
            }
            // Paket dekodieren
            int16_t angleRaw = (int16_t)((pkt[1] << 8) | pkt[2]);
            last_angle    = angleRaw / 10.0f;
            last_throttle = clamp((float)pkt[3], 0.0f, 255.0f);
            last_brake    = clamp((float)pkt[4], 0.0f, 255.0f);
            uint8_t btnHi = pkt[5];
            uint8_t btnLo = pkt[6];
            // extra: bit0=wheelReset (re-zero happens box-side; shown as RESET
            // pill in the GUI) bit1=handbrake bits2-4=shifterGear(0-6)
            // bit5=pcLinkStale - see CODE_REVIEW.md 2.1 and the box's
            // PKT_BOX2PC layout.
            uint8_t extra = pkt[7];

            // bit5: the Box is reachable (we are decoding its packet right
            // now) but our FFB packets have not been arriving. No debounce
            // needed here - the Box only raises this after its own
            // PC_TIMEOUT_MS of silence, so it cannot flicker. Unlike a soft
            // stall this is not cosmetic: FFB really has dropped to the Box's
            // built-in spring for at least that long and the user can feel it,
            // so both transitions are logged unconditionally.
            {
                bool stale = (extra & 0x20) != 0;
                if (stale != g.pc_link_stale.load()) {
                    g.pc_link_stale = stale;
                    g.log(stale
                        ? "FFB commands are not reaching the Box - force feedback is running on the Box's built-in spring only."
                        : "FFB link to Box restored.");
                }
            }
            bool wheelResetP = (extra & 0x01) != 0;
            if (wheelResetP) g.wheel_reset_tick = GetTickCount64();
            static bool s_lastWheelReset = false;
            if (wheelResetP && !s_lastWheelReset)
                g.log("Wheel reset button pressed - center + pedal offsets re-zeroed.");
            s_lastWheelReset = wheelResetP;
            last_handbrake = (extra & 0x02) ? 1 : 0;
            last_clutch    = clamp((float)pkt[8], 0.0f, 255.0f);
            if (g.has_shifter) {
                int gear = (extra >> 2) & 0x07;
                if (gear != last_shifter_gear) { sendGearKeyPress(gear); last_shifter_gear = gear; }
            }

            std::vector<int> buttons;
            for (int i = 0; i < 8; i++) { if ((btnLo  >> i) & 1) buttons.push_back(i);     }
            for (int i = 0; i < 8; i++) { if ((btnHi  >> i) & 1) buttons.push_back(i + 8); }

            // Snapshot telemetry once under the lock - reused below for the ACC
            // angle correction, the gamepad update and the FFB computation, so
            // no code path touches currentTelemetry (a std::string-bearing struct
            // written concurrently by the UDP reader threads) without the lock.
            TelemetryData t; { std::lock_guard<std::mutex> lk(telemetryMutex); t = currentTelemetry; }

            float corrected = last_angle;
            if (t.gameName == "ACC") corrected += 3.0f;
            // Clamp just inside +-halfAngle (half of the user-configurable max
            // steering angle, see the main window's "Max Steering Angle" slider)
            // so games never receive a full wrap-around which causes the
            // in-game wheel to spin uncontrollably.
            float halfAngle = g_maxSteerAngleDeg.load(std::memory_order_relaxed) * 0.5f;
            corrected = clamp(corrected, -(halfAngle - 0.1f), halfAngle - 0.1f);

            gamepad.update(corrected, last_throttle, last_brake, last_clutch,
                           g.has_clutch, last_handbrake, g.has_handbrake, buttons,
                           t.gameName, t.f1Year, halfAngle);

            { std::lock_guard<std::mutex> lk(g.mtx);
              g.steering = last_angle; g.throttle = last_throttle / 255.0f;
              g.brake    = last_brake  / 255.0f; g.clutch = last_clutch / 255.0f;
              g.handbrake = last_handbrake; g.active_buttons = buttons; }
            // The Box floods packets at ~1kHz, but repainting the GUI that often
            // is wasted work - gate the repaint notification to ~60Hz. State above
            // is still updated every packet, so the next allowed repaint always
            // shows the latest values.
            {
                static auto s_lastGuiUpdate = std::chrono::steady_clock::now();
                auto nowGui = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::microseconds>(nowGui - s_lastGuiUpdate).count() >= 16667) {
                    s_lastGuiUpdate = nowGui;
                    PostMessage(g.hwnd, WM_APP_INPUT_UPDATE, 0, 0);
                }
            }

            // Hold Start + Back for 3s to reset config
            bool startP = false, backP = false;
            { std::lock_guard<std::mutex> lk(buttonMapMutex);
              for (int idx : buttons) { auto it = buttonMap.find(idx); if (it != buttonMap.end()) { if (it->second == XBTN_START) startP = true; if (it->second == XBTN_BACK) backP = true; } } }
            if (startP && backP) {
                if (!reconfigTrig) { reconfigStart = std::chrono::steady_clock::now(); reconfigTrig = true; }
                else if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - reconfigStart).count() > RECONFIG_HOLD_TIME_MS) {
                    g.log("Reconfig triggered. Restarting wizard.");
                    serial.close();
                    std::remove(cfg.getConfigPath().c_str());
                    for (auto& ge : games) { if (ge.running) { ge.onStop(); ge.running = false; } }
                    g_cfg = nullptr;
                    WSACleanup(); delete lang;
                    g.auto_close_requested = true;
                    PostMessage(g.hwnd, WM_CLOSE, 0, 0); return;
                }
            } else reconfigTrig = false;
        }

        // ── FFB berechnen und senden ────────────────────────────────────────────
        // Deliberately OUTSIDE "if (readPacket())", on its own fixed timer, using
        // the last known angle/telemetry even when no fresh Box packet arrived
        // this tick. It used to live inside that if-block, which meant a single
        // stalled Box→PC packet silently stopped all PC→Box traffic too. Since
        // the Box only resumes sending once it sees a fresh PC packet (its own
        // PC_TIMEOUT_MS logic), that turned a one-off glitch (see CODE_REVIEW.md
        // 1.3, the sync-byte-aliasing bug - much more likely to fire now that
        // torque bytes vary with real telemetry instead of sitting at a
        // constant ~127) into a permanent mutual stall: PC waiting for a Box
        // packet before it would send anything, Box waiting for a PC packet
        // before it would resume sending - neither side ever spoke first again.
        // Sending unconditionally breaks that deadlock.
        // Interval: see FFB_TX_INTERVAL_MS - it sets how much kerb/impact detail
        // survives the Box's torque filter, so it is not a free-choice number.
        static auto s_lastFfbTx = std::chrono::steady_clock::now();
        auto nowFfb = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(nowFfb - s_lastFfbTx).count() >= FFB_TX_INTERVAL_MS) {
            s_lastFfbTx = nowFfb;

            TelemetryData t; { std::lock_guard<std::mutex> lk(telemetryMutex); t = currentTelemetry; }

            float spring = 0.0f, gLat = 0.0f;
            float torque = computeTorque(t, last_angle, &spring, &gLat);
            int   rumble = (int)computeRumble(t);
            bool  gameOn = (t.gameName != "None");

            // ── Wheel texture: kerbs, surface and impacts through the rim ─────
            // The rumble byte below only ever reaches the Box's *pedal* vibration
            // motors, and the Wheel has no vibration motor at all - so on a rig
            // without pedal motors this signal used to go nowhere and nothing
            // about the road surface reached the driver. The predecessor of this
            // block acknowledged that ("so surface details remain perceptible")
            // but only ran when the spring was already past 95% of full lock,
            // i.e. essentially never, and at 15 units it was inaudible anyway.
            // See TEXTURE_MAX_TORQUE / TEXTURE_TOGGLE_MS.
            float torqueFinal = torque;
            if (rumble > 0 && gameOn) {
                float wheelRumbleStr; { std::lock_guard<std::mutex> lk(g_ffbSettingsMutex); wheelRumbleStr = g_ffbSettings.wheelRumbleStr; }
                float amp = (rumble / 255.0f) * TEXTURE_MAX_TORQUE * wheelRumbleStr;

                static auto texToggle = std::chrono::steady_clock::now();
                static bool texPhase  = false;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(nowFfb - texToggle).count() >= TEXTURE_TOGGLE_MS) {
                    texToggle = nowFfb;
                    texPhase  = !texPhase;
                }

                // Shift the whole oscillation inward when the main force sits too
                // close to the limit for it to fit. Simply clamping there would
                // flatten one half of the swing against the rail and the driver
                // would feel nothing at exactly the moments (full lock, big
                // impacts) that matter most. Biasing keeps the full peak-to-peak
                // amplitude at the cost of a slightly lower mean force - which is
                // the same trade the old 95% special case made, just applied
                // across the whole range instead of at one arbitrary point.
                float bias = 0.0f;
                if      (torque + amp >  127.0f) bias =  127.0f - amp - torque;
                else if (torque - amp < -127.0f) bias = -127.0f + amp - torque;

                torqueFinal = clamp(torque + bias + (texPhase ? amp : -amp), -127.0f, 127.0f);
            }

            // NOTE: no output scaling here on purpose. The Box already maps the
            // torque byte onto its own 0..motorMaxPwm range (see applyMotor in
            // the Box sketch), so scaling by motorMaxPwm/255 here applied that
            // setting a second time: at the default 178 the motor could only ever
            // reach 178/255 * 178 = 125 of 255 PWM, and - worse for feel - the
            // entire lower half of the command range landed below the Box's
            // minimum-PWM floor and was flattened onto a single constant value,
            // which is exactly where fine modulation would have lived.
            // Max PWM in Box HW Settings is now the single authority over output
            // level, and it means what it says.
            //
            // GUI shows unscaled spring+gLat so 100% = full computed FFB,
            // independent of the hardware output scale.
            lastTorque = clamp(spring + gLat, -127.0f, 127.0f) + 127.0f;
            int tm     = (int)(torqueFinal + 127.0f);

            int rp = 0;
            bool blink = false;
            if (gameOn) {
                static const std::map<std::string, int> fallbackMaxRpm = {
                    {"F1",15000},{"AMS2",9000},{"RaceRoom",9000},{"ACC",9000},
                    {"Forza",8500},{"Assetto Corsa",9000},{"iRacing",9000},
                    {"DiRT Rally",8000},{"EA WRC",8000},{"ETS2/ATS",2500},
                    {"BeamNG",9000},{"rFactor 2",9000},{"GRID Autosport",8000}
                };
                int mr = t.maxRpm;
                if (mr <= 0) { auto it = fallbackMaxRpm.find(t.gameName); mr = (it != fallbackMaxRpm.end()) ? it->second : 9000; }
                rp    = (int)clamp(((float)t.rpm / mr) * 100.0f, 0.0f, 100.0f);
                blink = (mr > 0 && t.rpm >= (int)(mr * RPM_BLINK));
            }

            if (g.ffb_debug_on.load()) {
                char tbuf[96];
                sprintf_s(tbuf, "TX  torque=%d  rumble=%d  rpmPct=%d  blink=%d  gameOn=%d",
                    tm, (int)clamp((float)rumble,0,255), rp, (int)blink, (int)gameOn);
                debugLog(tbuf);
            }
            // ACC and AC use 0=R,1=N,2=1st convention; all others use -1=R,0=N,1=1st
            // Gear already normalized to -1=R,0=N,1=1st by normalizeGear() in readers.
            int8_t gearForWheel = (int8_t)t.gear;
            uint8_t speedForWheel = (uint8_t)clamp(t.speed, 0.0f, 255.0f);
            bool wrote = serial.writePacket((uint8_t)tm, (uint8_t)clamp((float)rumble,0,255), (uint8_t)rp, blink, gameOn, gearForWheel, speedForWheel);

            // See WRITE_FAIL_STREAK_THRESHOLD: a lone failed WriteFile doesn't
            // mean the Box is dead, so don't escalate on it alone.
            static int s_writeFailStreak = 0;
            if (wrote) s_writeFailStreak = 0; else s_writeFailStreak++;

            auto gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowFfb - s_lastValidRx).count();

            // Soft stall: flip the UI to "disconnected" but deliberately leave
            // the COM handle open - see WHEEL_HARD_RECONNECT_TIMEOUT_MS for why.
            // Most stalls clear up on their own if we just keep talking on the
            // same handle (writePacket() above already ran unconditionally),
            // and readPacket() succeeding again above flips this back.
            if (g.connected.load() && gapMs > WHEEL_DISCONNECT_TIMEOUT_MS) {
                // Not logged to the console - see the matching comment on the
                // "Wheel responding again." site above.
                if (g.ffb_debug_on.load()) g.log("Wheel not responding...");
                g.connected = false;
                PostMessage(g.hwnd, WM_APP_CONN_STATUS, 0, 0);
                { std::lock_guard<std::mutex> lk(g.mtx); g.steering=0; g.throttle=0; g.brake=0; g.clutch=0; g.handbrake=0; g.active_buttons.clear(); }
                PostMessage(g.hwnd, WM_APP_INPUT_UPDATE, 0, 0);
            }

            // Hard stall (or writePacket() failing repeatedly, e.g. a real
            // unplug): the handle is very likely actually dead now - worth
            // the disruptive close()+rescan (which resets the Box via DTR).
            bool writeDead = s_writeFailStreak >= WRITE_FAIL_STREAK_THRESHOLD;
            if (writeDead || gapMs > WHEEL_HARD_RECONNECT_TIMEOUT_MS) {
                g.log(writeDead ? "Wheel disconnected. Reconnecting..." : "Wheel still not responding. Reconnecting...");
                s_writeFailStreak = 0;
                while (g.running.load() && !scanForWheel()) for (int i = 0; i < 30 && g.running.load(); i++) Sleep(100);
                if (!g.running.load()) break;
                s_lastValidRx = std::chrono::steady_clock::now();  // fresh baseline after reconnect
                // Stale is a property of the old link - the Box we just
                // (re)handshaked with starts out clean, so don't carry the
                // flag over and make the next packet log a bogus "restored".
                g.pc_link_stale = false;
            }
        }
    }

    // Stop any readers that are still active
    for (auto& ge : games) {
        if (ge.running) {
            ge.onStop();
            ge.running = false;
        }
    }
    serial.close();
    WSACleanup();
    delete lang;
    g_cfg = nullptr;
    debugClose();
}
#pragma warning(pop)
