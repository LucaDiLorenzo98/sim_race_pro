#ifndef SIM_TELEMETRY_H
#define SIM_TELEMETRY_H

#include <stdint.h>

// ######### TELEMETRY TYPES #########

// Compact enum for curb side
enum CurbSide : uint8_t
{
    CURB_NONE = 0,
    CURB_LEFT,
    CURB_RIGHT,
    CURB_CENTER
};

// Telemetry frame (tolerates missing tail fields)
struct TelemetryFrame
{
    float speed_kmh = 0.0f;
    int8_t gear = 0;       // -1=R, 0=N, 1..8
    float throttle = 0.0f; // 0..1
    float brake = 0.0f;    // 0..1

    bool has_steer = false;
    float steer = 0.0f; // -1..1
    bool has_rpm = false;
    int rpm = 0;

    bool has_g_lat = false;
    float g_lat = 0.0f;
    bool has_g_lon = false;
    float g_lon = 0.0f;
    bool has_g_vert = false;
    float g_vert = 0.0f;

    bool has_on_curb = false;
    bool on_curb = false;
    bool has_curb_side = false;
    CurbSide curb_side = CURB_NONE;
};

// ######### TELEMETRY PARSERS #########

// Parse telemetry packet only (sep default: '-')
// Expected order:
// speed_kmh-gear-throttle-brake-steer-rpm-g_lat-g_lon-g_vert-on_curb-curb_side
bool parseTelemetry(const char *line, TelemetryFrame &t, char sep = '-');

// Parse full wheel line:
// degrees-acc-brk-<telemetry packet...>
bool parseWheelPacket(const char *line,
                      float &degrees, int &acc, int &brk,
                      TelemetryFrame &t, char sep = '-');

#endif // SIM_TELEMETRY_H
