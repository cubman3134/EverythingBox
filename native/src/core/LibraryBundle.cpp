#include "LibraryBundle.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QIODevice>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QXmlStreamReader>
#include <algorithm>
#include <functional>

namespace LibraryBundle
{
namespace
{
    // The transferable set, by extension. Art and metadata only: a trailer (.mp4), a theme song (.mp3) and a
    // manual (.pdf/.cbz) are megabytes apiece and are NOT what this feature moves, so they are excluded here
    // — on both ends, which is what keeps the two stamps comparable.
    bool allowedExtension(const QString& lowerName)
    {
        static const char* kExts[] = { ".json", ".png", ".jpg", ".jpeg", ".webp", ".gif",
                                       ".bmp",  ".xml", ".txt", ".svg" };
        for (const char* e : kExts)
            if (lowerName.endsWith(QLatin1String(e))) return true;
        return false;
    }

    // Windows swallows these as device names whatever the extension, so a file called "con.png" is not a file
    // name we will ever create. Refusing them here means a bundle behaves the same on every platform.
    bool reservedDeviceName(const QString& lowerName)
    {
        const int dot = lowerName.indexOf(QLatin1Char('.'));
        const QString stem = dot < 0 ? lowerName : lowerName.left(dot);
        static const char* kNames[] = { "con", "prn", "aux", "nul" };
        for (const char* n : kNames)
            if (stem == QLatin1String(n)) return true;
        if (stem.size() == 4 && (stem.startsWith(QLatin1String("com")) || stem.startsWith(QLatin1String("lpt")))
            && stem.at(3).isDigit() && stem.at(3) != QLatin1Char('0'))
            return true;
        return false;
    }

    QList<FileEntry> sortedByName(QList<FileEntry> files)
    {
        std::sort(files.begin(), files.end(),
                  [](const FileEntry& a, const FileEntry& b) { return a.name < b.name; });
        return files;
    }

    // The cheap half of the stamp: names, sizes and times, and NOT meta.json's contents. Used only to decide
    // whether a cached stamp is still valid, which is the whole saving — an unchanged item costs a directory
    // listing instead of a meta.json read.
    QString signatureOf(const QList<FileEntry>& files)
    {
        QCryptographicHash h(QCryptographicHash::Sha256);
        for (const FileEntry& f : sortedByName(files))
        {
            h.addData(f.name.toUtf8());
            h.addData(QByteArray("\0", 1));
            h.addData(QByteArray::number(f.size));
            h.addData(QByteArray("\0", 1));
            h.addData(QByteArray::number(f.mtimeMs));
            h.addData(QByteArray("\n", 1));
        }
        return QString::fromLatin1(h.result().toHex());
    }

    QString incomingRoot(const QString& root) { return root + QLatin1Char('/') + QLatin1String(kIncomingDir); }
    QString retiredRoot(const QString& root)  { return root + QLatin1Char('/') + QLatin1String(kRetiredDir);  }

    QString stampCachePath(const QString& root) { return root + QLatin1Char('/') + QLatin1String(kStampCache); }

    // Put one item back after an interrupted landing. The retired folder is the ONLY copy of the item while a
    // swap is half done, so it is restored before anything else looks at the tree; a retired folder whose live
    // folder is already there is simply the tail of a landing that succeeded, and is dropped.
    void recoverItem(const QString& root, const QString& id)
    {
        const QString live    = itemDir(root, id);
        const QString retired = retiredRoot(root) + QLatin1Char('/') + id;
        if (QFileInfo::exists(retired))
        {
            if (!QFileInfo::exists(live)) QDir().rename(retired, live);
            else                          QDir(retired).removeRecursively();
        }
        const QString staged = incomingRoot(root) + QLatin1Char('/') + id;
        if (QFileInfo::exists(staged)) QDir(staged).removeRecursively();
    }

    bool writeFileWithTime(const QString& path, const QByteArray& data, qint64 mtimeMs, QString& error)
    {
        {
            QFile out(path);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            {
                error = QStringLiteral("could not write a file into the cache");
                return false;
            }
            if (out.write(data) != qint64(data.size()))
            {
                error = QStringLiteral("the cache ran out of room");
                return false;
            }
            out.close();
        }
        // The source's modification time, restored. THIS is what makes a second run transfer nothing: the
        // target recomputes the same stamp the source sent. Reopened read-write because setting a time needs
        // write access to the handle, and done after the close so no buffered write can overwrite it again.
        // A filesystem that refuses (or rounds) the time is not an error — the item simply looks changed next
        // run and is sent again, which is slower and never wrong.
        if (mtimeMs > 0)
        {
            QFile t(path);
            if (t.open(QIODevice::ReadWrite))
            {
                t.setFileTime(QDateTime::fromMSecsSinceEpoch(mtimeMs), QFileDevice::FileModificationTime);
                t.close();
            }
        }
        return true;
    }

    // One file of a landing: its name and time, and how its bytes get into the staging folder. v1 writes
    // them from memory; v2 (#291) copies them straight off the spooled body.
    struct StagedFile
    {
        QString name;
        qint64  mtimeMs = 0;
    };
    using WriteOne = std::function<bool(int index, const QString& path, QString& error)>;

    // Defined in section 9: whether a spool path belongs to a body this process is still receiving.
    bool spoolIsLive(const QString& absPath);

    // Defined in section 9, beside the v2 landing that shares it.
    LandResult landCommon(const QString& root, const QString& id, const QString& stamp, qint64 updatedMs,
                          const QList<StagedFile>& files, const WriteOne& writeOne, const LandOptions& opts,
                          QString& error);
}

// ------------------------------------------------------------------ 1. safety ------------------------------

bool safeItemId(const QString& id)
{
    if (id.size() != 40) return false;
    for (int i = 0; i < id.size(); ++i)
    {
        const QChar c = id.at(i);
        const bool hex = (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                      || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
        if (!hex) return false;
    }
    return true;
}

bool safeFileName(const QString& name)
{
    if (name.isEmpty() || name.size() > 128) return false;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) return false;
    if (name.contains(QLatin1Char(':'))) return false;
    if (name.startsWith(QLatin1Char('.'))) return false;      // ".." and every hidden/bookkeeping file
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) return false;
    for (int i = 0; i < name.size(); ++i)
    {
        const QChar c = name.at(i);
        const bool ok = c.isDigit()
                     || (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                     || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
                     || c == QLatin1Char('.') || c == QLatin1Char('_') || c == QLatin1Char('-');
        if (!ok) return false;
    }
    const QString lower = name.toLower();
    if (reservedDeviceName(lower)) return false;
    return allowedExtension(lower);
}

// ------------------------------------------------------------------ 2. the stamp ---------------------------

QString stampOf(const QList<FileEntry>& files, const QByteArray& metaJson)
{
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(QByteArray("ebbundle/") + QByteArray::number(kFormatVersion) + QByteArray("\n"));
    for (const FileEntry& f : sortedByName(files))
    {
        h.addData(f.name.toUtf8());
        h.addData(QByteArray("\0", 1));
        if (f.name == QLatin1String("meta.json"))
        {
            // meta.json is small and is the one file whose CONTENT decides whether an item changed, so it is
            // hashed byte for byte. Everything else contributes size and time — see the header for why the
            // art itself is not read, and what that costs.
            h.addData(QCryptographicHash::hash(metaJson, QCryptographicHash::Sha256));
        }
        else
        {
            h.addData(QByteArray::number(f.size));
            h.addData(QByteArray("\0", 1));
            h.addData(QByteArray::number(f.mtimeMs));
        }
        h.addData(QByteArray("\n", 1));
    }
    return QString::fromLatin1(h.result().toHex());
}

qint64 updatedMsOf(const QList<FileEntry>& files)
{
    qint64 newest = 0;
    for (const FileEntry& f : files)
        if (f.mtimeMs > newest) newest = f.mtimeMs;
    return newest;
}

// ------------------------------------------------------------------ 3. the inventory -----------------------

QByteArray inventoryJson(const QList<Entry>& entries)
{
    QJsonArray arr;
    for (const Entry& e : entries)
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), e.id);
        o.insert(QStringLiteral("stamp"), e.stamp);
        o.insert(QStringLiteral("updated"), double(e.updatedMs));
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("v"), kFormatVersion);
    root.insert(QStringLiteral("items"), arr);
    // #291: what this device accepts. NOT a bump of "v" -- an older source refuses an inventory whose "v" is
    // higher than its own -- but a field an older parser simply does not read.
    root.insert(QStringLiteral("bundle"), QJsonArray{ kFormatVersion, kPayloadFormatV2 });
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool parseInventory(const QByteArray& json, QList<Entry>& out, QString& error)
{
    out.clear();
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) { error = QStringLiteral("that device sent an inventory we could not read"); return false; }
    const QJsonObject root = doc.object();
    const int v = root.value(QStringLiteral("v")).toInt(kFormatVersion);
    if (v > kFormatVersion)
    {
        error = QStringLiteral("that device keeps its library in a newer format than this one understands");
        return false;
    }
    const QJsonArray arr = root.value(QStringLiteral("items")).toArray();
    for (const QJsonValue& v2 : arr)
    {
        const QJsonObject o = v2.toObject();
        Entry e;
        e.id        = o.value(QStringLiteral("id")).toString();
        e.stamp     = o.value(QStringLiteral("stamp")).toString();
        e.updatedMs = qint64(o.value(QStringLiteral("updated")).toDouble());
        if (e.id.isEmpty() || e.stamp.isEmpty()) continue;   // a half-named entry is not an entry
        out << e;
    }
    return true;
}

