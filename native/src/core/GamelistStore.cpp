#include "GamelistStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>
#include <QVector>
#include <QXmlStreamReader>

namespace
{
// One <game> as read from the XML (media are the relative paths, exactly as stored).
struct Entry
{
    QString name, desc, developer, publisher, genre, players, releasedate, rating;
    QString image, thumbnail, marquee, video, fanart;
    bool found = false;
};

// Normalized "clean" title for fuzzy matching: drop everything in () or [] (region / revision / hack tags),
// then keep only lowercase alphanumerics. So "Super Mario Bros 3 (U) (PRG 0)" and the gamelist's
// "Super Mario Bros. 3 (USA) (Rev 1)" / name "Super Mario Bros. 3" all collapse to "supermariobros3" — which
// is how a GoodNES-named ROM matches a No-Intro-scraped gamelist (a very common RetroBat mismatch).
static QString cleanTitle(const QString& s)
{
    static const QRegularExpression tags(QStringLiteral("[\\(\\[][^\\)\\]]*[\\)\\]]"));
    QString t = s;
    t.remove(tags);
    QString out;
    for (const QChar c : t) if (c.isLetterOrNumber()) out += c.toLower();
    return out;
}

// A parsed gamelist for one system folder: games keyed by ROM filename, by base-name (no extension), and by
// a normalized "clean" title, so a match works even when the extension or the region/version tags differ.
struct Parsed
{
    bool parsed = false;
    QHash<QString, Entry> byFile;  // "game (usa).zip" -> entry
    QHash<QString, Entry> byBase;  // "game (usa)"     -> entry
    QHash<QString, Entry> byClean; // "game"           -> entry (fuzzy: tags/punctuation stripped)
};

QHash<QString, Parsed>& cache() { static QHash<QString, Parsed> c; return c; }

const Parsed& parsedFor(const QString& romDir)
{
    Parsed& p = cache()[romDir];
    if (p.parsed) return p;
    p.parsed = true;

    QFile f(romDir + QStringLiteral("/gamelist.xml"));
    if (!f.open(QIODevice::ReadOnly)) return p; // no gamelist here
    QXmlStreamReader xml(&f);
    while (!xml.atEnd())
    {
        xml.readNext();
        if (!(xml.isStartElement() && xml.name() == QLatin1String("game"))) continue;
        Entry e; e.found = true; QString path;
        while (!xml.atEnd() && !(xml.isEndElement() && xml.name() == QLatin1String("game")))
        {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            const QString tag = xml.name().toString();
            const QString val = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
            if (val.isEmpty() && tag != QStringLiteral("path")) continue;
            if      (tag == QStringLiteral("path"))        path = val;
            else if (tag == QStringLiteral("name"))        e.name = val;
            else if (tag == QStringLiteral("desc"))        e.desc = val;
            else if (tag == QStringLiteral("developer"))   e.developer = val;
            else if (tag == QStringLiteral("publisher"))   e.publisher = val;
            else if (tag == QStringLiteral("genre"))       e.genre = val;
            else if (tag == QStringLiteral("players"))     e.players = val;
            else if (tag == QStringLiteral("releasedate")) e.releasedate = val;
            else if (tag == QStringLiteral("rating"))      e.rating = val;
            else if (tag == QStringLiteral("image"))       e.image = val;
            else if (tag == QStringLiteral("thumbnail"))   e.thumbnail = val;
            else if (tag == QStringLiteral("marquee"))     e.marquee = val;
            else if (tag == QStringLiteral("video"))       e.video = val;
            else if (tag == QStringLiteral("fanart"))      e.fanart = val;
        }
        if (path.isEmpty()) continue;
        const QFileInfo fi(path);
        p.byFile.insert(fi.fileName().toLower(), e);
        p.byBase.insert(fi.completeBaseName().toLower(), e);
        // Fuzzy keys from BOTH the path base and the <name>, so a differently-named ROM still matches. On a
        // collision (several regional/hacked entries clean to the same title) prefer the richer one — the one
        // that actually has a video — so a fuzzy match isn't stuck with a video-less duplicate.
        auto insertClean = [&](const QString& key) {
            if (key.isEmpty()) return;
            auto ex = p.byClean.find(key);
            if (ex == p.byClean.end()) p.byClean.insert(key, e);
            else if (!e.video.isEmpty() && ex.value().video.isEmpty()) ex.value() = e;
        };
        insertClean(cleanTitle(fi.completeBaseName()));
        insertClean(cleanTitle(e.name));
    }
    return p;
}

const Entry* entryFor(const QString& romPath)
{
    const QFileInfo rfi(romPath);
    const Parsed& p = parsedFor(rfi.absolutePath());
    auto it = p.byFile.constFind(rfi.fileName().toLower());       // exact filename
    if (it != p.byFile.constEnd()) return &it.value();
    it = p.byBase.constFind(rfi.completeBaseName().toLower());    // filename minus extension
    if (it != p.byBase.constEnd()) return &it.value();
    const QString clean = cleanTitle(rfi.completeBaseName());     // fuzzy: tags/punctuation stripped
    it = clean.isEmpty() ? p.byClean.constEnd() : p.byClean.constFind(clean);
    return it != p.byClean.constEnd() ? &it.value() : nullptr;
}

// A gamelist release date is YYYYMMDDThhmmss; show just the year when it's plausible.
QString yearOf(const QString& releasedate)
{
    if (releasedate.size() < 4) return {};
    const QString y = releasedate.left(4);
    bool ok = false; const int n = y.toInt(&ok);
    return (ok && n > 1900 && n < 2100) ? y : QString();
}
} // namespace

