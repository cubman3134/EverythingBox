#include "MobiHeader.h"

#include <QCoreApplication>
#include <QVector>

namespace MobiHeader
{
namespace
{
    inline quint16 be16(const uchar* p) { return quint16((quint16(p[0]) << 8) | p[1]); }
    inline quint32 be32(const uchar* p)
    {
        return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
    }

    // PalmDoc / MOBI text decompression (an LZ77 variant). Moved verbatim from MobiBook.
    QByteArray palmDocDecompress(const QByteArray& in)
    {
        QByteArray out;
        out.reserve(in.size() * 4);
        const uchar* d = reinterpret_cast<const uchar*>(in.constData());
        const int n = in.size();
        int i = 0;
        while (i < n)
        {
            const uint c = d[i++];
            if (c == 0x00)                 out.append(char(0));                      // literal NUL
            else if (c <= 0x08)            { for (uint j = 0; j < c && i < n; ++j) out.append(char(d[i++])); } // copy next c bytes
            else if (c <= 0x7F)            out.append(char(c));                       // literal ASCII
            else if (c >= 0xC0)            { out.append(' '); out.append(char(c ^ 0x80)); } // space + (c^0x80)
            else                           // 0x80..0xBF: LZ77 back-reference (2 bytes)
            {
                if (i >= n) break;
                const uint combined = (c << 8) | d[i++];
                const int distance = (combined >> 3) & 0x07FF;
                const int length = (combined & 0x07) + 3;
                if (distance == 0) break;
                for (int j = 0; j < length; ++j)
                {
                    const int src = out.size() - distance;
                    if (src < 0) { j = length; break; }
                    out.append(out.at(src));
                }
            }
        }
        return out;
    }

    // MOBI text records may carry trailing "extra data" (multibyte overlap + indexed entries) after the text,
    // controlled by the header's extra-data flags. How many trailing bytes to drop before decompressing.
    int trailingEntrySize(const uchar* rec, int end)
    {
        int bitpos = 0, result = 0, size = end;
        while (size > 0)
        {
            const uint v = rec[size - 1];
            result |= (v & 0x7F) << bitpos;
            bitpos += 7;
            size -= 1;
            if ((v & 0x80) || bitpos >= 28) break;
        }
        return result;
    }
    int trailingDataSize(const uchar* rec, int recLen, int flags)
    {
        int num = 0;
        for (int tf = flags >> 1; tf; tf >>= 1)
            if (tf & 1) { const int e = recLen - num; if (e > 0) num += trailingEntrySize(rec, e); }
        if (flags & 1) { const int idx = recLen - num - 1; if (idx >= 0) num += (rec[idx] & 0x03) + 1; }
        return num;
    }

    // ---- The record list -----------------------------------------------------------------------------
    struct Records
    {
        QVector<int> off;      // off[i] .. off[i+1] is record i; off has count+1 entries
        int count = 0;
        bool valid() const { return count >= 2; }
    };

    Records recordList(const QByteArray& data)
    {
        Records r;
        if (data.size() < 78) return r;
        const uchar* d = reinterpret_cast<const uchar*>(data.constData());
        const int n = be16(d + 76);
        if (n < 2 || 78 + n * 8 > data.size()) return r;
        r.off.resize(n + 1);
        for (int i = 0; i < n; ++i) r.off[i] = int(be32(d + 78 + i * 8));
        r.off[n] = data.size();
        // A record list whose offsets do not ascend inside the file is a damaged one; reading it would walk
        // off the end of the buffer with a negative length.
        for (int i = 0; i < n; ++i)
            if (r.off[i] < 0 || r.off[i] > data.size() || r.off[i] > r.off[i + 1]) return Records();
        r.count = n;
        return r;
    }

