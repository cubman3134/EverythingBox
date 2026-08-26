#include "EpubMeta.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QUrl>
#include <QXmlStreamReader>
#include <cstring>

#include "miniz.h"

namespace EpubMeta
{
namespace
{
    // ---- NAMESPACE PROCESSING IS OFF, AND THAT IS DELIBERATE ------------------------------------------
    //
    // With it ON (the default), QXmlStreamReader treats an UNDECLARED prefix as a fatal error and stops
    // dead. Real EPUBs carry them: `<dc:creator opf:role="aut">` with no `xmlns:opf` on the package is a
    // thing several exporters emit, and the reader would then return the title (read before the bad
    // attribute) and nothing whatever after it — an author, a series and a cover silently missing, with the
    // file opening perfectly well in the reader. That is the shape of failure this whole feature is written
    // against, and it turned up on the very first fixture written for it.
    //
    // Off, a qualified name arrives WHOLE ("dc:title" rather than "title"), so the comparison below drops
    // anything up to the last ':' itself. Every attribute this file reads is unprefixed, so nothing else
    // changes. The cost of being wrong in this direction is nil: an element called `title` in some other
    // namespace inside an OPF's <metadata> is not a thing that exists.
    //
    // configure() is where that decision is made, once, so neither parser below can be built without it.
    void configure(QXmlStreamReader& xml) { xml.setNamespaceProcessing(false); }

    bool is(const QXmlStreamReader& xml, const char* tag)
    {
        QStringView n = xml.name();
        const qsizetype c = n.lastIndexOf(QLatin1Char(':'));
        if (c >= 0) n = n.mid(c + 1);
        return n.compare(QLatin1String(tag), Qt::CaseInsensitive) == 0;
    }

    // "2011-03-04T00:00:00+00:00", "2011-03", "2011" -> 2011. Anything whose first four characters are not a
    // plausible year yields 0: a date this cannot read is a date this does not report, and a book with no
    // year shows none rather than a wrong one.
    int yearOf(const QString& date)
    {
        const QString head = date.trimmed().left(4);
        bool ok = false;
        const int y = head.toInt(&ok);
        return (ok && y >= 1000 && y <= 2999) ? y : 0;
    }

    // A series index as the package spelled it. Calibre writes "2", "2.5" and occasionally "2,5" in a
    // comma-decimal locale; only the first two are accepted, because rewriting a comma would also rewrite
    // "1,000" into 1.0 and this is not a field worth guessing at. Non-numeric => 0 ("unnumbered").
    double indexOf(const QString& s)
    {
        bool ok = false;
        const double v = s.trimmed().toDouble(&ok);
        return (ok && v > 0.0) ? v : 0.0;
    }

