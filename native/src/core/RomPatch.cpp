#include "RomPatch.h"
#include "AppPaths.h"
#include "Settings.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QObject>
#include <QCryptographicHash>
#include <cstring>

namespace {

// ---- CRC32 (zlib polynomial 0xEDB88320, the checksum BPS/UPS embed) ------------------------------------
// Small table-driven implementation so we do not pull in zlib just for this. Matches zlib::crc32 exactly.
quint32 crc32(const QByteArray& data)
{
    static quint32 table[256];
    static bool built = false;
    if (!built)
    {
        for (quint32 n = 0; n < 256; ++n)
        {
            quint32 c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        built = true;
    }
    quint32 c = 0xFFFFFFFFu;
    const auto* p = reinterpret_cast<const quint8*>(data.constData());
    const int n = data.size();
    for (int i = 0; i < n; ++i)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Read a little-endian u32 at `off`; false if the buffer is too short.
bool readU32LE(const QByteArray& b, int off, quint32& out)
{
    if (off < 0 || off + 4 > b.size()) return false;
    const auto* p = reinterpret_cast<const quint8*>(b.constData());
    out = quint32(p[off]) | (quint32(p[off + 1]) << 8) | (quint32(p[off + 2]) << 16) | (quint32(p[off + 3]) << 24);
    return true;
}

// byuu variable-width integer, shared by BPS and UPS. Advances `pos`. False on a truncated integer.
bool readVarint(const QByteArray& b, int& pos, quint64& out)
{
    quint64 value = 0, shift = 1;
    while (true)
    {
        if (pos >= b.size()) return false;
        const quint8 x = quint8(b.at(pos++));
        value += (quint64(x & 0x7F)) * shift;
        if (x & 0x80) break;
        shift <<= 7;
        value += shift;
    }
    out = value;
    return true;
}

bool magicIs(const QByteArray& b, const char* m, int len)
{
    return b.size() >= len && std::memcmp(b.constData(), m, len) == 0;
}

// ---- IPS -----------------------------------------------------------------------------------------------
// "PATCH" then records: 3-byte BE offset, 2-byte BE length. length==0 => RLE: 2-byte BE run length, 1 value
// byte. offset == "EOF" (0x454F46) ends the stream; an optional trailing 3-byte offset after EOF truncates
// the output. IPS carries no checksum, so there is nothing to verify against the source — the magic is all
// the identity it has.
bool applyIps(const QByteArray& source, const QByteArray& patch, QByteArray& out, QString* error)
{
    auto err = [&](const QString& m) { if (error) *error = m; return false; };
    out = source;                 // records overwrite/extend a copy of the source
    int p = 5;                    // past "PATCH"
    const auto* pd = reinterpret_cast<const quint8*>(patch.constData());
    const int n = patch.size();
    while (true)
    {
        if (p + 3 > n) return err(QStringLiteral("IPS patch ended without an EOF marker"));
        const quint32 off = (quint32(pd[p]) << 16) | (quint32(pd[p + 1]) << 8) | quint32(pd[p + 2]);
        p += 3;
        if (off == 0x454F46u) // "EOF"
        {
            // Optional truncation extension: exactly 3 more bytes = new length.
            if (p + 3 <= n)
            {
                const quint32 tlen = (quint32(pd[p]) << 16) | (quint32(pd[p + 1]) << 8) | quint32(pd[p + 2]);
                out.truncate(int(tlen));
            }
            return true;
        }
        if (p + 2 > n) return err(QStringLiteral("IPS patch truncated in a record header"));
        const quint32 len = (quint32(pd[p]) << 8) | quint32(pd[p + 1]);
        p += 2;
        quint32 writeEnd;
        if (len == 0) // RLE record
        {
            if (p + 3 > n) return err(QStringLiteral("IPS patch truncated in an RLE record"));
            const quint32 rlen = (quint32(pd[p]) << 8) | quint32(pd[p + 1]);
            const quint8 val = pd[p + 2];
            p += 3;
            writeEnd = off + rlen;
            if (writeEnd > quint32(out.size())) out.resize(int(writeEnd));
            for (quint32 i = 0; i < rlen; ++i) out[int(off + i)] = char(val);
        }
        else // literal data record
        {
            if (p + int(len) > n) return err(QStringLiteral("IPS patch truncated in a data record"));
            writeEnd = off + len;
            if (writeEnd > quint32(out.size())) out.resize(int(writeEnd));
            std::memcpy(out.data() + off, patch.constData() + p, len);
            p += int(len);
        }
    }
}

// ---- UPS -----------------------------------------------------------------------------------------------
// "UPS1", source-size (varint), target-size (varint), then XOR records until the last 12 bytes (three LE
// CRC32s: source, target, patch). Each record is a relative skip (varint) then a run of XOR bytes terminated
// by 0x00 — every non-zero byte is XORed into the output at the running position. The output is a copy of the
// source zero-extended/truncated to target-size, XORed in place.
bool applyUps(const QByteArray& source, const QByteArray& patch, QByteArray& out, QString* error)
{
    auto err = [&](const QString& m) { if (error) *error = m; return false; };
    const int n = patch.size();
    if (n < 4 + 12) return err(QStringLiteral("UPS patch too small"));

    int p = 4; // past "UPS1"
    quint64 srcSize = 0, tgtSize = 0;
    if (!readVarint(patch, p, srcSize) || !readVarint(patch, p, tgtSize))
        return err(QStringLiteral("UPS patch has a corrupt size header"));

    if (quint64(source.size()) != srcSize)
        return err(QStringLiteral("UPS patch expects a %1-byte ROM but this ROM is %2 bytes")
                       .arg(srcSize).arg(source.size()));

    // Verify the embedded source CRC32 before doing anything — refuse a patch built for a different ROM.
    quint32 wantSrc = 0, wantTgt = 0, wantPatch = 0;
    if (!readU32LE(patch, n - 12, wantSrc) || !readU32LE(patch, n - 8, wantTgt) || !readU32LE(patch, n - 4, wantPatch))
        return err(QStringLiteral("UPS patch is missing its checksum footer"));
    if (crc32(patch.left(n - 4)) != wantPatch)
        return err(QStringLiteral("UPS patch is corrupt (its own checksum does not match)"));
    if (crc32(source) != wantSrc)
        return err(QStringLiteral("UPS patch does not match this ROM (source checksum mismatch)"));

    out.resize(int(tgtSize));
    for (int i = 0; i < out.size(); ++i)
        out[i] = (i < source.size()) ? source.at(i) : char(0);

    const int bodyEnd = n - 12; // XOR records live before the 12-byte checksum footer
    quint64 pos = 0;
    while (p < bodyEnd)
    {
        quint64 skip = 0;
        if (!readVarint(patch, p, skip)) return err(QStringLiteral("UPS patch has a corrupt record offset"));
        pos += skip;
        while (true)
        {
            if (p >= bodyEnd) return err(QStringLiteral("UPS patch truncated in an XOR run"));
            const quint8 x = quint8(patch.at(p++));
            if (x == 0) break;
            if (pos >= quint64(out.size())) return err(QStringLiteral("UPS patch writes past the target size"));
            out[int(pos)] = char(quint8(out.at(int(pos))) ^ x);
            ++pos;
        }
        ++pos; // step over the position the terminating zero stood for
    }

    if (crc32(out) != wantTgt)
        return err(QStringLiteral("UPS patch produced the wrong bytes (target checksum mismatch)"));
    return true;
}

// ---- BPS -----------------------------------------------------------------------------------------------
// "BPS1", source-size, target-size, metadata-size + metadata, then actions until the last 12 bytes (three LE
// CRC32s). Each action is a varint: command = value & 3, length = (value >> 2) + 1.
//   0 SourceRead : copy `length` source bytes at the current output offset.
//   1 TargetRead : copy `length` bytes straight from the patch stream.
//   2 SourceCopy : a signed varint moves a source cursor, then copy `length` bytes from it.
//   3 TargetCopy : a signed varint moves a cursor within the output already written, then copy `length`.
bool applyBps(const QByteArray& source, const QByteArray& patch, QByteArray& out, QString* error)
{
    auto err = [&](const QString& m) { if (error) *error = m; return false; };
    const int n = patch.size();
    if (n < 4 + 12) return err(QStringLiteral("BPS patch too small"));

    int p = 4; // past "BPS1"
    quint64 srcSize = 0, tgtSize = 0, metaSize = 0;
    if (!readVarint(patch, p, srcSize) || !readVarint(patch, p, tgtSize) || !readVarint(patch, p, metaSize))
        return err(QStringLiteral("BPS patch has a corrupt header"));
    if (metaSize > quint64(n)) return err(QStringLiteral("BPS metadata length is impossible"));
    p += int(metaSize); // skip metadata block

    if (quint64(source.size()) != srcSize)
        return err(QStringLiteral("BPS patch expects a %1-byte ROM but this ROM is %2 bytes")
                       .arg(srcSize).arg(source.size()));

    quint32 wantSrc = 0, wantTgt = 0, wantPatch = 0;
    if (!readU32LE(patch, n - 12, wantSrc) || !readU32LE(patch, n - 8, wantTgt) || !readU32LE(patch, n - 4, wantPatch))
        return err(QStringLiteral("BPS patch is missing its checksum footer"));
    if (crc32(patch.left(n - 4)) != wantPatch)
        return err(QStringLiteral("BPS patch is corrupt (its own checksum does not match)"));
    if (crc32(source) != wantSrc)
        return err(QStringLiteral("BPS patch does not match this ROM (source checksum mismatch)"));

    out.resize(int(tgtSize));
    const int bodyEnd = n - 12;
    quint64 outPos = 0, srcRel = 0, tgtRel = 0;
    while (p < bodyEnd)
    {
        quint64 data = 0;
        if (!readVarint(patch, p, data)) return err(QStringLiteral("BPS patch has a corrupt action"));
        const int command = int(data & 3);
        const quint64 length = (data >> 2) + 1;
        if (outPos + length > tgtSize) return err(QStringLiteral("BPS action writes past the target size"));

        switch (command)
        {
        case 0: // SourceRead
            for (quint64 i = 0; i < length; ++i)
            {
                if (outPos >= quint64(source.size())) return err(QStringLiteral("BPS SourceRead reads past the ROM"));
                out[int(outPos)] = source.at(int(outPos));
                ++outPos;
            }
            break;
        case 1: // TargetRead
            for (quint64 i = 0; i < length; ++i)
            {
                if (p >= bodyEnd) return err(QStringLiteral("BPS TargetRead ran out of patch data"));
                out[int(outPos++)] = patch.at(p++);
            }
            break;
        case 2: // SourceCopy
        {
            quint64 mv = 0;
            if (!readVarint(patch, p, mv)) return err(QStringLiteral("BPS SourceCopy has a corrupt offset"));
            srcRel += (mv & 1) ? -(qint64(mv >> 1)) : qint64(mv >> 1);
            for (quint64 i = 0; i < length; ++i)
            {
                if (srcRel >= quint64(source.size())) return err(QStringLiteral("BPS SourceCopy reads past the ROM"));
                out[int(outPos++)] = source.at(int(srcRel++));
            }
            break;
        }
        case 3: // TargetCopy
        {
            quint64 mv = 0;
            if (!readVarint(patch, p, mv)) return err(QStringLiteral("BPS TargetCopy has a corrupt offset"));
            tgtRel += (mv & 1) ? -(qint64(mv >> 1)) : qint64(mv >> 1);
            for (quint64 i = 0; i < length; ++i)
            {
                if (tgtRel >= outPos) return err(QStringLiteral("BPS TargetCopy reads output not yet written"));
                out[int(outPos++)] = out.at(int(tgtRel++));
            }
            break;
        }
        }
    }

    if (outPos != tgtSize) return err(QStringLiteral("BPS patch did not fill the target"));
    if (crc32(out) != wantTgt)
        return err(QStringLiteral("BPS patch produced the wrong bytes (target checksum mismatch)"));
    return true;
}

} // namespace

namespace RomPatch {

bool isPatchExtension(const QString& suffixLower)
{
    return suffixLower == QStringLiteral("ips") || suffixLower == QStringLiteral("bps")
        || suffixLower == QStringLiteral("ups");
}

Format detectFormat(const QByteArray& patch)
{
    if (magicIs(patch, "PATCH", 5)) return Format::Ips;
    if (magicIs(patch, "UPS1", 4))  return Format::Ups;
    if (magicIs(patch, "BPS1", 4))  return Format::Bps;
    return Format::None;
}

bool apply(const QByteArray& source, const QByteArray& patch, QByteArray& out, QString* error)
{
    switch (detectFormat(patch))
    {
    case Format::Ips: return applyIps(source, patch, out, error);
    case Format::Ups: return applyUps(source, patch, out, error);
    case Format::Bps: return applyBps(source, patch, out, error);
    case Format::None:
        if (error) *error = QStringLiteral("not a recognised ROM patch (no IPS/UPS/BPS magic)");
        return false;
    }
    return false;
}

QString cacheDir()
{
    const QString d = AppPaths::dataDir() + QStringLiteral("/patched-roms");
    QDir().mkpath(d);
    return d;
}

// Find a sidecar patch beside `romPath`: same directory + base name, a recognised patch extension. Returns
// an empty string if there is none. (A ROM named "Game.sfc" pairs with "Game.ips"/"Game.bps"/"Game.ups".)
static QString sidecarPatchFor(const QString& romPath)
{
    const QFileInfo fi(romPath);
    const QString base = fi.completeBaseName();
    const QDir dir = fi.absoluteDir();
    for (const QString& ext : { QStringLiteral("ips"), QStringLiteral("bps"), QStringLiteral("ups") })
    {
        const QString cand = dir.absoluteFilePath(base + QLatin1Char('.') + ext);
        if (QFileInfo::exists(cand)) return cand;
    }
    return QString();
}

QString resolvePatchedRom(const QString& romPath, QString* error)
{
    if (!Settings::autoApplyRomPatches()) return romPath;

    const QString patchPath = sidecarPatchFor(romPath);
    if (patchPath.isEmpty()) return romPath; // no sidecar — launch the ROM as-is

    QFile rf(romPath);
    if (!rf.open(QIODevice::ReadOnly))
    {
        if (error) *error = QStringLiteral("could not read the ROM to patch it");
        return QString();
    }
    const QByteArray source = rf.readAll();
    rf.close();

    QFile pf(patchPath);
    if (!pf.open(QIODevice::ReadOnly))
    {
        if (error) *error = QStringLiteral("could not read the patch file");
        return QString();
    }
    const QByteArray patch = pf.readAll();
    pf.close();

    // Content-addressed cache key: a hash of the ROM bytes + the patch bytes. Same ROM + same patch always
    // maps to the same file, so a relaunch reuses a valid cached result instead of re-patching, and two
    // different patches for one ROM never collide. Keeping the ROM's extension on the cache file matters —
    // the emulator resolves the system from it downstream.
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(source);
    h.addData(patch);
    const QString key = QString::fromLatin1(h.result().toHex().left(16));
    const QString ext = QFileInfo(romPath).suffix();
    const QString outPath = cacheDir() + QLatin1Char('/') + key
                          + (ext.isEmpty() ? QString() : (QLatin1Char('.') + ext));

    // Already patched and intact? Reuse it. (Size check is a cheap guard against a half-written file from a
    // killed previous run; the key already pins the content.)
    if (QFileInfo::exists(outPath) && QFileInfo(outPath).size() > 0)
        return outPath;

    QByteArray out;
    QString aerr;
    if (!apply(source, patch, out, &aerr))
    {
        // A patch is present but bad — fail loudly. The caller must NOT launch the unpatched ROM as if all
        // were well: the user put that patch there on purpose.
        if (error) *error = QObject::tr("Couldn't apply the patch %1: %2")
                                .arg(QFileInfo(patchPath).fileName(), aerr);
        return QString();
    }

    // Write atomically via a temp sibling so a crash mid-write never leaves a half-patched file under the key.
    const QString tmp = outPath + QStringLiteral(".part");
    QFile of(tmp);
    if (!of.open(QIODevice::WriteOnly | QIODevice::Truncate) || of.write(out) != out.size())
    {
        if (error) *error = QStringLiteral("could not write the patched ROM to the cache");
        of.close();
        QFile::remove(tmp);
        return QString();
    }
    of.close();
    QFile::remove(outPath);           // in case a zero-byte stub slipped through the reuse check
    if (!QFile::rename(tmp, outPath))
    {
        QFile::remove(tmp);
        if (error) *error = QStringLiteral("could not finalise the patched ROM in the cache");
        return QString();
    }
    return outPath;
}

bool writePatched(const QString& romPath, const QByteArray& patch, const QString& outPath, QString* error)
{
    QFile rf(romPath);
    if (!rf.open(QIODevice::ReadOnly))
    {
        if (error) *error = QObject::tr("Couldn't read the ROM to patch it.");
        return false;
    }
    const QByteArray source = rf.readAll();
    rf.close();

    // apply() makes every refusal: an unrecognised magic, a malformed patch, and — for BPS/UPS — a source
    // checksum built for a different dump. Nothing is written when it says no, so a refused install cannot
    // leave a half-made ROM sitting in the library looking playable.
    QByteArray out;
    QString aerr;
    if (!apply(source, patch, out, &aerr))
    {
        if (error) *error = aerr;
        return false;
    }

    QDir().mkpath(QFileInfo(outPath).absolutePath());

    // Atomic via a temp sibling, same discipline as the cache path above: a crash mid-write leaves the .part
    // behind, never a truncated ROM under the real name.
    const QString tmp = outPath + QStringLiteral(".part");
    QFile of(tmp);
    if (!of.open(QIODevice::WriteOnly | QIODevice::Truncate) || of.write(out) != out.size())
    {
        of.close();
        QFile::remove(tmp);
        if (error) *error = QObject::tr("Couldn't write the patched game.");
        return false;
    }
    of.close();
    QFile::remove(outPath);           // replacing an earlier install of the same hack
    if (!QFile::rename(tmp, outPath))
    {
        QFile::remove(tmp);
        if (error) *error = QObject::tr("Couldn't finalise the patched game.");
        return false;
    }
    return true;
}

} // namespace RomPatch
