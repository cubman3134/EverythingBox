#include "core/ps3/Ps3Firmware.h"

#include <QDir>
#include <QFile>
#include <QStringList>

namespace Ps3Firmware {

bool installed(const QString& binDir)
{
    QFile f(binDir + QStringLiteral("/dev_flash/vsh/etc/version.txt"));
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

bool maybeInstall(const QString& binDir, const QString& rpcs3Exe, const QString& tmpDir,
                  const FeedFetcher& fetch, const Downloader& download,
                  const Installer& install, const Progress& progress)
{
    if (installed(binDir)) return false;

    const auto body = fetch ? fetch() : std::nullopt;
    if (!body || body->trimmed().isEmpty()) return false;

    const auto info = parseUpdateList(*body);
    if (!info) return false;

    if (progress)
        progress(info->version.isEmpty()
                     ? QStringLiteral("Installing PS3 firmware…")
                     : QStringLiteral("Installing PS3 firmware… v%1").arg(info->version));

    QDir().mkpath(tmpDir);
    const QString pup = QDir(tmpDir).filePath(QStringLiteral("PS3UPDAT.PUP"));
    bool ok = download && download(info->url, pup);
    if (ok) ok = install && install(rpcs3Exe, pup) == 0;
    QFile::remove(pup); // the PUP is only a means to dev_flash — never leave ~230MB behind, pass or fail

    return ok && installed(binDir);
}

} // namespace Ps3Firmware
