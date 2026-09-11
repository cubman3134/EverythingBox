#include "TextBook.h"
#include "HtmlText.h"
#include "MarkdownHtml.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringConverter>
#include <QStringDecoder>

namespace
{
    bool hasSuffix(const QString& path, std::initializer_list<const char*> exts)
    {
        const QString e = QFileInfo(path).suffix().toLower();
        for (const char* x : exts) if (e == QLatin1String(x)) return true;
        return false;
    }

    QString escapeHtml(QString s)
    {
        s.replace(QLatin1Char('&'), QLatin1String("&amp;"));
        s.replace(QLatin1Char('<'), QLatin1String("&lt;"));
        s.replace(QLatin1Char('>'), QLatin1String("&gt;"));
        return s;
    }

    // The WHATWG Encoding Standard's labels for windows-1252. "iso-8859-1" and "us-ascii" are among them: on
    // the web those names MEAN windows-1252, and the pages that say them were written by tools that meant it.
    bool isWindows1252Label(const QByteArray& label)
    {
        for (const char* x : { "windows-1252", "cp1252", "x-cp1252", "iso-8859-1", "iso8859-1", "iso88591",
                               "iso_8859-1", "iso_8859-1:1987", "iso-ir-100", "latin1", "l1", "csisolatin1",
                               "ibm819", "cp819", "us-ascii", "ascii", "ansi_x3.4-1968" })
            if (label == x) return true;
        return false;
    }

    // windows-1252, decoded here rather than through the platform's codec list, so that it means the same
    // thing on every platform (Qt's own list has no windows-1252 where it is built without ICU). It is Latin-1
    // except for 0x80-0x9F, where Latin-1 has invisible C1 controls and windows-1252 has the typographic set.
    QString windows1252(const QByteArray& bytes)
    {
        static const char16_t high[32] = {
            0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
            0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
            0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
            0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178 };
        QString s(bytes.size(), Qt::Uninitialized);
        for (qsizetype i = 0; i < bytes.size(); ++i)
        {
            const quint8 c = quint8(bytes.at(i));
            s[i] = (c >= 0x80 && c < 0xA0) ? QChar(high[c - 0x80]) : QChar(c);
        }
        return s;
    }
}

bool TextBook::isPlainTextPath(const QString& path) { return hasSuffix(path, { "txt", "text" }); }
bool TextBook::isMarkdownPath(const QString& path)  { return hasSuffix(path, { "md", "markdown", "mdown", "mkd" }); }
bool TextBook::isHtmlPath(const QString& path)      { return hasSuffix(path, { "html", "htm" }); }

const char* TextBook::encodingName(Encoding e)
{
    switch (e)
    {
    case Encoding::Utf8Bom:    return "UTF-8 (BOM)";
    case Encoding::Utf16LeBom: return "UTF-16 LE (BOM)";
    case Encoding::Utf16BeBom: return "UTF-16 BE (BOM)";
    case Encoding::Utf8:       return "UTF-8";
    case Encoding::System:     return "system 8-bit codec";
    case Encoding::Latin1:     return "Latin-1";
    case Encoding::Declared:   return "the document's declared charset";
    }
    return "";
}

QString TextBook::decode(const QByteArray& bytes, Encoding* used)
{
    return decode(bytes, QByteArray(), used);
}

QString TextBook::decode(const QByteArray& bytes, const QByteArray& declaredCharset, Encoding* used)
{
    auto answer = [&](Encoding e, const QString& s) { if (used) *used = e; return s; };

    // 1. A byte-order mark. QStringDecoder consumes the BOM it is given when the encoding matches, so the
    //    marks are matched explicitly here and the decoder is handed the body — never the mark.
    if (bytes.size() >= 3 && quint8(bytes[0]) == 0xEF && quint8(bytes[1]) == 0xBB && quint8(bytes[2]) == 0xBF)
        return answer(Encoding::Utf8Bom, QString::fromUtf8(bytes.mid(3)));
    if (bytes.size() >= 2 && quint8(bytes[0]) == 0xFF && quint8(bytes[1]) == 0xFE)
    {
        QStringDecoder d(QStringConverter::Utf16LE);
        return answer(Encoding::Utf16LeBom, d(bytes.mid(2)));
    }
    if (bytes.size() >= 2 && quint8(bytes[0]) == 0xFE && quint8(bytes[1]) == 0xFF)
    {
        QStringDecoder d(QStringConverter::Utf16BE);
        return answer(Encoding::Utf16BeBom, d(bytes.mid(2)));
    }

    // 1b. The document's own statement (issue #259; the header says why it sits exactly here). A label that
    //     names nothing decodable, or bytes that fail the decoder it names, fall through to rung 2.
    const QByteArray label = declaredCharset.trimmed().toLower();
    if (!label.isEmpty())
    {
        if (isWindows1252Label(label)) return answer(Encoding::Declared, windows1252(bytes));
        // A UTF-16 label found by an ASCII scan contradicts itself (UTF-16 text is not ASCII-readable, and a
        // UTF-16 file has a BOM, handled above); the Encoding Standard reads it as UTF-8, and so does this.
        const QByteArray name = (label.startsWith("utf-16") || label == "unicode") ? QByteArray("utf-8") : label;
        // STATELESS, so a sequence cut short at the end of the file is an error here rather than bytes the
        // decoder quietly holds back waiting for more that never come.
        QStringDecoder d(name.constData(), QStringConverter::Flag::Stateless);
        if (d.isValid())
        {
            const QString s = d(bytes);
            if (!d.hasError()) return answer(Encoding::Declared, s);
        }
    }

    // 2. Strict UTF-8: the ERROR FLAG, not a look at the output. A file that decodes clean here is UTF-8 to
    //    a standard of proof no heuristic reaches.
    {
        QStringDecoder d(QStringConverter::Utf8);
        const QString s = d(bytes);
        if (!d.hasError()) return answer(Encoding::Utf8, s);
    }

    // 3. The system's own 8-bit codec — the ANSI codepage on Windows, which is what wrote the .txt files
    //    that are not UTF-8.
    {
        QStringDecoder d(QStringConverter::System);
        const QString s = d(bytes);
        if (!d.hasError()) return answer(Encoding::System, s);
    }

    // 4. Latin-1 cannot fail: every byte is a code point. See the header on why a readable page with the
    //    wrong accents beats a page of replacement characters.
    return answer(Encoding::Latin1, QString::fromLatin1(bytes));
}

