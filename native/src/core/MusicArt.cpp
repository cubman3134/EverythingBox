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

QString keyedCover(const QString& key, const QString& folder, const QString& cacheDir)
{
    const QString cached = cachedCoverPath(cacheDir, key);
    if (!cached.isEmpty() && QFile::exists(cached)) return cached;   // the embedded art, already extracted
    return siblingCover(folder);                                     // ...else the folder's own image
}

QString albumCover(const MusicLibrary::Album& album, const QString& cacheDir)
{
    return keyedCover(album.key, album.folder, cacheDir);
}

bool writeKeyedCover(const QString& key, const QByteArray& encodedImage, const QString& cacheDir)
{
    if (cacheDir.isEmpty() || encodedImage.isEmpty()) return false;
    const QString out = cachedCoverPath(cacheDir, key);
    if (out.isEmpty() || QFile::exists(out)) return false;           // already cached: the steady state

    QImage img;
    if (!img.loadFromData(encodedImage)) return false;               // corrupt picture: skip, retry next scan
    if (img.width() > kMaxEdge || img.height() > kMaxEdge)
        img = img.scaled(kMaxEdge, kMaxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QDir().mkpath(cacheDir);
    return img.save(out, "JPG", 88);
}

bool extractCoverFor(const QString& key, const QString& sourceFile, const QString& cacheDir)
{
    if (cacheDir.isEmpty() || sourceFile.isEmpty()) return false;
    const QString out = cachedCoverPath(cacheDir, key);
    if (out.isEmpty() || QFile::exists(out)) return false;           // already cached, and asked BEFORE the
                                                                     // tag read so a cached album costs one
                                                                     // existence check rather than a parse
    const AudioTags::Picture pic = AudioTags::read(sourceFile).cover;
    if (pic.isNull()) return false;                                  // hasCover said yes and the read says no
    return writeKeyedCover(key, pic.data, cacheDir);
}

namespace {
// The first track of this album that says it carries embedded art. Ordered by the album's own track order, so
// the cover an album shows is the one on its first track rather than on whichever file the walk reached first.
QString firstCoverTrack(const MusicLibrary::Album& album)
{
    // sourcePath, not path: a cue album's track `path` is an mpv clip url rather than a file (#196 part 3),
    // and AudioTags::read below needs the bytes on disk. For every ordinary track the two are the same string.
    for (const MusicLibrary::IndexTrack& t : album.tracks)
        if (t.hasCover) return t.sourcePath;
    return QString();
}
} // namespace

int extractCovers(const MusicLibrary::Index& idx, const QString& cacheDir)
{
    if (cacheDir.isEmpty()) return 0;
    int written = 0;
    for (const MusicLibrary::Artist& a : idx.artists)
        for (const MusicLibrary::Album& b : a.albums)
            if (extractCoverFor(b.key, firstCoverTrack(b), cacheDir)) ++written;
    return written;
}

} // namespace MusicArt
