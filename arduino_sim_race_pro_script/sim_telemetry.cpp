#include "sim_telemetry.h"
struct TelemetryFrame;

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
    return (c == '1' || c == 't' || c == 'y');
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int clampi(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

// Read-only tokenizer: returns start of next token and advances p past the separator.
// Does not write into the source buffer; uses the global NUL terminator only.
static inline const char *nextTokenRO(const char *&p, char sep)
{
    if (!p || !*p)
        return nullptr;
    while (*p == sep)
        ++p;
    if (!*p)
        return nullptr;
    const char *start = p;
    while (*p && *p != sep)
        ++p;
    if (*p == sep)
        ++p;
    return start;
}

bool parseTelemetry(const char *line, TelemetryFrame &t, char sep)
{
    if (!line || !*line)
        return false;

    t.has_steer = false;
    t.has_rpm = false;
    t.has_g_lat = false;
    t.has_g_lon = false;
    t.has_g_vert = false;
    t.has_on_curb = false;
    t.has_curb_side = false;

    const char *p = line;
    for (int idx = 0;; ++idx)
    {
        const char *tok = nextTokenRO(p, sep);
        if (!tok)
            break;

        switch (idx)
        {
        case 0:
            t.speed_kmh = (float)strtod(tok, nullptr);
            break;
        case 1:
            t.gear = (int8_t)strtol(tok, nullptr, 10);
            break;
        case 2:
            t.throttle = (float)strtod(tok, nullptr);
            break;
        case 3:
            t.brake = (float)strtod(tok, nullptr);
            break;
        case 4:
            t.has_steer = true;
            t.steer = (float)strtod(tok, nullptr);
            break;
        case 5:
            t.has_rpm = true;
            t.rpm = (int)strtol(tok, nullptr, 10);
            break;
        case 6:
            t.has_g_lat = true;
            t.g_lat = (float)strtod(tok, nullptr);
            break;
        case 7:
            t.has_g_lon = true;
            t.g_lon = (float)strtod(tok, nullptr);
            break;
        case 8:
            t.has_g_vert = true;
            t.g_vert = (float)strtod(tok, nullptr);
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
            break;
        }
    }

    t.throttle = clampf(t.throttle, 0.0f, 1.0f);
    t.brake = clampf(t.brake, 0.0f, 1.0f);
    if (t.has_steer)
        t.steer = clampf(t.steer, -1.0f, 1.0f);

    return true;
}

bool parseWheelPacket(const char *line,
                      float &degrees, int &acc, int &brk,
                      TelemetryFrame &t, char sep)
{
    if (!line || !*line)
        return false;

    const char *p = line;

    const char *tok = nextTokenRO(p, sep);
    if (!tok)
        return false;
    degrees = (float)strtod(tok, nullptr);

    tok = nextTokenRO(p, sep);
    if (!tok)
        return false;
    acc = clampi((int)strtol(tok, nullptr, 10), 0, 255);

    tok = nextTokenRO(p, sep);
    if (!tok)
        return false;
    brk = clampi((int)strtol(tok, nullptr, 10), 0, 255);

    if (!p || !*p)
        return false;

    return parseTelemetry(p, t, sep);
}
