#include "ArchiveRom.h"
#include "ArchiveSafePath.h"
#include "SevenZip.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QCryptographicHash>
#include <QMutex>
#include <QHash>
#include <QSharedPointer>
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

// One lock per destination temp dir: extraction now runs on a worker thread (open()), so a double-open of the
// same archive — or the split-pane GUI path racing a worker — could otherwise run two extract passes into the
// SAME dir at once, interleaving truncating writes and stamping the completion marker while the other pass is
// mid-rewrite. The second caller blocks here, then finds the cache/marker warm and returns instantly. The
// QSharedPointer keeps each mutex alive for the process; the map itself is guarded by a single mutex.
QMutex& extractionLockFor(const QString& dir)
{
    static QMutex mapMx;
    static QHash<QString, QSharedPointer<QMutex>> locks;
    QMutexLocker g(&mapMx);
    QSharedPointer<QMutex>& m = locks[dir];
    if (!m) m = QSharedPointer<QMutex>::create();
    return *m;
}

QString baseName(const QString& n)
{
    const int s = qMax(n.lastIndexOf(QLatin1Char('/')), n.lastIndexOf(QLatin1Char('\\')));
    return s >= 0 ? n.mid(s + 1) : n;
}

// Disc "sheet" formats reference sibling data files by name (.cue -> .bin, .gdi/.ccd/.mds -> tracks) or list
// other discs (.m3u). Extracting only the sheet leaves those siblings behind, so the emulator opens the sheet
// and then can't find its .bin. When the target system uses one of these, the whole archive must be extracted.
const QStringList kSheetExts = {
    QStringLiteral(".m3u"), QStringLiteral(".cue"), QStringLiteral(".gdi"),
    QStringLiteral(".ccd"), QStringLiteral(".mds")
};

// After a whole-archive extraction, choose the file to hand the emulator: a multi-disc playlist (.m3u) wins,
// then a single disc sheet (.cue/.gdi/.ccd/.mds), then — for an archive that was really one image (a lone
// .chd/.iso/.pbp) — the largest non-junk member matching the system's extensions. Empty if nothing usable.
QString pickDiscEntryPoint(const QString& dir, const QStringList& wantedExts)
{
    QString m3u, sheet, largestMatch, largestAny;
    qint64 matchSz = -1, anySz = -1;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString p = it.next();
        if (QFileInfo(p).fileName().startsWith(QLatin1Char('.'))) continue; // our .eb_disc_extracted marker / dotfiles
        if (p.endsWith(QStringLiteral(".m3u"), Qt::CaseInsensitive)) { if (m3u.isEmpty()) m3u = p; continue; }
        if (endsWithAny(p, { QStringLiteral(".cue"), QStringLiteral(".gdi"),
                             QStringLiteral(".ccd"), QStringLiteral(".mds") }))
        { if (sheet.isEmpty()) sheet = p; continue; }
        if (endsWithAny(p, kJunkExts)) continue;                // never let a readme/cover win the fallback
        const qint64 sz = QFileInfo(p).size();
        if (sz > anySz) { largestAny = p; anySz = sz; }         // fallback: largest non-junk member, any ext
        const bool match = wantedExts.isEmpty() || endsWithAny(p, wantedExts);
        if (match && sz > matchSz) { largestMatch = p; matchSz = sz; }
    }
    // Playlist > disc sheet > largest system-matching image > largest non-junk member (a sheet-less archive
    // holding a bare .iso/.img whose ext isn't in the system's list still launches, as it did pre-fix).
    if (!m3u.isEmpty())          return m3u;
    if (!sheet.isEmpty())        return sheet;
    if (!largestMatch.isEmpty()) return largestMatch;
    return largestAny;
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
    QMutexLocker exLock(&extractionLockFor(dir)); // serialize concurrent extraction into the same temp dir

    // Multi-file disc images (.cue+.bin, .gdi+tracks, .m3u of several discs): extracting only the sheet leaves
    // its data files behind. When the system this ROM opens for uses a sheet format, extract the WHOLE archive
    // into the temp dir so the sheet's relative references resolve, then hand back the sheet. A completion
    // marker means a prior extraction that was interrupted (or a pre-fix single-file extraction that left a
    // lone .cue) is redone rather than trusted. A disc archive that is actually one .chd/.iso extracts the same.
    bool wantsSheet = false;
    for (const QString& e : wantedExts)
        if (kSheetExts.contains(e, Qt::CaseInsensitive)) { wantsSheet = true; break; }
    if (wantsSheet)
    {
        // The marker records the archive's size+mtime, so a prior extraction is reused only when the archive
        // at this path is unchanged. A pre-fix dir (lone .cue, no marker) or a re-downloaded/updated archive
        // (different stamp) is re-extracted rather than trusted.
        const QFileInfo ai(archivePath);
        const QByteArray stamp = QByteArray::number(ai.size()) + ':'
                               + QByteArray::number(ai.lastModified().toSecsSinceEpoch());
        const QString marker = dir + QStringLiteral("/.eb_disc_extracted");
        bool fresh = false;
        { QFile mk(marker); if (mk.open(QIODevice::ReadOnly)) fresh = (mk.readAll() == stamp); }
        if (!fresh)
        {
            if (!extractAll(archivePath, dir, error))
                return QString();
            QFile mk(marker); if (mk.open(QIODevice::WriteOnly)) mk.write(stamp); // stamp: this dir is fully extracted
        }
        const QString entry = pickDiscEntryPoint(dir, wantedExts);
        if (entry.isEmpty() && error) *error = QStringLiteral("the archive contains no disc image");
        return entry;
    }

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

    // Route by CONTENT, not name — same reason extractToTemp does: the content server names cache files
    // ".zip" regardless of the real container, so a 7-Zip archive can arrive named ".zip" (miniz rejects it
    // as "not a valid zip archive"). Sniff the magic bytes; fall back to the extension only when neither
    // signature matches. Without this, the disc-extraction path above re-breaks the 7z-named-.zip case.
    const ArchiveKind kind = sniffArchiveKind(archivePath);
    const bool isSevenZip = (kind == ArchiveKind::SevenZip) ||
                            (kind == ArchiveKind::Unknown && lower.endsWith(QStringLiteral(".7z")));
    if (isSevenZip)
        return SevenZip::extractAllToDir(archivePath, destDir, error);
    if (kind == ArchiveKind::Unknown && !lower.endsWith(QStringLiteral(".zip")))
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
        const QString name = QString::fromUtf8(st.m_filename);
        // Zip-slip guard: a malicious member name ("../../evil.exe", an absolute path, a drive/UNC spec)
        // must not write outside destDir. Refuse the whole archive rather than partially extracting one
        // that is trying to escape — a crafted archive is not something to unpack halfway.
        const QString outPath = ArchiveSafePath::join(destDir, name);
        if (outPath.isEmpty())
        {
            ok = false;
            if (error) *error = QStringLiteral("the archive contains an unsafe member path");
            break;
        }
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

