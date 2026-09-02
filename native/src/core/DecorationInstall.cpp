#include "DecorationInstall.h"

#include "ArchiveRom.h"        // extractAll — the product's ONE whole-archive extractor (zip-slip guarded)

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstring>

extern "C" {
#include "miniz.h"
}

namespace {

// Dot-prefixed so DecorationPack::installedPacks() and packsForSystem() skip it (both drop dot-names), and
// INSIDE `root` so the final move of a system's folder is a same-volume rename rather than a copy. A staging
// directory on the temp volume would turn every install into a cross-device copy of tens of megabytes of PNG,
// and would make the swap non-atomic on exactly the machines (a TV box with a small internal volume and the
// data dir on a stick) where an interrupted install is most likely.
const QLatin1String kStagingName(".eb-decorations-installing");

} // namespace

namespace DecorationInstall {

bool listMembers(const QString& zipPath, QStringList* members, qint64* uncompressedBytes, QString* error)
{
    if (members) members->clear();
    if (uncompressedBytes) *uncompressedBytes = 0;

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    // init_file, not init_mem: the archive is read from disk rather than buffered whole, the same reason
    // ArchiveRom stopped doing readAll() on a 1 GB ROM zip and started streaming.
    if (!mz_zip_reader_init_file(&zip, zipPath.toUtf8().constData(), 0))
    {
        if (error) *error = QStringLiteral("That download is not a readable zip file.");
        return false;
    }

    qint64 total = 0;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        if (members) *members << QString::fromUtf8(st.m_filename);
        total += qint64(st.m_uncomp_size);
    }
    mz_zip_reader_end(&zip);

    if (uncompressedBytes) *uncompressedBytes = total;
    return true;
}

bool installBytes(const QByteArray& zipBytes, const QString& root, const DecorationPack::Entry& entry,
                  const QStringList& knownSystems, Result* out, QString* error)
{
    auto fail = [error](const QString& m) { if (error) *error = m; return false; };
    if (zipBytes.isEmpty()) return fail(QStringLiteral("That download is empty."));

    // A DIRECTORY from QTemporaryDir, holding a PLAIN QFile — never QTemporaryFile. See the header: a
    // QTemporaryFile's handle is still open after close(), so the zip reader that opens the path next reads
    // a zero-length file. A plain QFile closes, and the directory still gets the random name and the
    // automatic cleanup that made QTemporaryFile attractive in the first place.
    QTemporaryDir dir;
    if (!dir.isValid()) return fail(QStringLiteral("Couldn't make a temporary folder for the download."));
    const QString path = dir.path() + QStringLiteral("/pack.zip");
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail(QStringLiteral("Couldn't save the downloaded pack."));
        const bool wrote = f.write(zipBytes) == qint64(zipBytes.size());
        f.flush();
        f.close();
        // The write is buffered, so a full disk surfaces only at the flush — and ~QFile would swallow it.
        // An unchecked write here is a truncated zip that the digest then refuses with a checksum message,
        // which sends the reader to blame the registry for the local disk being full.
        if (!wrote || f.error() != QFileDevice::NoError)
            return fail(QStringLiteral("Couldn't save all of the downloaded pack — the disk may be full."));
    }
    return installZip(path, root, entry, knownSystems, out, error);
}