    // ---- One header record (a PalmDOC header + MOBI header + EXTH), wherever it sits ------------------
    //
    // EVERY OFFSET BELOW IS RELATIVE TO THE START OF THE RECORD, which is what the MOBI format's own
    // documentation counts from and what KindleUnpack reads. The MOBI header begins at 16, so a field the
    // tables list at record offset X sits at X — not at 16 + X. (Getting that wrong reads Output Language
    // as the title's offset, which is how MOBI titles used to come out as six bytes of binary.)
    struct Part
    {
        bool     ok = false;
        bool     hasMobiHeader = false;
        uint     compression = 0;
        uint     encryption = 0;
        int      textRecordCount = 0;
        uint     textEncoding = 1252;
        int      fileVersion = 0;
        int      extraDataFlags = 0;
        int      firstImageIndex = 0;
        int      kf8BoundaryRecord = -1;   // EXTH 121, when this part names one
        int      coverOffset = -1;         // EXTH 201, relative to firstImageIndex
        int      thumbOffset = -1;         // EXTH 203, same basis
        QString  title;
        QString  author;
    };

    Part parsePart(const QByteArray& data, const Records& recs, int recordIndex)
    {
        Part p;
        if (recordIndex < 0 || recordIndex >= recs.count) return p;
        const int start = recs.off[recordIndex], end = recs.off[recordIndex + 1];
        const int len = end - start;
        if (len < 16) return p;
        const uchar* h = reinterpret_cast<const uchar*>(data.constData()) + start;

        p.compression     = be16(h + 0);
        p.textRecordCount = be16(h + 8);
        p.encryption      = be16(h + 12);
        p.ok = true;

        if (len < 20 || QByteArray(reinterpret_cast<const char*>(h) + 16, 4) != QByteArray("MOBI"))
            return p;                                   // an old TEXtREAd PalmDOC: no MOBI header at all
        p.hasMobiHeader = true;

        const uint mobiHdrLen = be32(h + 20);
        auto has = [&](int upto) { return len >= upto; };
        if (has(32)) p.textEncoding = be32(h + 28);
        if (has(40)) p.fileVersion  = int(be32(h + 36));
        int fullNameOff = 0, fullNameLen = 0;
        if (has(92)) { fullNameOff = int(be32(h + 84)); fullNameLen = int(be32(h + 88)); }
        if (has(112)) p.firstImageIndex = int(be32(h + 108));
        uint exthFlags = 0;
        if (has(132)) exthFlags = be32(h + 128);
        if (mobiHdrLen >= 0xE4 && has(244)) p.extraDataFlags = be16(h + 242);

        if (fullNameLen > 0 && fullNameOff >= 0 && fullNameOff + fullNameLen <= len)
            p.title = decodeText(QByteArray(reinterpret_cast<const char*>(h) + fullNameOff, fullNameLen),
                                 p.textEncoding);

        if (!(exthFlags & 0x40)) return p;
        const int ex = 16 + int(mobiHdrLen);
        if (ex < 0 || ex + 12 > len) return p;
        if (QByteArray(reinterpret_cast<const char*>(h) + ex, 4) != QByteArray("EXTH")) return p;

        const int count = int(be32(h + ex + 8));
        int q = ex + 12;
        for (int k = 0; k < count && q + 8 <= len; ++k)
        {
            const int type = int(be32(h + q));
            const int recLen = int(be32(h + q + 4));
            if (recLen < 8 || q + recLen > len) break;
            const char* body = reinterpret_cast<const char*>(h) + q + 8;
            const int bodyLen = recLen - 8;
            switch (type)
            {
            case 100:   // author
                if (p.author.isEmpty()) p.author = decodeText(QByteArray(body, bodyLen), p.textEncoding);
                break;
            case 503:   // updated title — more reliable than the full-name field when both are present
                if (p.title.isEmpty()) p.title = decodeText(QByteArray(body, bodyLen), p.textEncoding);
                break;
            case 121:   // KF8 boundary: the record index of the KF8 header in a combined file
                if (bodyLen >= 4)
                {
                    const quint32 v = be32(reinterpret_cast<const uchar*>(body));
                    if (v != 0xFFFFFFFFu) p.kf8BoundaryRecord = int(v);
                }
                break;
            case 201:   // cover offset, relative to firstImageIndex
                if (bodyLen >= 4)
                {
                    const quint32 v = be32(reinterpret_cast<const uchar*>(body));
                    if (v != 0xFFFFFFFFu) p.coverOffset = int(v);
                }
                break;
            case 203:   // thumbnail offset, same basis
                if (bodyLen >= 4)
                {
                    const quint32 v = be32(reinterpret_cast<const uchar*>(body));
                    if (v != 0xFFFFFFFFu) p.thumbOffset = int(v);
                }
                break;
            default: break;
            }
            q += recLen;
        }
        return p;
    }

