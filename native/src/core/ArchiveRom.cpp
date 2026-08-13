#include "ArchiveRom.h"
#include "SevenZip.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCryptographicHash>
#include <cstring>

extern "C" {
#include "miniz.h" // third_party/miniz.h on the include path (also used for comics)
}

namespace {

// When no extension filter is given we take the largest file, but skip the usual repack cruft so a
// readme/cover doesn't win over a tiny ROM.
const QStringList kJunkExts = {
    QStringLiteral(".txt"), QStringLiteral(".nfo"), QStringLiteral(".sfv"), QStringLiteral(".diz"),
    QStringLiteral(".url"), QStringLiteral(".md"),  QStringLiteral(".jpg"), QStringLiteral(".jpeg"),
    QStringLiteral(".png"), QStringLiteral(".dat"), QStringLiteral(".xml")
};

bool endsWithAny(const QString& name, const QStringList& exts)
{
    for (const QString& e : exts)
        if (name.endsWith(e, Qt::CaseInsensitive))
            return true;
    return false;
}

QString baseName(const QString& n)
{
    const int s = qMax(n.lastIndexOf(QLatin1Char('/')), n.lastIndexOf(QLatin1Char('\\')));
    return s >= 0 ? n.mid(s + 1) : n;
}

// A stable per-archive temp folder, so re-opening the same archive reuses the extracted ROM.
QString outDirFor(const QString& archivePath)
{
    const QByteArray h = QCryptographicHash::hash(archivePath.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    const QString d = QDir::tempPath() + QStringLiteral("/everythingbox-roms/") + QString::fromLatin1(h);
    QDir().mkpath(d);
    return d;
}

// How a file's *content* classifies, independent of its name. A ROM streamed from the content server
// lands in the cache under a name the server chose (".zip"), which may not match its real container —
// the confirmed bug is a 7-Zip archive named ".zip". Route by the magic bytes, not the extension.
enum class ArchiveKind { Unknown, Zip, SevenZip };

ArchiveKind sniffArchiveKind(const QString& path)
{
    // Read the first 6 bytes with QFile (its open() is unicode-safe on Windows, unlike a bare fopen).
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return ArchiveKind::Unknown;
    char buf[6] = {0};
    const qint64 n = f.read(buf, sizeof(buf));
    if (n < 6)
        return ArchiveKind::Unknown; // too short to hold either signature
    const unsigned char* b = reinterpret_cast<const unsigned char*>(buf);
    // 7-Zip: 37 7A BC AF 27 1C
    if (b[0] == 0x37 && b[1] == 0x7A && b[2] == 0xBC && b[3] == 0xAF && b[4] == 0x27 && b[5] == 0x1C)
        return ArchiveKind::SevenZip;
    // ZIP: PK\x03\x04 (local file header), PK\x05\x06 (empty EOCD), PK\x07\x08 (spanned/split marker).
    if (b[0] == 0x50 && b[1] == 0x4B &&
        ((b[2] == 0x03 && b[3] == 0x04) || (b[2] == 0x05 && b[3] == 0x06) || (b[2] == 0x07 && b[3] == 0x08)))
        return ArchiveKind::Zip;
    return ArchiveKind::Unknown;
}

} // namespace

bool ArchiveRom::isArchive(const QString& path)
{
    const QString s = path.toLower();
    return s.endsWith(QStringLiteral(".zip")) || s.endsWith(QStringLiteral(".7z"));
}

QString ArchiveRom::extractToTemp(const QString& archivePath, const QStringList& wantedExts, QString* error)
{
    const QString lower = archivePath.toLower();
    const QString dir = outDirFor(archivePath);

    // Route by CONTENT, not name. The content server names cache files ".zip" regardless of the real
    // container, so a 7-Zip archive can arrive named ".zip" (the confirmed bug: miniz correctly rejected
    // it as "not a valid zip archive"). Sniff the magic bytes; only fall back to the extension when the
    // header matches neither signature, so edge cases that predate this still route as before.
    const ArchiveKind kind = sniffArchiveKind(archivePath);
    const bool isSevenZip = (kind == ArchiveKind::SevenZip) ||
                            (kind == ArchiveKind::Unknown && lower.endsWith(QStringLiteral(".7z")));

    // ---- .7z : vendored LZMA SDK (decodes straight to a file, reusing a prior extraction) -----------
    if (isSevenZip)
        return SevenZip::extractBestToFile(archivePath, wantedExts, dir, error);

    // ---- .zip : bundled miniz, streamed from disk (never buffer the whole archive: a TorrentZip of a
    // GameCube dump can be ~1GB, and readAll()+init_mem+extract_to_heap ran the machine out of memory and
    // mis-reported it as "not a valid zip archive"). miniz's MZ_FOPEN is _wfopen_s on Windows (UTF-8→UTF-16
    // in mz_fopen), so init_file/extract_to_file with a toUtf8() path are unicode-safe. ------------------
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, archivePath.toUtf8().constData(), 0))
    {
        if (error) *error = QStringLiteral("not a valid zip archive");
        return QString();
    }

    int bestIdx = -1;
    mz_uint64 bestSize = 0;
    bool bestExt = false;
    QString bestName;
    int keyIdx = -1; // a companion disc key (Wii U .wux/.wud dumps ship a tiny <game>.key beside the image)
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st))
            continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        const QString name = QString::fromUtf8(st.m_filename);
        if (name.endsWith(QStringLiteral(".key"), Qt::CaseInsensitive) && st.m_uncomp_size <= 4096)
            keyIdx = int(i);
        if (wantedExts.isEmpty() && endsWithAny(name, kJunkExts))
            continue;
        const bool extMatch = !wantedExts.isEmpty() && endsWithAny(name, wantedExts);
        const bool better = (extMatch && !bestExt) || (extMatch == bestExt && st.m_uncomp_size > bestSize);
        if (bestIdx < 0 || better)
        {
            bestIdx = int(i);
            bestSize = st.m_uncomp_size;
            bestExt = extMatch;
            bestName = name;
        }
    }

    QString result;
    if (bestIdx >= 0)
    {
        const QString out = dir + QLatin1Char('/') + baseName(bestName);
        if (QFileInfo::exists(out) && static_cast<mz_uint64>(QFileInfo(out).size()) == bestSize)
        {
            result = out; // cached from a previous open
        }
        else
        {
            // Stream the chosen entry straight to disk — no giant heap allocation (the old
            // extract_to_heap held the whole ROM in memory before writing it).
            if (mz_zip_reader_extract_to_file(&zip, static_cast<mz_uint>(bestIdx), out.toUtf8().constData(), 0))
                result = out;
            else if (error)
                *error = QStringLiteral("failed to extract the ROM from the zip");
        }
    }
    else if (error)
        *error = QStringLiteral("the zip contains no ROM file");

    // Companion disc key: Wii U dumps ship a 16-byte <game>.key next to the .wux/.wud. Cemu decrypts the
    // disc with it, looking beside the image with the extension swapped to .key. Extract it to match.
    if (!result.isEmpty() && keyIdx >= 0)
    {
        const QString keyDest = dir + QLatin1Char('/') + QFileInfo(result).completeBaseName() + QStringLiteral(".key");
        if (!QFileInfo::exists(keyDest))
            mz_zip_reader_extract_to_file(&zip, static_cast<mz_uint>(keyIdx), keyDest.toUtf8().constData(), 0);
    }

    mz_zip_reader_end(&zip);
    return result;
}

