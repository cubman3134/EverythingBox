#include "Fb2Book.h"
#include "Fb2Meta.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStandardPaths>
#include <QXmlStreamReader>

namespace
{
    QString esc(const QString& s)
    {
        QString o;
        o.reserve(s.size() + 8);
        for (const QChar c : s)
        {
            if (c == QLatin1Char('&'))      o += QLatin1String("&amp;");
            else if (c == QLatin1Char('<')) o += QLatin1String("&lt;");
            else if (c == QLatin1Char('>')) o += QLatin1String("&gt;");
            else                            o += c;
        }
        return o;
    }

    // An extension for a <binary content-type>. Only what FB2 actually carries; anything else is written
    // with no extension at all, which QTextBrowser still loads because it sniffs the bytes.
    QString extForType(const QString& contentType)
    {
        const QString t = contentType.toLower();
        if (t.contains(QLatin1String("png")))  return QStringLiteral(".png");
        if (t.contains(QLatin1String("gif")))  return QStringLiteral(".gif");
        if (t.contains(QLatin1String("webp"))) return QStringLiteral(".webp");
        if (t.contains(QLatin1String("jpeg")) || t.contains(QLatin1String("jpg")))
            return QStringLiteral(".jpg");
        return QString();
    }

    // A <binary id> is free text and lands on a filesystem, so it is not used as a filename: the staged name
    // is the id's index plus the declared type's extension, and the id maps to it. An FB2 whose ids are
    // "../../evil" therefore writes nothing outside the staging folder.
    QHash<QString, QString> stageBinaries(const QByteArray& xml, const QString& dir)
    {
        QHash<QString, QString> byId;
        QXmlStreamReader r(xml);
        int n = 0;
        while (!r.atEnd())
        {
            if (r.readNext() != QXmlStreamReader::StartElement) continue;
            if (r.name() != QLatin1String("binary")) continue;
            const QString id = r.attributes().value(QStringLiteral("id")).toString();
            const QString type = r.attributes().value(QStringLiteral("content-type")).toString();
            const QByteArray bytes = QByteArray::fromBase64(r.readElementText().toLatin1());
            if (id.isEmpty() || bytes.isEmpty()) continue;
            const QString name = QStringLiteral("img%1%2").arg(++n).arg(extForType(type));
            QFile f(dir + QLatin1Char('/') + name);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) continue;
            f.write(bytes);
            f.close();
            byId.insert(id, name);
        }
        return byId;
    }

    QString hrefId(const QXmlStreamAttributes& attrs)
    {
        QString href = attrs.value(QStringLiteral("l:href")).toString();
        if (href.isEmpty()) href = attrs.value(QStringLiteral("xlink:href")).toString();
        if (href.isEmpty())
            for (const QXmlStreamAttribute& a : attrs)
                if (a.name() == QLatin1String("href")) { href = a.value().toString(); break; }
        if (href.startsWith(QLatin1Char('#'))) href.remove(0, 1);
        return href;
    }

    struct Chapter
    {
        QString title;
        QString html;
        bool hasContent() const { return !html.trimmed().isEmpty(); }
    };
}

