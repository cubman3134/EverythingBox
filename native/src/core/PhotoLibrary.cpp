#include "PhotoLibrary.h"
#include "NaturalOrder.h"
#include "Settings.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QCollator>
#include <algorithm>

namespace PhotoLibrary
{
namespace
{
    // The viewable image extensions. Kept lowercase; the predicate lowercases its input. jpg/jpeg/png/webp/
    // gif/bmp are decoded by Qt's built-in plugins on every platform; avif is bundled on desktop. heic is
    // intentionally absent (see the header) — it decodes only where the platform Qt provides the plugin.
    const QStringList& imageExts()
    {
        static const QStringList exts = {
            QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"), QStringLiteral("webp"),
            QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("avif")
        };
        return exts;
    }

    // A natural (numeric-aware, case-insensitive) collator, built once. img1 < img2 < img10. Built through
    // NaturalOrder: an inline `QCollator c; c.setNumericMode(true);` is INERT under the C locale and orders
    // img10 before img2 with no warning of any kind (issue #205 — see NaturalOrder.h).
    const QCollator& naturalCollator()
    {
        static QCollator coll = NaturalOrder::collator();
        return coll;
    }
}

bool isPhotoFile(const QString& path)
{
    return imageExts().contains(QFileInfo(path).suffix().toLower());
}

QVector<PhotoEntry> scanFolder(const QString& root)
{
    QVector<PhotoEntry> out;
    if (root.isEmpty() || !QFileInfo::exists(root)) return out;

    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        if (!isPhotoFile(path)) continue;
        const QFileInfo fi(path);
        PhotoEntry e;
        e.path   = fi.absoluteFilePath();
        e.folder = fi.absolutePath();
        e.mtime  = fi.lastModified().toSecsSinceEpoch();
        out.push_back(e);
    }

    // Natural order overall: first by folder, then by filename within the folder. Deterministic regardless of
    // the filesystem's iteration order (QDirIterator makes no ordering promise).
    const QCollator& coll = naturalCollator();
    std::sort(out.begin(), out.end(), [&coll](const PhotoEntry& a, const PhotoEntry& b) {
        if (a.folder != b.folder) return coll.compare(a.folder, b.folder) < 0;
        return coll.compare(QFileInfo(a.path).fileName(), QFileInfo(b.path).fileName()) < 0;
    });
    return out;
}

QStringList imagesInFolder(const QString& folder)
{
    QStringList out;
    if (folder.isEmpty() || !QFileInfo::exists(folder)) return out;

    QDir dir(folder);
    const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& fi : files)
        if (isPhotoFile(fi.filePath()))
            out.push_back(fi.absoluteFilePath());

    const QCollator& coll = naturalCollator();
    std::sort(out.begin(), out.end(), [&coll](const QString& a, const QString& b) {
        return coll.compare(QFileInfo(a).fileName(), QFileInfo(b).fileName()) < 0;
    });
    return out;
}

QMap<QString, QVector<PhotoEntry>> groupByFolder(const QVector<PhotoEntry>& entries)
{
    QMap<QString, QVector<PhotoEntry>> groups;
    for (const PhotoEntry& e : entries)
        groups[e.folder].push_back(e);
    return groups;
}

bool hasImages(const QString& root)
{
    if (root.isEmpty() || !QFileInfo::exists(root)) return false;
    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
        if (isPhotoFile(it.next())) return true;   // stop at the first image — no full scan for the gate
    return false;
}

QString root() { return Settings::photosFolder(); }

} // namespace PhotoLibrary
