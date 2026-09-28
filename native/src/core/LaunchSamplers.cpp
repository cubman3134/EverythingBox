#include "LaunchSamplers.h"

#ifdef Q_OS_WIN
#include <QSettings>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

bool LaunchSamplers::supported()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QStringList LaunchSamplers::runningImagePaths()
{
    QStringList out;
#ifdef Q_OS_WIN
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    {
        if (pe.th32ProcessID == 0) continue;  // the idle process has no image
        // LIMITED_INFORMATION is enough for QueryFullProcessImageName and is granted for most processes of
        // this user; a protected one refuses it and is simply not a candidate.
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t buf[MAX_PATH * 2];
        DWORD len = DWORD(sizeof(buf) / sizeof(buf[0]));
        if (QueryFullProcessImageNameW(h, 0, buf, &len) && len > 0)
            out << QString::fromWCharArray(buf, int(len));
        CloseHandle(h);
    }
    CloseHandle(snap);
#endif
    return out;
}

LaunchWatcher::Sampler LaunchSamplers::steam(const QString& appid, const QString& installDir)
{
#ifdef Q_OS_WIN
    if (appid.isEmpty()) return {};
    return [appid, installDir] {
        // Re-read every tick: Steam rewrites the DWORD as games start and stop. An absent value is an invalid
        // QVariant, which is what hands the decision to the install-dir fallback.
        QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"), QSettings::NativeFormat);
        const QVariant running = reg.value(QStringLiteral("RunningAppID"));
        std::function<bool()> fallback;
        if (!installDir.isEmpty())
            fallback = [installDir] { return LaunchWatch::anyImageUnderDir(runningImagePaths(), installDir); };
        return LaunchWatch::steamRunning(running, appid, fallback);
    };
#else
    Q_UNUSED(appid) Q_UNUSED(installDir)
    return {};
#endif
}

LaunchWatcher::Sampler LaunchSamplers::epic(const QString& installLocation)
{
#ifdef Q_OS_WIN
    if (installLocation.isEmpty()) return {};
    return [installLocation] { return LaunchWatch::anyImageUnderDir(runningImagePaths(), installLocation); };
#else
    Q_UNUSED(installLocation)
    return {};
#endif
}