// ------------------------------------------------------------------ 4. the diff ----------------------------

Verdict verdictFor(const Entry& source, const Entry* target)
{
    if (!target) return Verdict::Send;                       // missing: always
    if (target->stamp == source.stamp) return Verdict::Unchanged;
    // Different content. The newer copy wins, and "newer" is a time because two stamps do not compare. A tie
    // goes to the TARGET: warming a cache is never worth overwriting something a user may have just made.
    if (target->updatedMs >= source.updatedMs) return Verdict::TargetNewer;
    return Verdict::Send;
}

Plan planTransfer(const QList<Entry>& source, const QList<Entry>& target)
{
    QHash<QString, Entry> byId;
    byId.reserve(target.size());
    for (const Entry& e : target) byId.insert(e.id, e);

    Plan p;
    for (const Entry& s : source)
    {
        if (s.id.isEmpty()) continue;
        const auto it = byId.constFind(s.id);
        const Entry* t = it == byId.constEnd() ? nullptr : &it.value();
        switch (verdictFor(s, t))
        {
            case Verdict::Send:        p.send        << s.id; break;
            case Verdict::Unchanged:   p.unchanged   << s.id; break;
            case Verdict::TargetNewer: p.targetNewer << s.id; break;
        }
    }
    return p;
}

// ------------------------------------------------------------------ 5. the payload -------------------------

QByteArray encodePayload(const Payload& p)
{
    QJsonArray files;
    for (const PayloadFile& f : p.files)
    {
        QJsonObject o;
        o.insert(QStringLiteral("name"), f.name);
        o.insert(QStringLiteral("mtime"), double(f.mtimeMs));
        o.insert(QStringLiteral("data"), QString::fromLatin1(f.data.toBase64()));
        files.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("v"), p.version);
    root.insert(QStringLiteral("id"), p.id);
    root.insert(QStringLiteral("stamp"), p.stamp);
    root.insert(QStringLiteral("updated"), double(p.updatedMs));
    root.insert(QStringLiteral("files"), files);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool decodePayload(const QByteArray& bytes, Payload& out, Refusal& why, QString& message)
{
    out = Payload();
    why = Refusal::None;
    message.clear();

    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject())
    {
        why = Refusal::Malformed;
        message = QStringLiteral("that bundle was not readable");
        return false;
    }
    const QJsonObject root = doc.object();
    out.version = root.value(QStringLiteral("v")).toInt(0);
    if (out.version <= 0)
    {
        why = Refusal::Malformed;
        message = QStringLiteral("that bundle did not say what format it is");
        return false;
    }
    if (out.version > kFormatVersion)
    {
        // #127's no-cross-version-translation guard: half-understanding a future cache format is how a cache
        // becomes wrong rather than merely stale.
        why = Refusal::FutureFormat;
        message = QStringLiteral("that device sends a newer library format than this one understands");
        return false;
    }
    out.id        = root.value(QStringLiteral("id")).toString();
    out.stamp     = root.value(QStringLiteral("stamp")).toString();
    out.updatedMs = qint64(root.value(QStringLiteral("updated")).toDouble());
    if (!safeItemId(out.id))
    {
        // A traversal attempt lands here, and it lands here BEFORE any path is built out of the id.
        why = Refusal::UnsafeId;
        message = QStringLiteral("that bundle named an item this device will not write");
        return false;
    }

    const QJsonArray files = root.value(QStringLiteral("files")).toArray();
    qint64 total = 0;
    for (const QJsonValue& v : files)
    {
        const QJsonObject o = v.toObject();
        PayloadFile f;
        f.name    = o.value(QStringLiteral("name")).toString();
        f.mtimeMs = qint64(o.value(QStringLiteral("mtime")).toDouble());
        f.data    = QByteArray::fromBase64(o.value(QStringLiteral("data")).toString().toLatin1());
        if (!safeFileName(f.name))
        {
            why = Refusal::UnsafeFileName;
            message = QStringLiteral("that bundle carried a file this device will not write");
            return false;
        }
        if (qint64(f.data.size()) > kMaxFileBytes)
        {
            why = Refusal::TooLarge;
            message = QStringLiteral("that bundle carried a file too large for an art transfer");
            return false;
        }
        total += qint64(f.data.size());
        out.files << f;
    }
    if (out.files.isEmpty())
    {
        why = Refusal::Empty;
        message = QStringLiteral("that bundle carried nothing");
        return false;
    }
    if (total > kMaxItemBytes)
    {
        why = Refusal::TooLarge;
        message = QStringLiteral("that bundle was too large for an art transfer");
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ 6. the disk ----------------------------

QString itemDir(const QString& root, const QString& id)
{
    return root + QLatin1Char('/') + id;
}

bool scanItem(const QString& root, const QString& id, Entry& entry, QList<FileEntry>& files)
{
    files.clear();
    entry = Entry();
    if (!safeItemId(id)) return false;

    QDir d(itemDir(root, id));
    if (!d.exists()) return false;

    const QFileInfoList list = d.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& fi : list)
    {
        const QString name = fi.fileName();
        if (!safeFileName(name)) continue;                       // not ours to move
        if (fi.size() > kMaxFileBytes) continue;                 // media-sized: excluded on BOTH ends
        FileEntry f;
        f.name    = name;
        f.size    = fi.size();
        f.mtimeMs = fi.lastModified().toMSecsSinceEpoch();
        files << f;
    }
    if (files.isEmpty()) return false;

    QByteArray metaJson;
    QFile meta(itemDir(root, id) + QStringLiteral("/meta.json"));
    if (meta.open(QIODevice::ReadOnly)) metaJson = meta.readAll();

    entry.id        = id;
    entry.stamp     = stampOf(files, metaJson);
    entry.updatedMs = updatedMsOf(files);
    return true;
}

QList<Entry> inventoryFor(const QString& root)
{
    QList<Entry> out;
    QDir r(root);
    if (!r.exists()) return out;

    // An interrupted landing is put right before anything reports what this device holds — otherwise the
    // inventory would name an item that is only half here, and the diff would build on it.
    sweepPartials(root);

    // The stamp cache. Keyed by item id, valid while the cheap signature (names/sizes/times) is unchanged;
    // a hit skips the meta.json read, which is the only I/O the stamp needs beyond the directory listing.
    QJsonObject cache;
    {
        QFile f(stampCachePath(root));
        if (f.open(QIODevice::ReadOnly))
        {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject() && doc.object().value(QStringLiteral("v")).toInt() == kFormatVersion)
                cache = doc.object().value(QStringLiteral("items")).toObject();
        }
    }
    QJsonObject fresh;
    bool cacheChanged = false;

    const QStringList dirs = r.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& id : dirs)
    {
        if (!safeItemId(id)) continue;                 // skips .eb-incoming / .eb-retired without a special case

        QDir d(itemDir(root, id));
        QList<FileEntry> files;
        const QFileInfoList list = d.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& fi : list)
        {
            if (!safeFileName(fi.fileName())) continue;
            if (fi.size() > kMaxFileBytes) continue;
            FileEntry f;
            f.name    = fi.fileName();
            f.size    = fi.size();
            f.mtimeMs = fi.lastModified().toMSecsSinceEpoch();
            files << f;
        }
        if (files.isEmpty()) continue;

        const QString sig = signatureOf(files);
        Entry e;
        e.id        = id;
        e.updatedMs = updatedMsOf(files);

        const QJsonObject cached = cache.value(id).toObject();
        if (cached.value(QStringLiteral("sig")).toString() == sig)
        {
            e.stamp = cached.value(QStringLiteral("stamp")).toString();
        }
        if (e.stamp.isEmpty())
        {
            QByteArray metaJson;
            QFile meta(itemDir(root, id) + QStringLiteral("/meta.json"));
            if (meta.open(QIODevice::ReadOnly)) metaJson = meta.readAll();
            e.stamp = stampOf(files, metaJson);
            cacheChanged = true;
        }

        QJsonObject entryObj;
        entryObj.insert(QStringLiteral("sig"), sig);
        entryObj.insert(QStringLiteral("stamp"), e.stamp);
        fresh.insert(id, entryObj);
        out << e;
    }

    if (cacheChanged || fresh.size() != cache.size())
    {
        QJsonObject doc;
        doc.insert(QStringLiteral("v"), kFormatVersion);
        doc.insert(QStringLiteral("items"), fresh);
        QFile f(stampCachePath(root));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(QJsonDocument(doc).toJson(QJsonDocument::Compact));
    }
    return out;
}

bool readPayload(const QString& root, const QString& id, Payload& out, QString& error)
{
    return readPayload(root, id, out, error, kMaxItemBytes);
}

