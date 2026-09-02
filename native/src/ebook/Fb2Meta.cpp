#include "Fb2Meta.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <cstring>

#include "miniz.h"

namespace Fb2Meta
{
namespace
{
    bool endsWithCi(const QString& s, const char* suffix)
    {
        return s.endsWith(QLatin1String(suffix), Qt::CaseInsensitive);
    }

    // The zipped wire forms. NOT a plain ".zip": a zip in a books folder is not claimed as anything (the
    // rule BookLibrary.h states for bare archives), and the two names below are the ones FB2 libraries
    // actually publish.
    bool isZippedFb2(const QString& path)
    {
        return endsWithCi(path, ".fb2.zip") || endsWithCi(path, ".fbz");
    }

    // The first .fb2 member of a zip. FB2's zipped form holds exactly one, so "first" is the whole rule;
    // a zip with none yields nothing rather than a guess at its largest member.
    QByteArray fb2MemberOfZip(const QString& path)
    {
        QByteArray out;
        mz_zip_archive zip;
        std::memset(&zip, 0, sizeof(zip));
        if (!mz_zip_reader_init_file(&zip, path.toUtf8().constData(), 0)) return out;
        const mz_uint count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < count; ++i)
        {
            if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
            const QString name = QString::fromUtf8(st.m_filename);
            if (!endsWithCi(name, ".fb2")) continue;
            size_t sz = 0;
            void* p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
            if (p) { out = QByteArray(static_cast<const char*>(p), int(sz)); mz_free(p); }
            break;
        }
        mz_zip_reader_end(&zip);
        return out;
    }

    // "First Last" out of FB2's split name elements, falling back to the nickname a great many FB2 files
    // carry instead. Whitespace-joined so a missing half does not leave a stray space.
    QString readAuthor(QXmlStreamReader& r)
    {
        QString first, middle, last, nick;
        while (!r.atEnd())
        {
            const auto t = r.readNext();
            if (t == QXmlStreamReader::EndElement && r.name() == QLatin1String("author")) break;
            if (t != QXmlStreamReader::StartElement) continue;
            const QStringView n = r.name();
            if (n == QLatin1String("first-name"))       first  = r.readElementText().trimmed();
            else if (n == QLatin1String("middle-name")) middle = r.readElementText().trimmed();
            else if (n == QLatin1String("last-name"))   last   = r.readElementText().trimmed();
            else if (n == QLatin1String("nickname"))    nick   = r.readElementText().trimmed();
        }
        QStringList parts;
        for (const QString& p : { first, middle, last }) if (!p.isEmpty()) parts << p;
        if (!parts.isEmpty()) return parts.join(QLatin1Char(' '));
        return nick;
    }

    // A <coverpage>'s image href: "#binary-id", occasionally a bare id. The leading '#' is XLink's, not part
    // of the <binary id> it names, so it is stripped here — once, where the reference is read.
    QString hrefTarget(const QXmlStreamAttributes& attrs)
    {
        QString href = attrs.value(QStringLiteral("l:href")).toString();
        if (href.isEmpty()) href = attrs.value(QStringLiteral("xlink:href")).toString();
        if (href.isEmpty())
            for (const QXmlStreamAttribute& a : attrs)
                if (a.name() == QLatin1String("href")) { href = a.value().toString(); break; }
        if (href.startsWith(QLatin1Char('#'))) href.remove(0, 1);
        return href;
    }

    int yearOf(const QString& s)
    {
        // "1998", "1998-05-02", "May 1998" — the first four-digit run that could be a year, and nothing
        // else. A date FB2 could not parse is left as no year at all rather than as a wrong one.
        static const QRegularExpression re(QStringLiteral("(1[5-9][0-9]{2}|2[0-9]{3})"));
        const auto m = re.match(s);
        return m.hasMatch() ? m.captured(1).toInt() : 0;
    }
}

bool isFb2Path(const QString& path)
{
    return endsWithCi(path, ".fb2") || isZippedFb2(path);
}

