#include "MusicArt.h"
#include "AppPaths.h"
#include "../media/AudioTags.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>

namespace MusicArt
{

QString siblingCover(const QString& folder)
{
    if (folder.isEmpty()) return QString();
    // Fixed precedence, and the SAME list the audiobook now-playing page has always used. "cover" first
    // because it is what rippers write; "albumart" last because it is mostly a Windows Media artefact.
    static const QStringList stems = { QStringLiteral("cover"), QStringLiteral("folder"),
                                       QStringLiteral("front"), QStringLiteral("albumart") };
    static const QStringList exts  = { QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                       QStringLiteral("png"), QStringLiteral("webp") };
    const QDir dir(folder);
    for (const QString& s : stems)
        for (const QString& e : exts)
        {
            const QString p = dir.absoluteFilePath(s + QLatin1Char('.') + e);
            if (QFile::exists(p)) return p;
        }
    return QString();
}

QString cacheDir() { return AppPaths::dataDir() + QStringLiteral("/musicart"); }

QString cachedCoverPath(const QString& cacheDir, const QString& albumKey)
{
    if (cacheDir.isEmpty() || albumKey.isEmpty()) return QString();
    // SHA-1 of the key's UTF-8, hex: the album key holds folded tag text and 0x1F separators, so it cannot be
    // a filename. A digest also keeps the name a fixed length, which matters because an untagged album's key
    // embeds a full directory path and Windows still has a path limit.
    const QByteArray h = QCryptographicHash::hash(albumKey.toUtf8(), QCryptographicHash::Sha1).toHex();
    return cacheDir + QLatin1Char('/') + QString::fromLatin1(h) + QStringLiteral(".jpg");
}

QString albumCover(const MusicLibrary::Album& album, const QString& cacheDir)
{
    const QString cached = cachedCoverPath(cacheDir, album.key);
    if (!cached.isEmpty() && QFile::exists(cached)) return cached;   // the embedded art, already extracted
    return siblingCover(album.folder);                               // ...else the folder's own image
}

namespace {
// The first track of this album that says it carries embedded art. Ordered by the album's own track order, so
// the cover an album shows is the one on its first track rather than on whichever file the walk reached first.
QString firstCoverTrack(const MusicLibrary::Album& album)
{
    for (const MusicLibrary::IndexTrack& t : album.tracks)
        if (t.hasCover) return t.path;
    return QString();
}
} // namespace

int extractCovers(const MusicLibrary::Index& idx, const QString& cacheDir)
{
    if (cacheDir.isEmpty()) return 0;
    int written = 0;
    bool madeDir = false;
    for (const MusicLibrary::Artist& a : idx.artists)
        for (const MusicLibrary::Album& b : a.albums)
        {
            const QString out = cachedCoverPath(cacheDir, b.key);
            if (out.isEmpty() || QFile::exists(out)) continue;       // already cached: the steady state
            const QString src = firstCoverTrack(b);
            if (src.isEmpty()) continue;                             // no embedded art; sibling file, or nothing

            const AudioTags::Picture pic = AudioTags::read(src).cover;
            if (pic.isNull()) continue;                              // hasCover said yes and the read says no

            QImage img;
            if (!img.loadFromData(pic.data)) continue;               // corrupt frame: skip, retry next scan
            if (img.width() > kMaxEdge || img.height() > kMaxEdge)
                img = img.scaled(kMaxEdge, kMaxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);

            if (!madeDir) { QDir().mkpath(cacheDir); madeDir = true; }
            if (img.save(out, "JPG", 88)) ++written;
        }
    return written;
}

} // namespace MusicArt