bool readPayload(const QString& root, const QString& id, Payload& out, QString& error, qint64 maxItemBytes)
{
    out = Payload();
    error.clear();
    Entry e;
    QList<FileEntry> files;
    if (!scanItem(root, id, e, files))
    {
        error = QStringLiteral("that item is no longer in this device's cache");
        return false;
    }

    qint64 total = 0;
    for (const FileEntry& f : files)
    {
        QFile in(itemDir(root, id) + QLatin1Char('/') + f.name);
        if (!in.open(QIODevice::ReadOnly)) continue;
        PayloadFile pf;
        pf.name    = f.name;
        pf.mtimeMs = f.mtimeMs;
        pf.data    = in.readAll();
        total += qint64(pf.data.size());
        out.files << pf;
    }
    if (out.files.isEmpty())
    {
        error = QStringLiteral("that item's files could not be read");
        return false;
    }
    if (total > maxItemBytes)
    {
        out = Payload();
        error = QStringLiteral("that item's art is larger than a bundle carries");
        return false;
    }
    out.version   = kFormatVersion;
    out.id        = id;
    out.stamp     = e.stamp;
    out.updatedMs = e.updatedMs;
    return true;
}

LandResult landItem(const QString& root, const Payload& p, QString& error)
{
    return landItem(root, p, error, LandOptions());
}

LandResult landItem(const QString& root, const Payload& p, QString& error, const LandOptions& opts)
{
    error.clear();
    if (!safeItemId(p.id))
    {
        error = QStringLiteral("that bundle named an item this device will not write");
        return LandResult::Refused;
    }
    if (p.files.isEmpty())
    {
        error = QStringLiteral("that bundle carried nothing");
        return LandResult::Refused;
    }
    qint64 total = 0;
    for (const PayloadFile& f : p.files)
    {
        if (!safeFileName(f.name))
        {
            error = QStringLiteral("that bundle carried a file this device will not write");
            return LandResult::Refused;
        }
        total += qint64(f.data.size());
    }
    if (total > kMaxItemBytes)
    {
        error = QStringLiteral("that bundle was too large for an art transfer");
        return LandResult::Refused;
    }

    QList<StagedFile> files;
    for (const PayloadFile& f : p.files)
    {
        StagedFile sf;
        sf.name = f.name;
        sf.mtimeMs = f.mtimeMs;
        files << sf;
    }
    const WriteOne fromMemory = [&p](int index, const QString& path, QString& err) {
        return writeFileWithTime(path, p.files.at(index).data, 0, err);
    };
    return landCommon(root, p.id, p.stamp, p.updatedMs, files, fromMemory, opts, error);
}

int sweepPartials(const QString& root)
{
    int touched = 0;
    QDir retired(retiredRoot(root));
    if (retired.exists())
    {
        const QStringList ids = retired.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& id : ids)
        {
            if (!safeItemId(id)) { QDir(retired.filePath(id)).removeRecursively(); continue; }
            recoverItem(root, id);
            ++touched;
        }
    }
    QDir incoming(incomingRoot(root));
    if (incoming.exists())
    {
        const QStringList ids = incoming.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& id : ids)
        {
            QDir(incoming.filePath(id)).removeRecursively();
            ++touched;
        }
        // #291: a spooled v2 body that no transfer in THIS process owns is a crash leftover. One that is still
        // registered belongs to a body arriving right now -- an inventory request in the middle of it must
        // not pull the file out from under the socket.
        const QFileInfoList strays = incoming.entryInfoList(QDir::Files | QDir::Hidden | QDir::System);
        for (const QFileInfo& fi : strays)
        {
            if (spoolIsLive(fi.absoluteFilePath())) continue;
            if (QFile::remove(fi.absoluteFilePath())) ++touched;
        }
    }
    // Drop the two bookkeeping folders themselves once they are empty. rmdir refuses a non-empty directory,
    // so this can only ever remove one that holds nothing — and leaving them behind would put two folders a
    // user did not create in a cache directory for the rest of the install's life.
    QDir().rmdir(retiredRoot(root));
    QDir().rmdir(incomingRoot(root));
    return touched;
}

// ------------------------------------------------------------------ 7. the answers -------------------------

Receipt receiptFor(LandResult r, const QString& message)
{
    Receipt out;
    switch (r)
    {
        case LandResult::Landed:         out.httpStatus = 200; out.result = QStringLiteral("landed");  break;
        case LandResult::AlreadyCurrent: out.httpStatus = 200; out.result = QStringLiteral("current"); break;
        case LandResult::KeptNewer:      out.httpStatus = 200; out.result = QStringLiteral("kept");    break;
        case LandResult::Refused:        out.httpStatus = 400; out.result = QStringLiteral("refused"); break;
        case LandResult::Interrupted:    out.httpStatus = 500; out.result = QStringLiteral("failed");  break;
        case LandResult::WriteFailed:    out.httpStatus = 500; out.result = QStringLiteral("failed");  break;
        // #292: a gamelist entry for a system or ROM this device lacks. A decision, not an error.
        case LandResult::NotApplicable:  out.httpStatus = 200; out.result = QStringLiteral("notapplicable"); break;
    }
    if (r != LandResult::Landed && r != LandResult::AlreadyCurrent) out.reason = message;
    return out;
}

QByteArray receiptJson(const Receipt& r)
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), r.httpStatus == 200);
    o.insert(QStringLiteral("result"), r.result);
    if (!r.reason.isEmpty()) o.insert(QStringLiteral("reason"), r.reason);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

bool parseReceipt(const QByteArray& json, Receipt& out)
{
    out = Receipt();
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return false;
    const QJsonObject o = doc.object();
    out.result = o.value(QStringLiteral("result")).toString();
    out.reason = o.value(QStringLiteral("reason")).toString();
    out.httpStatus = o.value(QStringLiteral("ok")).toBool() ? 200 : 400;
    return !out.result.isEmpty();
}

// ------------------------------------------------------------------ 8. progress ----------------------------

