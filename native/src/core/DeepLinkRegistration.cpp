#include "DeepLinkRegistration.h"
#include "DeepLink.h"
#include "DeepLinkChannel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace DeepLinkRegistration
{
QString launcherPath()
{
#if defined(Q_OS_LINUX)
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (!appImage.isEmpty()) return appImage;
#endif
    return QCoreApplication::applicationFilePath();
}

#ifdef Q_OS_WIN
namespace
{
std::wstring w(const QString& s) { return s.toStdWString(); }

// The key's default value, or "" when the key or value is absent.
QString readDefault(const QString& key)
{
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, w(key).c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return QString();
    return QString::fromWCharArray(buf);
}

bool run(const DeepLink::RegOp& op)
{
    if (op.kind == DeepLink::RegOp::DeleteTree)
    {
        // RegDeleteTree empties the key; RegDeleteKey then removes the key itself. Absent is success.
        const LSTATUS a = RegDeleteTreeW(HKEY_CURRENT_USER, w(op.key).c_str());
        const LSTATUS b = RegDeleteKeyW(HKEY_CURRENT_USER, w(op.key).c_str());
        return (a == ERROR_SUCCESS || a == ERROR_FILE_NOT_FOUND) && (b == ERROR_SUCCESS || b == ERROR_FILE_NOT_FOUND);
    }
    const std::wstring value = w(op.value);
    return RegSetKeyValueW(HKEY_CURRENT_USER, w(op.key).c_str(), op.name.isEmpty() ? nullptr : w(op.name).c_str(),
                           REG_SZ, value.c_str(), DWORD((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
} // namespace
#endif

Outcome apply(bool enable)
{
    Outcome o;
    o.scheme = DeepLinkChannel::registrationScheme();
    if (o.scheme.isEmpty())
    {
        o.detail = QStringLiteral("no scheme may be registered here (the UI-test channel needs a test scheme)");
        return o;
    }
#if defined(Q_OS_WIN)
    const QString exe = launcherPath();
    if (enable)
    {
        const QVector<DeepLink::RegOp> plan = DeepLink::windowsRegisterPlan(o.scheme, exe);
        if (plan.isEmpty()) { o.detail = QStringLiteral("this exe's path cannot be registered"); return o; }
        const QString cmdKey = plan.last().key;
        if (readDefault(cmdKey) == plan.last().value)
        {
            o.ok = true;
            o.detail = QStringLiteral("already registered for this exe");
            return o;
        }
        o.ok = true;
        for (const DeepLink::RegOp& op : plan) o.ok = run(op) && o.ok;
        o.changed = true;
        o.detail = o.ok ? QStringLiteral("registered HKCU\\%1").arg(DeepLink::windowsClassKey(o.scheme))
                        : QStringLiteral("could not write HKCU\\%1").arg(DeepLink::windowsClassKey(o.scheme));
        return o;
    }
    const QVector<DeepLink::RegOp> plan = DeepLink::windowsUnregisterPlan(o.scheme);
    o.ok = !plan.isEmpty();
    for (const DeepLink::RegOp& op : plan) o.ok = run(op) && o.ok;
    o.changed = true;
    o.detail = o.ok ? QStringLiteral("removed HKCU\\%1").arg(DeepLink::windowsClassKey(o.scheme))
                    : QStringLiteral("could not remove HKCU\\%1").arg(DeepLink::windowsClassKey(o.scheme));
    return o;
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    const QString name = DeepLink::linuxDesktopFileName(o.scheme);
    if (dir.isEmpty() || name.isEmpty()) { o.detail = QStringLiteral("no applications folder"); return o; }
    const QString path = dir + QLatin1Char('/') + name;
    if (!enable)
    {
        o.changed = QFile::exists(path);
        o.ok = !o.changed || QFile::remove(path);
        o.detail = o.ok ? QStringLiteral("removed %1").arg(name) : QStringLiteral("could not remove %1").arg(name);
        return o;
    }
    const QString entry = DeepLink::linuxDesktopEntry(o.scheme, launcherPath());
    if (entry.isEmpty()) { o.detail = QStringLiteral("this executable's path cannot be registered"); return o; }
    QFile existing(path);
    const bool same = existing.open(QIODevice::ReadOnly) && existing.readAll() == entry.toUtf8();
    existing.close();
    if (!same)
    {
        QDir().mkpath(dir);
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(entry.toUtf8()) < 0 || !f.commit())
        {
            o.detail = QStringLiteral("could not write %1").arg(name);
            return o;
        }
        o.changed = true;
    }
    // Make it the handler, and refresh the desktop's cache. Both are best effort and asynchronous: a desktop
    // without xdg-utils still finds the MimeType line through the .desktop file itself on its next scan.
    QProcess::startDetached(QStringLiteral("xdg-mime"),
                            { QStringLiteral("default"), name, QStringLiteral("x-scheme-handler/") + o.scheme });
    QProcess::startDetached(QStringLiteral("update-desktop-database"), { dir });
    o.ok = true;
    o.detail = same ? QStringLiteral("already registered (%1)").arg(name) : QStringLiteral("registered %1").arg(name);
    return o;
#else
    // macOS: the bundle's Info.plist declares the scheme (CFBundleURLTypes); LaunchServices needs nothing
    // written. Mobile: not registered this way (Android's intent filter is a later increment of #80).
    Q_UNUSED(enable);
    o.ok = true;
    o.detail = QStringLiteral("declared by the app bundle; nothing to write");
    return o;
#endif
}
} // namespace DeepLinkRegistration
