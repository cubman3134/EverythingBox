#include "EpubBook.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QXmlStreamReader>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <cstring>

#include "miniz.h"

// ---- zip extraction (miniz) -------------------------------------------------------------------------

bool EpubBook::extract(const QString& epubPath, const QString& destDir, QString* error)
{
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, epubPath.toUtf8().constData(), 0))
    {
        if (error) *error = QStringLiteral("Not a readable .epub (zip) file.");
        return false;
    }

    bool ok = true;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;

        const QString name = QString::fromUtf8(st.m_filename);
        const QString outPath = destDir + QStringLiteral("/") + name;
        if (mz_zip_reader_is_file_a_directory(&zip, i) || name.endsWith('/'))
        {
            QDir().mkpath(outPath);
            continue;
        }
        QDir().mkpath(QFileInfo(outPath).absolutePath());
        if (!mz_zip_reader_extract_to_file(&zip, i, outPath.toUtf8().constData(), 0))
        {
            ok = false; // skip the bad entry but keep going
        }
    }
    mz_zip_reader_end(&zip);
    if (!ok && error) *error = QStringLiteral("Some files in the book could not be extracted.");
    return true; // partial extraction is still openable
}

// ---- public ----------------------------------------------------------------------------------------

bool EpubBook::open(const QString& epubPath, QString* error)
{
    *this = EpubBook(); // reset
    sourcePath_ = epubPath;

    if (!epubPath.endsWith(QStringLiteral(".epub"), Qt::CaseInsensitive))
    { if (error) *error = QStringLiteral("Not an .epub file."); return false; }
    if (!QFile::exists(epubPath))
    { if (error) *error = QStringLiteral("File not found."); return false; }

    // Extract to a stable per-book temp folder (reused across opens to make reopening fast).
    const QByteArray hash = QCryptographicHash::hash(epubPath.toUtf8(), QCryptographicHash::Md5).toHex().left(10);
    rootDir_ = QDir::tempPath() + QStringLiteral("/mymediavault-ebooks/")
               + QFileInfo(epubPath).completeBaseName() + QStringLiteral("-") + QString::fromLatin1(hash);

    if (!QFile::exists(rootDir_ + QStringLiteral("/META-INF/container.xml")))
        if (!extract(epubPath, rootDir_, error))
            return false;

    QString opfRel;
    if (!parseContainer(&opfRel, error)) return false;
    if (!parseOpf(opfRel, error)) return false;

    if (chapterFiles_.isEmpty())
    { if (error) *error = QStringLiteral("The book has no readable chapters."); return false; }
    return true;
}

