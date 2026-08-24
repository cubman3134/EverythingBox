#include "RomPatch.h"
#include "AppPaths.h"
#include "Settings.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QObject>
#include <QCryptographicHash>
#include <cstring>
#include <limits>

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

// ---- xdelta3 / VCDIFF (RFC 3284) ----------------------------------------------------------------------
// The header's hdr_indicator bits. Every real xdelta3 patch sampled sets 0x04 and nothing else; the other two
// name optional machinery we do not implement, and a patch that asks for either is refused by name rather than
// half-parsed into plausible-looking garbage.
constexpr quint8 kVcdDecompress = 0x01;   // a secondary compressor (DJW / LZMA) — not implemented
constexpr quint8 kVcdCodetable  = 0x02;   // a custom instruction code table    — not implemented
constexpr quint8 kVcdAppHeader  = 0x04;   // an application header — present on every real patch; skipped

// A window's win_indicator bits: where the window's COPY source segment comes from.
constexpr quint8 kVcdSource = 0x01;       // a slice of the SOURCE file
constexpr quint8 kVcdTarget = 0x02;       // a slice of the TARGET decoded so far

// VCD_ADLER32 is an xdelta3 EXTENSION, not part of RFC 3284 — but every window of every real xdelta3 patch
// sampled sets it, so a decoder written from the RFC alone reads every one of them wrong. It adds four bytes,
// a big-endian adler32 of this window's target output, AFTER the three section lengths and BEFORE the three
// sections.
//
// It is also the only integrity guarantee VCDIFF gives us. Unlike BPS and UPS, the format embeds no source
// checksum, so a patch built for a different dump applies happily to whatever ROM is long enough and hands
// back a corrupt game. VCD_ADLER32 recovers the property that actually protects the user: not identifying the
// source up front, but catching a wrong RESULT before it is handed over. So we verify it rather than skip it.
constexpr quint8 kVcdAdler32 = 0x04;

