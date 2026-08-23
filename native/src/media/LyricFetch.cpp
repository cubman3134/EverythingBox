#include "LyricFetch.h"

#include "../core/AppBrand.h"
#include "../core/MetaCache.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>

namespace
{
// The bundle key the whole feature lives under, and the file the text is written to. One nested object rather
// than three top-level keys, so MetaCache::merge's "top-level keys replace" rule swaps the lyric record as a
// unit — a success must clear an older miss, and it does that by replacing the object rather than by leaving
// a stale "missAt" beside a fresh "file".
const QLatin1String kBundleKey("lyrics");
const QLatin1String kFileName("lyrics.lrc");
const QLatin1String kFileField("file");
const QLatin1String kMissField("missAt");

QJsonObject record(const QString& key)
{
    return MetaCache::load(key).value(kBundleKey).toObject();
}

// One shared manager, created lazily on the app's thread — the same shape MetaCache uses for artwork, and for
// the same reason: a manager per request leaks connections and defeats keep-alive against a service we are
// deliberately being light on.
QNetworkAccessManager* nam()
{
    static QPointer<QNetworkAccessManager> mgr;
    if (!mgr) mgr = new QNetworkAccessManager(QCoreApplication::instance());
    return mgr;
}

QNetworkRequest requestFor(const QUrl& url)
{
    QNetworkRequest req{ url };
    // LRCLIB asks clients to identify themselves rather than to register for a key. Sending the app's own
    // agent is the whole of the "authentication" this source has, and it is what keeps it a good citizen of a
    // volunteer-run service.
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return req;
}
} // namespace

QString LyricFetch::cacheKey(const QString& audioPath)
{
    if (audioPath.isEmpty())
        return {};
    // Absolute and cleaned, so the same track reached through "album/../album/track.mp3" and through its plain
    // path share one cache entry rather than fetching twice into two folders.
    return QFileInfo(audioPath).absoluteFilePath();
}

int LyricFetch::missRetryDays() { return 30; }

bool LyricFetch::missIsFresh(qint64 missAtSecs, qint64 nowSecs)
{
    if (missAtSecs <= 0)
        return false;
    // A missAt in the FUTURE is honoured rather than ignored. It means the clock moved backwards (a timezone
    // fix, an NTP correction, a restored backup), and the alternative — treating it as expired — turns every
    // such machine into one that re-asks about every wordless track on every play.
    if (missAtSecs > nowSecs)
        return true;
    return (nowSecs - missAtSecs) < qint64(missRetryDays()) * 24 * 60 * 60;
}

QString LyricFetch::cachedText(const QString& key)
{
    if (key.isEmpty())
        return {};
    const QString file = record(key).value(kFileField).toString();
    if (file.isEmpty())
        return {};
    QFile f(MetaCache::dirFor(key) + QLatin1Char('/') + file);
    if (!f.open(QIODevice::ReadOnly))
        return {}; // recorded but gone (a hand-cleared folder): treat as uncached, not as a miss
    return QString::fromUtf8(f.readAll());
}

bool LyricFetch::missRecorded(const QString& key)
{
    if (key.isEmpty())
        return false;
    return missIsFresh(qint64(record(key).value(kMissField).toDouble()),
                       QDateTime::currentSecsSinceEpoch());
}

void LyricFetch::storeText(const QString& key, const QString& text)
{
    if (key.isEmpty() || text.trimmed().isEmpty())
        return;
    QDir().mkpath(MetaCache::dirFor(key));
    QSaveFile f(MetaCache::dirFor(key) + QLatin1Char('/') + kFileName);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(text.toUtf8());
    if (!f.commit())
        return;
    // Replaces the whole record, so a success clears any miss that was standing.
    MetaCache::merge(key, { { kBundleKey, QJsonObject{ { kFileField, QString(kFileName) } } } });
}

void LyricFetch::storeMiss(const QString& key)
{
    if (key.isEmpty())
        return;
    MetaCache::merge(key, { { kBundleKey,
                              QJsonObject{ { kMissField, double(QDateTime::currentSecsSinceEpoch()) } } } });
}

void LyricFetch::fetch(const QString& key, const Lrclib::Query& query,
                       std::function<void(const QString&)> onDone)
{
    auto finish = [onDone = std::move(onDone)](const QString& text) { if (onDone) onDone(text); };

    // Three ways to answer with no network at all, all checked before a socket exists.
    const QString have = cachedText(key);
    if (!have.isEmpty())      { finish(have);      return; }
    if (missRecorded(key))    { finish(QString()); return; }
    if (!Lrclib::isUsable(query)) { finish(QString()); return; }

    QNetworkReply* reply = nam()->get(requestFor(Lrclib::getUrl(query)));
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, key, query, finish] {
        reply->deleteLater();
        const QString exact = (reply->error() == QNetworkReply::NoError)
                                  ? Lrclib::bestText(Lrclib::parseGet(reply->readAll()))
                                  : QString();
        if (!exact.trimmed().isEmpty())
        {
            LyricFetch::storeText(key, exact);
            finish(exact);
            return;
        }
        // A TRANSPORT failure is not a verdict. Being offline, or a DNS failure, or the service being down,
        // must not record a miss that then suppresses lookups for a month — the one thing worse than asking
        // too often is caching "no lyrics exist" because the wifi was off. Only a reply that arrived, and a
        // 404 is a reply that arrived, earns the fallback and then the recorded miss below.
        const bool arrived = reply->error() == QNetworkReply::NoError
                          || reply->error() == QNetworkReply::ContentNotFoundError;
        if (!arrived) { finish(QString()); return; }

        // The exact endpoint agrees only when artist, track, album and duration all line up, and a re-tagged
        // or differently-mastered file misses on any one of them. The fuzzy search is the second and last try.
        QNetworkReply* s = nam()->get(requestFor(Lrclib::searchUrl(query)));
        QObject::connect(s, &QNetworkReply::finished, s, [s, key, finish] {
            s->deleteLater();
            if (s->error() != QNetworkReply::NoError) { finish(QString()); return; }
            const QString found = Lrclib::bestText(Lrclib::parseSearch(s->readAll()));
            if (found.trimmed().isEmpty())
            {
                LyricFetch::storeMiss(key); // the service answered and has nothing; stop asking for a while
                finish(QString());
                return;
            }
            LyricFetch::storeText(key, found);
            finish(found);
        });
    });
}