QString describeSize(qint64 bytes)
{
    // Below a megabyte, say kilobytes. A run that moved a few hundred kilobytes of PNG reporting "0.0 MB"
    // reads as a run that moved nothing, which is the one thing this feature's other message means.
    if (bytes < 1024LL * 1024LL)
        return QString::number(double(bytes) / 1024.0, 'f', 1) + QStringLiteral(" KB");
    return QString::number(double(bytes) / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB");
}

namespace
{
    // #292: the gamelist half of a run, as its own clause. Games added are counted apart from art items, and a
    // quiet second run says so in numbers ("0 games added, 12 already listed") rather than by omission. A game
    // the target has no ROM for is a decision the target made, so it is named, but not as a failure.
    QString gamelistClause(const Progress& p, bool withSize)
    {
        if (!p.gamelists) return QString();
        QString s = QStringLiteral(" Gamelists: %1 %2 added")
                        .arg(p.gamesAdded)
                        .arg(p.gamesAdded == 1 ? QStringLiteral("game") : QStringLiteral("games"));
        if (withSize && p.bytesSent > 0) s += QStringLiteral(" (%1)").arg(describeSize(p.bytesSent));
        s += QStringLiteral(", %1 already listed").arg(p.gamesListed);
        if (p.gamesNotApplicable > 0)
            s += QStringLiteral(", %1 not on that device").arg(p.gamesNotApplicable);
        if (p.gamesFailed > 0)
            s += QStringLiteral(", %1 could not be added").arg(p.gamesFailed);
        return s + QLatin1Char('.');
    }
}

QString describeProgress(const Progress& p, const QString& deviceName)
{
    const QString who = deviceName.isEmpty() ? QStringLiteral("that device") : deviceName;
    if (p.itemsTotal == 0 && p.gamelists && (p.gamesAdded > 0 || p.gamesFailed > 0))
    {
        // No art moved, but gamelist entries did (or tried to): "nothing to send" would be untrue.
        QString s = who + QStringLiteral("'s art cache is already up to date.");
        if (p.keptNewer > 0)
            s += (p.keptNewer == 1 ? QStringLiteral(" 1 item is newer there and was left alone.")
                                   : QStringLiteral(" %1 items are newer there and were left alone.").arg(p.keptNewer));
        return s + gamelistClause(p, true);
    }
    if (p.itemsTotal == 0)
    {
        // The second run, and the sentence that says the feature worked: nothing left this machine. But an
        // item the TARGET has a newer copy of was a deliberate decision, not an absence of one, so it is
        // named — "up to date" on its own would quietly claim the two ends agree when they do not.
        if (p.keptNewer > 0)
            return who + QStringLiteral(" is already up to date — nothing to send. %1 %2 newer there and %3 "
                                        "left alone.")
                             .arg(p.keptNewer)
                             .arg(p.keptNewer == 1 ? QStringLiteral("is") : QStringLiteral("are"))
                             .arg(p.keptNewer == 1 ? QStringLiteral("was") : QStringLiteral("were"))
                   + gamelistClause(p, false);
        return who + QStringLiteral(" is already up to date — nothing to send.") + gamelistClause(p, false);
    }
    QString s = QStringLiteral("Sent %1 of %2 items (%3) to %4.")
                    .arg(p.itemsSent).arg(p.itemsTotal)
                    .arg(describeSize(p.bytesSent)).arg(who);
    if (p.unchanged > 0)
        s += (p.unchanged == 1 ? QStringLiteral(" 1 was already there.")
                               : QStringLiteral(" %1 were already there.").arg(p.unchanged));
    if (p.keptNewer > 0)
        s += (p.keptNewer == 1 ? QStringLiteral(" 1 is newer there and was left alone.")
                               : QStringLiteral(" %1 are newer there and were left alone.").arg(p.keptNewer));
    if (p.failed > 0)
        s += (p.failed == 1 ? QStringLiteral(" 1 could not be sent.")
                            : QStringLiteral(" %1 could not be sent.").arg(p.failed));
    return s + gamelistClause(p, false);
}

// ------------------------------------------------------------------ 9. the raw-body format (#291) ---------

namespace
{
    const char kMagic[] = "EBBUNDLE";    // 8 bytes, no terminator on the wire
    constexpr qint64 kCopyChunk = 256 * 1024;

    void appendU32(QByteArray& b, quint32 v)
    {
        b.append(char((v >> 24) & 0xff));
        b.append(char((v >> 16) & 0xff));
        b.append(char((v >> 8) & 0xff));
        b.append(char(v & 0xff));
    }

    quint32 u32At(const QByteArray& b, int at)
    {
        return (quint32(quint8(b.at(at))) << 24) | (quint32(quint8(b.at(at + 1))) << 16)
             | (quint32(quint8(b.at(at + 2))) << 8) | quint32(quint8(b.at(at + 3)));
    }

    bool refuse(Refusal r, const QString& text, Refusal& why, QString& message)
    {
        why = r;
        message = text;
        return false;
    }

    void setMtime(const QString& path, qint64 mtimeMs)
    {
        if (mtimeMs <= 0) return;
        QFile t(path);
        if (t.open(QIODevice::ReadWrite))
        {
            t.setFileTime(QDateTime::fromMSecsSinceEpoch(mtimeMs), QFileDevice::FileModificationTime);
            t.close();
        }
    }

    // The landing both formats share: recover, keep-the-newer, stage, carry the untouched files across, swap.
    // Callers have ALREADY validated the id, the names and the sizes; the per-file name check below is a second
    // line, so a caller that forgot cannot write a file the allowlist refuses -- but it is not where a refusal
    // is meant to happen, and for v2 it would come too late (after earlier files' bytes were staged).
    LandResult landCommon(const QString& root, const QString& id, const QString& stamp, qint64 updatedMs,
                          const QList<StagedFile>& files, const WriteOne& writeOne, const LandOptions& opts,
                          QString& error)
    {
        recoverItem(root, id);

        // Warm, never fight. An identical stamp is already the answer; a LOCAL copy that is newer is kept, and
        // the source is told so rather than being let believe it overwrote something.
        {
            Entry cur;
            QList<FileEntry> curFiles;
            if (scanItem(root, id, cur, curFiles))
            {
                if (cur.stamp == stamp) return LandResult::AlreadyCurrent;
                if (cur.updatedMs > updatedMs) return LandResult::KeptNewer;
            }
        }

        const QString staging = incomingRoot(root) + QLatin1Char('/') + id;
        QDir(staging).removeRecursively();
        if (!QDir().mkpath(staging))
        {
            error = QStringLiteral("this device could not open its cache for writing");
            return LandResult::WriteFailed;
        }

        int written = 0;
        QSet<QString> staged;
        for (int i = 0; i < files.size(); ++i)
        {
            const StagedFile& f = files.at(i);
            if (opts.failAfterFiles >= 0 && written >= opts.failAfterFiles)
            {
                // Interrupted mid-item. The live folder has not been touched yet, so the target still holds
                // exactly what it held before; the staged remains are swept on the next run.
                error = QStringLiteral("the transfer was interrupted");
                return LandResult::Interrupted;
            }
            if (!safeFileName(f.name))
            {
                QDir(staging).removeRecursively();
                QDir().rmdir(incomingRoot(root));
                error = QStringLiteral("that bundle carried a file this device will not write");
                return LandResult::Refused;
            }
            if (!writeOne(i, staging + QLatin1Char('/') + f.name, error))
            {
                QDir(staging).removeRecursively();
                QDir().rmdir(incomingRoot(root));
                return LandResult::WriteFailed;
            }
            setMtime(staging + QLatin1Char('/') + f.name, f.mtimeMs);
            staged.insert(f.name);
            ++written;
        }

        const QString live    = itemDir(root, id);
        const QString retired = retiredRoot(root) + QLatin1Char('/') + id;
        QDir().mkpath(retiredRoot(root));
        QDir(retired).removeRecursively();

        const bool hadLive = QFileInfo::exists(live);
        if (hadLive && !QDir().rename(live, retired))
        {
            QDir(staging).removeRecursively();
            error = QStringLiteral("this device could not replace its copy of that item");
            return LandResult::WriteFailed;
        }
        if (hadLive)
        {
            // Carry across whatever the bundle did NOT bring — the trailer, the theme song, the manual. Warming
            // an art cache must not cost the target the megabyte roles it already fetched.
            const QFileInfoList kept = QDir(retired).entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
            for (const QFileInfo& fi : kept)
            {
                if (staged.contains(fi.fileName())) continue;
                QFile::rename(fi.absoluteFilePath(), staging + QLatin1Char('/') + fi.fileName());
            }
        }
        if (!QDir().rename(staging, live))
        {
            if (hadLive) QDir().rename(retired, live);         // put the old item back rather than leave a hole
            QDir(staging).removeRecursively();
            error = QStringLiteral("this device could not move that item into place");
            return LandResult::WriteFailed;
        }
        QDir(retired).removeRecursively();
        // Tidy the two bookkeeping folders while they are empty, so a finished transfer leaves a cache directory
        // holding items and nothing else. Both refuse to remove a non-empty directory, so a landing that runs
        // beside another one in flight (or beside a spooling body) cannot take its staging away.
        QDir().rmdir(retiredRoot(root));
        QDir().rmdir(incomingRoot(root));
        return LandResult::Landed;
    }

    // The spools this process has open. A spool NOT in here is a crash leftover, and sweepPartials removes it;
    // one in here belongs to a body still arriving, and an inventory request in the meantime must not take it.
    QMutex& spoolMutex()
    {
        static QMutex m;
        return m;
    }
    QSet<QString>& liveSpools()
    {
        static QSet<QString> s;
        return s;
    }
    bool spoolIsLive(const QString& absPath)
    {
        QMutexLocker lock(&spoolMutex());
        return liveSpools().contains(absPath);
    }
}

bool parseInventory(const QByteArray& json, QList<Entry>& out, QList<int>& formats, QString& error)
{
    formats = QList<int>{ kFormatVersion };
    if (!parseInventory(json, out, error)) return false;
    // The capability field. Absent (an older device), or anything but an array of numbers, means v1 only.
    const QJsonValue adv = QJsonDocument::fromJson(json).object().value(QStringLiteral("bundle"));
    if (adv.isArray())
    {
        for (const QJsonValue& v : adv.toArray())
        {
            if (!v.isDouble()) continue;
            const int f = v.toInt(0);
            if (f > 0 && !formats.contains(f)) formats << f;
        }
    }
    return true;
}

int chooseBundleFormat(const QList<int>& advertised)
{
    return advertised.contains(kPayloadFormatV2) ? kPayloadFormatV2 : kFormatVersion;
}

qint64 maxItemBytesFor(int format)
{
    return format == kPayloadFormatV2 ? kMaxItemBytesV2 : kMaxItemBytes;
}

qint64 fileBytesOf(const Payload& p)
{
    qint64 total = 0;
    for (const PayloadFile& f : p.files) total += qint64(f.data.size());
    return total;
}

QByteArray encodePayloadV2(const Payload& p)
{
    QJsonArray files;
    for (const PayloadFile& f : p.files)
    {
        QJsonObject o;
        o.insert(QStringLiteral("name"), f.name);
        o.insert(QStringLiteral("mtime"), double(f.mtimeMs));
        o.insert(QStringLiteral("size"), double(f.data.size()));
        files.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("id"), p.id);
    root.insert(QStringLiteral("stamp"), p.stamp);
    root.insert(QStringLiteral("updated"), double(p.updatedMs));
    root.insert(QStringLiteral("files"), files);
    const QByteArray header = QJsonDocument(root).toJson(QJsonDocument::Compact);

    QByteArray out;
    out.reserve(int(kV2PreambleBytes + header.size() + fileBytesOf(p)));
    out.append(kMagic, 8);
    appendU32(out, quint32(kPayloadFormatV2));
    appendU32(out, quint32(header.size()));
    out.append(header);
    for (const PayloadFile& f : p.files) out.append(f.data);
    return out;
}

namespace
{
    // The preamble and header JSON every v2 body starts with, whatever its kind (#291's rules, shared by #292's
    // gamelist entries). Leaves `in` just past the header.
    bool readV2HeaderObject(QIODevice& in, QJsonObject& root, Refusal& why, QString& message)
    {
        // The length check each decoder ends with needs the body's size, so a stream that cannot say is refused
        // outright rather than half-trusted. The target spools to a file for exactly this reason.
        if (in.isSequential() || !in.isReadable())
            return refuse(Refusal::Malformed, QStringLiteral("that bundle was not readable"), why, message);

        const QByteArray pre = in.read(kV2PreambleBytes);
        if (pre.size() != kV2PreambleBytes || !pre.startsWith(QByteArray(kMagic, 8)))
            return refuse(Refusal::Malformed, QStringLiteral("that bundle was not readable"), why, message);

        const quint32 version = u32At(pre, 8);
        if (version > quint32(kPayloadFormatV2))
            return refuse(Refusal::FutureFormat,
                          QStringLiteral("that device sends a newer library format than this one understands"),
                          why, message);
        if (version != quint32(kPayloadFormatV2))
            return refuse(Refusal::Malformed, QStringLiteral("that bundle did not say what format it is"), why, message);

        const quint32 headerLen = u32At(pre, 12);
        if (headerLen == 0 || qint64(headerLen) > kMaxV2HeaderBytes)
            return refuse(Refusal::Malformed, QStringLiteral("that bundle was not readable"), why, message);
        const QByteArray header = in.read(qint64(headerLen));
        if (header.size() != int(headerLen))
            return refuse(Refusal::Malformed, QStringLiteral("that bundle was not readable"), why, message);

        const QJsonDocument doc = QJsonDocument::fromJson(header);
        if (!doc.isObject())
            return refuse(Refusal::Malformed, QStringLiteral("that bundle was not readable"), why, message);
        root = doc.object();
        return true;
    }
}

bool decodeHeaderV2(QIODevice& in, BundleHeader& out, Refusal& why, QString& message)
{
    out = BundleHeader();
    why = Refusal::None;
    message.clear();

    QJsonObject root;
    if (!readV2HeaderObject(in, root, why, message)) return false;
    out.version = kPayloadFormatV2;

    // #292: a v2 body says what kind it is when it is not art. A gamelist entry is refused here as a kind --
    // with a sentence that names it -- rather than as an item id it never had.
    const QJsonValue kind = root.value(QStringLiteral("kind"));
    if (!kind.isUndefined() && kind.toString() != QLatin1String("art"))
        return refuse(Refusal::UnsupportedKind,
                      kind.toString() == QLatin1String(kSidecarKindGamelist)
                          ? QStringLiteral("that was a gamelist entry, and this device's art cache does not take those")
                          : QStringLiteral("that bundle is of a kind this device does not take"),
                      why, message);

    out.id        = root.value(QStringLiteral("id")).toString();
    out.stamp     = root.value(QStringLiteral("stamp")).toString();
    out.updatedMs = qint64(root.value(QStringLiteral("updated")).toDouble());
    if (!safeItemId(out.id))
        return refuse(Refusal::UnsafeId, QStringLiteral("that bundle named an item this device will not write"),
                      why, message);

    // EVERY file entry is judged here, before the first file byte is read. A decoder that checked each name
    // as it reached that file's bytes would already have read -- and a landing would already have staged --
    // the files ahead of the bad one.
    QSet<QString> names;
    qint64 total = 0;
    const QJsonArray files = root.value(QStringLiteral("files")).toArray();
    for (const QJsonValue& v : files)
    {
        const QJsonObject o = v.toObject();
        FileEntry f;
        f.name    = o.value(QStringLiteral("name")).toString();
        f.mtimeMs = qint64(o.value(QStringLiteral("mtime")).toDouble());
        if (!safeFileName(f.name))
            return refuse(Refusal::UnsafeFileName,
                          QStringLiteral("that bundle carried a file this device will not write"), why, message);
        const QJsonValue sizeValue = o.value(QStringLiteral("size"));
        if (!sizeValue.isDouble())
            return refuse(Refusal::Malformed, QStringLiteral("that bundle was not readable"), why, message);
        const double size = sizeValue.toDouble();
        if (size > double(kMaxFileBytes))
            return refuse(Refusal::TooLarge,
                          QStringLiteral("that bundle carried a file too large for an art transfer"), why, message);
        if (size < 0 || size != double(qint64(size)))
            return refuse(Refusal::Malformed, QStringLiteral("that bundle was not readable"), why, message);
        if (names.contains(f.name))
            return refuse(Refusal::Malformed, QStringLiteral("that bundle was not readable"), why, message);
        names.insert(f.name);
        f.size = qint64(size);
        total += f.size;
        out.files << f;
    }
    if (out.files.isEmpty())
        return refuse(Refusal::Empty, QStringLiteral("that bundle carried nothing"), why, message);
    if (total > kMaxItemBytesV2)
        return refuse(Refusal::TooLarge, QStringLiteral("that bundle was too large for an art transfer"),
                      why, message);

    // The declared sizes must account for EXACTLY the bytes that follow. Short means a truncated body; long
    // means bytes nobody declared. Either way the header is not describing this body, and nothing of it lands.
    if (in.size() - in.pos() != total)
        return refuse(Refusal::Malformed, QStringLiteral("that bundle's length did not match what it declared"),
                      why, message);

    out.fileBytes = total;
    return true;
}

bool decodePayloadV2(QIODevice& in, Payload& out, Refusal& why, QString& message)
{
    out = Payload();
    BundleHeader h;
    if (!decodeHeaderV2(in, h, why, message)) return false;
    out.version   = h.version;
    out.id        = h.id;
    out.stamp     = h.stamp;
    out.updatedMs = h.updatedMs;
    for (const FileEntry& f : h.files)
    {
        PayloadFile pf;
        pf.name    = f.name;
        pf.mtimeMs = f.mtimeMs;
        pf.data    = in.read(f.size);
        if (qint64(pf.data.size()) != f.size)
        {
            out = Payload();
            return refuse(Refusal::Malformed, QStringLiteral("that bundle ended early"), why, message);
        }
        out.files << pf;
    }
    return true;
}

LandResult landItemV2(const QString& root, QIODevice& in, QString& error)
{
    return landItemV2(root, in, error, LandOptions());
}

LandResult landItemV2(const QString& root, QIODevice& in, QString& error, const LandOptions& opts)
{
    error.clear();
    BundleHeader h;
    Refusal why = Refusal::None;
    if (!decodeHeaderV2(in, h, why, error)) return LandResult::Refused;

    QList<StagedFile> files;
    for (const FileEntry& f : h.files)
    {
        StagedFile s;
        s.name = f.name;
        s.mtimeMs = f.mtimeMs;
        files << s;
    }
    // The copy. Files are read in body order, which is the order landCommon asks for them; each goes straight
    // from the body into the staging folder a chunk at a time, so an item is never in memory whole.
    const WriteOne copyOne = [&in, &h](int index, const QString& path, QString& err) {
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            err = QStringLiteral("could not write a file into the cache");
            return false;
        }
        qint64 left = h.files.at(index).size;
        while (left > 0)
        {
            const QByteArray chunk = in.read(qMin(left, kCopyChunk));
            if (chunk.isEmpty())
            {
                err = QStringLiteral("that bundle ended early");
                return false;
            }
            if (out.write(chunk) != qint64(chunk.size()))
            {
                err = QStringLiteral("the cache ran out of room");
                return false;
            }
            left -= chunk.size();
        }
        out.close();
        return true;
    };
    return landCommon(root, h.id, h.stamp, h.updatedMs, files, copyOne, opts, error);
}