// The body walk. ONE streaming pass, a small stack of open tags, and no DOM: an FB2 element either maps to
// an HTML tag or contributes nothing but its children, which is what makes an unknown element harmless
// rather than a hole in the text.
static QVector<Chapter> renderBody(const QByteArray& xml, const QHash<QString, QString>& images)
{
    QVector<Chapter> chapters;
    Chapter cur;
    QString pendingTitle;      // plain text of the <title> currently being read
    int sectionDepth = 0, titleDepth = 0;
    bool inBody = false, bodyDone = false, anySection = false;

    auto flush = [&]() {
        if (cur.hasContent()) chapters.append(cur);
        cur = Chapter();
        pendingTitle.clear();
    };

    QXmlStreamReader r(xml);
    while (!r.atEnd() && !bodyDone)
    {
        const auto t = r.readNext();

        if (t == QXmlStreamReader::StartElement)
        {
            const QStringView n = r.name();
            if (!inBody)
            {
                // The FIRST unnamed <body>. FB2's <body name="notes"> is apparatus, not a chapter (header).
                if (n == QLatin1String("body") && r.attributes().value(QStringLiteral("name")).isEmpty())
                    inBody = true;
                continue;
            }

            if (n == QLatin1String("section"))
            {
                // A new top-level section is a new chapter - EXCEPT the first, which absorbs whatever the
                // body said before it (its own <title>, an epigraph). Flushing that as a chapter of its own
                // would open every FB2 on a one-line page holding nothing but the book's name.
                if (sectionDepth == 0)
                {
                    if (anySection) flush();
                    else { anySection = true; cur.title.clear(); pendingTitle.clear(); }
                }
                ++sectionDepth;
                continue;
            }
            if (n == QLatin1String("title"))
            {
                ++titleDepth;
                cur.html += sectionDepth <= 1 ? QStringLiteral("<h2>") : QStringLiteral("<h3>");
                continue;
            }
            if (titleDepth > 0)
            {
                // Inside a heading, FB2's <p> lines are ONE title on two lines, not two paragraphs — the
                // <p> tags would close the <h2> and drop the rest of the heading into the body text.
                if (n == QLatin1String("p") && !cur.html.endsWith(QLatin1Char('>')))
                { cur.html += QLatin1Char(' '); pendingTitle += QLatin1Char(' '); }
                continue;
            }

            if (n == QLatin1String("p"))                 cur.html += QStringLiteral("<p>");
            else if (n == QLatin1String("subtitle"))     cur.html += QStringLiteral("<h4>");
            else if (n == QLatin1String("empty-line"))   cur.html += QStringLiteral("<p>&nbsp;</p>");
            else if (n == QLatin1String("emphasis"))     cur.html += QStringLiteral("<i>");
            else if (n == QLatin1String("strong"))       cur.html += QStringLiteral("<b>");
            else if (n == QLatin1String("strikethrough"))cur.html += QStringLiteral("<s>");
            else if (n == QLatin1String("code"))         cur.html += QStringLiteral("<code>");
            else if (n == QLatin1String("sub"))          cur.html += QStringLiteral("<sub>");
            else if (n == QLatin1String("sup"))          cur.html += QStringLiteral("<sup>");
            else if (n == QLatin1String("cite") || n == QLatin1String("epigraph")
                     || n == QLatin1String("poem"))      cur.html += QStringLiteral("<blockquote>");
            else if (n == QLatin1String("v"))            cur.html += QStringLiteral("<p>");
            else if (n == QLatin1String("text-author"))  cur.html += QStringLiteral("<p><i>");
            else if (n == QLatin1String("image"))
            {
                const QString file = images.value(hrefId(r.attributes()));
                if (!file.isEmpty()) cur.html += QStringLiteral("<p><img src=\"%1\"></p>").arg(esc(file));
            }
            continue;
        }

        if (t == QXmlStreamReader::EndElement)
        {
            if (!inBody) continue;
            const QStringView n = r.name();
            if (n == QLatin1String("body")) { flush(); bodyDone = true; continue; }
            if (n == QLatin1String("section"))
            {
                if (sectionDepth > 0) --sectionDepth;
                if (sectionDepth == 0) flush();
                continue;
            }
            if (n == QLatin1String("title"))
            {
                --titleDepth;
                cur.html += sectionDepth <= 1 ? QStringLiteral("</h2>") : QStringLiteral("</h3>");
                if (cur.title.isEmpty()) cur.title = pendingTitle.simplified();
                pendingTitle.clear();
                continue;
            }
            if (titleDepth > 0) continue;

            if (n == QLatin1String("p") || n == QLatin1String("v")) cur.html += QStringLiteral("</p>");
            else if (n == QLatin1String("subtitle"))     cur.html += QStringLiteral("</h4>");
            else if (n == QLatin1String("emphasis"))     cur.html += QStringLiteral("</i>");
            else if (n == QLatin1String("strong"))       cur.html += QStringLiteral("</b>");
            else if (n == QLatin1String("strikethrough"))cur.html += QStringLiteral("</s>");
            else if (n == QLatin1String("code"))         cur.html += QStringLiteral("</code>");
            else if (n == QLatin1String("sub"))          cur.html += QStringLiteral("</sub>");
            else if (n == QLatin1String("sup"))          cur.html += QStringLiteral("</sup>");
            else if (n == QLatin1String("cite") || n == QLatin1String("epigraph")
                     || n == QLatin1String("poem"))      cur.html += QStringLiteral("</blockquote>");
            else if (n == QLatin1String("text-author"))  cur.html += QStringLiteral("</i></p>");
            continue;
        }

        // Whitespace-only text is NOT dropped: it is the space between "<emphasis>world</emphasis>" and the
        // word after it, and HTML collapses the pretty-printer's indentation anyway.
        if (t == QXmlStreamReader::Characters && inBody)
        {
            cur.html += esc(r.text().toString());
            if (titleDepth > 0) pendingTitle += r.text().toString();
        }
    }

    flush();
    return chapters;
}

