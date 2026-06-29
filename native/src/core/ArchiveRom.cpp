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
    const QString d = QDir::tempPath() + QStringLiteral("/mymediavault-roms/") + QString::fromLatin1(h);
    QDir().mkpath(d);
    return d;
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

    // ---- .7z : vendored LZMA SDK ----------------------------------------------------------------
    if (lower.endsWith(QStringLiteral(".7z")))
    {
        QString member;
        QByteArray data;
        if (!SevenZip::extractBest(archivePath, wantedExts, member, data, error))
            return QString();

        const QString out = dir + QLatin1Char('/') + member;
        if (QFileInfo::exists(out) && QFileInfo(out).size() == data.size())
            return out; // already extracted on a previous open

        QFile f(out);
        if (!f.open(QIODevice::WriteOnly))
        {
            if (error) *error = QStringLiteral("could not write the extracted ROM");
            return QString();
        }
        f.write(data);
        f.close();
        return out;
    }

    // ---- .zip : bundled miniz (read the whole archive via QFile so unicode paths work everywhere) ----
    QFile af(archivePath);
    if (!af.open(QIODevice::ReadOnly))
    {
        if (error) *error = QStringLiteral("could not open the zip archive");
        return QString();
    }
    const QByteArray zbytes = af.readAll();
    af.close();

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zbytes.constData(), static_cast<size_t>(zbytes.size()), 0))
    {
        if (error) *error = QStringLiteral("not a valid zip archive");
        return QString();
    }

    int bestIdx = -1;
    mz_uint64 bestSize = 0;
    bool bestExt = false;
    QString bestName;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st))
            continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        const QString name = QString::fromUtf8(st.m_filename);
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
            size_t outSize = 0;
            void* p = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(bestIdx), &outSize, 0);
            if (p)
            {
                QFile f(out);
                if (f.open(QIODevice::WriteOnly))
                {
                    f.write(static_cast<const char*>(p), static_cast<qsizetype>(outSize));
                    f.close();
                    result = out;
                }
                else if (error)
                    *error = QStringLiteral("could not write the extracted ROM");
                mz_free(p);
            }
            else if (error)
                *error = QStringLiteral("failed to extract the ROM from the zip");
        }
    }
    else if (error)
        *error = QStringLiteral("the zip contains no ROM file");

    mz_zip_reader_end(&zip);
    return result;
}