// ------------------------------------------------------------------ 10. the spool (#291) -------------------

QString openSpool(const QString& root, QFile& file)
{
    if (file.isOpen()) file.close();
    const QString dir = incomingRoot(root);
    if (!QDir().mkpath(dir)) return QString();
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const QString path = QFileInfo(dir + QStringLiteral("/bundle-")
                                       + QUuid::createUuid().toString(QUuid::Id128)
                                       + QStringLiteral(".spool")).absoluteFilePath();
        file.setFileName(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        {
            QMutexLocker lock(&spoolMutex());
            liveSpools().insert(path);
            return path;
        }
    }
    QDir().rmdir(dir);
    return QString();
}

void discardSpool(QFile& file)
{
    const QString path = QFileInfo(file.fileName()).absoluteFilePath();
    if (file.isOpen()) file.close();
    if (!file.fileName().isEmpty()) QFile::remove(path);
    {
        QMutexLocker lock(&spoolMutex());
        liveSpools().remove(path);
    }
    // Only ever the bookkeeping folder, and only when empty -- rmdir refuses anything else.
    const QFileInfo parent(QFileInfo(path).absolutePath());
    if (parent.fileName() == QLatin1String(kIncomingDir)) QDir().rmdir(parent.absoluteFilePath());
}

int spoolsInFlight()
{
    QMutexLocker lock(&spoolMutex());
    return int(liveSpools().size());
}

// ------------------------------------------------------------------ 11. gamelist sidecars (#292) ----------
namespace
{
    constexpr const char* kSidecarImageExts[] = { "png", "jpg", "jpeg", "webp", "gif", "bmp" };

    // Windows device names, judged on the part before the first dot with trailing spaces ignored ("con .sfc"
    // is the console too). Stricter than the art allowlist's check, because a ROM name is free text.
    bool deviceNameSegment(const QString& lower)
    {
        const int dot = lower.indexOf(QLatin1Char('.'));
        QString stem = dot < 0 ? lower : lower.left(dot);
        while (stem.endsWith(QLatin1Char(' '))) stem.chop(1);
        static const char* kNames[] = { "con", "prn", "aux", "nul", "conin$", "conout$", "clock$" };
        for (const char* n : kNames)
            if (stem == QLatin1String(n)) return true;
        if (stem.size() == 4 && (stem.startsWith(QLatin1String("com")) || stem.startsWith(QLatin1String("lpt"))))
        {
            const QChar d = stem.at(3);
            if ((d >= QLatin1Char('1') && d <= QLatin1Char('9')) || d == QChar(0x00B9) || d == QChar(0x00B2)
                || d == QChar(0x00B3))
                return true;
        }
        return false;
    }

