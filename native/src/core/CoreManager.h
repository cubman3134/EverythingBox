// Locates libretro cores in <app>/cores, and auto-downloads them from the libretro nightly buildbot when
// missing (progress is reported via a callback so the caller can show it inline). Cores are zipped on the
// buildbot; extracted with miniz. The right build is fetched for the running OS/arch: Windows .dll, macOS
// .dylib, Linux .so, Android _android.so.
#pragma once
#include <QString>
#include <functional>

class QObject;

namespace CoreManager
{
    QString coresDir();                                  // <app>/cores (created if needed)
    QString corePath(const QString& coreName);           // <coresDir>/<core>_libretro.<dll|dylib|so>
    bool isInstalled(const QString& coreName);

    // Returns the core's .dll path, downloading + extracting it if absent. Empty on failure; when it fails
    // *error (if given) holds a message for the caller to show inline. onProgress(percent) is called during
    // the download so the caller can render an inline progress indicator (no popup).
    QString ensureCore(const QString& coreName, QString* error = nullptr,
                       const std::function<void(int percent)>& onProgress = {});

    // <data>/system : the libretro "system" folder, where cores look for BIOS / firmware. Passed to each
    // core via RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY (see RetroView / LibretroCore).
    QString systemDir();

    // Download any BIOS files `systemId` needs (BiosCatalog) into destDir, skipping ones already present.
    // Best-effort and synchronous (blocks on a local event loop, like ensureCore); a failed file is left
    // missing so the core/emulator reports it as it would have anyway. onStatus(text) reports progress.
    // Launch paths use ensureBiosAsync instead — this stays for flows that already run off a user-driven
    // panel/manager (Settings ▸ BIOS repair, EmulatorManager's install pipeline).
    void ensureBios(const QString& systemId, const QString& destDir,
                    const std::function<void(const QString& text)>& onStatus = {});

    // Async ensureBios: the same best-effort download, chained on QNetworkAccessManager signals instead of
    // a nested event loop, so a slow or dead network can never stall the caller (a transfer timeout fails a
    // stuck file rather than hanging). onDone always fires — after the last file settles, or immediately
    // (synchronously) when nothing is missing — unless `context` is destroyed first, which cancels the whole
    // chain and drops both callbacks. Callbacks run on `context`'s thread.
    void ensureBiosAsync(const QString& systemId, const QString& destDir, QObject* context,
                         const std::function<void(const QString& text)>& onStatus = {},
                         const std::function<void()>& onDone = {});
}