    // One <meta property="belongs-to-collection"> and whatever refines it.
    struct Collection
    {
        QString id;
        QString name;
        QString type;      // collection-type: "series" | "set" | "" (unstated)
        QString position;  // group-position
    };
}

QString opfPathFromContainer(const QByteArray& containerXml)
{
    QXmlStreamReader xml(containerXml);
    configure(xml);
    while (!xml.atEnd())
    {
        xml.readNext();
        if (xml.isStartElement() && is(xml, "rootfile"))
        {
            const QString fp = xml.attributes().value(QStringLiteral("full-path")).toString();
            if (!fp.isEmpty()) return fp;
        }
    }
    return QString();
}

Metadata parseOpfMetadata(const QByteArray& opfXml)
{
    Metadata m;

    // Resolved at the END, because a manifest can legally precede or follow the metadata that names an id
    // in it, and a reader that resolved as it went would work on one exporter's files and not another's.
    QHash<QString, QString> idToHref;    // manifest id -> href
    QString propsCoverHref;              // an item carrying properties="cover-image" (EPUB 3)
    QString metaCoverId;                 // <meta name="cover" content="…"> (EPUB 2)
    QString calibreSeries, calibreIndex;
    QVector<Collection> collections;
    QHash<QString, int> collectionAt;    // "#id" -> position in `collections`

    bool inMetadata = false;
    QXmlStreamReader xml(opfXml);
    configure(xml);
    while (!xml.atEnd())
    {
        xml.readNext();
        if (xml.isEndElement() && is(xml, "metadata")) { inMetadata = false; continue; }
        if (!xml.isStartElement()) continue;

        const QXmlStreamAttributes a = xml.attributes();

        if (is(xml, "metadata")) { inMetadata = true; continue; }

        if (is(xml, "item"))
        {
            const QString id   = a.value(QStringLiteral("id")).toString();
            const QString href = a.value(QStringLiteral("href")).toString();
            if (!id.isEmpty() && !href.isEmpty()) idToHref.insert(id, href);
            // EPUB 3 names the cover by a manifest PROPERTY. Split on whitespace and compare whole tokens:
            // `properties="cover-image"` and `properties="svg cover-image"` are the same statement, while a
            // contains() would also match a hypothetical "not-cover-image".
            if (propsCoverHref.isEmpty() && !href.isEmpty()
                && a.value(QStringLiteral("properties")).toString()
                     .split(QLatin1Char(' '), Qt::SkipEmptyParts)
                     .contains(QStringLiteral("cover-image")))
                propsCoverHref = href;
            continue;
        }
        if (is(xml, "itemref"))
        {
            if (!a.value(QStringLiteral("idref")).toString().isEmpty()) ++m.spineCount;
            continue;
        }
        if (!inMetadata) continue;

        if (is(xml, "title") && m.title.isEmpty())
        { m.title = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed(); continue; }

        // THE FIRST dc:creator, and only the first. An EPUB may list an author, an illustrator, a translator
        // and an editor as four creators with `opf:role` codes that half the exporters in the world omit;
        // joining them would put "Neil Gaiman & Dave McKean" on the shelf as ONE author string, which is a
        // bucket neither of them is in. The first creator is the author in every file anybody actually has,
        // and a real multi-author model is a follow-up rather than a guess made here.
        if (is(xml, "creator") && m.author.isEmpty())
        { m.author = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed(); continue; }

        if (is(xml, "language") && m.language.isEmpty())
        { m.language = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed(); continue; }

        if (is(xml, "date") && m.year == 0)
        { m.year = yearOf(xml.readElementText(QXmlStreamReader::IncludeChildElements)); continue; }

        if (is(xml, "meta"))
        {
            // EPUB 2 dialect: name/content pairs.
            const QString name    = a.value(QStringLiteral("name")).toString();
            const QString content = a.value(QStringLiteral("content")).toString();
            if (name.compare(QStringLiteral("calibre:series"), Qt::CaseInsensitive) == 0)
                calibreSeries = content.trimmed();
            else if (name.compare(QStringLiteral("calibre:series_index"), Qt::CaseInsensitive) == 0)
                calibreIndex = content.trimmed();
            else if (name.compare(QStringLiteral("cover"), Qt::CaseInsensitive) == 0)
                metaCoverId = content.trimmed();

            // EPUB 3 dialect: property/refines. The value is the element's TEXT, so read it here — and note
            // that readElementText consumes to the end element, which is why nothing below this uses `a`
            // again.
            const QString prop    = a.value(QStringLiteral("property")).toString();
            const QString refines = a.value(QStringLiteral("refines")).toString();
            const QString id      = a.value(QStringLiteral("id")).toString();
            if (prop.isEmpty()) continue;
            const QString text = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();

            if (prop.compare(QStringLiteral("belongs-to-collection"), Qt::CaseInsensitive) == 0)
            {
                Collection c;
                c.id = id.isEmpty() ? QString() : (QLatin1Char('#') + id);
                c.name = text;
                if (!c.id.isEmpty()) collectionAt.insert(c.id, collections.size());
                collections.push_back(c);
            }
            else if (!refines.isEmpty())
            {
                const int at = collectionAt.value(refines, -1);
                if (at < 0) continue;
                if (prop.compare(QStringLiteral("collection-type"), Qt::CaseInsensitive) == 0)
                    collections[at].type = text;
                else if (prop.compare(QStringLiteral("group-position"), Qt::CaseInsensitive) == 0)
                    collections[at].position = text;
            }
        }
    }

    // ---- Series: Calibre's field first, then EPUB 3's collection. See the header for the precedence. ----
    if (!calibreSeries.isEmpty())
    {
        m.series      = calibreSeries;
        m.seriesIndex = indexOf(calibreIndex);
    }
    else
    {
        for (const Collection& c : collections)
        {
            if (c.name.trimmed().isEmpty()) continue;
            // A collection is a SERIES when it says so and when it says nothing. `set` is the spec's word
            // for a boxed bundle — "the Complete Works" — which is not the axis a shelf orders by, so a
            // stated `set` is skipped rather than filed as a series somebody never named.
            if (!c.type.isEmpty() && c.type.compare(QStringLiteral("series"), Qt::CaseInsensitive) != 0)
                continue;
            m.series      = c.name.trimmed();
            m.seriesIndex = indexOf(c.position);
            break;
        }
    }

    // ---- Cover: the EPUB 3 property, else the EPUB 2 meta. Never a filename guess. ----
    m.coverHref = !propsCoverHref.isEmpty() ? propsCoverHref
                                            : idToHref.value(metaCoverId);
    return m;
}

namespace
{
    // One archive member, by exact name, into memory. Returns an empty array for "not present" and for
    // "would not inflate" — neither is worth telling a scan apart from the other.
    QByteArray memberBytes(mz_zip_archive* zip, const QString& name)
    {
        const int idx = mz_zip_reader_locate_file(zip, name.toUtf8().constData(), nullptr, 0);
        if (idx < 0) return QByteArray();
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(zip, mz_uint(idx), &sz, 0);
        if (!p) return QByteArray();
        const QByteArray out(static_cast<const char*>(p), int(sz));
        mz_free(p);
        return out;
    }
}

bool readEpubFile(const QString& epubPath, Metadata* out, QByteArray* coverOut)
{
    if (!out) return false;
    *out = Metadata();
    if (coverOut) coverOut->clear();

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, epubPath.toUtf8().constData(), 0)) return false;

    bool ok = false;
    const QString opfRel = opfPathFromContainer(memberBytes(&zip, QStringLiteral("META-INF/container.xml")));
    if (!opfRel.isEmpty())
    {
        const QByteArray opf = memberBytes(&zip, opfRel);
        if (!opf.isEmpty())
        {
            *out = parseOpfMetadata(opf);
            ok = true;

            if (coverOut && !out->coverHref.isEmpty())
            {
                // The href is relative to the OPF's own directory and is percent-encoded in the package,
                // while a zip member name is not. cleanPath collapses the "../" an OPF in a subfolder
                // legitimately uses to reach a shared images directory.
                const QString dir = QFileInfo(opfRel).path();
                const QString rel = QUrl::fromPercentEncoding(out->coverHref.toUtf8());
                const QString member = QDir::cleanPath(dir.isEmpty() || dir == QStringLiteral(".")
                                                           ? rel
                                                           : dir + QLatin1Char('/') + rel);
                *coverOut = memberBytes(&zip, member);
            }
        }
    }
    mz_zip_reader_end(&zip);
    return ok;
}

} // namespace EpubMeta