    // The part to READ: the KF8 one when this container has one, else the plain MOBI6 one. Returns the
    // record index of the chosen header, plus the parsed part.
    void choosePart(const QByteArray& data, const Records& recs, Part* chosen, int* bootRecord, bool* isKf8,
                    Part* base)
    {
        const Part first = parsePart(data, recs, 0);
        *base = first;
        *chosen = first;
        *bootRecord = 0;
        *isKf8 = first.fileVersion >= 8;
        if (*isKf8) return;                      // a standalone .azw3: record 0 already IS the KF8 header

        if (first.kf8BoundaryRecord <= 0 || first.kf8BoundaryRecord >= recs.count) return;
        const Part kf8 = parsePart(data, recs, first.kf8BoundaryRecord);
        // A KF8 part that does not hold together, or that has no text of its own, is not preferred: the
        // MOBI6 half of the same file is still a readable book, and half a fallback is worse than none.
        if (!kf8.ok || !kf8.hasMobiHeader || kf8.textRecordCount <= 0) return;
        *chosen = kf8;
        *bootRecord = first.kf8BoundaryRecord;
        *isKf8 = true;
    }

    QByteArray recordBytes(const QByteArray& data, const Records& recs, int i)
    {
        if (i < 0 || i >= recs.count) return QByteArray();
        return data.mid(recs.off[i], recs.off[i + 1] - recs.off[i]);
    }
}

bool isMobiContainer(const QByteArray& head)
{
    const QByteArray sig = head.mid(60, 8);
    return sig == QByteArray("BOOKMOBI") || sig == QByteArray("TEXtREAd");
}

QString decodeText(const QByteArray& b, uint encoding)
{
    if (encoding == 65001) return QString::fromUtf8(b); // UTF-8
    static const ushort hi[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178 };
    QString s;
    s.reserve(b.size());
    for (uchar c : b) s.append(c >= 0x80 && c <= 0x9F ? QChar(hi[c - 0x80]) : QChar(c));
    return s;
}

QString message(Result r)
{
    switch (r)
    {
    case Result::Ok:      return QString();
    case Result::NotMobi: return QCoreApplication::translate("MobiHeader", "This isn't a Kindle book file.");
    case Result::Corrupt: return QCoreApplication::translate("MobiHeader", "This Kindle book file is damaged.");
    case Result::DrmProtected: return QCoreApplication::translate(
        "MobiHeader", "This book is DRM-protected, so it can't be opened here. "
                      "Only DRM-free files can be read.");
    case Result::UnsupportedCompression: return QCoreApplication::translate(
        "MobiHeader", "This book uses HUFF/CDIC compression, which isn't supported yet.");
    case Result::NoText: return QCoreApplication::translate(
        "MobiHeader", "This book has no readable text.");
    }
    return QString();
}

Result read(const QByteArray& data, Info* info)
{
    if (!info) return Result::NotMobi;
    *info = Info();
    if (!isMobiContainer(data.left(68))) return Result::NotMobi;

    const Records recs = recordList(data);
    if (!recs.valid()) return Result::Corrupt;

    Part chosen, base;
    int boot = 0;
    bool kf8 = false;
    choosePart(data, recs, &chosen, &boot, &kf8, &base);
    if (!chosen.ok) return Result::Corrupt;

    // THE ENCRYPTION FIELD, BEFORE ANYTHING ELSE IS BELIEVED. Both parts are checked: a combined file with a
    // DRM'd KF8 half is a DRM'd book, whichever half we would otherwise have preferred.
    if (base.encryption != 0 || chosen.encryption != 0) return Result::DrmProtected;

    info->title        = chosen.title.isEmpty() ? base.title : chosen.title;
    info->author       = chosen.author.isEmpty() ? base.author : chosen.author;
    info->textEncoding = chosen.textEncoding;
    info->fileVersion  = chosen.fileVersion;
    info->kf8          = kf8;
    info->bootRecord   = boot;
    // The cover lives in the shared image records, which the MOBI6 header indexes; a standalone AZW3 indexes
    // its own. Ask whichever part actually named one.
    const Part& art = (base.coverOffset >= 0 || base.thumbOffset >= 0) ? base : chosen;
    info->hasCover = (art.coverOffset >= 0 || art.thumbOffset >= 0) && art.firstImageIndex > 0;
    return Result::Ok;
}

Result readText(const QByteArray& data, Info* info, QByteArray* text)
{
    if (!info || !text) return Result::NotMobi;
    text->clear();
    const Result meta = read(data, info);
    if (meta != Result::Ok) return meta;

    const Records recs = recordList(data);
    Part chosen, base;
    int boot = 0;
    bool kf8 = false;
    choosePart(data, recs, &chosen, &boot, &kf8, &base);

    auto extract = [&](const Part& part, int bootIndex, QByteArray* out) -> Result {
        if (part.compression != 1 && part.compression != 2) return Result::UnsupportedCompression;
        // The text records follow the part's OWN header record. For a plain MOBI that is records 1..N; for
        // the KF8 half of a combined file it is boundary+1 .. boundary+N, which is the whole of what makes
        // an AZW3 different from a MOBI here.
        for (int i = 1; i <= part.textRecordCount; ++i)
        {
            const int idx = bootIndex + i;
            if (idx >= recs.count) break;
            QByteArray rec = recordBytes(data, recs, idx);
            if (rec.isEmpty()) continue;
            const int trail = trailingDataSize(reinterpret_cast<const uchar*>(rec.constData()),
                                               rec.size(), part.extraDataFlags);
            if (trail > 0 && trail < rec.size()) rec.chop(trail);
            *out += (part.compression == 2) ? palmDocDecompress(rec) : rec;
        }
        return out->isEmpty() ? Result::NoText : Result::Ok;
    };

    const Result got = extract(chosen, boot, text);
    if (got == Result::Ok) return got;

    // A KF8 part that would not read falls back to the MOBI6 half of the same file rather than failing the
    // open: the book is still there, in the older markup.
    if (kf8 && boot != 0)
    {
        text->clear();
        info->kf8 = false;
        info->bootRecord = 0;
        info->textEncoding = base.textEncoding;
        const Result again = extract(base, 0, text);
        if (again == Result::Ok) return again;
    }
    return got;
}

QByteArray coverBytes(const QByteArray& data)
{
    if (!isMobiContainer(data.left(68))) return QByteArray();
    const Records recs = recordList(data);
    if (!recs.valid()) return QByteArray();

    Part chosen, base;
    int boot = 0;
    bool kf8 = false;
    choosePart(data, recs, &chosen, &boot, &kf8, &base);
    if (base.encryption != 0 || chosen.encryption != 0) return QByteArray();

    const Part& art = (base.coverOffset >= 0 || base.thumbOffset >= 0) ? base : chosen;
    if (art.firstImageIndex <= 0) return QByteArray();
    // EXTH 201 is the COVER and 203 is the thumbnail of it; the full-size one first, the small one only if
    // the book named no other.
    for (int off : { art.coverOffset, art.thumbOffset })
    {
        if (off < 0) continue;
        const QByteArray rec = recordBytes(data, recs, art.firstImageIndex + off);
        // A MOBI image record is the encoded image and nothing else — no wrapper, no header. Anything that
        // does not start like an image is one of the format's own index records, not a picture.
        if (rec.size() > 4
            && (rec.startsWith("\xFF\xD8") || rec.startsWith("\x89PNG") || rec.startsWith("GIF8")))
            return rec;
    }
    return QByteArray();
}

} // namespace MobiHeader