// adler32 (RFC 1950): a = 1 + sum(bytes) mod 65521, b = sum(a) mod 65521, checksum = (b << 16) | a.
quint32 adler32(const char* data, int len)
{
    constexpr quint32 kMod = 65521;
    quint32 a = 1, b = 0;
    for (int i = 0; i < len; ++i)
    {
        a = (a + quint8(data[i])) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

// A VCDIFF integer (RFC 3284 §2): base-128, BIG-endian, most-significant group first, with the high bit SET on
// every byte except the last. Reads no further than `limit`.
//
// This is NOT readVarint() above. That one is byuu's format for BPS/UPS: little-endian-first, continuation
// signalled by the bit being CLEAR, and with a running `+= shift` bias. The two are unrelated, and feeding a
// VCDIFF stream to the byuu reader yields plausible-looking garbage rather than an error — which is exactly
// why they are two functions with two names and not one "readInt" anybody could reach for by accident.
bool readVcdInt(const QByteArray& b, int& pos, int limit, quint64& out)
{
    quint64 value = 0;
    for (int i = 0; i < 10; ++i)          // 10 * 7 bits > 64; refuse anything longer
    {
        if (pos >= limit) return false;
        const quint8 x = quint8(b.at(pos++));
        if (value > (std::numeric_limits<quint64>::max() >> 7)) return false;   // would overflow
        value = (value << 7) | (x & 0x7F);
        if ((x & 0x80) == 0) { out = value; return true; }
    }
    return false;
}

enum : quint8 { kInstNoop = 0, kInstAdd = 1, kInstRun = 2, kInstCopy = 3 };

// One half of a code-table entry. `size == 0` means the real size follows as a VCDIFF integer in the
// instruction stream; `mode` is meaningful only for COPY.
struct VcdInst { quint8 type; quint8 size; quint8 mode; };
struct VcdCode { VcdInst first; VcdInst second; };

// How many entries defaultCodeTable() actually filled. RFC 3284 §5.4 fixes it at 256; it is checked at
// runtime (not with Q_ASSERT, which vanishes in Release) both by applyXdelta and by the probe.
int gVcdCodeTableEntries = 0;

// RFC 3284 §5.4's default code table, GENERATED rather than transcribed — a 256-row literal is exactly the
// kind of thing that acquires a typo nobody finds, and a single wrong row silently mis-decodes real patches.
const VcdCode* defaultCodeTable()
{
    static VcdCode t[256] {};
    static bool built = false;
    if (built) return t;

    int i = 0;
    auto put = [&](quint8 t1, quint8 s1, quint8 m1, quint8 t2 = kInstNoop, quint8 s2 = 0, quint8 m2 = 0) {
        if (i < 256)
        {
            t[i].first  = { t1, s1, m1 };
            t[i].second = { t2, s2, m2 };
        }
        ++i;
    };

    put(kInstRun, 0, 0);                                         // 0
    for (quint8 s = 0; s <= 17; ++s) put(kInstAdd, s, 0);        // 1..18  (s==0 => size follows)
    for (quint8 mode = 0; mode <= 8; ++mode)                     // 19..162
    {
        put(kInstCopy, 0, mode);
        for (quint8 s = 4; s <= 18; ++s) put(kInstCopy, s, mode);
    }
    for (quint8 mode = 0; mode <= 5; ++mode)                     // 163..234
        for (quint8 addSize = 1; addSize <= 4; ++addSize)
            for (quint8 copySize = 4; copySize <= 6; ++copySize)
                put(kInstAdd, addSize, 0, kInstCopy, copySize, mode);
    for (quint8 mode = 6; mode <= 8; ++mode)                     // 235..246
        for (quint8 addSize = 1; addSize <= 4; ++addSize)
            put(kInstAdd, addSize, 0, kInstCopy, 4, mode);
    for (quint8 mode = 0; mode <= 8; ++mode)                     // 247..255
        put(kInstCopy, 4, mode, kInstAdd, 1, 0);

    gVcdCodeTableEntries = i;
    built = true;
    return t;
}

// RFC 3284 §5.3. COPY addresses are coded RELATIVE to a cache, and getting this wrong is the worst failure
// mode in the whole format: it produces addresses that are in range but wrong, i.e. output that looks like a
// plausible ROM rather than an error. near_[] is a round-robin of recent addresses; same_[] is a 256-way
// direct map keyed on the low bits of the address. The sizes are fixed by the default code table's mode count
// (2 + 4 near + 3 same = 9 COPY modes), and the whole cache is reset at the start of every window.
struct VcdAddrCache
{
    static constexpr int kNear = 4;
    static constexpr int kSame = 3;
    quint64 near_[kNear] {};
    int nextNear = 0;
    quint64 same_[kSame * 256] {};

    void update(quint64 addr)
    {
        near_[nextNear] = addr;
        nextNear = (nextNear + 1) % kNear;
        same_[addr % (kSame * 256)] = addr;
    }

    // Decode one address for `mode`, reading from the window's addresses section [pos, limit). `here` is the
    // current position in the window's combined address space (source segment then target-so-far).
    bool decode(const QByteArray& b, int& pos, int limit, quint64 here, quint8 mode, quint64& out)
    {
        quint64 v = 0;
        if (mode == 0)                                  // VCD_SELF: an absolute address
        {
            if (!readVcdInt(b, pos, limit, v)) return false;
            out = v;
        }
        else if (mode == 1)                             // VCD_HERE: back from the current position
        {
            if (!readVcdInt(b, pos, limit, v)) return false;
            if (v > here) return false;
            out = here - v;
        }
        else if (mode < 2 + kNear)                      // near cache: an offset from a recent address
        {
            if (!readVcdInt(b, pos, limit, v)) return false;
            if (v > std::numeric_limits<quint64>::max() - near_[mode - 2]) return false;
            out = near_[mode - 2] + v;
        }
        else if (mode < 2 + kNear + kSame)              // same cache: a single byte indexes it
        {
            if (pos >= limit) return false;
            const quint8 x = quint8(b.at(pos++));
            out = same_[(mode - (2 + kNear)) * 256 + x];
        }
        else
        {
            return false;                               // no such mode in the default code table
        }
        update(out);
        return true;
    }
};

// Apply a VCDIFF (xdelta3) patch. Header, then a sequence of windows; each window declares where its COPY
// source segment comes from and carries three sections (data for ADD/RUN, instructions, COPY addresses).
// Every read is bounds-checked against its own section, and a window that does not produce exactly its
// declared target length is a refusal — the alternative is a half-decoded file that still looks like a ROM.
bool applyXdelta(const QByteArray& source, const QByteArray& patch, QByteArray& out, QString* error)
{
    auto err = [&](const QString& m) { out.clear(); if (error) *error = m; return false; };
    out.clear();                  // a refusal writes nothing
    if (patch.size() < 5) return err(QStringLiteral("truncated VCDIFF header"));

    const quint8 indicator = quint8(patch.at(4));
    if (indicator & kVcdDecompress)
        return err(QObject::tr("this patch uses VCDIFF secondary compression, which is not supported"));
    if (indicator & kVcdCodetable)
        return err(QObject::tr("this patch uses a custom VCDIFF code table, which is not supported"));
    if (indicator & ~(kVcdDecompress | kVcdCodetable | kVcdAppHeader))
        return err(QStringLiteral("VCDIFF header sets an indicator bit this format does not define"));

    const VcdCode* table = defaultCodeTable();
    if (gVcdCodeTableEntries != 256)
        return err(QStringLiteral("internal error: the VCDIFF code table generated %1 entries, not 256")
                       .arg(gVcdCodeTableEntries));

    const int n = patch.size();
    int p = 5;
    if (indicator & kVcdAppHeader)
    {
        // An application header: xdelta3 writes the command line here. It is metadata; skip it by its length.
        quint64 appLen = 0;
        if (!readVcdInt(patch, p, n, appLen))
            return err(QStringLiteral("VCDIFF application header has a corrupt length"));
        if (appLen > quint64(n - p))
            return err(QStringLiteral("VCDIFF application header runs past the end of the patch"));
        p += int(appLen);
    }

    while (p < n)
    {
        const quint8 winInd = quint8(patch.at(p++));
        if (winInd & ~(kVcdSource | kVcdTarget | kVcdAdler32))
            return err(QStringLiteral("VCDIFF window sets an indicator bit this format does not define"));
        if ((winInd & kVcdSource) && (winInd & kVcdTarget))
            return err(QStringLiteral("VCDIFF window claims both a source and a target copy window"));

        // The window's copy segment. `seg` points into `source` or into the target decoded so far; neither is
        // written while this window decodes (the window is built in its own buffer), so the pointer is stable.
        const char* seg = nullptr;
        quint64 segLen = 0;
        if (winInd & (kVcdSource | kVcdTarget))
        {
            quint64 sLen = 0, sPos = 0;
            if (!readVcdInt(patch, p, n, sLen) || !readVcdInt(patch, p, n, sPos))
                return err(QStringLiteral("VCDIFF window has a corrupt source segment header"));
            const QByteArray& from = (winInd & kVcdSource) ? source : out;
            if (sPos > quint64(from.size()) || sLen > quint64(from.size()) - sPos)
                return err(QObject::tr("this patch reads past the end of the ROM "
                                       "(it was probably built for a different dump)"));
            seg = from.constData() + int(sPos);
            segLen = sLen;
        }

        quint64 deltaLen = 0;
        if (!readVcdInt(patch, p, n, deltaLen))
            return err(QStringLiteral("VCDIFF window has a corrupt delta encoding length"));
        if (deltaLen > quint64(n - p))
            return err(QStringLiteral("VCDIFF window runs past the end of the patch"));
        const int deltaEnd = p + int(deltaLen);

        quint64 tgtLen = 0;
        if (!readVcdInt(patch, p, deltaEnd, tgtLen))
            return err(QStringLiteral("VCDIFF window has a corrupt target length"));
        const quint64 kMaxOut = quint64(std::numeric_limits<int>::max());
        if (tgtLen > kMaxOut || quint64(out.size()) + tgtLen > kMaxOut)
            return err(QStringLiteral("VCDIFF window declares an impossible target length"));

        if (p >= deltaEnd) return err(QStringLiteral("VCDIFF window is truncated"));
        const quint8 deltaInd = quint8(patch.at(p++));
        if (deltaInd != 0)
            return err(QObject::tr("this patch uses VCDIFF secondary compression on a window section, "
                                   "which is not supported"));

        quint64 dataLen = 0, instLen = 0, addrLen = 0;
        if (!readVcdInt(patch, p, deltaEnd, dataLen) || !readVcdInt(patch, p, deltaEnd, instLen)
            || !readVcdInt(patch, p, deltaEnd, addrLen))
            return err(QStringLiteral("VCDIFF window has corrupt section lengths"));
        // The xdelta3 VCD_ADLER32 extension: four big-endian bytes sitting between the section lengths and the
        // sections themselves. Reading the sections without stepping over them starts the data section four
        // bytes early on every window of every real patch.
        bool haveAdler = false;
        quint32 wantAdler = 0;
        if (winInd & kVcdAdler32)
        {
            if (deltaEnd - p < 4)
                return err(QStringLiteral("VCDIFF window is too short to hold its adler32 checksum"));
            wantAdler = (quint32(quint8(patch.at(p))) << 24) | (quint32(quint8(patch.at(p + 1))) << 16)
                      | (quint32(quint8(patch.at(p + 2))) << 8) | quint32(quint8(patch.at(p + 3)));
            p += 4;
            haveAdler = true;
        }

        const quint64 remain = quint64(deltaEnd - p);
        if (dataLen > remain || instLen > remain - dataLen || addrLen > remain - dataLen - instLen)
            return err(QStringLiteral("VCDIFF window sections do not fit inside the window"));
        const int dataEnd = p + int(dataLen);
        const int instEnd = dataEnd + int(instLen);
        const int addrEnd = instEnd + int(addrLen);
        if (addrEnd != deltaEnd)
            return err(QStringLiteral("VCDIFF window sections do not fill the window"));

        int dp = p, ip = dataEnd, ap = instEnd;   // the three section cursors
        QByteArray win(int(tgtLen), '\0');
        int produced = 0;
        VcdAddrCache cache;                       // RFC 3284 §5.3: a fresh cache for every window

        while (produced < int(tgtLen))
        {
            if (ip >= instEnd)
                return err(QStringLiteral("VCDIFF window ran out of instructions before filling its target"));
            const VcdCode& code = table[quint8(patch.at(ip++))];

            for (int half = 0; half < 2; ++half)
            {
                const VcdInst& inst = (half == 0) ? code.first : code.second;
                if (inst.type == kInstNoop) continue;

                quint64 size = inst.size;
                if (size == 0)   // a zero size in the code table means the size follows in the stream
                {
                    if (!readVcdInt(patch, ip, instEnd, size))
                        return err(QStringLiteral("VCDIFF instruction has a corrupt size"));
                }
                if (size > quint64(int(tgtLen) - produced))
                    return err(QStringLiteral("VCDIFF instruction writes past the end of its target window"));

                if (inst.type == kInstAdd)
                {
                    if (size > quint64(dataEnd - dp))
                        return err(QStringLiteral("VCDIFF ADD reads past the end of the data section"));
                    // Source and destination are different buffers here, so a bulk copy is safe (unlike COPY).
                    std::memcpy(win.data() + produced, patch.constData() + dp, size);
                    dp += int(size);
                    produced += int(size);
                }
                else if (inst.type == kInstRun)
                {
                    if (dp >= dataEnd)
                        return err(QStringLiteral("VCDIFF RUN reads past the end of the data section"));
                    const char v = patch.at(dp++);
                    for (quint64 i = 0; i < size; ++i) win[produced++] = v;
                }
                else // kInstCopy
                {
                    quint64 addr = 0;
                    const quint64 here = segLen + quint64(produced);
                    if (!cache.decode(patch, ap, addrEnd, here, inst.mode, addr))
                        return err(QStringLiteral("VCDIFF COPY has a corrupt address"));
                    if (addr >= here)
                        return err(QObject::tr("this patch copies from an address that does not exist "
                                               "(it was probably built for a different dump)"));
                    // BYTE BY BYTE, never memcpy/memmove: a COPY reaching into bytes it is itself writing is
                    // legal VCDIFF and is how run-like sequences get encoded. A bulk copy passes some inputs
                    // and silently corrupts others.
                    for (quint64 i = 0; i < size; ++i)
                    {
                        const quint64 a = addr + i;
                        char b;
                        if (a < segLen)
                        {
                            b = seg[int(a)];
                        }
                        else
                        {
                            const quint64 t = a - segLen;
                            if (t >= quint64(produced))
                                return err(QStringLiteral("VCDIFF COPY reads target bytes that do not exist yet"));
                            b = win.at(int(t));
                        }
                        win[produced++] = b;
                    }
                }
            }
        }

        if (produced != int(tgtLen))
            return err(QStringLiteral("VCDIFF window did not produce its declared target length"));
        if (ip != instEnd)
            return err(QStringLiteral("VCDIFF window filled its target with instructions left over"));

        // The one check that can tell a patch built for THIS dump from one built for another: the window's own
        // adler32 over the bytes we just produced. A wrong source decodes without complaint here (the format
        // carries no source checksum), so the mismatch is the only place the mistake becomes visible — and it
        // has to be a refusal, since the whole value of it is not handing over a corrupt game. When the bit is
        // absent nothing can be verified, and we apply without implying a check happened.
        if (haveAdler && adler32(win.constData(), win.size()) != wantAdler)
            return err(QObject::tr("this patch does not match this ROM "
                                   "(its checksum of the patched result disagrees) — it was built for a "
                                   "different dump"));

        out.append(win);
        p = deltaEnd;
    }

    return true;
}

} // namespace

namespace RomPatch {

// The one implementation, published. Everything in this file already used it; the verification path needs the
// same one rather than a second copy.
quint32 crc32(const QByteArray& data) { return ::crc32(data); }

// The generated VCDIFF code table's entry count, published so the probe can assert it at RUNTIME. The table
// is built lazily, so build it before reading the count.
int vcdiffCodeTableEntries()
{
    defaultCodeTable();
    return gVcdCodeTableEntries;
}


bool isPatchExtension(const QString& suffixLower)
{
    return suffixLower == QStringLiteral("ips") || suffixLower == QStringLiteral("bps")
        || suffixLower == QStringLiteral("ups") || suffixLower == QStringLiteral("xdelta")
        || suffixLower == QStringLiteral("xdelta3") || suffixLower == QStringLiteral("vcdiff");
}

Format detectFormat(const QByteArray& patch)
{
    if (magicIs(patch, "PATCH", 5)) return Format::Ips;
    if (magicIs(patch, "UPS1", 4))  return Format::Ups;
    if (magicIs(patch, "BPS1", 4))  return Format::Bps;
    // VCDIFF: D6 C3 C4 then a version byte. Only version 0 exists.
    if (patch.size() >= 4 && quint8(patch.at(0)) == 0xD6 && quint8(patch.at(1)) == 0xC3
        && quint8(patch.at(2)) == 0xC4 && quint8(patch.at(3)) == 0x00)
        return Format::Xdelta;
    return Format::None;
}

bool apply(const QByteArray& source, const QByteArray& patch, QByteArray& out, QString* error)
{
    switch (detectFormat(patch))
    {
    case Format::Ips:    return applyIps(source, patch, out, error);
    case Format::Ups:    return applyUps(source, patch, out, error);
    case Format::Bps:    return applyBps(source, patch, out, error);
    case Format::Xdelta: return applyXdelta(source, patch, out, error);
    case Format::None:
        // xdelta1 (%XDZ) is a different container from VCDIFF and cannot be read by the applier above. Naming
        // it is the point: "we do not support xdelta1" and "this file is corrupt" are different facts, and
        // only one of them tells the person holding the patch what to do next.
        if (magicIs(patch, "%XDZ", 4))
        {
            if (error) *error = QObject::tr("this is an xdelta1 patch, which is not supported "
                                            "(only xdelta3 / VCDIFF patches can be applied)");
            return false;
        }
        if (error) *error = QStringLiteral("not a recognised ROM patch (no IPS/UPS/BPS/VCDIFF magic)");
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
// an empty string if there is none. (A ROM named "Game.sfc" pairs with "Game.ips"/"Game.bps"/"Game.ups", or
// with "Game.xdelta"/"Game.xdelta3"/"Game.vcdiff".) This list and isPatchExtension()'s are two separate lists
// of the same thing — a name added to only one of them is recognised in one place and invisible in the other.
static QString sidecarPatchFor(const QString& romPath)
{
    const QFileInfo fi(romPath);
    const QString base = fi.completeBaseName();
    const QDir dir = fi.absoluteDir();
    for (const QString& ext : { QStringLiteral("ips"), QStringLiteral("bps"), QStringLiteral("ups"),
                                QStringLiteral("xdelta"), QStringLiteral("xdelta3"), QStringLiteral("vcdiff") })
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
