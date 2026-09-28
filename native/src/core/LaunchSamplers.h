// The platform half of issue #61's store-launch watch: the "is the game running now?" samplers a LaunchWatcher
// polls. The rules they apply are pure and live in LaunchWatch.h (probe_launchwatch pins them); this file only
// reads the OS.
//
// WINDOWS ONLY. Off Windows every factory returns an empty sampler, LaunchWatcher::watch refuses it, and a store
// launch stays fire-and-forget exactly as it was before #61 — no play time is recorded for it there.
#pragma once
#include "LaunchWatch.h"

namespace LaunchSamplers
{
    // Whether this platform has samplers at all.
    bool supported();

    // Full image paths of every process this user can query (Toolhelp32 + QueryFullProcessImageName). Empty
    // off Windows. A process we may not open (a protected/elevated one) is skipped, never an error.
    QStringList runningImagePaths();

    // Steam: HKCU\Software\Valve\Steam\RunningAppID == appid; when that value is absent, a process under
    // `installDir` (skipped when installDir is empty). Empty sampler off Windows.
    LaunchWatcher::Sampler steam(const QString& appid, const QString& installDir);

    // Epic: a process whose image is under the game's InstallLocation. Empty sampler off Windows or when the
    // install location is unknown.
    LaunchWatcher::Sampler epic(const QString& installLocation);
}
