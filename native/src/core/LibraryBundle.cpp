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
#include <QSet>
#include <algorithm>

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
    if (total > kMaxItemBytes)
    {
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

    recoverItem(root, p.id);

    // Warm, never fight. An identical stamp is already the answer; a LOCAL copy that is newer is kept, and
    // the source is told so rather than being let believe it overwrote something.
    {
        Entry cur;
        QList<FileEntry> curFiles;
        if (scanItem(root, p.id, cur, curFiles))
        {
            if (cur.stamp == p.stamp) return LandResult::AlreadyCurrent;
            if (cur.updatedMs > p.updatedMs) return LandResult::KeptNewer;
        }
    }

    const QString staging = incomingRoot(root) + QLatin1Char('/') + p.id;
    QDir(staging).removeRecursively();
    if (!QDir().mkpath(staging))
    {
        error = QStringLiteral("this device could not open its cache for writing");
        return LandResult::WriteFailed;
    }

    int written = 0;
    QSet<QString> staged;
    for (const PayloadFile& f : p.files)
    {
        if (opts.failAfterFiles >= 0 && written >= opts.failAfterFiles)
        {
            // Interrupted mid-item. The live folder has not been touched yet, so the target still holds
            // exactly what it held before; the staged remains are swept on the next run.
            error = QStringLiteral("the transfer was interrupted");
            return LandResult::Interrupted;
        }
        if (!writeFileWithTime(staging + QLatin1Char('/') + f.name, f.data, f.mtimeMs, error))
        {
            QDir(staging).removeRecursively();
            return LandResult::WriteFailed;
        }
        staged.insert(f.name);
        ++written;
    }

    const QString live    = itemDir(root, p.id);
    const QString retired = retiredRoot(root) + QLatin1Char('/') + p.id;
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
    return LandResult::Landed;
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
        retired.rmdir(QStringLiteral("."));
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
        incoming.rmdir(QStringLiteral("."));
    }
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

QString describeProgress(const Progress& p, const QString& deviceName)
{
    const QString who = deviceName.isEmpty() ? QStringLiteral("that device") : deviceName;
    if (p.itemsTotal == 0)
    {
        // The second run, and the sentence that says the feature worked: nothing left this machine.
        return who + QStringLiteral(" is already up to date — nothing to send.");
    }
    const double mb = double(p.bytesSent) / (1024.0 * 1024.0);
    QString s = QStringLiteral("Sent %1 of %2 items (%3 MB) to %4.")
                    .arg(p.itemsSent).arg(p.itemsTotal)
                    .arg(QString::number(mb, 'f', 1)).arg(who);
    if (p.unchanged > 0) s += QStringLiteral(" %1 were already there.").arg(p.unchanged);
    if (p.keptNewer > 0) s += QStringLiteral(" %1 were newer there and were left alone.").arg(p.keptNewer);
    if (p.failed > 0)    s += QStringLiteral(" %1 could not be sent.").arg(p.failed);
    return s;
}

} // namespace LibraryBundle
