#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// irsdk_minimal.h – Minimal iRacing SDK headers for SimRacePro
//
// Shared memory: "Local\IRSDKMemMapFileName"
// iRacing uses a name-based variable lookup system – variables are found by
// scanning a header array, not at fixed offsets.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>

#define IRSDK_MEMMAPFILENAME    "Local\\IRSDKMemMapFileName"
#define IRSDK_DATAVALIDEVENTNAME "Local\\IRSDKDataValidEvent"

#define IRSDK_MAX_BUFS   4
#define IRSDK_MAX_STRING 32
#define IRSDK_MAX_DESC   64

// Variable types
typedef enum irsdk_VarType {
    irsdk_char = 0, irsdk_bool, irsdk_int, irsdk_bitField,
    irsdk_float, irsdk_double, irsdk_ETCount
} irsdk_VarType;

static const int irsdk_VarTypeBytes[irsdk_ETCount] = {1,1,4,4,4,8};

// Single variable header entry
typedef struct {
    int type;       // irsdk_VarType
    int offset;     // offset in data buffer (bytes from start of data block)
    int count;      // number of values (arrays)
    char pad[1];
    char name[IRSDK_MAX_STRING];
    char desc[IRSDK_MAX_DESC];
    char unit[IRSDK_MAX_STRING];
} irsdk_varHeader;

// Buffer descriptor (one of up to 4 rotating buffers)
typedef struct {
    int  tickCount;   // when was this buffer last updated
    int  bufOffset;   // offset from start of shared memory
    int  pad[2];
} irsdk_varBuf;

// Top-level header at byte 0 of shared memory
typedef struct {
    int          ver;           // API version (currently 2)
    int          status;        // 1 = connected
    int          tickRate;      // frames per second
    int          sessionInfoUpdate; // count of session info updates
    int          sessionInfoLen;
    int          sessionInfoOffset;
    int          numVars;          // number of telemetry variables
    int          varHeaderOffset;  // offset to irsdk_varHeader array
    int          numBuf;           // active data buffers (max 4)
    int          bufLen;           // length of each data buffer
    int          pad[2];
    irsdk_varBuf varBuf[IRSDK_MAX_BUFS];
} irsdk_header;

#define IRSDK_STCONNECTED 1
