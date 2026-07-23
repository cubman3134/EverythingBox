#pragma once
#include <QString>
#include <QCoreApplication>
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
#include <QStandardPaths>
#include <QDir>
#endif

// The app's writable base directory. On desktop this is the executable's own folder - the app is portable,
// so mymediavault.ini, cores/, saves/, states/, downloads/, addons/, ... all live next to MyMediaVault.
// On Android and iOS the executable directory isn't writable, so use the app's private data location instead.
// Everything that builds a path off the app dir goes through here, so a platform only changes this function.
namespace AppPaths
{
    inline QString dataDir()
    {
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
        const QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(d);
        return d;
#else
        return QCoreApplication::applicationDirPath();
#endif
    }
}