QString ArchiveRom::extractGameTree(const QString& archivePath, QString* error)
{
    const QString dir = outDirFor(archivePath);
    QMutexLocker exLock(&extractionLockFor(dir)); // serialize concurrent extraction into the same temp dir
    // Reuse a prior full extraction only when the archive at this path is unchanged (size+mtime stamp), so a
    // warm re-open is instant. A partial/interrupted extraction (no marker) or a replaced archive re-extracts.
    const QFileInfo ai(archivePath);
    const QByteArray stamp = QByteArray::number(ai.size()) + ':'
                           + QByteArray::number(ai.lastModified().toSecsSinceEpoch());
    const QString marker = dir + QStringLiteral("/.eb_tree_extracted");
    bool fresh = false;
    { QFile mk(marker); if (mk.open(QIODevice::ReadOnly)) fresh = (mk.readAll() == stamp); }
    if (!fresh)
    {
        if (!extractAll(archivePath, dir, error))
            return QString();
        QFile mk(marker); if (mk.open(QIODevice::WriteOnly)) mk.write(stamp);
    }
    // Choose the boot path RPCS3 expects: the game's EBOOT.BIN (RPCS3 boots a SELF/EBOOT directly), else the
    // game root (the directory that holds PS3_GAME), else a top-level .pkg, else the flat extraction dir.
    {
        QDirIterator eb(dir, QStringList{ QStringLiteral("EBOOT.BIN") }, QDir::Files, QDirIterator::Subdirectories);
        if (eb.hasNext()) { return eb.next(); }
    }
    {
        QDirIterator pg(dir, QStringList{ QStringLiteral("PS3_GAME") }, QDir::Dirs, QDirIterator::Subdirectories);
        if (pg.hasNext()) { return QFileInfo(pg.next()).absolutePath(); } // parent of PS3_GAME = the game root
    }
    const QFileInfoList pkgs = QDir(dir).entryInfoList(QStringList{ QStringLiteral("*.pkg") }, QDir::Files);
    if (!pkgs.isEmpty()) return pkgs.first().absoluteFilePath();
    return dir;
}