int EpubBook::chapterIndexForHref(const QString& hrefFileName) const
{
    for (int i = 0; i < chapterHrefs_.size(); ++i)
        if (chapterHrefs_[i].compare(hrefFileName, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}

// ---- container.xml ----------------------------------------------------------------------------------

bool EpubBook::parseContainer(QString* opfRelPath, QString* error)
{
    QFile f(rootDir_ + QStringLiteral("/META-INF/container.xml"));
    if (!f.open(QIODevice::ReadOnly))
    { if (error) *error = QStringLiteral("Missing META-INF/container.xml."); return false; }

    QXmlStreamReader xml(&f);
    while (!xml.atEnd())
    {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QStringLiteral("rootfile"))
        {
            const QString fp = xml.attributes().value(QStringLiteral("full-path")).toString();
            if (!fp.isEmpty()) { *opfRelPath = fp; return true; }
        }
    }
    if (error) *error = QStringLiteral("container.xml has no rootfile entry.");
    return false;
}

// ---- OPF (manifest + spine + metadata) --------------------------------------------------------------

bool EpubBook::parseOpf(const QString& opfRelPath, QString* error)
{
    const QString opfPath = rootDir_ + QStringLiteral("/") + opfRelPath;
    QFile f(opfPath);
    if (!f.open(QIODevice::ReadOnly))
    { if (error) *error = QStringLiteral("Could not open the package (OPF) file."); return false; }

    htmlRoot_ = QFileInfo(opfPath).absolutePath();

    QHash<QString, QString> idToHref;
    QStringList spineIds;
    QString navHref, ncxHref, spineTocId;
    bool inMetadata = false;

    QXmlStreamReader xml(&f);
    while (!xml.atEnd())
    {
        xml.readNext();
        if (xml.isStartElement())
        {
            const QStringView n = xml.name();
            const QXmlStreamAttributes a = xml.attributes();
            if (n == QStringLiteral("metadata")) inMetadata = true;
            else if (n == QStringLiteral("item"))
            {
                const QString id = a.value(QStringLiteral("id")).toString();
                const QString href = a.value(QStringLiteral("href")).toString();
                const QString props = a.value(QStringLiteral("properties")).toString();
                const QString mt = a.value(QStringLiteral("media-type")).toString();
                if (!id.isEmpty()) idToHref.insert(id, href);
                if (props.split(QLatin1Char(' ')).contains(QStringLiteral("nav"))) navHref = href;
                if (mt == QStringLiteral("application/x-dtbncx+xml")) ncxHref = href;
            }
            else if (n == QStringLiteral("spine"))
            {
                spineTocId = a.value(QStringLiteral("toc")).toString();
            }
            else if (n == QStringLiteral("itemref"))
            {
                const QString idref = a.value(QStringLiteral("idref")).toString();
                if (!idref.isEmpty()) spineIds << idref;
            }
            else if (inMetadata && n == QStringLiteral("title") && title_.isEmpty())
                title_ = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
            else if (inMetadata && n == QStringLiteral("creator") && author_.isEmpty())
                author_ = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
        }
        else if (xml.isEndElement() && xml.name() == QStringLiteral("metadata"))
            inMetadata = false;
    }

    // Resolve spine -> existing chapter files.
    for (const QString& id : spineIds)
    {
        const QString href = idToHref.value(id);
        if (href.isEmpty()) continue;
        const QString rel = QUrl::fromPercentEncoding(href.toUtf8());
        const QString full = QDir::cleanPath(htmlRoot_ + QStringLiteral("/") + rel);
        if (!QFile::exists(full)) continue;
        chapterFiles_ << full;
        chapterHrefs_ << QFileInfo(rel).fileName();
    }

    // Table of contents: EPUB3 nav first, then EPUB2 NCX.
    if (ncxHref.isEmpty() && !spineTocId.isEmpty()) ncxHref = idToHref.value(spineTocId);
    if (!navHref.isEmpty())
        parseNav(QDir::cleanPath(htmlRoot_ + QStringLiteral("/") + QUrl::fromPercentEncoding(navHref.toUtf8())));
    if (toc_.isEmpty() && !ncxHref.isEmpty())
        parseNcx(QDir::cleanPath(htmlRoot_ + QStringLiteral("/") + QUrl::fromPercentEncoding(ncxHref.toUtf8())));

    if (title_.isEmpty()) title_ = QFileInfo(sourcePath_).completeBaseName();
    return true;
}

// ---- table of contents ------------------------------------------------------------------------------

void EpubBook::parseNav(const QString& navPath)
{
    QFile f(navPath);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QString text = QString::fromUtf8(f.readAll());

    // Prefer the <nav epub:type="toc">…</nav> region; fall back to the whole document.
    QString scope = text;
    QRegularExpression navRe(QStringLiteral("<nav\\b[^>]*epub:type\\s*=\\s*\"[^\"]*\\btoc\\b[^\"]*\"[^>]*>(.*?)</nav>"),
                            QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch nm = navRe.match(text);
    if (nm.hasMatch()) scope = nm.captured(1);

    QRegularExpression aRe(QStringLiteral("<a\\b[^>]*\\bhref\\s*=\\s*\"([^\"]+)\"[^>]*>(.*?)</a>"),
                          QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    auto it = aRe.globalMatch(scope);
    while (it.hasNext())
    {
        const QRegularExpressionMatch m = it.next();
        addTocEntry(m.captured(2), m.captured(1));
    }
}

void EpubBook::parseNcx(const QString& ncxPath)
{
    QFile f(ncxPath);
    if (!f.open(QIODevice::ReadOnly)) return;

    // navPoint: <navLabel><text>Title</text></navLabel><content src="file"/>
    QString pendingTitle;
    QXmlStreamReader xml(&f);
    while (!xml.atEnd())
    {
        xml.readNext();
        if (xml.isStartElement())
        {
            if (xml.name() == QStringLiteral("text"))
                pendingTitle = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
            else if (xml.name() == QStringLiteral("content"))
            {
                const QString src = xml.attributes().value(QStringLiteral("src")).toString();
                if (!src.isEmpty() && !pendingTitle.isEmpty()) addTocEntry(pendingTitle, src);
                pendingTitle.clear();
            }
        }
    }
}

void EpubBook::addTocEntry(const QString& rawTitle, const QString& rawHref)
{
    QString title = rawTitle;
    title.remove(QRegularExpression(QStringLiteral("<[^>]+>")));         // strip any markup
    title = title.simplified();
    // Decode a few common entities that survive markup stripping.
    title.replace(QStringLiteral("&amp;"), QStringLiteral("&"))
         .replace(QStringLiteral("&lt;"), QStringLiteral("<"))
         .replace(QStringLiteral("&gt;"), QStringLiteral(">"))
         .replace(QStringLiteral("&#39;"), QStringLiteral("'"))
         .replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    if (title.isEmpty() || rawHref.isEmpty()) return;

    const QString hrefFile = QFileInfo(QUrl::fromPercentEncoding(rawHref.toUtf8()).split(QLatin1Char('#')).first()).fileName();
    if (hrefFile.isEmpty()) return;
    toc_.push_back({ title, hrefFile, 0 });
}
