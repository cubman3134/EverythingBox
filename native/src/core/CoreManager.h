// Locates libretro core DLLs in <app>/cores, and auto-downloads them from the libretro nightly buildbot
// when missing (progress is reported via a callback so the caller can show it inline). Cores are zipped on
// the buildbot; extracted with miniz.
#pragma once
#include <QString>
#include <functional>

class QWidget;

namespace CoreManager
{
    QString coresDir();                                  // <app>/cores (created if needed)
    QString corePath(const QString& coreName);           // <coresDir>/<core>_libretro.dll
    bool isInstalled(const QString& coreName);

    // Returns the core's .dll path, downloading + extracting it if absent. Empty on failure; when it fails
    // *error (if given) holds a message for the caller to show inline. onProgress(percent) is called during
    // the download so the caller can render an inline progress indicator (no popup).
    QString ensureCore(const QString& coreName, QWidget* parent = nullptr, QString* error = nullptr,
                       const std::function<void(int percent)>& onProgress = {});
}