int Fb2Book::chapterIndexForHref(const QString& hrefFileName) const
{
    for (int i = 0; i < chapterFiles_.size(); ++i)
        if (QFileInfo(chapterFiles_.at(i)).fileName() == hrefFileName) return i;
    return -1;
}

bool Fb2Book::open(const QString& path, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };

    const QByteArray xml = Fb2Meta::documentBytes(path);
    if (xml.isEmpty()) return fail(QStringLiteral("Couldn't read the file."));

    Fb2Meta::Metadata meta;
    if (!Fb2Meta::readXml(xml, &meta)) return fail(QStringLiteral("This isn't a FictionBook (FB2) file."));

    // Staged beside the MOBI reader's own folder, and named the same way (a hash of the path), so a second
    // open of the same book reuses one folder instead of accreting one per open.
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    rootDir_ = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                   .filePath(QStringLiteral("eb-fb2-") + hash);
    if (!QDir().mkpath(rootDir_)) return fail(QStringLiteral("Couldn't stage the book for reading."));

    const QHash<QString, QString> images = stageBinaries(xml, rootDir_);
    const QVector<Chapter> chapters = renderBody(xml, images);
    if (chapters.isEmpty()) return fail(QStringLiteral("This FictionBook has no readable text."));

    chapterFiles_.clear();
    toc_.clear();
    for (int i = 0; i < chapters.size(); ++i)
    {
        const QString name = QStringLiteral("chapter%1.html").arg(i + 1, 4, 10, QLatin1Char('0'));
        const QString file = rootDir_ + QLatin1Char('/') + name;
        QFile out(file);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) continue;
        QByteArray page = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body>";
        page += chapters.at(i).html.toUtf8();
        page += "</body></html>";
        out.write(page);
        out.close();
        chapterFiles_ << file;

        EpubTocEntry e;
        // A section with no <title> still needs a row in the contents panel, or the panel would be shorter
        // than the book and every entry after it would point at the wrong chapter.
        e.title = chapters.at(i).title.isEmpty()
                      ? QStringLiteral("Chapter %1").arg(i + 1) : chapters.at(i).title;
        e.href  = name;
        e.depth = 0;
        toc_ << e;
    }
    if (chapterFiles_.isEmpty()) return fail(QStringLiteral("Couldn't stage the book for reading."));

    sourcePath_ = path;
    title_  = meta.title.trimmed();
    author_ = meta.author.trimmed();
    if (title_.isEmpty()) title_ = QFileInfo(path).completeBaseName();
    return true;
}
