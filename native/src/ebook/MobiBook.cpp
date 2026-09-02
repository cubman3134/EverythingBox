#include "MobiBook.h"
#include "MobiHeader.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QRegularExpression>

bool MobiBook::open(const QString& path, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("Couldn't read the file."));
    const QByteArray data = f.readAll();
    f.close();

    // THE WHOLE CONTAINER WALK IS MobiHeader'S — the PalmDB record list, the MOBI header, the EXTH block, the
    // KF8 (AZW3) boundary and the DRM refusal. This file's job starts at "here is the book's HTML".
    MobiHeader::Info info;
    QByteArray htmlBytes;
    const MobiHeader::Result r = MobiHeader::readText(data, &info, &htmlBytes);
    if (r != MobiHeader::Result::Ok) return fail(MobiHeader::message(r));

    QString html = MobiHeader::decodeText(htmlBytes, info.textEncoding);
    const auto ci = QRegularExpression::CaseInsensitiveOption;
    const auto dotAll = QRegularExpression::DotMatchesEverythingOption;
    // The reader can't resolve MOBI's recindex images (or KF8's kindle:embed: ones), so drop <img> tags —
    // the text stays readable.
    html.remove(QRegularExpression(QStringLiteral("</?img[^>]*>"), ci));
    // KF8 points at its stylesheets with <link href="kindle:flow:…">, a scheme nothing outside a Kindle can
    // resolve; QTextBrowser would try to fetch each one. The text does not need them.
    html.remove(QRegularExpression(QStringLiteral("<link\\b[^>]*>"), ci));
    // Strip the document's own wrappers (MOBI's <head> holds only a <guide>/filepos block, KF8's holds the
    // flow links just removed) so we can wrap the body content once, cleanly, with a UTF-8 charset.
    html.remove(QRegularExpression(QStringLiteral("<\\?xml[^>]*\\?>"), ci));
    html.remove(QRegularExpression(QStringLiteral("<head\\b[^>]*>.*?</head>"), QRegularExpression::PatternOptions(ci | dotAll)));
    html.remove(QRegularExpression(QStringLiteral("</?(html|body)\\b[^>]*>"), ci));

    // Stage the HTML as a single chapter file for EbookView's QTextBrowser.
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    rootDir_ = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                   .filePath(QStringLiteral("eb-mobi-") + hash);
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
    title_ = info.title;
    author_ = info.author;
    if (title_.trimmed().isEmpty()) title_ = QFileInfo(path).completeBaseName();
    return true;
}
