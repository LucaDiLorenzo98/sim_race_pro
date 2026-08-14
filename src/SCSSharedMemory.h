#pragma once
// SCSSharedMemory.h - scsTelemetryMap_t matching RenCloud scs-sdk-plugin v1.12
// Shared memory name: "Local\SCSTelemetry"
// Plugin: https://github.com/RenCloud/scs-sdk-plugin
//
// Key offsets (no implicit padding - struct uses explicit char buffers):
//   0:    sdkActive (bool)
//   4:    paused (bool)
//   8:    time (uint64)
//   40:   scs_values.telemetry_plugin_revision (uint)
//   504:  truck_i.gear (int)
//   508:  truck_i.gearDashboard (int)
//   740:  config_f.engineRpmMax (float)
//   948:  truck_f.speed (float, m/s)
//   952:  truck_f.engineRpm (float)
//   960:  truck_f.userThrottle (float, 0-1)
//   964:  truck_f.userBrake (float, 0-1)
//   1916: truck_fv.accelerationX (float, lateral m/s²)
//
// WARNING - unresolved discrepancy: this struct actually places accelerationX at
// offset 1892 (verified with offsetof), not the 1916 documented above. Every
// other offset in this list matches. Either the note is wrong (truck_fv really
// starts with lv/av/acceleration, 3 vectors before it) or the struct is missing
// two fvectors and ETS2/ATS lateral g is read from the wrong field. Check
// against scs-sdk-plugin's scsTelemetryMap before relying on g_lat here.

#pragma pack(push, 1)

struct SCSGameState {
    // Zone 1: offset 0
    bool     sdkActive;
    char     _pad1[3];
    bool     paused;
    char     _pad2[3];
    uint64_t time;
    uint64_t simulatedTime;
    uint64_t renderTime;
    int64_t  multiplayerTimeOffset;
    // offset 40

    // Zone 2: unsigned ints
    struct {
        unsigned int telemetry_plugin_revision;
        unsigned int version_major;
        unsigned int version_minor;
        unsigned int game;
        unsigned int telemetry_version_game_major;
        unsigned int telemetry_version_game_minor;
    } scs_values;
    // common_ui
    unsigned int time_abs;
    // config_ui (9 uints)
    unsigned int _cfg_ui[9];
    // truck_ui
    unsigned int shifterSlot;
    unsigned int retarderBrake;
    unsigned int lightsAuxFront;
    unsigned int lightsAuxRoof;
    unsigned int truck_wheelSubstance[16];
    unsigned int hshifterPosition[32];
    unsigned int hshifterBitmask[32];
    // gameplay_ui (3 uints)
    unsigned int _gp_ui[3];
    char _buf_ui[48];
    // offset 500

    // Zone 3: ints
    int restStop;
    int gear;           // offset 504
    int gearDashboard;  // offset 508
    int hshifterResulting[32];
    int _gp_i;
    char _buf_i[56];
    // offset 700

    // Zone 4: floats
    float scale;        // offset 700
    // config_f
    float fuelCapacity;
    float fuelWarningFactor;
    float adblueCapacity;
    float adblueWarningFactor;
    float airPressureWarning;
    float airPressureEmergency;
    float oilPressureWarning;
    float waterTemperatureWarning;
    float batteryVoltageWarning;
    float engineRpmMax;     // offset 740
    float gearDifferential;
    float cargoMass;
    float truckWheelRadius[16];
    float gearRatiosForward[24];
    float gearRatiosReverse[8];
    float unitMass;
    // truck_f - offset 948
    float speed;            // offset 948 (m/s)
    float engineRpm;        // offset 952
    float userSteer;
    float userThrottle;     // offset 960
    float userBrake;        // offset 964
    float userClutch;
    float gameSteer;
    float gameThrottle;
    float gameBrake;
    float gameClutch;
    float cruiseControlSpeed;
    float airPressure;
    float brakeTemperature;
    float fuel;
    float fuelAvgConsumption;
    float fuelRange;
    float adblue;
    float oilPressure;
    float oilTemperature;
    float waterTemperature;
    float batteryVoltage;
    float lightsDashboard;
    float wearEngine;
    float wearTransmission;
    float wearCabin;
    float wearChassis;
    float wearWheels;
    float truckOdometer;
    float routeDistance;
    float routeTime;
    float speedLimit;
    float truck_wheelSuspDeflection[16];
    float truck_wheelVelocity[16];
    float truck_wheelSteering[16];
    float truck_wheelRotation[16];
    float truck_wheelLift[16];
    float truck_wheelLiftOffset[16];
    // gameplay_f, job_f, buffer_f
    float _gp_f[3];
    float _job_f;
    char  _buf_f[28];
    // offset 1500

    // Zone 5: bools (offset 1500)
    char _zone5[140];
    // offset 1640

    // Zone 6: fvectors (offset 1640)
    // config_fv: cabin(3) + head(3) + hook(3) + wheelPos XYZ (16*3) = 57 floats
    float _config_fv[57];
    // truck_fv:
    float lv_accelerationX;  // linear velocity
    float lv_accelerationY;
    float lv_accelerationZ;
    float av_accelerationX;  // angular velocity
    float av_accelerationY;
    float av_accelerationZ;
    float accelerationX;     // lands at offset 1892 here - see WARNING at top
    float accelerationY;
    float accelerationZ;
    // rest not needed
};

#pragma pack(pop)