    QString roleSuffix(const QString& role)
    {
        if (role == QLatin1String("thumbnail")) return QStringLiteral("-thumb");
        if (role == QLatin1String("image"))     return QStringLiteral("-image");
        if (role == QLatin1String("marquee"))   return QStringLiteral("-marquee");
        if (role == QLatin1String("fanart"))    return QStringLiteral("-fanart");
        return QString();
    }

    // GamelistStore's fuzzy key, exactly: drop every (...) and [...] tag, keep lower-case letters and digits.
    QString cleanTitle(const QString& s)
    {
        static const QRegularExpression tags(QStringLiteral("[\\(\\[][^\\)\\]]*[\\)\\]]"));
        QString t = s;
        t.remove(tags);
        QString out;
        for (const QChar c : t)
            if (c.isLetterOrNumber()) out += c.toLower();
        return out;
    }

    // The three keys GamelistStore finds a ROM by, over one parsed list.
    struct ListedIndex
    {
        QSet<QString> byFile, byBase, byClean;
    };

    ListedIndex indexOf(const QList<GamelistGame>& games)
    {
        ListedIndex ix;
        for (const GamelistGame& g : games)
        {
            if (g.path.isEmpty()) continue;
            const QFileInfo fi(g.path);
            ix.byFile.insert(fi.fileName().toLower());
            ix.byBase.insert(fi.completeBaseName().toLower());
            const QString c1 = cleanTitle(fi.completeBaseName());
            const QString c2 = cleanTitle(g.fields.name);
            if (!c1.isEmpty()) ix.byClean.insert(c1);
            if (!c2.isEmpty()) ix.byClean.insert(c2);
        }
        return ix;
    }

    bool listedIn(const ListedIndex& ix, const QString& romName)
    {
        const QFileInfo rfi(romName);
        if (ix.byFile.contains(rfi.fileName().toLower())) return true;
        if (ix.byBase.contains(rfi.completeBaseName().toLower())) return true;
        const QString clean = cleanTitle(rfi.completeBaseName());
        return !clean.isEmpty() && ix.byClean.contains(clean);
    }

    QByteArray readWhole(const QString& path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    // The gamelist.xml text fields a sidecar carries, in the order they are written after name/desc/media.
    struct FieldRef
    {
        const char* tag;
        QString GamelistFields::*member;
    };
    const FieldRef kFields[] = {
        { "name", &GamelistFields::name },           { "desc", &GamelistFields::desc },
        { "rating", &GamelistFields::rating },       { "releasedate", &GamelistFields::releasedate },
        { "developer", &GamelistFields::developer }, { "publisher", &GamelistFields::publisher },
        { "genre", &GamelistFields::genre },         { "players", &GamelistFields::players },
    };

    bool anyField(const GamelistFields& f)
    {
        for (const FieldRef& r : kFields)
            if (!(f.*(r.member)).isEmpty()) return true;
        return false;
    }

    const char* const kSidecarUnsafe      = "that gamelist entry named a place this device will not write";
    const char* const kSidecarFileRefused = "that gamelist entry carried a file this device will not write";
    const char* const kSidecarUnreadable  = "that gamelist entry was not readable";
}

bool safePathSegment(const QString& segment)
{
    if (segment.isEmpty() || segment.size() > 255) return false;
    if (segment.startsWith(QLatin1Char('.'))) return false;            // ".", "..", and every hidden name
    if (segment.endsWith(QLatin1Char('.')) || segment.endsWith(QLatin1Char(' '))) return false;
    for (const QChar c : segment)
    {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == 0x7f) return false;
        if (u == u'/' || u == u'\\' || u == u':' || u == u'*' || u == u'?' || u == u'"' || u == u'<' || u == u'>'
            || u == u'|')
            return false;
    }
    return !deviceNameSegment(segment.toLower());
}

bool sidecarImageExtension(const QString& ext)
{
    for (const char* e : kSidecarImageExts)
        if (ext == QLatin1String(e)) return true;
    return false;
}

QStringList sidecarImageRoles()
{
    return { QStringLiteral("thumbnail"), QStringLiteral("image"), QStringLiteral("marquee"), QStringLiteral("fanart") };
}

QString sidecarImageName(const QString& romName, const QString& role, const QString& ext)
{
    const QString suffix = roleSuffix(role);
    if (suffix.isEmpty() || !sidecarImageExtension(ext) || !safePathSegment(romName)) return QString();
    const QString base = QFileInfo(romName).completeBaseName();
    if (base.isEmpty()) return QString();
    const QString name = base + suffix + QLatin1Char('.') + ext;
    return safePathSegment(name) ? name : QString();
}

QList<GamelistGame> parseGamelist(const QByteArray& xml)
{
    // The same walk GamelistStore makes: every <game>, each child's text, an empty value ignored.
    QList<GamelistGame> out;
    QXmlStreamReader r(xml);
    while (!r.atEnd())
    {
        r.readNext();
        if (!(r.isStartElement() && r.name() == QLatin1String("game"))) continue;
        GamelistGame g;
        while (!r.atEnd() && !(r.isEndElement() && r.name() == QLatin1String("game")))
        {
            r.readNext();
            if (!r.isStartElement()) continue;
            const QString tag = r.name().toString();
            const QString val = r.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
            if (val.isEmpty()) continue;
            if (tag == QLatin1String("path")) { g.path = val; continue; }
            bool isField = false;
            for (const FieldRef& f : kFields)
                if (tag == QLatin1String(f.tag)) { g.fields.*(f.member) = val; isField = true; break; }
            if (isField) continue;
            if (tag == QLatin1String("video") || !roleSuffix(tag).isEmpty()) g.media << qMakePair(tag, val);
        }
        if (g.path.isEmpty()) continue;
        QString rom = g.path;
        if (rom.startsWith(QLatin1String("./"))) rom = rom.mid(2);
        if (safePathSegment(rom)) g.rom = rom;
        out << g;
    }
    return out;
}

bool gamelistLists(const QList<GamelistGame>& games, const QString& romName)
{
    return listedIn(indexOf(games), romName);
}

QList<SidecarGame> sidecarGamesFor(const QString& romsRoot)
{
    QList<SidecarGame> out;
    if (romsRoot.isEmpty() || !QDir(romsRoot).exists()) return out;
    const QStringList systems = QDir(romsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& system : systems)
    {
        if (!safePathSegment(system)) continue;
        // ONLY the gamelist directly in a system folder: that folder's name is the key the target lands by.
        const QString listPath = romsRoot + QLatin1Char('/') + system + QLatin1Char('/') + QLatin1String(kGamelistFileName);
        if (!QFileInfo(listPath).isFile()) continue;
        QSet<QString> seen;
        for (const GamelistGame& g : parseGamelist(readWhole(listPath)))
        {
            if (g.rom.isEmpty() || seen.contains(g.rom)) continue;   // a subfolder path is not keyed by a ROM name
            SidecarGame s;
            s.system = system;
            s.rom = g.rom;
            s.fields = g.fields;
            for (const auto& m : g.media)
                if (!roleSuffix(m.first).isEmpty()) s.images << m;   // image roles only: a video never travels
            if (!anyField(s.fields) && s.images.isEmpty()) continue;
            seen.insert(g.rom);
            out << s;
        }
    }
    return out;
}

QList<SidecarSystem> sidecarInventoryFor(const QString& romsRoot)
{
    QList<SidecarSystem> out;
    if (romsRoot.isEmpty() || !QDir(romsRoot).exists()) return out;
    const QStringList systems = QDir(romsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& system : systems)
    {
        if (!safePathSegment(system)) continue;
        const QString dir = romsRoot + QLatin1Char('/') + system;
        SidecarSystem s;
        s.name = system;
        const QStringList files = QDir(dir).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& f : files)
        {
            if (!safePathSegment(f)) continue;
            if (f.compare(QLatin1String(kGamelistFileName), Qt::CaseInsensitive) == 0) continue;
            s.roms << f;
        }
        const QString listPath = dir + QLatin1Char('/') + QLatin1String(kGamelistFileName);
        if (!s.roms.isEmpty() && QFileInfo(listPath).isFile())
        {
            const ListedIndex ix = indexOf(parseGamelist(readWhole(listPath)));
            for (const QString& rom : s.roms)
                if (listedIn(ix, rom)) s.listed << rom;
        }
        out << s;
    }
    return out;
}

