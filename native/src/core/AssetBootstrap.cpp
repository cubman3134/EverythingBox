#include "AssetBootstrap.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

// First-run APK-asset extractor. See AssetBootstrap.h for the contract. sourceRoot is a real dir in the probe
// and "assets:/mmv" (Qt's Android asset file engine) in the app — QDir/QDirIterator/QFile all speak both, so
// the copy logic is platform-blind.
namespace
{
    // Copy every file under srcDir into dstDir (recursively), overwriting same-named destinations. Files copied
    // out of assets:/ arrive read-only, which would make the NEXT version-bump refresh fail on the overwrite;
    // stamp each copied file writable so refreshes keep working. Returns true if it wrote at least one file.
    bool copyTreeOverwrite(const QString& srcDir, const QString& dstDir)
    {
        if (!QDir(srcDir).exists()) return false;
        bool wrote = false;
        QDirIterator it(srcDir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString srcFile = it.next();
            const QString rel     = QDir(srcDir).relativeFilePath(srcFile);
            const QString dstFile = dstDir + QLatin1Char('/') + rel;
            QDir().mkpath(QFileInfo(dstFile).absolutePath());
            if (QFile::exists(dstFile))
            {
                QFile::setPermissions(dstFile, QFile::ReadOwner | QFile::WriteOwner);
                QFile::remove(dstFile);
            }
            if (QFile::copy(srcFile, dstFile))
            {
                QFile::setPermissions(dstFile, QFile::ReadOwner | QFile::WriteOwner);
                wrote = true;
            }
        }
        return wrote;
    }
}

namespace AssetBootstrap
{
    bool run(const QString& sourceRoot, const QString& dataDir, const QString& appVersion)
    {
        // No source (e.g. a desktop build with no bundled assets) -> clean no-op: create nothing, stamp nothing.
        if (!QDir(sourceRoot).exists())
            return false;

        // The stamp records which version's assets are on disk. Matching version -> nothing to do; leave every
        // file (incl. user edits to stock themes) exactly as-is.
        const QString stampPath = dataDir + QStringLiteral("/.assets-version");
        {
            QFile f(stampPath);
            if (f.open(QIODevice::ReadOnly) && QString::fromUtf8(f.readAll()).trimmed() == appVersion)
                return false;
        }

        QDir().mkpath(dataDir);

        // Stock themes are REFRESHED on every fresh/bumped run: source themes overwrite same-named files so a new
        // app version ships updated stock themes. User-added theme dirs (not present in the source) are never
        // iterated, so they survive untouched.
        copyTreeOverwrite(sourceRoot + QStringLiteral("/themes2"), dataDir + QStringLiteral("/themes2"));

        // Addons are COPY-IF-ABSENT at the addon-dir granularity: a first-party addon the user has configured
        // (or removed) must never be clobbered by an upgrade. Only addon dirs missing from disk get extracted.
        const QString srcAddons = sourceRoot + QStringLiteral("/addons");
        const QString dstAddons = dataDir + QStringLiteral("/addons");
        QDir().mkpath(dstAddons);

        // Crash-safety: extraction is a copy-into-tmp then atomic rename (below), so the ONLY way a "<name>"
        // dir exists on disk is a fully-copied addon. But a process death mid-copy can leave a partial
        // "<name>.extracting" sibling behind. Sweep those away first — otherwise a stale partial would block
        // its own rename target and the addon would never install. (A bare "<name>" dir is trusted-complete.)
        const QFileInfoList leftovers =
            QDir(dstAddons).entryInfoList({ QStringLiteral("*.extracting") }, QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& stale : leftovers)
            QDir(stale.absoluteFilePath()).removeRecursively();

        const QFileInfoList addonDirs = QDir(srcAddons).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& a : addonDirs)
        {
            const QString dstAddon = dstAddons + QLatin1Char('/') + a.fileName();
            if (QDir(dstAddon).exists())
                continue; // copy-if-absent: a present dir is complete and user-owned — never touch it

            // Extract into a temp sibling, then rename it into place as the final step. A crash before the
            // rename leaves only "<name>.extracting" (swept next run); the final "<name>" only ever appears
            // whole — no partial dir can masquerade as an installed addon.
            const QString tmpAddon = dstAddon + QStringLiteral(".extracting");
            QDir(tmpAddon).removeRecursively(); // paranoia: clear a same-name residue before writing
            copyTreeOverwrite(a.absoluteFilePath(), tmpAddon);
            QDir().rename(tmpAddon, dstAddon);
        }

        // Record the version we just extracted so the next same-version launch is a no-op.
        QFile stamp(stampPath);
        if (stamp.open(QIODevice::WriteOnly | QIODevice::Truncate))
            stamp.write(appVersion.toUtf8());

        qInfo("AssetBootstrap: extracted assets from %s into %s (version %s)",
              qUtf8Printable(sourceRoot), qUtf8Printable(dataDir), qUtf8Printable(appVersion));
        return true;
    }
}
