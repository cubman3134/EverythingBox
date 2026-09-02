#include "TextBook.h"
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
}

bool TextBook::isPlainTextPath(const QString& path) { return hasSuffix(path, { "txt", "text" }); }
bool TextBook::isMarkdownPath(const QString& path)  { return hasSuffix(path, { "md", "markdown", "mdown", "mkd" }); }

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
    }
    return "";
}

QString TextBook::decode(const QByteArray& bytes, Encoding* used)
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

    const QString text = decode(bytes);

    // Chapter bodies + their TOC titles, from whichever of the two formats this is.
    QVector<QPair<QString, QString>> chapters;   // (title, html)
    if (isMarkdownPath(path))
    {
        for (const MarkdownHtml::Section& s : MarkdownHtml::render(text))
            chapters.append({ s.title, s.html });
    }
    if (chapters.isEmpty())
        chapters.append({ QString(), plainTextToHtml(text) });   // .txt, and a .md with nothing in it

    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    rootDir_ = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                   .filePath(QStringLiteral("eb-text-") + hash);
    if (!QDir().mkpath(rootDir_)) return fail(QStringLiteral("Couldn't stage the book for reading."));

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
    return true;
}