bool installZip(const QString& zipPath, const QString& root, const DecorationPack::Entry& entry,
                const QStringList& knownSystems, Result* out, QString* error)
{
    auto fail = [error](const QString& m) { if (error) *error = m; return false; };
    if (out) *out = Result();

    if (root.isEmpty())
        return fail(QStringLiteral("There is nowhere to install decoration packs to."));
    if (!entry.isValid())
        return fail(QStringLiteral("This registry entry doesn't describe an installable decoration pack."));

    // ---- 1. the digest, before anything else ---------------------------------------------------------
    // The pack is an opaque binary fetched over a URL the index chose, from a host neither this app nor the
    // registry's maintainer controls. The digest is the only statement the registry makes that survives the
    // host serving something else, so it is checked before the archive is even opened — not after a
    // successful unpack, which would mean the wrong bytes had already been decompressed and inspected.
    QFile zf(zipPath);
    if (!zf.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Couldn't read the downloaded pack."));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&zf))    // streamed: a pack is tens of megabytes and this runs on a 32-bit box
    { zf.close(); return fail(QStringLiteral("Couldn't read the downloaded pack.")); }
    zf.close();
    const QString got = QString::fromLatin1(hash.result().toHex());
    if (got != entry.sha256)
        return fail(QStringLiteral("Refused \"%1\": the download doesn't match the checksum this registry "
                                   "published (expected %2…, got %3…).")
                        .arg(entry.name.isEmpty() ? entry.id : entry.name,
                             entry.sha256.left(12), got.left(12)));

    // ---- 2. list + plan, still without writing anything ----------------------------------------------
    QStringList members;
    qint64 uncompressed = 0;
    if (!listMembers(zipPath, &members, &uncompressed, error))
        return false;
    if (uncompressed > DecorationPack::kMaxUncompressedBytes)
        return fail(QStringLiteral("Refused: this pack unpacks to %1 MB, more than the %2 MB a decoration "
                                   "pack may write.")
                        .arg(uncompressed / (1024 * 1024))
                        .arg(DecorationPack::kMaxUncompressedBytes / (1024 * 1024)));

    // The systems this pack may land for: the ones the app knows about AND the ones the ENTRY DECLARED.
    // The intersection, not the zip's contents, because the card the user pressed said "snes, nes" and a
    // pack that quietly also drops art into bezels/dreamcast/ has installed something nobody agreed to.
    // Found by driving the live UI: the fixture's zip carried a folder for a fourth console, the index entry
    // did not name it, and it landed anyway. Anything in the zip outside this set is reported as ignored.
    QStringList allowed;
    for (const QString& s : knownSystems)
        if (entry.systems.contains(s, Qt::CaseInsensitive)) allowed << s;
    if (allowed.isEmpty())
        return fail(QStringLiteral("This pack is for %1, which this app doesn't emulate.")
                        .arg(entry.systems.join(QStringLiteral(", "))));

    const DecorationPack::Plan plan = DecorationPack::planInstall(members, allowed);
    if (!plan.ok())
        return fail(plan.error);

    // ---- 3. extract into staging ---------------------------------------------------------------------
    const QString staging = root + QLatin1Char('/') + kStagingName;
    QDir(staging).removeRecursively();          // residue of an interrupted previous install
    const QString raw = staging + QStringLiteral("/raw");
    const QString outRoot = staging + QStringLiteral("/out");
    if (!QDir().mkpath(raw) || !QDir().mkpath(outRoot))
    { QDir(staging).removeRecursively(); return fail(QStringLiteral("Couldn't prepare a staging folder.")); }

    auto abort = [&staging, &fail](const QString& m) {
        QDir(staging).removeRecursively();
        return fail(m);
    };

    QString exErr;
    // The product's one whole-archive extractor. Its per-member ArchiveSafePath::join is the same guard
    // planInstall already asked about above; both are kept because they answer at different moments — the
    // plan refuses before any I/O, this refuses if the central directory and the local headers disagree.
    if (!ArchiveRom::extractAll(zipPath, raw, &exErr))
        return abort(QStringLiteral("Couldn't unpack this decoration pack: %1.").arg(exErr));

    // ---- 4. re-shape into the install layout, still in staging ---------------------------------------
    for (const DecorationPack::Item& it : plan.items)
    {
        QString member = it.member;
        member.replace(QLatin1Char('\\'), QLatin1Char('/'));
        const QString src = raw + QLatin1Char('/') + member;
        const QString dst = outRoot + QLatin1Char('/') + it.system + QLatin1Char('/') + entry.id
                          + QLatin1Char('/') + it.rel;
        if (!QDir().mkpath(QFileInfo(dst).absolutePath()))
            return abort(QStringLiteral("Couldn't create the folder for %1.").arg(it.rel));
        if (!QFile::rename(src, dst))
            return abort(QStringLiteral("Couldn't place %1 from this pack.").arg(it.rel));
    }

    // The manifest names the pack on the Installed list. Written per system folder because each is
    // independently removable — a user who deletes bezels/snes/<id> by hand leaves the gba copy describing
    // itself correctly.
    const QByteArray manifest = DecorationPack::packManifest(entry);
    for (const QString& sys : plan.systems)
    {
        QFile mf(outRoot + QLatin1Char('/') + sys + QLatin1Char('/') + entry.id + QStringLiteral("/pack.json"));
        if (!mf.open(QIODevice::WriteOnly) || mf.write(manifest) != manifest.size())
            return abort(QStringLiteral("Couldn't record this pack's details."));
        mf.close();
    }

    // ---- 5. swap each system's folder into place -----------------------------------------------------
    // One directory rename per system, so what the renderer can ever see is the previous pack or the new
    // one, never a folder being filled in. A failure part-way through is reported with the systems that DID
    // land rather than being called a clean install.
    QStringList landed;
    for (const QString& sys : plan.systems)
    {
        const QString dest = DecorationPack::packDir(root, sys, entry.id);
        if (dest.isEmpty())
            return abort(QStringLiteral("\"%1\" is not a folder name this app can write.").arg(sys));
        if (!QDir().mkpath(root + QLatin1Char('/') + sys))
            return abort(QStringLiteral("Couldn't create the folder for %1.").arg(sys));
        if (QFileInfo::exists(dest) && !QDir(dest).removeRecursively())
        {
            QDir(staging).removeRecursively();
            return fail(QStringLiteral("Couldn't replace the copy of this pack already installed for %1.").arg(sys));
        }
        if (!QDir().rename(outRoot + QLatin1Char('/') + sys + QLatin1Char('/') + entry.id, dest))
        {
            QDir(staging).removeRecursively();
            return fail(landed.isEmpty()
                            ? QStringLiteral("Couldn't install this pack for %1.").arg(sys)
                            : QStringLiteral("Installed this pack for %1, but couldn't install it for %2.")
                                  .arg(landed.join(QStringLiteral(", ")), sys));
        }
        landed << sys;
    }

    QDir(staging).removeRecursively();

    if (out)
    {
        out->systems = plan.systems;
        out->ignored = plan.ignored;
        out->files   = int(plan.items.size());
    }
    return true;
}

} // namespace DecorationInstall