bool ArchiveRom::extractAll(const QString& archivePath, const QString& destDir, QString* error)
{
    QDir().mkpath(destDir);
    const QString lower = archivePath.toLower();

    if (lower.endsWith(QStringLiteral(".7z")))
        return SevenZip::extractAllToDir(archivePath, destDir, error);
    if (!lower.endsWith(QStringLiteral(".zip")))
    {
        if (error) *error = QStringLiteral("unsupported archive type");
        return false;
    }

    // .zip via miniz, streamed from disk and to disk (a repack can be many GB, so don't buffer it). File
    // names inside a repack are ASCII; miniz opens paths with fopen, so a non-ASCII path can fail on Windows.
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, archivePath.toUtf8().constData(), 0))
    {
        if (error) *error = QStringLiteral("not a valid zip archive");
        return false;
    }

    bool ok = true;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i)
    {
        if (mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st))
            continue;
        QString name = QString::fromUtf8(st.m_filename);
        name.replace(QLatin1Char('\\'), QLatin1Char('/'));
        const QString outPath = destDir + QLatin1Char('/') + name;
        QDir().mkpath(QFileInfo(outPath).absolutePath());
        if (!mz_zip_reader_extract_to_file(&zip, i, outPath.toUtf8().constData(), 0))
        {
            ok = false;
            if (error) *error = QStringLiteral("couldn't extract a file from the zip");
            break;
        }
    }

    mz_zip_reader_end(&zip);
    return ok;
}
