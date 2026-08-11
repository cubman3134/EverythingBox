#include "XmltvGuide.h"

#include <QRegularExpression>
#include <QXmlStreamReader>

#include "../../third_party/miniz.h"

namespace xmltv
{

QByteArray gunzip(const QByteArray& data)
{
    // Not gzip (needs at least the 10-byte header + 8-byte trailer, magic 1f 8b, deflate method 0x08) -> hand
    // the bytes straight back. A plain .xml feed and a truncated buffer both take this path unchanged.
    const int n = data.size();
    if (n < 18) return data;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.constData());
    if (p[0] != 0x1f || p[1] != 0x8b || p[2] != 0x08) return data;

    // Skip the gzip header, including the optional FEXTRA/FNAME/FCOMMENT/FHCRC fields the FLG byte announces,
    // to land on the raw DEFLATE body.
    const unsigned char flg = p[3];
    int off = 10;
    if (flg & 0x04) { if (off + 2 > n) return data; const int xlen = p[off] | (p[off + 1] << 8); off += 2 + xlen; }
    if (flg & 0x08) { while (off < n && p[off] != 0) ++off; ++off; }   // FNAME    (zero-terminated)
    if (flg & 0x10) { while (off < n && p[off] != 0) ++off; ++off; }   // FCOMMENT (zero-terminated)
    if (flg & 0x02) off += 2;                                          // FHCRC
    const int bodyLen = n - 8 - off;                                   // exclude the 8-byte CRC32+ISIZE trailer
    if (off < 0 || off >= n || bodyLen <= 0) return data;

    // Raw inflate (flags=0: no zlib header, no adler32). tinfl stops at DEFLATE's own end-of-stream marker, so
    // handing it exactly the body is correct; a decode failure returns null and we degrade to the input.
    size_t outLen = 0;
    void* out = tinfl_decompress_mem_to_heap(p + off, size_t(bodyLen), &outLen, 0);
    if (!out) return data;
    QByteArray result(reinterpret_cast<const char*>(out), int(outLen));
    mz_free(out);
    return result;
}

QDateTime parseXmltvTime(const QString& s)
{
    const QString t = s.trimmed();
    if (t.size() < 14) return QDateTime();
    // The first 14 characters are YYYYMMDDHHMMSS and must all be digits; anything else is malformed.
    const QString stamp = t.left(14);
    for (const QChar c : stamp)
        if (!c.isDigit()) return QDateTime();

    const QDate d(stamp.mid(0, 4).toInt(), stamp.mid(4, 2).toInt(), stamp.mid(6, 2).toInt());
    const QTime tm(stamp.mid(8, 2).toInt(), stamp.mid(10, 2).toInt(), stamp.mid(12, 2).toInt());
    if (!d.isValid() || !tm.isValid()) return QDateTime();

    // The wall-clock as written, tagged UTC for now; the zone offset (if any) is folded out below.
    QDateTime dt(d, tm, Qt::UTC);

    const QString rest = t.mid(14).trimmed();   // "+0100", "-0530", or "" when the feed omits the zone
    if (!rest.isEmpty())
    {
        static const QRegularExpression re(QStringLiteral("^([+-])(\\d{2})(\\d{2})$"));
        const QRegularExpressionMatch m = re.match(rest);
        if (m.hasMatch())
        {
            const int sign      = m.captured(1) == QLatin1String("-") ? -1 : 1;
            const int offsetSec = sign * (m.captured(2).toInt() * 3600 + m.captured(3).toInt() * 60);
            // The stamp reads `offsetSec` ahead of UTC, so UTC = wall-clock - offset.
            dt = dt.addSecs(-offsetSec);
        }
        // A present-but-unparseable trailer is ignored: the wall-clock stands as UTC rather than the whole
        // stamp being discarded.
    }
    return dt;
}

Guide parseXmltv(const QByteArray& xml)
{
    Guide g;
    QXmlStreamReader r(xml);
    while (!r.atEnd())
    {
        r.readNext();
        if (!r.isStartElement()) continue;

        if (r.name() == QLatin1String("channel"))
        {
            const QString id = r.attributes().value(QLatin1String("id")).toString();
            QString displayName;
            while (!r.atEnd() && !(r.isEndElement() && r.name() == QLatin1String("channel")))
            {
                r.readNext();
                if (r.isStartElement() && r.name() == QLatin1String("display-name") && displayName.isEmpty())
                    displayName = r.readElementText();   // leaves the reader on display-name's end element
            }
            if (!id.isEmpty()) g.channelNames.insert(id, displayName);
        }
        else if (r.name() == QLatin1String("programme"))
        {
            Programme pr;
            pr.channelId = r.attributes().value(QLatin1String("channel")).toString();
            pr.startUtc  = parseXmltvTime(r.attributes().value(QLatin1String("start")).toString());
            pr.stopUtc   = parseXmltvTime(r.attributes().value(QLatin1String("stop")).toString());
            while (!r.atEnd() && !(r.isEndElement() && r.name() == QLatin1String("programme")))
            {
                r.readNext();
                if (!r.isStartElement()) continue;
                if      (r.name() == QLatin1String("title") && pr.title.isEmpty()) pr.title = r.readElementText();
                else if (r.name() == QLatin1String("desc")  && pr.desc.isEmpty())  pr.desc  = r.readElementText();
            }
            g.programmes.push_back(pr);
        }
    }
    return g;
}

QVector<Programme> programmesForChannel(const Guide& g, const QString& tvgId)
{
    QVector<Programme> out;
    if (tvgId.isEmpty()) return out;
    for (const Programme& p : g.programmes)
        if (QString::compare(p.channelId, tvgId, Qt::CaseInsensitive) == 0)
            out.push_back(p);
    return out;
}

NowNext nowNext(const QVector<Programme>& forOneChannel, const QDateTime& nowUtc)
{
    NowNext r;
    for (const Programme& p : forOneChannel)
    {
        // current: the first programme whose [start, stop) window contains now.
        if (!r.hasCurrent && p.startUtc.isValid() && p.stopUtc.isValid()
            && p.startUtc <= nowUtc && nowUtc < p.stopUtc)
        {
            r.current = p;
            r.hasCurrent = true;
        }
        // next: the earliest programme starting strictly after now (order-independent; scans the whole list).
        if (p.startUtc.isValid() && p.startUtc > nowUtc && (!r.hasNext || p.startUtc < r.next.startUtc))
        {
            r.next = p;
            r.hasNext = true;
        }
    }
    return r;
}

}
