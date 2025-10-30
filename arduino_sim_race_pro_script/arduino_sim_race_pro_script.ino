#include "sim_telemetry.h"

// ============================================================================
//  SIM RACE PRO – Firmware Loader
// ============================================================================
//
// ✅ This project contains firmware for TWO separate Arduino boards:
//
//    1️⃣ BOX  (Force Feedback + Pedals + Sensors)
//    2️⃣ WHEEL (Buttons + OLED Display + LEDs)
//
//  Both firmwares share the SAME project folder.
//  You choose which firmware to compile by enabling ONE of the flags below:
//
//    #define BUILD_BOX     → upload to the BOX Arduino
//    #define BUILD_WHEEL   → upload to the WHEEL Arduino
//
//  IMPORTANT RULES:
//  ----------------
//  ✅ Enable ONLY ONE flag at a time
//  ✅ Before each upload, select the correct PORT in the Arduino IDE
//     (Tools → Port → choose the board connected)
//
//  QUICK WORKFLOW:
//  ---------------
//  ▶ To upload BOX firmware:
//       #define BUILD_BOX
//       // #define BUILD_WHEEL
//       Tools → Port → Arduino BOX ⇒ Upload
//
//  ▶ To upload WHEEL firmware:
//       // #define BUILD_BOX
//       #define BUILD_WHEEL
//       Tools → Port → Arduino WHEEL ⇒ Upload
//
//  SAFETY CHECK:
//  -------------
//  If both flags are active at the same time, compilation will FAIL intentionally
//  to prevent building two firmwares together.
//
// ============================================================================
//  SELECT TARGET BELOW
// ============================================================================

#define BUILD_BOX
// #define BUILD_WHEEL

#if defined(BUILD_BOX) && defined(BUILD_WHEEL)
#error "Choose only ONE: BUILD_BOX OR BUILD_WHEEL"
#endif