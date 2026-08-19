#include "core/ps3/Ps3Firmware.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

namespace Ps3Firmware {

namespace {
// How long a failed install suppresses the next attempt.
constexpr qint64 kRetryBackoffSecs = 3600;
} // namespace

QString devFlashRoot(Os os, const QString& binDir, const QString& home, const QString& xdgConfigHome)
{
    switch (os)
    {
    case Os::Windows: return binDir;
    case Os::MacOS:   return home + QStringLiteral("/Library/Application Support/rpcs3");
    case Os::Linux:   break;
    }
    return xdgConfigHome.isEmpty() ? home + QStringLiteral("/.config/rpcs3")
                                   : xdgConfigHome + QStringLiteral("/rpcs3");
}

QString devFlashRoot(const QString& binDir)
{
#if defined(Q_OS_WIN)
    return devFlashRoot(Os::Windows, binDir, QDir::homePath(), QString());
#elif defined(Q_OS_MACOS)
    return devFlashRoot(Os::MacOS, binDir, QDir::homePath(), QString());
#else
    return devFlashRoot(Os::Linux, binDir, QDir::homePath(), qEnvironmentVariable("XDG_CONFIG_HOME"));
#endif
}

bool installed(const QString& fwRoot)
{
    QFile f(fwRoot + QStringLiteral("/dev_flash/vsh/etc/version.txt"));
    return f.exists() && f.size() > 0;
}

std::optional<Info> parseUpdateList(const QByteArray& body)
{
    const QStringList lines = QString::fromUtf8(body).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines)
    {
        QString version, url;
        const QStringList fields = line.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString& field : fields)
        {
            const int eq = field.indexOf(QLatin1Char('='));
            if (eq <= 0) continue;
            const QString key = field.left(eq).trimmed();
            const QString val = field.mid(eq + 1).trimmed();
            if (key == QLatin1String("SystemSoftwareVersion")) version = val;
            else if (key == QLatin1String("CDN"))              url = val;
        }
        if (url.startsWith(QLatin1String("http://")) || url.startsWith(QLatin1String("https://")))
            return Info{ version, url };
    }
    return std::nullopt;
}

bool maybeInstall(const QString& fwRoot, const QString& rpcs3Exe, const QString& tmpDir,
                  const FeedFetcher& fetch, const Downloader& download,
                  const Installer& install, const Progress& progress)
{
    if (installed(fwRoot)) return false;

    // A persistently failing install (bad feed, rejected PUP) must not re-pay a ~230MB download on every
    // single launch — that would delay each boot by minutes, forever. One attempt per hour bounds the
    // cost; dev_flash appearing (e.g. the user installs it by hand) short-circuits everything below.
    // tmpDir may not exist yet — QFileInfo on a missing path simply reports !exists().
    // A future-dated marker — clock skew, a restored backup, a bad filesystem timestamp — must read as
    // STALE, not fresh, or installs stay suppressed until the wall clock catches up to the stamp.
    const QString marker = QDir(tmpDir).filePath(QStringLiteral("fw-install-failed"));
    const QFileInfo markerInfo(marker);
    const qint64 age = markerInfo.lastModified().toUTC().secsTo(QDateTime::currentDateTimeUtc());
    if (markerInfo.exists() && age >= 0 && age < kRetryBackoffSecs)
        return false;

    const auto noteFailure = [&](const QString& reason) {
        QDir().mkpath(tmpDir);
        QFile f(marker);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write((QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
                     + QLatin1String(" ") + reason + QLatin1String("\n")).toUtf8());
    };

    const auto body = fetch ? fetch() : std::nullopt;
    if (!body || body->trimmed().isEmpty()) { noteFailure(QStringLiteral("updatelist fetch failed")); return false; }

    const auto info = parseUpdateList(*body);
    if (!info) { noteFailure(QStringLiteral("updatelist carried no PUP url")); return false; }

    if (progress)
        progress(info->version.isEmpty()
                     ? QCoreApplication::translate("Ps3Firmware", "Installing PS3 firmware…")
                     : QCoreApplication::translate("Ps3Firmware", "Installing PS3 firmware… v%1").arg(info->version));

    QDir().mkpath(tmpDir);
    const QString pup = QDir(tmpDir).filePath(QStringLiteral("PS3UPDAT.PUP"));
    const bool downloaded  = download && download(info->url, pup);
    const bool ranInstaller = downloaded && static_cast<bool>(install);
    const bool installedOk  = ranInstaller && install(rpcs3Exe, pup) == 0;
    if (QFile::exists(pup)) QFile::remove(pup); // the PUP is only a means to dev_flash — never leave ~230MB behind, pass or fail

    // An installer that ran and failed (which includes "the caller killed it": bounded-run timeout, or
    // app-quit interruption) may still have written version.txt — the kill can even land mid-write and
    // leave it non-empty. installed() would then read true over a broken dev_flash and the entry check
    // would skip every future repair. Firmware was absent when this function was entered, so a
    // version.txt present now was written by this failed attempt and is safe to scrub.
    if (ranInstaller && !installedOk && installed(fwRoot))
        QFile::remove(fwRoot + QStringLiteral("/dev_flash/vsh/etc/version.txt"));

    const bool done = installedOk && installed(fwRoot);
    if (done) QFile::remove(marker); // cleared: a later launch must not be blocked by a stale failure
    if (!done) noteFailure(QStringLiteral("download or install failed"));
    return done;
}

} // namespace Ps3Firmware
