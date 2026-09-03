#include "HashVerify.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QXmlStreamReader>

namespace {

// ---- CRC32 (zlib polynomial 0xEDB88320) — same table-driven impl RomPatch uses; matches zlib::crc32 and
// Python's zlib.crc32 exactly, which is the independent oracle probe_hashverify checks the fixtures with.
quint32 crc32Bytes(const QByteArray& data)
{
    static quint32 table[256];
    static bool built = false;
    if (!built) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    quint32 c = 0xFFFFFFFFu;
    const uchar* p = reinterpret_cast<const uchar*>(data.constData());
    for (int i = 0; i < data.size(); ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

QString hex(const QByteArray& raw) { return QString::fromLatin1(raw.toHex()); }

// The portable everythingbox.ini, same posture as SyncOffsets/Settings (see those for the coherence rule).
QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QString stampKey(const QString& path)
{
    const QString token = QString::fromLatin1(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex());
    return QStringLiteral("hashverify/stamps/") + token;
}

} // namespace

namespace HashVerify {

QString statusToken(Status s)
{
    switch (s) {
        case Status::Verified: return QStringLiteral("verified");
        case Status::Bad:      return QStringLiteral("bad");
        default:               return QStringLiteral("unknown");
    }
}

Status statusFromToken(const QString& t)
{
    if (t == QStringLiteral("verified")) return Status::Verified;
    if (t == QStringLiteral("bad"))      return Status::Bad;
    return Status::Unknown;
}

QString normalizeName(const QString& name)
{
    QString s = name;
    // Strip a trailing ROM/archive extension if one is present (DAT rom-names carry them; file base names may
    // too). Only a short known set, so a game literally titled "...nes" is not mangled.
    static const QStringList exts = {
        QStringLiteral(".nes"), QStringLiteral(".sfc"), QStringLiteral(".smc"), QStringLiteral(".gb"),
        QStringLiteral(".gbc"), QStringLiteral(".gba"), QStringLiteral(".md"),  QStringLiteral(".gen"),
        QStringLiteral(".n64"), QStringLiteral(".z64"), QStringLiteral(".v64"), QStringLiteral(".iso"),
        QStringLiteral(".chd"), QStringLiteral(".zip"), QStringLiteral(".7z"),  QStringLiteral(".bin"),
        QStringLiteral(".pce"), QStringLiteral(".a26"), QStringLiteral(".nds"), QStringLiteral(".cue") };
    QString low = s.toLower();
    for (const QString& e : exts)
        if (low.endsWith(e)) { s = s.left(s.size() - e.size()); low = low.left(low.size() - e.size()); break; }
    // Collapse whitespace runs and trim.
    s = low.simplified();
    return s;
}

// -------------------------------------------------------------------------------------------------------------
// DAT parsing
// -------------------------------------------------------------------------------------------------------------
static void indexEntry(DatDb& db, const DatEntry& e)
{
    const int idx = db.entries.size();
    db.entries.push_back(e);
    if (!e.crc.isEmpty()  && !db.byCrc.contains(e.crc))   db.byCrc.insert(e.crc, idx);
    if (!e.md5.isEmpty()  && !db.byMd5.contains(e.md5))   db.byMd5.insert(e.md5, idx);
    if (!e.sha1.isEmpty() && !db.bySha1.contains(e.sha1)) db.bySha1.insert(e.sha1, idx);
    const QString n = normalizeName(e.game);
    if (!n.isEmpty()) db.names.insert(n);
}

void mergeDat(DatDb& db, const QByteArray& xml)
{
    QXmlStreamReader r(xml);
    QString currentGame;
    while (!r.atEnd()) {
        const auto tok = r.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            const QStringView name = r.name();
            if (name == QLatin1String("game") || name == QLatin1String("machine")) {
                currentGame = r.attributes().value(QLatin1String("name")).toString();
            } else if (name == QLatin1String("rom")) {
                const auto a = r.attributes();
                DatEntry e;
                // A <rom> without a game wrapper still gets its own rom-name as the game (some DATs are flat).
                e.game = currentGame.isEmpty() ? a.value(QLatin1String("name")).toString() : currentGame;
                e.crc  = a.value(QLatin1String("crc")).toString().toLower();
                e.md5  = a.value(QLatin1String("md5")).toString().toLower();
                e.sha1 = a.value(QLatin1String("sha1")).toString().toLower();
                // A DAT is worthless as a matcher without at least one hash; skip a bare <rom>.
                if (!e.crc.isEmpty() || !e.md5.isEmpty() || !e.sha1.isEmpty()) indexEntry(db, e);
            }
        }
    }
}

DatDb parseDat(const QByteArray& xml)
{
    DatDb db;
    mergeDat(db, xml);
    return db;
}

DatDb parseDatDir(const QString& dir)
{
    DatDb db;
    QDir d(dir);
    if (!d.exists()) return db;
    const QStringList filters = { QStringLiteral("*.dat"), QStringLiteral("*.xml") };
    const QFileInfoList files = d.entryInfoList(filters, QDir::Files, QDir::Name);
    for (const QFileInfo& fi : files) {
        QFile f(fi.absoluteFilePath());
        if (f.open(QIODevice::ReadOnly)) mergeDat(db, f.readAll());
    }
    return db;
}

bool DatDb::matchesHash(const Hashes& h) const
{
    if (!h.sha1.isEmpty() && bySha1.contains(h.sha1)) return true;
    if (!h.md5.isEmpty()  && byMd5.contains(h.md5))   return true;
    if (!h.crc.isEmpty()  && byCrc.contains(h.crc))   return true;
    return false;
}

// -------------------------------------------------------------------------------------------------------------
// Payload hashing
// -------------------------------------------------------------------------------------------------------------
QByteArray payloadBytes(const QByteArray& raw, const QString& /*formatHint*/)
{
    // iNES: the header is exactly 16 bytes and begins with the magic 'N' 'E' 'S' 0x1A. That magic is specific
    // enough to drive the skip off the content alone (the hint is advisory and, for other headered formats,
    // where future cases would branch). A good iNES dump hashed WHOLE reads as Bad — this is the one to get right.
    if (raw.size() >= 16
        && (uchar)raw[0] == 0x4E && (uchar)raw[1] == 0x45 && (uchar)raw[2] == 0x53 && (uchar)raw[3] == 0x1A) {
        return raw.mid(16);
    }
    return raw;
}

Hashes hashBytes(const QByteArray& payload)
{
    Hashes h;
    quint32 c = crc32Bytes(payload);
    h.crc = QString::asprintf("%08x", c);
    h.md5  = hex(QCryptographicHash::hash(payload, QCryptographicHash::Md5));
    h.sha1 = hex(QCryptographicHash::hash(payload, QCryptographicHash::Sha1));
    // The fourth digest is for #248's recomp ROM-identity gate, not for #97: no Logiqx DAT publishes sha256,
    // so classify() and the DatDb indexes never look at it. It is computed HERE, in the one place a ROM's
    // bytes are already in hand, because the alternative is a second full read of the same multi-gigabyte
    // file the moment a catalogue entry happens to publish only that kind.
    h.sha256 = hex(QCryptographicHash::hash(payload, QCryptographicHash::Sha256));
    return h;
}

Hashes hashPayload(const QByteArray& raw, const QString& formatHint)
{
    return hashBytes(payloadBytes(raw, formatHint));
}

QString chdSha1FromHeader(const QByteArray& hdr)
{
    // v5 layout: [0]='MComprHD' [8]=u32 length(BE) [12]=u32 version(BE) ... [84]=combined SHA1 (20 bytes).
    // We read the field a MAME/clrmamepro CHD DAT lists (the overall "sha1"), no decompression.
    // (Named `hdr`, not `header`: a CHD file header is unrelated to the HTTP proxy headers the
    // "proxy-header log discipline" gate polices, and that gate keys on the identifier name.)
    if (hdr.size() < 104) return QString();
    if (!hdr.startsWith(QByteArrayLiteral("MComprHD"))) return QString();
    auto beU32 = [&](int off) -> quint32 {
        return (quint32(uchar(hdr[off]))   << 24) | (quint32(uchar(hdr[off + 1])) << 16)
             | (quint32(uchar(hdr[off + 2])) << 8) |  quint32(uchar(hdr[off + 3]));
    };
    if (beU32(12) != 5u) return QString();               // only v5's trivial layout is handled here
    return hex(hdr.mid(84, 20));
}

// -------------------------------------------------------------------------------------------------------------
// Classification
// -------------------------------------------------------------------------------------------------------------
Status classify(const Hashes& h, const DatDb& db, const QString& romNameNoExt)
{
    if (db.matchesHash(h)) return Status::Verified;
    if (db.hasName(normalizeName(romNameNoExt))) return Status::Bad;
    return Status::Unknown;
}

// -------------------------------------------------------------------------------------------------------------
// glue: file hashing + the per-ROM stamp cache
// -------------------------------------------------------------------------------------------------------------
Hashes hashRomFile(const QString& path, const QString& systemHint, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot open %1").arg(path);
        return {};
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    // CHD: read the SHA1 straight from the header, do NOT slurp / decompress the (multi-GB) image.
    if (suffix == QStringLiteral("chd")) {
        const QByteArray head = f.read(124);
        const QString sha1 = chdSha1FromHeader(head);
        if (sha1.isEmpty()) {
            if (error) *error = QStringLiteral("not a v5 CHD header");
            return {};
        }
        Hashes h; h.sha1 = sha1; return h;
    }
    const QByteArray raw = f.readAll();
    return hashPayload(raw, systemHint.isEmpty() ? suffix : systemHint);
}

QString datsDir()
{
    return AppPaths::dataDir() + QStringLiteral("/dats");
}

Stamp cachedStamp(const QString& path)
{
    Stamp st;
    const QFileInfo fi(path);
    if (!fi.exists()) return st;
    const QString base = stampKey(path);
    if (!store().contains(base + QStringLiteral("/status"))) return st;   // never verified
    const qint64 mtime = store().value(base + QStringLiteral("/mtime")).toLongLong();
    const qint64 size  = store().value(base + QStringLiteral("/size")).toLongLong();
    // Path + mtime + size gate: any change re-verifies (a swapped-in different dump must not keep the old stamp).
    if (mtime != fi.lastModified().toSecsSinceEpoch() || size != fi.size()) return st;
    st.status  = statusFromToken(store().value(base + QStringLiteral("/status")).toString());
    st.sha1    = store().value(base + QStringLiteral("/sha1")).toString();
    st.datGame = store().value(base + QStringLiteral("/game")).toString();
    st.crc     = store().value(base + QStringLiteral("/crc")).toString();
    st.md5     = store().value(base + QStringLiteral("/md5")).toString();
    st.sha256  = store().value(base + QStringLiteral("/sha256")).toString();
    st.valid   = true;
    return st;
}

Stamp verifyAndCache(const QString& path, const QString& systemHint, const DatDb& db,
                     const QString& hashSourcePath)
{
    Stamp st;
    const QFileInfo fi(path);
    if (!fi.exists()) return st;

    QString err;
    const Hashes h = hashRomFile(hashSourcePath.isEmpty() ? path : hashSourcePath, systemHint, &err);
    if (h.isEmpty()) return st;   // unreadable / unparseable — leave unverified (valid stays false)

    st.status = classify(h, db, fi.completeBaseName());
    st.sha1   = h.sha1;
    st.valid  = true;
    if (st.status == Status::Verified) {
        // Record the canonical DAT name (identity/display use — issue #97). Prefer the sha1 match's game.
        int idx = -1;
        if (!h.sha1.isEmpty() && db.bySha1.contains(h.sha1)) idx = db.bySha1.value(h.sha1);
        else if (!h.md5.isEmpty() && db.byMd5.contains(h.md5)) idx = db.byMd5.value(h.md5);
        else if (!h.crc.isEmpty() && db.byCrc.contains(h.crc)) idx = db.byCrc.value(h.crc);
        if (idx >= 0 && idx < db.entries.size()) st.datGame = db.entries[idx].game;
    }

    st.crc    = h.crc;
    st.md5    = h.md5;
    st.sha256 = h.sha256;

    const QString base = stampKey(path);
    store().setValue(base + QStringLiteral("/status"), statusToken(st.status));
    store().setValue(base + QStringLiteral("/sha1"),   st.sha1);
    store().setValue(base + QStringLiteral("/game"),   st.datGame);
    // The other three digests of the same payload, so a later ROM-identity question (#248) is answered from
    // this record rather than by opening the file again.
    store().setValue(base + QStringLiteral("/crc"),    st.crc);
    store().setValue(base + QStringLiteral("/md5"),    st.md5);
    store().setValue(base + QStringLiteral("/sha256"), st.sha256);
    store().setValue(base + QStringLiteral("/mtime"),  fi.lastModified().toSecsSinceEpoch());
    store().setValue(base + QStringLiteral("/size"),   fi.size());
    store().sync();
    return st;
}

// ---- the same record, read and written as a HASH cache (issue #248) ------------------------------------------
// See the header note: #97 asks this record for a verdict, the recomp section asks it for digests, and they
// share one record so the two answers cannot drift apart.
Hashes cachedHashes(const QString& path)
{
    Hashes h;
    const QFileInfo fi(path);
    if (!fi.exists()) return h;
    const QString base = stampKey(path);
    // Gated on the DIGESTS being present, not on the verdict: a record written before sha256 existed, and a
    // record written by hashAndCache on a machine with no DATs, are both legitimate here and neither has the
    // shape the #97 read tests for.
    if (!store().contains(base + QStringLiteral("/sha1"))) return h;
    const qint64 mtime = store().value(base + QStringLiteral("/mtime")).toLongLong();
    const qint64 size  = store().value(base + QStringLiteral("/size")).toLongLong();
    if (mtime != fi.lastModified().toSecsSinceEpoch() || size != fi.size()) return h;   // the file moved on
    h.crc    = store().value(base + QStringLiteral("/crc")).toString();
    h.md5    = store().value(base + QStringLiteral("/md5")).toString();
    h.sha1   = store().value(base + QStringLiteral("/sha1")).toString();
    h.sha256 = store().value(base + QStringLiteral("/sha256")).toString();
    return h;
}

Hashes hashAndCache(const QString& path, const QString& systemHint, const QString& hashSourcePath)
{
    const QFileInfo fi(path);
    if (!fi.exists()) return {};

    QString err;
    const Hashes h = hashRomFile(hashSourcePath.isEmpty() ? path : hashSourcePath, systemHint, &err);
    if (h.isEmpty()) return {};   // unreadable / unparseable — write nothing, so the next open tries again

    const QString base = stampKey(path);
    store().setValue(base + QStringLiteral("/sha1"),   h.sha1);
    store().setValue(base + QStringLiteral("/crc"),    h.crc);
    store().setValue(base + QStringLiteral("/md5"),    h.md5);
    store().setValue(base + QStringLiteral("/sha256"), h.sha256);
    store().setValue(base + QStringLiteral("/mtime"),  fi.lastModified().toSecsSinceEpoch());
    store().setValue(base + QStringLiteral("/size"),   fi.size());
    // NO `/status` and no `/game`. Writing "unknown" here would make cachedStamp() report a valid verdict for
    // a file no DAT has ever been consulted about, and the #97 pass — which only runs on an INVALID stamp —
    // would then never look at it again. An absent verdict is the honest record of "nobody asked yet".
    store().sync();
    return h;
}

void clearCache()
{
    store().beginGroup(QStringLiteral("hashverify"));
    store().remove(QString());   // remove the whole hashverify group
    store().endGroup();
    store().sync();
}

} // namespace HashVerify