bool GamelistStore::has(const QString& romPath)
{
    return romPath.isEmpty() ? false : entryFor(romPath) != nullptr;
}

MediaDetail GamelistStore::lookup(const QString& romPath)
{
    MediaDetail d;
    if (romPath.isEmpty()) return d;
    const Entry* e = entryFor(romPath);
    if (!e) return d;

    const QDir romDir(QFileInfo(romPath).absolutePath());
    // Resolve a relative media path to a local file — but only when it actually exists on disk (a gamelist
    // often references art that wasn't downloaded).
    auto localMedia = [&](const QString& rel) -> QString {
        if (rel.isEmpty()) return {};
        const QString abs = QDir::cleanPath(romDir.absoluteFilePath(rel)); // collapse the "./" in "./images/…"
        return QFile::exists(abs) ? abs : QString();
    };

    d.title = e->name.isEmpty() ? QFileInfo(romPath).completeBaseName() : e->name;
    d.overview = e->desc;
    d.subtitle = yearOf(e->releasedate);

    // ES media roles -> our art roles. thumbnail is the box; image is the main artwork (box fallback +
    // screenshot); marquee is the clear-logo (title art); fanart is the backdrop; video is the trailer.
    const QString thumb = localMedia(e->thumbnail), image = localMedia(e->image),
                  marquee = localMedia(e->marquee), fanart = localMedia(e->fanart), video = localMedia(e->video);
    if (!thumb.isEmpty())   d.art.addImage(QStringLiteral("box"), thumb);
    if (!image.isEmpty())  { d.art.addImage(QStringLiteral("box"), image); d.art.addImage(QStringLiteral("screenshot"), image); }
    if (!marquee.isEmpty()) d.art.addImage(QStringLiteral("logo"), marquee);
    if (!fanart.isEmpty())  { d.art.addImage(QStringLiteral("fanart"), fanart); d.art.addImage(QStringLiteral("background"), fanart); }
    if (!video.isEmpty())   d.art.videos << video;
    d.imageUrl = d.art.image(QStringLiteral("box"));

    auto fact = [&](const QString& label, const QString& value) {
        if (!value.isEmpty()) d.facts.push_back({ label, value });
    };
    fact(QStringLiteral("Developer"), e->developer);
    fact(QStringLiteral("Publisher"), e->publisher);
    fact(QStringLiteral("Genre"), e->genre);
    fact(QStringLiteral("Players"), e->players);

    if (!e->developer.isEmpty()) d.art.meta.insert(QStringLiteral("developer"), e->developer);
    if (!e->publisher.isEmpty()) d.art.meta.insert(QStringLiteral("publisher"), e->publisher);
    if (!e->genre.isEmpty())     d.art.meta.insert(QStringLiteral("genre"), e->genre);
    if (!e->players.isEmpty())   d.art.meta.insert(QStringLiteral("players"), e->players);
    if (!e->rating.isEmpty())    d.art.meta.insert(QStringLiteral("rating"), e->rating);

    d.valid = !d.title.isEmpty() || !d.art.isEmpty();
    return d;
}

void GamelistStore::clearCache() { cache().clear(); }

// ---------------------------------------------------------------- write-back ("keep scraped data")

namespace
{
QNetworkAccessManager* nam()
{
    static QPointer<QNetworkAccessManager> m;
    if (!m) m = new QNetworkAccessManager(QCoreApplication::instance());
    return m;
}

QString xmlEscape(const QString& s)
{
    QString o = s;
    o.replace(QLatin1Char('&'), QLatin1String("&amp;")).replace(QLatin1Char('<'), QLatin1String("&lt;"))
     .replace(QLatin1Char('>'), QLatin1String("&gt;"));
    return o;
}

QString mediaExt(const QString& url, const QString& fallback)
{
    const QString e = QFileInfo(QUrl(url).path()).suffix().toLower();
    static const QStringList ok = { "jpg", "jpeg", "png", "webp", "gif", "mp4", "webm", "mkv", "mov" };
    return ok.contains(e) ? e : fallback;
}

// Downloads a scrape's media into the ROM folder (./images, ./videos), then APPENDS one <game> to
// gamelist.xml (never touches existing entries — write-back only runs for games not already listed).
// Async + self-owned; briefly-lived. Runs entirely on the app thread.
class GamelistWriter : public QObject
{
public:
    GamelistWriter(const QString& romPath, const MediaDetail& d) : rom_(romPath), detail_(d) {}

