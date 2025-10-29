#include "sim_telemetry.h"
struct TelemetryFrame;

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ######### INTERNAL UTILS #########

static inline CurbSide curbFrom(const char *s)
{
    if (!s || !*s)
        return CURB_NONE;
    char c = (char)tolower((unsigned char)s[0]);
    if (c == 'l')
        return CURB_LEFT;
    if (c == 'r')
        return CURB_RIGHT;
    if (c == 'c')
        return CURB_CENTER;
    return CURB_NONE;
}

static inline bool asBool(const char *s)
{
    if (!s || !*s)
        return false;
    char c = (char)tolower((unsigned char)s[0]);
    return (c == '1' || c == 't' || c == 'y'); // 1/true/yes
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Copies src into dst (bounded) and NUL-terminates.
static inline void copyLine(char *dst, size_t dstSize, const char *src)
{
    if (!dst || !dstSize)
        return;
    size_t i = 0;
    if (src)
    {
        while (src[i] && i < dstSize - 1)
        {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

// ######### PARSERS #########

bool parseTelemetry(const char *line, TelemetryFrame &t, char sep)
{
    if (!line || !*line)
        return false;

    // Local copy because strtok_r edits the buffer
    char buf[192];
    copyLine(buf, sizeof(buf), line);

    char seps[2] = {sep, '\0'};
    char *save = nullptr;
    char *tok = strtok_r(buf, seps, &save);

    for (int idx = 0; tok; ++idx, tok = strtok_r(nullptr, seps, &save))
    {
        switch (idx)
        {
        case 0:
            t.speed_kmh = (float)atof(tok);
            break;
        case 1:
            t.gear = (int8_t)atoi(tok);
            break;
        case 2:
            t.throttle = (float)atof(tok);
            break;
        case 3:
            t.brake = (float)atof(tok);
            break;
        case 4:
            t.has_steer = true;
            t.steer = (float)atof(tok);
            break;
        case 5:
            t.has_rpm = true;
            t.rpm = atoi(tok);
            break;
        case 6:
            t.has_g_lat = true;
            t.g_lat = (float)atof(tok);
            break;
        case 7:
            t.has_g_lon = true;
            t.g_lon = (float)atof(tok);
            break;
        case 8:
            t.has_g_vert = true;
            t.g_vert = (float)atof(tok);
            break;
        case 9:
            t.has_on_curb = true;
            t.on_curb = asBool(tok);
            break;
        case 10:
            t.has_curb_side = true;
            t.curb_side = curbFrom(tok);
            break;
        default:
            break; // ignore extra fields
        }
    }

    // Basic sanity
    t.throttle = clampf(t.throttle, 0.0f, 1.0f);
    t.brake = clampf(t.brake, 0.0f, 1.0f);
    if (t.has_steer)
        t.steer = clampf(t.steer, -1.0f, 1.0f);

    return true; // tolerant to missing tail fields
}

bool parseWheelPacket(const char *line,
                      float &degrees, int &acc, int &brk,
                      TelemetryFrame &t, char sep)
{
    if (!line || !*line)
        return false;

    char buf[256];
    copyLine(buf, sizeof(buf), line);

    char seps[2] = {sep, '\0'};
    char *save = nullptr;

    // degrees
    char *tok = strtok_r(buf, seps, &save);
    if (!tok)
        return false;
    degrees = (float)atof(tok);

    // acc
    tok = strtok_r(nullptr, seps, &save);
    if (!tok)
        return false;
    acc = atoi(tok);
    if (acc < 0)
        acc = 0;
    if (acc > 255)
        acc = 255;

    // brk
    tok = strtok_r(nullptr, seps, &save);
    if (!tok)
        return false;
    brk = atoi(tok);
    if (brk < 0)
        brk = 0;
    if (brk > 255)
        brk = 255;

    // Remaining substring is the telemetry packet
    const char *rest = save ? save : "";
    return parseTelemetry(rest, t, sep);
}
