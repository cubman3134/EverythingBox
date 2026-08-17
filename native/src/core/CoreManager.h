// Locates libretro cores in <app>/cores, and auto-downloads them from the libretro nightly buildbot when
// missing (progress is reported via a callback so the caller can show it inline). Cores are zipped on the
// buildbot; extracted with miniz. The right build is fetched for the running OS/arch: Windows .dll, macOS
// .dylib, Linux .so, Android _android.so.
#pragma once
#include <QString>
#include <functional>

class QObject;
class AddonManager;

namespace CoreManager
{
    // Wire the addon layer that serves BIOS (the EBS/Allarr file provider). The app calls this once at
    // startup with its AddonManager. Until it's set — or when no file provider is configured — every BIOS
    // fetch below is a no-op: BIOS now comes from a configured server, with no hardcoded fallback source.
    void setBiosProvider(AddonManager* addons);

    QString coresDir();                                  // <app>/cores (created if needed)
    QString corePath(const QString& coreName);           // <coresDir>/<core>_libretro.<dll|dylib|so>
    bool isInstalled(const QString& coreName);

    // Returns the core's .dll path, downloading + extracting it if absent. Empty on failure; when it fails
    // *error (if given) holds a message for the caller to show inline. onProgress(percent) is called during
    // the download so the caller can render an inline progress indicator (no popup). Synchronous (blocks on
    // a local event loop) — launch paths use ensureCoreAsync instead; this stays for user-driven panels
    // (Settings ▸ Games' per-system core picker).
    QString ensureCore(const QString& coreName, QString* error = nullptr,
                       const std::function<void(int percent)>& onProgress = {});

    // Async ensureCore: the same buildbot download + extract, chained on QNetworkAccessManager signals
    // instead of a nested event loop, so a stalled buildbot connection can never hang the caller (a transfer
    // timeout fails the download instead). onDone(corePath, error) always fires — immediately (synchronously)
    // when the core is already installed, else after the download settles; corePath empty => failed, with
    // `error` saying why — unless `context` is destroyed first, which aborts the download and drops both
    // callbacks. Callbacks run on `context`'s thread.
    void ensureCoreAsync(const QString& coreName, QObject* context,
                         const std::function<void(int percent)>& onProgress,
                         const std::function<void(const QString& corePath, const QString& error)>& onDone);

    // <data>/system : the libretro "system" folder, where cores look for BIOS / firmware. Passed to each
    // core via RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY (see RetroView / LibretroCore).
    QString systemDir();

    // <data>/saves : where cores are told to put the save files they write THEMSELVES (memory cards, .brm,
    // .smpc — everything the frontend does not manage through RETRO_MEMORY_SAVE_RAM). Passed to each core via
    // RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY. Deliberately the flat saves root and not saves/<system>: these
    // files are named and re-opened by the core itself, so moving the directory under a running install would
    // hide a memory card the user has been writing to for months. SaveSync walks saves/ recursively, so they
    // are backed up either way.
    QString savesDir();

    // Download any BIOS files `systemId` needs from the configured file provider into destDir (best-effort,
    // async, chained on QNetworkAccessManager signals — no nested event loop, so a slow or dead network can
    // never stall the caller). A system that needs NO BIOS (BiosCatalog::systemNeedsBios false — every
    // cartridge system) is a true zero-network no-op: onDone fires immediately with no server round-trip, so
    // those launches gain zero latency and a slow/unreachable BIOS server can't stall them. Files already
    // present with the right md5 are skipped; a wrong-hash copy is refetched; a downloaded file whose md5
    // mismatches is rejected (left missing). onDone always fires — immediately when the system needs no BIOS,
    // no provider is configured, or nothing is missing, else after the fetch chain settles — unless `context`
    // is destroyed first, which cancels the whole chain and drops both callbacks. Callbacks run on `context`'s
    // thread. A BIOS system delegates to AddonManager::ensureBiosAsync (see setBiosProvider).
    void ensureBiosAsync(const QString& systemId, const QString& destDir, QObject* context,
                         const std::function<void(const QString& text)>& onStatus = {},
                         const std::function<void()>& onDone = {});
}