QString TextBook::plainTextToHtml(const QString& text)
{
    QString html;
    QStringList para;
    auto flush = [&]() {
        if (para.isEmpty()) return;
        html += QStringLiteral("<p>") + escapeHtml(para.join(QLatin1Char(' '))) + QStringLiteral("</p>");
        para.clear();
    };

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines)
    {
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        // A form feed is the page break of the plain-text era; treat it as a paragraph break rather than
        // printing it as a box.
        line.replace(QLatin1Char('\f'), QLatin1Char(' '));
        if (line.trimmed().isEmpty()) { flush(); continue; }
        para << line.trimmed();
    }
    flush();
    if (html.isEmpty()) html = QStringLiteral("<p></p>");
    return html;
}

int TextBook::chapterIndexForHref(const QString& hrefFileName) const
{
    for (int i = 0; i < chapterFiles_.size(); ++i)
        if (QFileInfo(chapterFiles_.at(i)).fileName() == hrefFileName) return i;
    return -1;
}

bool TextBook::open(const QString& path, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("Couldn't read the file."));
    const QByteArray bytes = f.readAll();
    f.close();

    // An .html may say what its encoding is, and is asked (issue #259); a .txt or .md cannot, and is not.
    const bool html = isHtmlPath(path);
    const QString text = html ? decode(bytes, HtmlText::declaredCharset(bytes), &encoding_)
                              : decode(bytes, &encoding_);

    // Chapter bodies + their TOC titles, from whichever of the three formats this is.
    QVector<QPair<QString, QString>> chapters;   // (title, html)
    HtmlText::Document htmlDoc;
    if (html)
    {
        // Relative images resolve against the file's OWN folder, not the staging folder the chapters land in.
        htmlDoc = HtmlText::parse(text, QFileInfo(path).absolutePath());
        for (const HtmlText::Chapter& c : htmlDoc.chapters)
            chapters.append({ c.title, c.html });
    }
    else if (isMarkdownPath(path))
    {
        for (const MarkdownHtml::Section& s : MarkdownHtml::render(text))
            chapters.append({ s.title, s.html });
    }
    if (chapters.isEmpty())   // .txt, and a .md with nothing in it (HtmlText::parse never returns none)
        chapters.append({ QString(), html ? QStringLiteral("<p></p>") : plainTextToHtml(text) });

    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    rootDir_ = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                   .filePath(QStringLiteral("eb-text-") + hash);
    if (!QDir().mkpath(rootDir_)) return fail(QStringLiteral("Couldn't stage the book for reading."));

    // Images an .html carried INSIDE itself (data: URIs), staged beside the chapters under the names
    // HtmlText chose and the chapter HTML already uses - the same way an FB2's <binary> images are.
    for (const HtmlText::StagedImage& img : htmlDoc.images)
    {
        QFile out(rootDir_ + QLatin1Char('/') + img.name);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) continue;
        out.write(img.bytes);
        out.close();
    }

    chapterFiles_.clear();
    toc_.clear();
    for (int i = 0; i < chapters.size(); ++i)
    {
        const QString name = QStringLiteral("chapter%1.html").arg(i + 1, 4, 10, QLatin1Char('0'));
        const QString file = rootDir_ + QLatin1Char('/') + name;
        QFile out(file);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) continue;
        QByteArray page = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body>";
        page += chapters.at(i).second.toUtf8();
        page += "</body></html>";
        out.write(page);
        out.close();
        chapterFiles_ << file;

        EpubTocEntry e;
        e.title = chapters.at(i).first.isEmpty() ? QStringLiteral("Chapter %1").arg(i + 1)
                                                 : chapters.at(i).first;
        e.href  = name;
        e.depth = 0;
        toc_ << e;
    }
    if (chapterFiles_.isEmpty()) return fail(QStringLiteral("Couldn't stage the book for reading."));

    // A single-chapter document needs no contents panel: one row saying "Chapter 1" is furniture. A .md that
    // named its own chapters keeps them.
    if (toc_.size() == 1 && chapters.first().first.isEmpty()) toc_.clear();

    sourcePath_ = path;
    // Nothing in a .txt or a .md states a title or an author. The FIRST top-level heading of a Markdown
    // document is the nearest thing to one — an author's own H1 — and is taken; a .txt keeps its filename,
    // which is what BookLibrary would have fallen back to anyway.
    title_ = isMarkdownPath(path) && !chapters.first().first.isEmpty() ? chapters.first().first
                                                                      : QFileInfo(path).completeBaseName();
    author_.clear();
    // An .html can state both: its <title> (else its first heading) and an author <meta>. Never guessed.
    if (html)
    {
        if (!htmlDoc.title.trimmed().isEmpty()) title_ = htmlDoc.title.trimmed();
        author_ = htmlDoc.author.trimmed();
    }
    return true;
}
