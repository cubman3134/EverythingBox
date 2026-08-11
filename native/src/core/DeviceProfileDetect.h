// Device performance profiles (issue #119) — the THIN, platform-specific detection glue over the pure
// DeviceProfile classifier. This half gathers the machine's identity (a DMI product string on Windows, an
// ro.product string on Android) plus a cheap capability triple (GPU renderer / logical cores / total RAM), calls
// DeviceProfile::identify(), and CACHES the result under a device-local ini key — hardware identity does not
// change between runs, and it must never sync to another machine (a Deck's tuned defaults would crush a phone).
//
// It also honours a MANUAL OVERRIDE (detection can be wrong, and an appliance image may want to pin a profile):
// if "device/profileOverride" holds a valid Kind token, that profile is used and detection is skipped. The
// resolved-and-cached token lives in "device/profile"; both sit under the "device/" prefix CloudSync already
// carves out as device-local (asserted in probe_cloudmerge), so no new sync-classification is needed.
//
// Everything OS-specific is confined here; DeviceProfile.h stays pure and headlessly testable.
#pragma once
#include <QString>
#include "DeviceProfile.h"
#include "EmuSettings.h"

namespace DeviceProfileDetect
{
    // The active profile for THIS machine. Resolution order:
    //   1. a valid "device/profileOverride" token (user/appliance pin) — detection skipped;
    //   2. the cached "device/profile" token from a previous run;
    //   3. a fresh detection pass (platform inputs -> DeviceProfile::identify), then cached.
    // Cheap after the first call (memoised in-process); safe under the offscreen QPA (no GUI).
    DeviceProfile::Profile active();

    // The tuned graphics defaults for THIS device and the given emulator id — the WEAKEST layer GameLauncher
    // resolves under per-system and per-game. An Unknown / no-opinion device yields an all-unset Settings (no-op).
    EmuGfx::Settings defaultsForEmulator(const QString& emulatorId);

    // Persist a manual override (empty token clears it, reverting to detection). Invalidates the memo.
    void setOverride(const QString& kindToken);
    QString overrideToken();   // "" when none

    // The raw detected profile, ignoring any override — for the settings readout's "detected: X" line.
    DeviceProfile::Profile detected();

    void invalidate();   // drop the in-process memo (test reset / external ini change)
}
