#pragma once
// =============================================================================
// SimRaceProDefs.h  -  Custom application defines
// This file is NOT managed by Visual Studio - safe to edit manually.
// =============================================================================

// VER_STRING and VER_STR are defined in resource.h (shared with Version.rc)

// ─────────────────────────────────────────────────────────────────────────────
// Config schema version - the app version in which the ON-DISK CONFIG FORMAT
// last changed. runBackend() wipes config.ini / hardware.ini / ffb.ini /
// layout.ini on startup only when the configs were written by a version OLDER
// than this (see the version check at the top of runBackend), NOT on every
// VER_STRING bump - a pure GUI/firmware/bugfix release must not cost the user
// their button mapping and wizard runs.
//
// BUMP THIS TO THE CURRENT VER_STRING whenever a change makes an existing
// config file unusable or misinterpreted, e.g.:
//   - a key is renamed, removed, or its unit/range/meaning changes
//   - a new key is added that MUST be filled in by a wizard (a plain new key
//     with a sane default in the struct does NOT need a bump - the loader just
//     keeps the default)
//   - the button-mapping / wiring-layout semantics change
//   - a config file is renamed or moved
// Leave it alone for GUI-only changes, firmware changes, and bugfixes.
//
// 3.1.6: hardware.ini gained the Box pin-mapping block (PinConfig).
// 3.1.7: GUI-only (slider flicker) - no config change, so NOT bumped.
// 3.1.8: Box framing fix + app-side link-stale reporting - no config change,
//        so NOT bumped. Users keep their settings across this update.
// 3.2.0: First public GitHub release. Version/URL change only - the on-disk
//        format is byte-identical to 3.1.6, so NOT bumped: users coming from
//        a 3.1.x build keep their mapping, wiring, hardware and FFB settings.
#define CONFIG_SCHEMA_VERSION      "3.1.6"

// Expected firmware versions (must match FW_VERSION in the Arduino sketches).
//
// RULE: NEITHER of these may ever be LOWER than VER_STRING (resource.h).
// A user comparing "software v3.1.8" against "Wheel ver. 3.1.6" cannot tell a
// deliberate split from a botched release, and the mismatch dialog names both
// numbers - so a trailing firmware version reads as a bug report waiting to
// happen. When VER_STRING is bumped, bump BOTH sketches' FW_VERSION with it,
// even when a sketch is functionally unchanged (it costs the user one extra
// flash, which is cheaper than the confusion).
//
// This overrides the 3.1.7 approach, where the Box was bumped alone and the
// Wheel deliberately left behind to spare users flashing an unchanged sketch.
#define EXPECTED_BOX_FW_VERSION    "ver. 3.2.0"
#define EXPECTED_WHEEL_FW_VERSION  "ver. 3.2.0"

// ─────────────────────────────────────────────────────────────────────────────
// GitHub update check
//
// checkForUpdatesAsync() (SimRacePro.cpp) does a plain HTTPS GET on
// HOST + PATH and expects the body to be nothing but a "major.minor.patch"
// string. version.txt in the repository root is exactly that file, so the
// raw.githubusercontent.com URL below serves it verbatim - no API token, no
// JSON parsing, no User-Agent requirements (the GitHub REST API would need
// all three).
//
// RELEASE ORDER MATTERS: publish the GitHub release FIRST, then push the
// version.txt bump. The check compares numerically and only fires when the
// remote version is HIGHER, so a pushed-but-unreleased version.txt would send
// every user to a releases page that does not yet have the new build.
//
// The branch name is part of the path - if the default branch is ever renamed
// away from "main", this path has to follow.
#define UPDATE_CHECK_HOST    L"raw.githubusercontent.com"
#define UPDATE_CHECK_PATH    L"/LucaDiLorenzo98/sim_race_pro/main/version.txt"

// Landing page opened when the user accepts the update prompt. Points at the
// releases page rather than a direct asset URL so the download keeps working
// when the asset file name changes between releases.
#define UPDATE_RELEASE_URL   "https://github.com/LucaDiLorenzo98/sim_race_pro/releases/latest"