QByteArray documentBytes(const QString& path)
{
    if (isZippedFb2(path)) return fb2MemberOfZip(path);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

bool readXml(const QByteArray& xml, Metadata* out)
{
    if (!out) return false;
    *out = Metadata();
    if (xml.isEmpty()) return false;

    QXmlStreamReader r(xml);
    bool sawFictionBook = false;
    bool inDescription = false;
    bool inTitleInfo = false, inPublishInfo = false, inDocumentInfo = false, inCoverpage = false;
    QString documentDate;

    while (!r.atEnd())
    {
        const auto t = r.readNext();
        if (t == QXmlStreamReader::StartElement)
        {
            const QStringView n = r.name();
            if (n == QLatin1String("FictionBook")) { sawFictionBook = true; continue; }
            if (n == QLatin1String("description")) { inDescription = true; continue; }

            if (inDescription)
            {
                if (n == QLatin1String("title-info"))         inTitleInfo = true;
                else if (n == QLatin1String("publish-info"))  inPublishInfo = true;
                else if (n == QLatin1String("document-info")) inDocumentInfo = true;
                else if (inTitleInfo && n == QLatin1String("coverpage")) inCoverpage = true;
                else if (inTitleInfo && n == QLatin1String("book-title") && out->title.isEmpty())
                    out->title = r.readElementText().trimmed();
                else if (inTitleInfo && n == QLatin1String("lang") && out->language.isEmpty())
                    out->language = r.readElementText().trimmed();
                else if (inTitleInfo && n == QLatin1String("author") && out->author.isEmpty())
                    out->author = readAuthor(r);
                else if (inTitleInfo && n == QLatin1String("sequence") && out->series.isEmpty())
                {
                    const auto a = r.attributes();
                    out->series = a.value(QStringLiteral("name")).toString().trimmed();
                    // A DECIMAL, for the reason EpubMeta.h gives about Calibre's 2.5: truncating a half-book's
                    // number collides it with the whole one beside it.
                    out->seriesIndex = a.value(QStringLiteral("number")).toString().toDouble();
                }
                else if (inCoverpage && n == QLatin1String("image") && out->coverId.isEmpty())
                    out->coverId = hrefTarget(r.attributes());
                else if (inPublishInfo && n == QLatin1String("year") && out->year == 0)
                    out->year = yearOf(r.readElementText().trimmed());
                else if (inDocumentInfo && n == QLatin1String("date") && documentDate.isEmpty())
                {
                    documentDate = r.attributes().value(QStringLiteral("value")).toString();
                    if (documentDate.isEmpty()) documentDate = r.readElementText().trimmed();
                }
            }
            continue;
        }

        if (t != QXmlStreamReader::EndElement) continue;
        const QStringView n = r.name();
        if (n == QLatin1String("coverpage"))          inCoverpage = false;
        else if (n == QLatin1String("title-info"))    inTitleInfo = false;
        else if (n == QLatin1String("publish-info"))  inPublishInfo = false;
        else if (n == QLatin1String("document-info")) inDocumentInfo = false;
        else if (n == QLatin1String("description"))   break;   // the bodies are Fb2Book's, not this file's
    }

    // A book that never states a publication year still tells us WHEN THE FILE WAS MADE, which for the
    // scanned-and-typed-in books FB2 is full of is the only date in the document. Second choice, never
    // first: a 2011 transcription of a 1954 novel is a 1954 novel.
    if (out->year == 0 && !documentDate.isEmpty()) out->year = yearOf(documentDate);

    // The section count is a body question, so it is counted in the pass Fb2Book makes; the scan fills it
    // through readFile() below rather than paying for a second walk here.
    return sawFictionBook;
}

QByteArray binary(const QByteArray& xml, const QString& id)
{
    if (id.isEmpty() || xml.isEmpty()) return QByteArray();
    QXmlStreamReader r(xml);
    while (!r.atEnd())
    {
        if (r.readNext() != QXmlStreamReader::StartElement) continue;
        if (r.name() != QLatin1String("binary")) continue;
        if (r.attributes().value(QStringLiteral("id")).toString() != id) continue;
        // FB2 wraps its base64 in whitespace and newlines; QByteArray::fromBase64 ignores both by default.
        return QByteArray::fromBase64(r.readElementText().toLatin1());
    }
    return QByteArray();
}

bool readFile(const QString& path, Metadata* out, QByteArray* cover)
{
    if (!out) return false;
    const QByteArray xml = documentBytes(path);
    if (!readXml(xml, out)) return false;

    // Top-level <section>s of the FIRST body — the reader's chapters. A body named "notes" (FB2's footnote
    // body) is not one of them, and neither is a <section> nested inside another.
    {
        QXmlStreamReader r(xml);
        int bodyDepth = -1, sectionDepth = 0;
        bool counted = false;
        while (!r.atEnd() && !counted)
        {
            const auto t = r.readNext();
            if (t == QXmlStreamReader::StartElement)
            {
                if (r.name() == QLatin1String("body") && bodyDepth < 0)
                {
                    if (!r.attributes().value(QStringLiteral("name")).isEmpty()) continue; // notes body
                    bodyDepth = 0;
                }
                else if (bodyDepth >= 0 && r.name() == QLatin1String("section"))
                {
                    if (sectionDepth == 0) out->sectionCount += 1;
                    ++sectionDepth;
                }
            }
            else if (t == QXmlStreamReader::EndElement)
            {
                if (bodyDepth >= 0 && r.name() == QLatin1String("section")) --sectionDepth;
                else if (bodyDepth >= 0 && r.name() == QLatin1String("body")) counted = true;
            }
        }
        // A body with no sections at all is still one chapter of prose, which is what the reader will show.
        if (bodyDepth >= 0 && out->sectionCount == 0) out->sectionCount = 1;
    }

    if (cover && !out->coverId.isEmpty()) *cover = binary(xml, out->coverId);
    return true;
}

} // namespace Fb2Meta
