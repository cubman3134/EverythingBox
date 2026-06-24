// Locates libretro core DLLs in <app>/cores, and auto-downloads them from the libretro nightly buildbot
// when missing (shows a modal progress dialog). Cores are zipped on the buildbot; extracted with miniz.
#pragma once
#include <QString>

class QWidget;

namespace CoreManager
{
    QString coresDir();                                  // <app>/cores (created if needed)
    QString corePath(const QString& coreName);           // <coresDir>/<core>_libretro.dll
    bool isInstalled(const QString& coreName);

    // Returns the core's .dll path, downloading + extracting it if absent. Empty on failure/cancel; when it
    // fails (not a user cancel), *error (if given) holds a message for the caller to show inline.
    QString ensureCore(const QString& coreName, QWidget* parent = nullptr, QString* error = nullptr);
}
