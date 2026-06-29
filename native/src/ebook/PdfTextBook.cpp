#include "PdfTextBook.h"

#if !defined(Q_OS_ANDROID)

#include <QPdfDocument>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QCryptographicHash>

namespace {

// Turn a PDF page's extracted text into reflowable HTML paragraphs. PDF text comes line by line, so we
// join wrapped lines (full-width lines are continuations) into paragraphs and break on blank or "short"
// lines that end a sentence - a rough but readable reflow that scales with the reader's font size.
QString pageHtml(const QString& text)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    int maxLen = 1;
    for (const QString& l : lines) maxLen = qMax(maxLen, int(l.trimmed().size()));

    QString html, para;
    auto flush = [&] {
        const QString p = para.trimmed();
        if (!p.isEmpty()) html += QStringLiteral("<p>") + p.toHtmlEscaped() + QStringLiteral("</p>");
        para.clear();
    };
    for (const QString& raw : lines)
    {
        const QString l = raw.trimmed();
        if (l.isEmpty()) { flush(); continue; }
        para += l + QLatin1Char(' ');
        const QChar last = l.at(l.size() - 1);
        const bool sentenceEnd = last == QLatin1Char('.') || last == QLatin1Char('!') || last == QLatin1Char('?')
                                 || last == QLatin1Char('"') || last == QChar(0x201D);
        if (l.size() < maxLen * 2 / 3 && sentenceEnd) flush(); // a short closing line ends a paragraph
    }
    flush();
    return html;
}

} // namespace

bool PdfTextBook::open(const QString& path, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };

    QPdfDocument doc;
    if (doc.load(path) != QPdfDocument::Error::None) return fail(QStringLiteral("Couldn't open the PDF."));
    const int pages = doc.pageCount();
    if (pages <= 0) return fail(QStringLiteral("The PDF has no pages."));

    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    rootDir_ = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                   .filePath(QStringLiteral("mmv-pdf-") + hash);
    QDir().mkpath(rootDir_);

    qint64 totalChars = 0;
    QStringList files;
    for (int p = 0; p < pages; ++p)
    {
        const QString text = doc.getAllText(p).text();
        totalChars += text.size();
        const QString fp = rootDir_ + QStringLiteral("/page-%1.html").arg(p, 5, 10, QLatin1Char('0'));
        QFile f(fp);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            QByteArray page = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body>";
            page += pageHtml(text).toUtf8();
            page += "</body></html>";
            f.write(page);
            f.close();
            files << fp;
        }
    }

    // A scanned / image-only PDF has no text layer - bail so the caller falls back to the page-image view.
    if (totalChars < qint64(pages) * 20)
    {
        QDir(rootDir_).removeRecursively();
        return fail(QStringLiteral("This PDF has no extractable text (it's a scanned image)."));
    }

    chapterFiles_ = files;
    sourcePath_ = path;
    title_ = doc.metaData(QPdfDocument::MetaDataField::Title).toString().trimmed();
    author_ = doc.metaData(QPdfDocument::MetaDataField::Author).toString().trimmed();
    if (title_.isEmpty()) title_ = QFileInfo(path).completeBaseName();
    return true;
}

#else // Q_OS_ANDROID: QtPdf isn't available.

#include <QString>
bool PdfTextBook::open(const QString&, QString* error)
{
    if (error) *error = QStringLiteral("PDF viewing isn't available on this platform.");
    return false;
}

#endif
