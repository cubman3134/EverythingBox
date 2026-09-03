#include "RarComic.h"
#include "ComicPageOrder.h"

#include <QCollator>
#include <QCoreApplication>
#include <QFile>
#include <QtGlobal>
#include <algorithm>

extern "C" {
#include "unarr.h"
}

namespace RarComic
{
namespace
{
    // RAR's own file signatures. RAR 2.9/3.x/4.x is seven bytes ending 0x00; RAR5 is eight ending 0x01 0x00.
    // The shared first seven characters are why the check must be on the EIGHTH byte and not on "Rar!".
    const char kRar4Sig[7] = { 'R', 'a', 'r', '!', '\x1A', '\x07', '\x00' };
    const char kRar5Sig[8] = { 'R', 'a', 'r', '!', '\x1A', '\x07', '\x01', '\x00' };

    // Open the file for unarr. On Windows the WIDE entry point, because a path this app hands around is a
    // QString and ar_open_file() takes a char* that MSVC's fopen interprets in the system codepage — a comic
    // under a folder with a non-Latin-1 name would simply not open.
    ar_stream* openStream(const QString& path)
    {
#ifdef Q_OS_WIN
        return ar_open_file_w(reinterpret_cast<const wchar_t*>(path.utf16()));
#else
        return ar_open_file(path.toUtf8().constData());
#endif
    }

    // The whole reader, once. `wanted` empty means "every image member"; otherwise only that one name is
    // kept (and the pass still stops at nothing else — see the header on solid archives).
    //
    // EVERY entry is uncompressed, including the ones dropped: the decompressor's window has to be walked
    // forward whatever we intend to keep. A failed entry is skipped rather than fatal — half a comic is
    // worth showing, and unarr fails an entry by returning false, not by throwing.
    QVector<QPair<QString, QByteArray>> readPass(const QString& path, const QString& wanted, Status* status)
    {
        QVector<QPair<QString, QByteArray>> out;
        auto done = [&](Status s) { if (status) *status = s; return out; };

        const Status sig = signatureOf(path);
        if (sig != Status::Ok) return done(sig);

        ar_stream* stream = openStream(path);
        if (!stream) return done(Status::NotRar);
        ar_archive* ar = ar_open_rar_archive(stream);
        if (!ar) { ar_close(stream); return done(Status::Unreadable); }

        bool sawAnyEntry = false;
        while (ar_parse_entry(ar))
        {
            sawAnyEntry = true;
            const char* raw = ar_entry_get_name(ar);
            const QString name = raw ? QString::fromUtf8(raw) : QString();
            const size_t size = ar_entry_get_size(ar);

            // A page image can be large but not THAT large; a header claiming a gigabyte is a damaged
            // archive, and reserving it would be the damage's whole payload.
            const bool sane = size > 0 && size <= size_t(256) * 1024 * 1024;
            if (!sane) continue;                    // nothing to walk the window with; unarr skips it too

            // A NAMED member is kept whether or not it is a page image — that is the ComicInfo.xml door
            // (#152). With no name asked for, the rule is what it always was: every image member.
            const bool keep = wanted.isEmpty() ? ComicPages::isImageName(name) : (name == wanted);

            QByteArray bytes(int(size), '\0');
            if (!ar_entry_uncompress(ar, bytes.data(), size)) continue;   // damaged entry: skip, keep going
            if (keep) out.append({ name, bytes });
        }

        ar_close_archive(ar);
        ar_close(stream);

        if (out.isEmpty()) return done(sawAnyEntry ? Status::NoPages : Status::Unreadable);
        return done(Status::Ok);
    }
}

Status signatureOf(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return Status::NotRar;
    const QByteArray head = f.read(8);
    f.close();
    if (head.size() >= 8 && std::equal(kRar5Sig, kRar5Sig + 8, head.constData())) return Status::Rar5;
    if (head.size() >= 7 && std::equal(kRar4Sig, kRar4Sig + 7, head.constData())) return Status::Ok;
    return Status::NotRar;
}

QString message(Status s)
{
    switch (s)
    {
    case Status::Ok:      return QString();
    case Status::NotRar:  return QCoreApplication::translate(
        "RarComic", "This isn't a readable comic archive (CBR).");
    case Status::Rar5:    return QCoreApplication::translate(
        "RarComic", "This comic is packed in the RAR5 format, which this reader can't open yet. "
                    "Repacking it as a .cbz will open it.");
    case Status::Unreadable: return QCoreApplication::translate(
        "RarComic", "This comic archive is damaged or password-protected, so it can't be read.");
    case Status::NoPages: return QCoreApplication::translate(
        "RarComic", "No page images found in this comic.");
    }
    return QString();
}

QStringList imageNames(const QString& path, Status* status, QStringList* otherNames)
{
    QStringList out;
    if (otherNames) otherNames->clear();
    auto done = [&](Status s) { if (status) *status = s; return out; };

    const Status sig = signatureOf(path);
    if (sig != Status::Ok) return done(sig);

    ar_stream* stream = openStream(path);
    if (!stream) return done(Status::NotRar);
    ar_archive* ar = ar_open_rar_archive(stream);
    if (!ar) { ar_close(stream); return done(Status::Unreadable); }

    bool sawAnyEntry = false;
    // HEADERS ONLY: ar_parse_entry walks the block chain by its declared sizes and touches no decompressor,
    // which is what makes this cheap enough for a per-file library scan.
    while (ar_parse_entry(ar))
    {
        sawAnyEntry = true;
        const char* raw = ar_entry_get_name(ar);
        if (!raw) continue;
        const QString name = QString::fromUtf8(raw);
        if (ar_entry_get_size(ar) <= 0) continue;
        if (ComicPages::isImageName(name)) out.append(name);
        else if (otherNames)              otherNames->append(name);
    }

    ar_close_archive(ar);
    ar_close(stream);

    if (out.isEmpty()) return done(sawAnyEntry ? Status::NoPages : Status::Unreadable);
    return done(Status::Ok);
}

QVector<QPair<QString, QByteArray>> imagePages(const QString& path, Status* status)
{
    return readPass(path, QString(), status);
}

QByteArray coverBytes(const QString& path, Status* status)
{
    Status listStatus = Status::Ok;
    QStringList names = imageNames(path, &listStatus);
    if (listStatus != Status::Ok || names.isEmpty())
    {
        if (status) *status = listStatus;
        return QByteArray();
    }

    // THE READER'S ORDER, not the archive's. A comic whose pages were added out of sequence would otherwise
    // put a shelf's picture on a page the reader never opens on.
    const QCollator coll = ComicPages::collator();
    std::sort(names.begin(), names.end(),
              [&coll](const QString& a, const QString& b) { return ComicPages::lessThan(coll, a, b); });

    const QVector<QPair<QString, QByteArray>> one = readPass(path, names.first(), status);
    return one.isEmpty() ? QByteArray() : one.first().second;
}

QByteArray memberBytes(const QString& path, const QString& name, Status* status)
{
    if (name.isEmpty())
    {
        // An empty name means "every image member" to readPass, which is emphatically not what a caller
        // asking for a named member wants back.
        if (status) *status = Status::NoPages;
        return QByteArray();
    }
    const QVector<QPair<QString, QByteArray>> one = readPass(path, name, status);
    return one.isEmpty() ? QByteArray() : one.first().second;
}

} // namespace RarComic