QByteArray sidecarInventoryJson(const QList<SidecarSystem>& systems)
{
    QJsonArray arr;
    for (const SidecarSystem& s : systems)
    {
        QJsonObject o;
        o.insert(QStringLiteral("name"), s.name);
        o.insert(QStringLiteral("roms"), QJsonArray::fromStringList(s.roms));
        o.insert(QStringLiteral("listed"), QJsonArray::fromStringList(s.listed));
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("v"), kSidecarFormat);
    root.insert(QStringLiteral("systems"), arr);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool parseSidecarInventory(const QByteArray& json, QList<SidecarSystem>& out, QString& error)
{
    out.clear();
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
    {
        error = QStringLiteral("that device sent a gamelist inventory we could not read");
        return false;
    }
    if (doc.object().value(QStringLiteral("v")).toInt(0) != kSidecarFormat)
    {
        error = QStringLiteral("that device keeps its gamelist inventory in a format this one does not understand");
        return false;
    }
    const auto strings = [](const QJsonValue& v) {
        QStringList l;
        const QJsonArray a = v.toArray();
        for (const QJsonValue& x : a)
            if (x.isString()) l << x.toString();
        return l;
    };
    const QJsonArray systems = doc.object().value(QStringLiteral("systems")).toArray();
    for (const QJsonValue& v : systems)
    {
        const QJsonObject o = v.toObject();
        SidecarSystem s;
        s.name = o.value(QStringLiteral("name")).toString();
        if (s.name.isEmpty()) continue;
        s.roms = strings(o.value(QStringLiteral("roms")));
        s.listed = strings(o.value(QStringLiteral("listed")));
        out << s;
    }
    return true;
}

QByteArray inventoryJson(const QList<Entry>& entries, bool sidecars)
{
    if (!sidecars) return inventoryJson(entries);
    QJsonObject root = QJsonDocument::fromJson(inventoryJson(entries)).object();
    // #292: a field an older parser does not read, beside "bundle", whose meaning is unchanged.
    root.insert(QStringLiteral("sidecars"), QJsonArray{ kSidecarFormat });
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool advertisesSidecars(const QByteArray& inventory)
{
    const QJsonValue v = QJsonDocument::fromJson(inventory).object().value(QStringLiteral("sidecars"));
    if (!v.isArray()) return false;
    const QJsonArray a = v.toArray();
    for (const QJsonValue& x : a)
        if (x.isDouble() && x.toInt(0) == kSidecarFormat) return true;
    return false;
}

SidecarPlan planSidecars(const QList<SidecarGame>& source, const QList<SidecarSystem>& target)
{
    QHash<QString, QPair<QSet<QString>, QSet<QString>>> bySystem;
    for (const SidecarSystem& s : target)
    {
        auto& e = bySystem[s.name];
        for (const QString& r : s.roms)   e.first.insert(r);
        for (const QString& r : s.listed) e.second.insert(r);
    }
    SidecarPlan p;
    for (const SidecarGame& g : source)
    {
        const auto it = bySystem.constFind(g.system);
        if (it == bySystem.constEnd() || !it.value().first.contains(g.rom)) { ++p.notApplicable; continue; }
        if (it.value().second.contains(g.rom)) { ++p.alreadyListed; continue; }
        p.send << g;
    }
    return p;
}

bool readSidecarPayload(const QString& romsRoot, const SidecarGame& game, SidecarPayload& out, QString& error)
{
    out = SidecarPayload();
    error.clear();
    if (!safePathSegment(game.system) || !safePathSegment(game.rom))
    {
        error = QString::fromLatin1(kSidecarUnsafe);
        return false;
    }
    out.system = game.system;
    out.rom = game.rom;
    out.fields = game.fields;

    const QString sysDir = QDir::cleanPath(QDir(romsRoot + QLatin1Char('/') + game.system).absolutePath());
    for (const QString& role : sidecarImageRoles())
    {
        QString rel;
        for (const auto& m : game.images)
            if (m.first == role) { rel = m.second; break; }
        if (rel.isEmpty()) continue;
        // This device's OWN gamelist, but still: an image that resolves outside the game's system folder is not
        // read, so an entry cannot put some other file of this machine on the wire.
        const QString abs = QDir::cleanPath(QDir(sysDir).absoluteFilePath(rel));
        if (!abs.startsWith(sysDir + QLatin1Char('/'))) continue;
        const QFileInfo fi(abs);
        const QString ext = fi.suffix().toLower();
        if (!fi.isFile() || fi.size() > kMaxFileBytes || !sidecarImageExtension(ext)) continue;
        QFile f(abs);
        if (!f.open(QIODevice::ReadOnly)) continue;
        SidecarImage img;
        img.role = role;
        img.ext = ext;
        img.data = f.readAll();
        out.images << img;
    }
    return true;
}

qint64 fileBytesOf(const SidecarPayload& p)
{
    qint64 total = 0;
    for (const SidecarImage& i : p.images) total += qint64(i.data.size());
    return total;
}

QByteArray encodeSidecarV2(const SidecarPayload& p)
{
    QJsonObject game;
    for (const FieldRef& f : kFields)
        if (!(p.fields.*(f.member)).isEmpty()) game.insert(QString::fromLatin1(f.tag), p.fields.*(f.member));
    QJsonArray files;
    for (const SidecarImage& i : p.images)
    {
        QJsonObject o;
        o.insert(QStringLiteral("role"), i.role);
        o.insert(QStringLiteral("ext"), i.ext);
        o.insert(QStringLiteral("size"), double(i.data.size()));
        files.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("kind"), QString::fromLatin1(kSidecarKindGamelist));
    root.insert(QStringLiteral("system"), p.system);
    root.insert(QStringLiteral("rom"), p.rom);
    root.insert(QStringLiteral("game"), game);
    root.insert(QStringLiteral("files"), files);
    const QByteArray header = QJsonDocument(root).toJson(QJsonDocument::Compact);

    QByteArray out;
    out.reserve(int(kV2PreambleBytes + header.size() + fileBytesOf(p)));
    out.append(kMagic, 8);
    appendU32(out, quint32(kPayloadFormatV2));
    appendU32(out, quint32(header.size()));
    out.append(header);
    for (const SidecarImage& i : p.images) out.append(i.data);
    return out;
}

BodyKind bodyKindV2(QIODevice& in)
{
    BodyKind kind = BodyKind::Unknown;
    if (in.isSequential()) return kind;
    const qint64 start = in.pos();
    QJsonObject root;
    Refusal why = Refusal::None;
    QString message;
    if (readV2HeaderObject(in, root, why, message))
    {
        const QJsonValue k = root.value(QStringLiteral("kind"));
        if (k.isUndefined() || k.toString() == QLatin1String("art")) kind = BodyKind::Art;
        else if (k.toString() == QLatin1String(kSidecarKindGamelist)) kind = BodyKind::Gamelist;
    }
    in.seek(start);
    return kind;
}

bool decodeSidecarHeaderV2(QIODevice& in, SidecarHeader& out, Refusal& why, QString& message)
{
    out = SidecarHeader();
    why = Refusal::None;
    message.clear();

    QJsonObject root;
    if (!readV2HeaderObject(in, root, why, message)) return false;
    if (root.value(QStringLiteral("kind")).toString() != QLatin1String(kSidecarKindGamelist))
        return refuse(Refusal::UnsupportedKind, QStringLiteral("that was not a gamelist entry"), why, message);

    // THE NAMES FIRST, before any path is built: each must be one safe segment, the way an item id must be a hash.
    const QJsonValue system = root.value(QStringLiteral("system"));
    const QJsonValue rom = root.value(QStringLiteral("rom"));
    if (!system.isString() || !safePathSegment(system.toString()) || !rom.isString() || !safePathSegment(rom.toString())
        || rom.toString().compare(QLatin1String(kGamelistFileName), Qt::CaseInsensitive) == 0)
        return refuse(Refusal::UnsafeId, QString::fromLatin1(kSidecarUnsafe), why, message);
    out.system = system.toString();
    out.rom = rom.toString();

    const QJsonValue game = root.value(QStringLiteral("game"));
    if (!game.isUndefined() && !game.isObject())
        return refuse(Refusal::Malformed, QString::fromLatin1(kSidecarUnreadable), why, message);
    const QJsonObject g = game.toObject();
    for (const FieldRef& f : kFields)
    {
        const QJsonValue v = g.value(QString::fromLatin1(f.tag));
        if (v.isUndefined()) continue;
        if (!v.isString()) return refuse(Refusal::Malformed, QString::fromLatin1(kSidecarUnreadable), why, message);
        out.fields.*(f.member) = v.toString();
    }

    // EVERY file entry, before the first file byte: an image role, an image extension, a sane size, once each.
    QSet<QString> roles;
    qint64 total = 0;
    const QJsonArray files = root.value(QStringLiteral("files")).toArray();
    for (const QJsonValue& v : files)
    {
        const QJsonObject o = v.toObject();
        SidecarFile f;
        f.role = o.value(QStringLiteral("role")).toString();
        f.ext = o.value(QStringLiteral("ext")).toString().toLower();
        if (roleSuffix(f.role).isEmpty() || !sidecarImageExtension(f.ext))
            return refuse(Refusal::UnsafeFileName, QString::fromLatin1(kSidecarFileRefused), why, message);
        const QJsonValue sizeValue = o.value(QStringLiteral("size"));
        if (!sizeValue.isDouble())
            return refuse(Refusal::Malformed, QString::fromLatin1(kSidecarUnreadable), why, message);
        const double size = sizeValue.toDouble();
        if (size > double(kMaxFileBytes))
            return refuse(Refusal::TooLarge, QStringLiteral("that gamelist entry carried an image too large to take"),
                          why, message);
        if (size < 0 || size != double(qint64(size)) || roles.contains(f.role))
            return refuse(Refusal::Malformed, QString::fromLatin1(kSidecarUnreadable), why, message);
        roles.insert(f.role);
        f.size = qint64(size);
        total += f.size;
        out.files << f;
    }
    if (total > kMaxSidecarFileBytes)
        return refuse(Refusal::TooLarge, QStringLiteral("that gamelist entry was too large to take"), why, message);
    if (in.size() - in.pos() != total)
        return refuse(Refusal::Malformed, QStringLiteral("that gamelist entry's length did not match what it declared"),
                      why, message);
    out.fileBytes = total;
    return true;
}

bool decodeSidecarV2(QIODevice& in, SidecarPayload& out, Refusal& why, QString& message)
{
    out = SidecarPayload();
    SidecarHeader h;
    if (!decodeSidecarHeaderV2(in, h, why, message)) return false;
    out.system = h.system;
    out.rom = h.rom;
    out.fields = h.fields;
    for (const SidecarFile& f : h.files)
    {
        SidecarImage img;
        img.role = f.role;
        img.ext = f.ext;
        img.data = in.read(f.size);
        if (qint64(img.data.size()) != f.size)
        {
            out = SidecarPayload();
            return refuse(Refusal::Malformed, QString::fromLatin1(kSidecarUnreadable), why, message);
        }
        out.images << img;
    }
    return true;
}

QString gamelistXmlText(const QString& value)
{
    // Characters XML 1.0 cannot hold at all (most control characters, U+FFFE/U+FFFF) are dropped: one of those
    // would make the WHOLE file unparseable, and every game already listed in it would vanish with it.
    QString clean;
    clean.reserve(value.size());
    const QList<uint> units = value.toUcs4();
    for (const uint u : units)
    {
        const bool ok = u == 0x9 || u == 0xA || u == 0xD || (u >= 0x20 && u <= 0xD7FF)
                     || (u >= 0xE000 && u <= 0xFFFD) || (u >= 0x10000 && u <= 0x10FFFF);
        if (!ok) continue;
        const char32_t cp = char32_t(u);
        clean += QString::fromUcs4(&cp, 1);
    }
    clean.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    clean.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    clean.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    return clean;
}

QByteArray gamelistEntryXml(const QString& romName, const GamelistFields& fields,
                            const QList<QPair<QString, QString>>& media)
{
    QString g = QStringLiteral("\t<game>\n");
    const auto tag = [&g](const QString& name, const QString& value) {
        if (!value.isEmpty())
            g += QStringLiteral("\t\t<") + name + QLatin1Char('>') + gamelistXmlText(value) + QStringLiteral("</")
               + name + QStringLiteral(">\n");
    };
    tag(QStringLiteral("path"), QStringLiteral("./") + romName);
    tag(QStringLiteral("name"), fields.name);
    tag(QStringLiteral("desc"), fields.desc);
    for (const auto& m : media)
        if (!roleSuffix(m.first).isEmpty()) tag(m.first, m.second);
    for (const FieldRef& f : kFields)
    {
        const QLatin1String t(f.tag);
        if (t == QLatin1String("name") || t == QLatin1String("desc")) continue;
        tag(QString(t), fields.*(f.member));
    }
    g += QStringLiteral("\t</game>\n");
    return g.toUtf8();
}

bool insertGamelistEntry(const QByteArray& existing, const QByteArray& block, QByteArray& out)
{
    out.clear();
    if (existing.trimmed().isEmpty())
    {
        out = QByteArray("<?xml version=\"1.0\"?>\n<gameList>\n") + block + QByteArray("</gameList>\n");
        return true;
    }
    // The existing bytes stay exactly as they are -- its declaration, its comments, its line endings, whatever
    // follows the root -- and the new block goes in just before the root's close, as GamelistWriter does.
    const int close = existing.lastIndexOf("</gameList>");
    if (close < 0) return false;
    out = existing.left(close) + block + existing.mid(close);
    return true;
}

LandResult landSidecarV2(const QString& romsRoot, QIODevice& in, QString& error)
{
    return landSidecarV2(romsRoot, in, error, LandOptions());
}

LandResult landSidecarV2(const QString& romsRoot, QIODevice& in, QString& error, const LandOptions& opts)
{
    error.clear();
    SidecarHeader h;
    Refusal why = Refusal::None;
    if (!decodeSidecarHeaderV2(in, h, why, error)) return LandResult::Refused;

    // THE TARGET DECIDES. Its own ROM root, the system folder only if it is already there (never created), and
    // only for a ROM that is a file in it.
    if (romsRoot.isEmpty())
    {
        error = QStringLiteral("this device has no ROM folder");
        return LandResult::NotApplicable;
    }
    const QString sysDir = romsRoot + QLatin1Char('/') + h.system;
    if (!QFileInfo(sysDir).isDir())
    {
        error = QStringLiteral("this device has no folder for that system");
        return LandResult::NotApplicable;
    }
    if (!QFileInfo(sysDir + QLatin1Char('/') + h.rom).isFile())
    {
        error = QStringLiteral("this device does not have that game");
        return LandResult::NotApplicable;
    }

    // WARM, NEVER FIGHT: a game the list already has -- by GamelistStore's own rule -- is left exactly as it is.
    const QString listPath = sysDir + QLatin1Char('/') + QLatin1String(kGamelistFileName);
    QByteArray existing;
    if (QFileInfo::exists(listPath))
    {
        QFile f(listPath);
        if (!f.open(QIODevice::ReadOnly))
        {
            error = QStringLiteral("this device could not read that system's gamelist");
            return LandResult::WriteFailed;
        }
        existing = f.readAll();
    }
    if (gamelistLists(parseGamelist(existing), h.rom)) return LandResult::AlreadyCurrent;
    {
        QByteArray check;
        if (!insertGamelistEntry(existing, QByteArray(), check))
        {
            error = QStringLiteral("that system's gamelist is not one this device can add to");
            return LandResult::WriteFailed;
        }
    }

    // THE IMAGES, named here. A name already taken is not overwritten -- the image is dropped from the entry --
    // and every file this landing creates is removed again if the landing does not finish.
    const QString imagesDir = sysDir + QStringLiteral("/images");
    QStringList created;
    bool madeImagesDir = false;
    const auto rollback = [&created, &madeImagesDir, &imagesDir] {
        for (const QString& p : created) QFile::remove(p);
        if (madeImagesDir) QDir().rmdir(imagesDir);
    };
    QList<QPair<QString, QString>> media;
    int written = 0;
    for (const SidecarFile& f : h.files)
    {
        if (opts.failAfterFiles >= 0 && written >= opts.failAfterFiles)
        {
            rollback();
            error = QStringLiteral("the transfer was interrupted");
            return LandResult::Interrupted;
        }
        const QString name = sidecarImageName(h.rom, f.role, f.ext);
        const QString path = imagesDir + QLatin1Char('/') + name;
        if (name.isEmpty() || QFileInfo::exists(path))
        {
            if (f.size > 0 && in.skip(f.size) != f.size)
            {
                rollback();
                error = QStringLiteral("that gamelist entry ended early");
                return LandResult::WriteFailed;
            }
            continue;
        }
        if (!QFileInfo(imagesDir).isDir())
        {
            if (QFileInfo::exists(imagesDir) || !QDir().mkdir(imagesDir))
            {
                rollback();
                error = QStringLiteral("this device could not make that system's images folder");
                return LandResult::WriteFailed;
            }
            madeImagesDir = true;
        }
        // The images folder must really be inside the system folder: a link that points elsewhere is not followed.
        const QString canonSys = QFileInfo(sysDir).canonicalFilePath();
        const QString canonImages = QFileInfo(imagesDir).canonicalFilePath();
        if (canonSys.isEmpty() || !canonImages.startsWith(canonSys + QLatin1Char('/')))
        {
            rollback();
            error = QStringLiteral("that system's images folder is not inside it");
            return LandResult::WriteFailed;
        }
        QFile outFile(path);
        if (!outFile.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        {
            rollback();
            error = QStringLiteral("this device could not write that game's image");
            return LandResult::WriteFailed;
        }
        created << path;
        qint64 left = f.size;
        while (left > 0)
        {
            const QByteArray chunk = in.read(qMin(left, kCopyChunk));
            if (chunk.isEmpty() || outFile.write(chunk) != qint64(chunk.size()))
            {
                outFile.close();
                rollback();
                error = QStringLiteral("this device could not write that game's image");
                return LandResult::WriteFailed;
            }
            left -= chunk.size();
        }
        outFile.close();
        media << qMakePair(f.role, QStringLiteral("./images/") + name);
        ++written;
    }

    // THE LIST, rewritten atomically: a temporary file beside it, renamed over it only once it is whole.
    QByteArray updated;
    insertGamelistEntry(existing, gamelistEntryXml(h.rom, h.fields, media), updated);
    QSaveFile save(listPath);
    if (!save.open(QIODevice::WriteOnly))
    {
        rollback();
        error = QStringLiteral("this device could not write that system's gamelist");
        return LandResult::WriteFailed;
    }
    if (opts.failAfterFiles >= 0 && written >= opts.failAfterFiles)
    {
        // The test seam: half the new list is in the temporary file when the write stops.
        save.write(updated.left(updated.size() / 2));
        save.cancelWriting();
        rollback();
        error = QStringLiteral("the transfer was interrupted");
        return LandResult::Interrupted;
    }
    if (save.write(updated) != qint64(updated.size()) || !save.commit())
    {
        rollback();
        error = QStringLiteral("this device could not write that system's gamelist");
        return LandResult::WriteFailed;
    }
    return LandResult::Landed;
}


} // namespace LibraryBundle
