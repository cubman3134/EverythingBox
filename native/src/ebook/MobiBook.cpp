#include "MobiBook.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QRegularExpression>

namespace {

inline quint16 be16(const uchar* p) { return (quint16(p[0]) << 8) | p[1]; }
inline quint32 be32(const uchar* p)
{
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

// PalmDoc / MOBI text decompression (LZ77 variant).
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
// controlled by the header's extra-data flags. Return how many trailing bytes to drop before decompressing.
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

// Windows-1252 -> Unicode for the 0x80..0x9F bytes that differ from Latin-1 (smart quotes, dashes, …).
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

} // namespace

bool MobiBook::open(const QString& path, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("Couldn't read the file."));
    const QByteArray data = f.readAll();
    f.close();
    if (data.size() < 78) return fail(QStringLiteral("Not a MOBI file."));
    const uchar* d = reinterpret_cast<const uchar*>(data.constData());

    // PalmDB header: "BOOKMOBI" (or old "TEXtREAd") at offset 60, record count at 76, then 8-byte record info.
    const QByteArray sig = data.mid(60, 8);
    if (sig != QByteArray("BOOKMOBI") && sig != QByteArray("TEXtREAd"))
        return fail(QStringLiteral("Not a MOBI book."));
    const int numRecords = be16(d + 76);
    if (numRecords < 2 || 78 + numRecords * 8 > data.size())
        return fail(QStringLiteral("Corrupt MOBI (record list)."));
    QVector<int> off(numRecords + 1);
    for (int i = 0; i < numRecords; ++i) off[i] = int(be32(d + 78 + i * 8));
    off[numRecords] = data.size();

    // Record 0: PalmDOC header (16 bytes) + MOBI header.
    const int r0 = off[0], r0end = off[1];
    if (r0 < 0 || r0end > data.size() || r0end - r0 < 16) return fail(QStringLiteral("Corrupt MOBI (header)."));
    const uchar* h = d + r0;
    const int r0len = r0end - r0;
    const uint compression = be16(h + 0);
    const int textRecordCount = be16(h + 8);

    uint textEncoding = 1252, exthFlags = 0;
    int extraDataFlags = 0, fullNameOff = 0, fullNameLen = 0;
    if (r0len >= 24 && QByteArray(reinterpret_cast<const char*>(h) + 16, 4) == QByteArray("MOBI"))
    {
        const uint mobiHdrLen = be32(h + 20);
        if (r0len >= 16 + 0x10) textEncoding = be32(h + 16 + 0x0C);
        if (r0len >= 16 + 0x5C) { fullNameOff = int(be32(h + 16 + 0x54)); fullNameLen = int(be32(h + 16 + 0x58)); }
        if (r0len >= 16 + 0x74) exthFlags = be32(h + 16 + 0x70);
        if (mobiHdrLen >= 0xE4 && r0len >= 16 + 0xE4) extraDataFlags = be16(h + 16 + 0xE2);

        if (fullNameLen > 0 && fullNameOff + fullNameLen <= r0len)
            title_ = decodeText(QByteArray(reinterpret_cast<const char*>(h) + fullNameOff, fullNameLen), textEncoding);

        // EXTH metadata block (author = record type 100).
        if (exthFlags & 0x40)
        {
            const int ex = 16 + int(mobiHdrLen);
            if (ex + 12 <= r0len && QByteArray(reinterpret_cast<const char*>(h) + ex, 4) == QByteArray("EXTH"))
            {
                const int count = int(be32(h + ex + 8));
                int p = ex + 12;
                for (int k = 0; k < count && p + 8 <= r0len; ++k)
                {
                    const int type = int(be32(h + p));
                    const int len = int(be32(h + p + 4));
                    if (len < 8 || p + len > r0len) break;
                    if (type == 100 && author_.isEmpty())            // 100 = author
                        author_ = decodeText(QByteArray(reinterpret_cast<const char*>(h) + p + 8, len - 8), textEncoding);
                    else if (type == 503 && title_.isEmpty())        // 503 = updated title (more reliable than full-name)
                        title_ = decodeText(QByteArray(reinterpret_cast<const char*>(h) + p + 8, len - 8), textEncoding);
                    p += len;
                }
            }
        }
    }

    if (compression != 1 && compression != 2)
        return fail(QStringLiteral("This MOBI uses HUFF/CDIC compression, which isn't supported yet."));

    // Decompress the text records (1 .. textRecordCount) and concatenate the HTML.
    QByteArray htmlBytes;
    for (int i = 1; i <= textRecordCount && i < numRecords; ++i)
    {
        const int s = off[i], e = off[i + 1];
        if (s < 0 || e > data.size() || e <= s) continue;
        QByteArray rec = data.mid(s, e - s);
        const int trail = trailingDataSize(reinterpret_cast<const uchar*>(rec.constData()), rec.size(), extraDataFlags);
        if (trail > 0 && trail < rec.size()) rec.chop(trail);
        htmlBytes += (compression == 2) ? palmDocDecompress(rec) : rec;
    }
    if (htmlBytes.isEmpty())
        return fail(QStringLiteral("This book has no readable MOBI text (it may be AZW3/KF8 only)."));

    QString html = decodeText(htmlBytes, textEncoding);
    const auto ci = QRegularExpression::CaseInsensitiveOption;
    const auto dotAll = QRegularExpression::DotMatchesEverythingOption;
    // The reader can't resolve MOBI's recindex images, so drop <img> tags (text stays readable).
    html.remove(QRegularExpression(QStringLiteral("</?img[^>]*>"), ci));
    // Strip the MOBI's own document wrappers (its <head> holds only a <guide>/filepos block) so we can wrap
    // the body content once, cleanly, with a UTF-8 charset for QTextBrowser.
    html.remove(QRegularExpression(QStringLiteral("<\\?xml[^>]*\\?>"), ci));
    html.remove(QRegularExpression(QStringLiteral("<head\\b[^>]*>.*?</head>"), QRegularExpression::PatternOptions(ci | dotAll)));
    html.remove(QRegularExpression(QStringLiteral("</?(html|body)\\b[^>]*>"), ci));

    // Stage the HTML as a single chapter file for EbookView's QTextBrowser.
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    rootDir_ = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                   .filePath(QStringLiteral("mmv-mobi-") + hash);
    QDir().mkpath(rootDir_);
    const QString chapterPath = rootDir_ + QStringLiteral("/book.html");
    QFile out(chapterPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("Couldn't stage the book for reading."));
    QByteArray page = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body>";
    page += html.toUtf8();
    page += "</body></html>";
    out.write(page);
    out.close();

    chapterFiles_ = { chapterPath };
    sourcePath_ = path;
    if (title_.trimmed().isEmpty()) title_ = QFileInfo(path).completeBaseName();
    return true;
}