    void run()
    {
        const QFileInfo rfi(rom_);
        romDir_ = rfi.absolutePath();
        base_ = rfi.completeBaseName();
        romFile_ = rfi.fileName();

        // ES tag <- our best art for that role. <image> is the main display art; <thumbnail> the box.
        auto want = [&](const QString& tag, const QString& sub, const QString& suffix, const QString& url) {
            if (url.startsWith(QStringLiteral("http")))
                jobs_.push_back({ tag, sub, base_ + suffix + QLatin1Char('.') + mediaExt(url, sub == QStringLiteral("videos") ? QStringLiteral("mp4") : QStringLiteral("jpg")), url });
        };
        const QString shot = detail_.art.image(QStringLiteral("screenshot"));
        want(QStringLiteral("image"),     QStringLiteral("images"), QStringLiteral("-image"),   shot.isEmpty() ? detail_.art.image(QStringLiteral("box")) : shot);
        want(QStringLiteral("thumbnail"), QStringLiteral("images"), QStringLiteral("-thumb"),   detail_.art.image(QStringLiteral("box")));
        want(QStringLiteral("marquee"),   QStringLiteral("images"), QStringLiteral("-marquee"), detail_.art.image(QStringLiteral("logo")));
        want(QStringLiteral("fanart"),    QStringLiteral("images"), QStringLiteral("-fanart"),  detail_.art.image(QStringLiteral("fanart")));
        if (!detail_.art.videos.isEmpty())
            want(QStringLiteral("video"), QStringLiteral("videos"), QStringLiteral("-video"),   detail_.art.videos.first());

        pending_ = jobs_.size();
        if (pending_ == 0) { finish(); return; }
        for (const Job& j : jobs_) fetch(j);
    }

private:
    struct Job { QString tag, sub, file, url; };

    void fetch(const Job& j)
    {
        QNetworkRequest req{ QUrl(j.url) };
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* r = nam()->get(req);
        connect(r, &QNetworkReply::finished, this, [this, r, j] {
            r->deleteLater();
            if (r->error() == QNetworkReply::NoError)
            {
                const QByteArray data = r->readAll();
                if (!data.isEmpty())
                {
                    QDir(romDir_).mkpath(j.sub);
                    QSaveFile f(romDir_ + QLatin1Char('/') + j.sub + QLatin1Char('/') + j.file);
                    if (f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit())
                        rel_.insert(j.tag, QStringLiteral("./") + j.sub + QLatin1Char('/') + j.file);
                }
            }
            if (--pending_ == 0) finish();
        });
    }

    void finish()
    {
        QString g = QStringLiteral("\t<game>\n");
        auto tag = [&](const QString& name, const QString& val) {
            if (!val.isEmpty()) g += QStringLiteral("\t\t<%1>%2</%1>\n").arg(name, xmlEscape(val));
        };
        tag(QStringLiteral("path"), QStringLiteral("./") + romFile_);
        tag(QStringLiteral("name"), detail_.title);
        tag(QStringLiteral("desc"), detail_.overview);
        for (auto it = rel_.constBegin(); it != rel_.constEnd(); ++it) tag(it.key(), it.value());
        for (const MediaFact& f : detail_.facts)
        {
            const QString l = f.label.toLower();
            if (l == QStringLiteral("developer")) tag(QStringLiteral("developer"), f.value);
            else if (l == QStringLiteral("publisher")) tag(QStringLiteral("publisher"), f.value);
            else if (l == QStringLiteral("genre")) tag(QStringLiteral("genre"), f.value);
            else if (l == QStringLiteral("players")) tag(QStringLiteral("players"), f.value);
        }
        if (detail_.subtitle.size() == 4) // a year -> ES releasedate
            tag(QStringLiteral("releasedate"), detail_.subtitle + QStringLiteral("0101T000000"));
        g += QStringLiteral("\t</game>\n");

        const QString path = romDir_ + QStringLiteral("/gamelist.xml");
        QByteArray existing;
        { QFile f(path); if (f.open(QIODevice::ReadOnly)) existing = f.readAll(); }
        QString out;
        const int close = QString::fromUtf8(existing).lastIndexOf(QStringLiteral("</gameList>"));
        if (close >= 0)
            out = QString::fromUtf8(existing).left(close) + g + QStringLiteral("</gameList>\n");
        else
            out = QStringLiteral("<?xml version=\"1.0\"?>\n<gameList>\n") + g + QStringLiteral("</gameList>\n");

        QSaveFile f(path);
        if (f.open(QIODevice::WriteOnly)) { f.write(out.toUtf8()); f.commit(); }
        GamelistStore::clearCache(); // next read sees the new entry
        deleteLater();
    }

    QString rom_, romDir_, base_, romFile_;
    MediaDetail detail_;
    QVector<Job> jobs_;
    QHash<QString, QString> rel_; // es-tag -> relative local path of the downloaded media
    int pending_ = 0;
};
} // namespace

void GamelistStore::write(const QString& romPath, const MediaDetail& detail)
{
    if (romPath.isEmpty() || !detail.valid) return;
    if (has(romPath)) return; // already listed -> don't duplicate (write-back only adds new entries)
    (new GamelistWriter(romPath, detail))->run(); // self-owned, async, self-destructs when done
}
