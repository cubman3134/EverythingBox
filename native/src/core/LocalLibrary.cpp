#include "LocalLibrary.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>
#include <QXmlStreamReader>

namespace LocalLibrary
{
namespace
{
    const QStringList kVideoExt = {
        QStringLiteral("mkv"), QStringLiteral("mp4"), QStringLiteral("avi"), QStringLiteral("m4v"),
        QStringLiteral("mov"), QStringLiteral("webm"), QStringLiteral("mpg"), QStringLiteral("mpeg"),
        QStringLiteral("ts"),  QStringLiteral("wmv"), QStringLiteral("flv")
    };

    QString cleanName(QString s)
    {
        s.replace(QLatin1Char('.'), QLatin1Char(' '));
        s.replace(QLatin1Char('_'), QLatin1Char(' '));
        s = s.simplified();
        while (s.endsWith(QLatin1Char('-')) || s.endsWith(QLatin1Char(':')))
            s.chop(1);
        return s.simplified();
    }

    // Reads a Kodi .nfo, filling imdbId/plot/thumbPath on `e`. Malformed/empty => returns false, e untouched.
    bool readNfo(const QString& nfoPath, VideoEntry& e)
    {
        QFile f(nfoPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        QXmlStreamReader xml(&f);
        QString imdb, uniqueImdb, plot, thumb;
        while (!xml.atEnd())
        {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            const QStringView n = xml.name();
            if (n == u"imdbid" && imdb.isEmpty()) imdb = xml.readElementText();
            else if (n == u"uniqueid")
            {
                const QString type = xml.attributes().value(QStringLiteral("type")).toString();
                const QString v = xml.readElementText();
                if (type.compare(QStringLiteral("imdb"), Qt::CaseInsensitive) == 0 && uniqueImdb.isEmpty())
                    uniqueImdb = v;
            }
            else if (n == u"plot" && plot.isEmpty()) plot = xml.readElementText();
            else if (n == u"thumb" && thumb.isEmpty()) thumb = xml.readElementText();
        }
        QString id = !uniqueImdb.isEmpty() ? uniqueImdb : imdb;
        if (id.isEmpty() && plot.isEmpty() && thumb.isEmpty()) return false;  // nothing useful (or malformed)
        if (!id.isEmpty())
        {
            if (!id.startsWith(QStringLiteral("tt"))) id = QStringLiteral("tt") + id;
            e.imdbId = id;
        }
        if (!plot.isEmpty()) e.plot = plot;
        if (!thumb.isEmpty())
        {
            if (!thumb.startsWith(QStringLiteral("http")) && !QFileInfo(thumb).isAbsolute())
                thumb = QFileInfo(nfoPath).dir().absoluteFilePath(thumb);
            e.thumbPath = thumb;
        }
        return true;
    }
}

VideoEntry parseFile(const QString& path)
{
    const QFileInfo fi(path);
    VideoEntry e; e.path = path;
    const QString base = fi.completeBaseName();

    static const QRegularExpression reSE(QStringLiteral("[Ss](\\d{1,2})[Ee](\\d{1,2})"));
    static const QRegularExpression reX(QStringLiteral("(?:^|[^\\d])(\\d{1,2})[xX](\\d{1,2})(?:[^\\d]|$)"));
    QRegularExpressionMatch m = reSE.match(base);
    if (!m.hasMatch()) m = reX.match(base);
    if (m.hasMatch())
    {
        e.kind = Kind::Episode;
        e.season = m.captured(1).toInt();
        e.episode = m.captured(2).toInt();
        QString show = cleanName(base.left(m.capturedStart(0)));
        const QString dirName = fi.dir().dirName();
        if (show.isEmpty() || dirName.startsWith(QStringLiteral("Season"), Qt::CaseInsensitive))
        {
            QDir d = fi.dir();
            if (dirName.startsWith(QStringLiteral("Season"), Qt::CaseInsensitive)) d.cdUp();
            const QString fromDir = cleanName(d.dirName());
            if (!fromDir.isEmpty()) show = fromDir;
        }
        e.show = show;
        e.title = base;
        return e;
    }

    // Movie. Prefer a parenthesized (YYYY); else a dot/underscore-delimited bare year (scene-release
    // convention "Title.YEAR.quality"). A SPACE-delimited trailing number is treated as part of the
    // title (e.g. "Blade Runner 2049", "2001 A Space Odyssey"), never as the year.
    e.kind = Kind::Movie;
    static const QRegularExpression reParenYear(QStringLiteral("\\((19\\d{2}|20\\d{2})\\)"));
    static const QRegularExpression reSceneYear(QStringLiteral("[._](19\\d{2}|20\\d{2})(?:[._]|$)"));
    QRegularExpressionMatch py = reParenYear.match(base);
    if (py.hasMatch())
    {
        e.year = py.captured(1).toInt();
        e.title = cleanName(base.left(py.capturedStart(0)));
    }
    else
    {
        QRegularExpressionMatch sy = reSceneYear.match(base);
        if (sy.hasMatch())
        {
            e.year = sy.captured(1).toInt();
            e.title = cleanName(base.left(sy.capturedStart(0)));
        }
        else
        {
            e.title = cleanName(base);
            e.year = 0;
        }
    }
    return e;
}

QVector<VideoEntry> scanFolder(const QString& root)
{
    QVector<VideoEntry> out;
    if (root.isEmpty() || !QFileInfo::exists(root)) return out;

    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        const QString ext = QFileInfo(path).suffix().toLower();
        if (!kVideoExt.contains(ext)) continue;

        VideoEntry e = parseFile(path);

        // Sidecar NFO: "<file>.nfo" beside it, else "movie.nfo" in the folder (movies only).
        const QString sidecar = path.left(path.size() - ext.size() - 1) + QStringLiteral(".nfo");
        if (QFileInfo::exists(sidecar)) readNfo(sidecar, e);
        else if (e.kind == Kind::Movie)
        {
            const QString movieNfo = QFileInfo(path).dir().absoluteFilePath(QStringLiteral("movie.nfo"));
            if (QFileInfo::exists(movieNfo)) readNfo(movieNfo, e);
        }

        // Episode: series imdb id from tvshow.nfo (this dir, then up one level).
        if (e.kind == Kind::Episode)
        {
            QDir d = QFileInfo(path).dir();
            for (int up = 0; up < 2; ++up)
            {
                const QString tv = d.absoluteFilePath(QStringLiteral("tvshow.nfo"));
                VideoEntry s;
                if (QFileInfo::exists(tv) && readNfo(tv, s) && !s.imdbId.isEmpty())
                {
                    e.seriesImdbId = s.imdbId;
                    break;
                }
                if (!d.cdUp()) break;
            }
        }

        out.push_back(e);
    }
    return out;
}

OwnedIndex buildIndex(const QVector<VideoEntry>& entries)
{
    OwnedIndex idx;
    idx.entries = entries;
    for (const VideoEntry& e : entries)
    {
        if (e.kind == Kind::Movie && !e.imdbId.isEmpty())
            idx.pathById.insert(e.imdbId, e.path);
        else if (e.kind == Kind::Episode && !e.seriesImdbId.isEmpty())
        {
            const QString epKey = e.seriesImdbId + QStringLiteral(":")
                                + QString::number(e.season) + QStringLiteral(":")
                                + QString::number(e.episode);
            idx.pathById.insert(epKey, e.path);
            idx.seriesCount[e.seriesImdbId] += 1;
        }
    }
    return idx;
}

QString displayTitle(const VideoEntry& e)
{
    if (e.kind == Kind::Episode)
        return QStringLiteral("%1 S%2E%3")
            .arg(e.show.isEmpty() ? QObject::tr("Episode") : e.show)
            .arg(e.season, 2, 10, QLatin1Char('0'))
            .arg(e.episode, 2, 10, QLatin1Char('0'));
    return e.year > 0 ? QStringLiteral("%1 (%2)").arg(e.title).arg(e.year) : e.title;
}

} // namespace LocalLibrary
