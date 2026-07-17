#include "MetaCache.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

namespace
{
QString metaRoot() { return AppPaths::dataDir() + QStringLiteral("/metadata"); }

QString metaFile(const QString& key) { return MetaCache::dirFor(key) + QStringLiteral("/meta.json"); }

// One shared network manager for artwork fetches, created lazily on the app's thread.
QNetworkAccessManager* nam()
{
    static QPointer<QNetworkAccessManager> mgr;
    if (!mgr) mgr = new QNetworkAccessManager(QCoreApplication::instance());
    return mgr;
}

// In-flight artwork downloads (key + role), so a shelf rebuild doesn't refetch the same poster.
QSet<QString>& inflight() { static QSet<QString> s; return s; }

// Image file extension from the url path, else from a content type, else jpg.
QString imageExt(const QUrl& url, const QString& contentType)
{
    const QString fromPath = QFileInfo(url.path()).suffix().toLower();
    static const QStringList known = { QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
                                       QStringLiteral("webp"), QStringLiteral("gif"), QStringLiteral("svg") };
    if (known.contains(fromPath)) return fromPath;
    if (contentType.contains(QStringLiteral("png")))  return QStringLiteral("png");
    if (contentType.contains(QStringLiteral("webp"))) return QStringLiteral("webp");
    if (contentType.contains(QStringLiteral("svg")))  return QStringLiteral("svg");
    if (contentType.contains(QStringLiteral("gif")))  return QStringLiteral("gif");
    return QStringLiteral("jpg");
}
} // namespace

static bool isYoutube(const QString& url); // defined lower down; used by saveArt's prefetch

QString MetaCache::keyFor(const MediaItem& item)
{
    return item.id.isEmpty() ? item.url : item.id;
}

QString MetaCache::dirFor(const QString& key)
{
    // Hash the key for the folder name: addon ids can hold any character (urls, "igdb:123", paths).
    const QByteArray h = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex();
    return metaRoot() + QLatin1Char('/') + QString::fromLatin1(h);
}

QJsonObject MetaCache::load(const QString& key)
{
    if (key.isEmpty()) return {};
    QFile f(metaFile(key));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

void MetaCache::merge(const QString& key, const QJsonObject& patch)
{
    if (key.isEmpty()) return;
    QJsonObject obj = load(key);   // keep everything already stored (including keys we don't know about)
    for (auto it = patch.constBegin(); it != patch.constEnd(); ++it)
        obj.insert(it.key(), it.value());
    obj.insert(QStringLiteral("v"), 1);
    obj.insert(QStringLiteral("key"), key);
    obj.insert(QStringLiteral("savedAt"), QDateTime::currentSecsSinceEpoch());
    QDir().mkpath(dirFor(key));
    QSaveFile f(metaFile(key));    // atomic: a crash mid-write can't corrupt an existing bundle
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    f.commit();
}

void MetaCache::saveItem(const MediaItem& item)
{
    const QString key = keyFor(item);
    if (key.isEmpty()) return;
    QJsonObject it;
    it.insert(QStringLiteral("id"), item.id);
    it.insert(QStringLiteral("title"), item.title);
    it.insert(QStringLiteral("subtitle"), item.subtitle);
    it.insert(QStringLiteral("type"), item.type);
    it.insert(QStringLiteral("thumbnailUrl"), item.thumbnailUrl);
    it.insert(QStringLiteral("mime"), item.mime);
    if (!item.systemHint.isEmpty())   it.insert(QStringLiteral("systemHint"), item.systemHint);
    if (!item.imdbStreamId.isEmpty()) it.insert(QStringLiteral("imdbStreamId"), item.imdbStreamId);
    if (!item.altNames.isEmpty())     it.insert(QStringLiteral("altNames"), QJsonArray::fromStringList(item.altNames));
    merge(key, { { QStringLiteral("item"), it } });
    saveArt(key, item.art); // extra artwork/videos/audio/meta the item carries (merges + prefetches)
}

void MetaCache::saveDetail(const QString& key, const MediaDetail& d)
{
    if (key.isEmpty() || !d.valid) return;
    QJsonObject det;
    det.insert(QStringLiteral("title"), d.title);
    det.insert(QStringLiteral("subtitle"), d.subtitle);
    det.insert(QStringLiteral("overview"), d.overview);
    det.insert(QStringLiteral("imageUrl"), d.imageUrl);
    if (!d.imdbStreamId.isEmpty()) det.insert(QStringLiteral("imdbStreamId"), d.imdbStreamId);
    QJsonArray facts;
    for (const MediaFact& f : d.facts)
        facts.append(QJsonObject{ { QStringLiteral("label"), f.label }, { QStringLiteral("value"), f.value } });
    det.insert(QStringLiteral("facts"), facts);
    merge(key, { { QStringLiteral("detail"), det } });
    saveArt(key, d.art); // logo/box/fanart/screenshots/trailers/theme-music/extra facts (merges + prefetches)
}

void MetaCache::saveArt(const QString& key, const MediaArt& art)
{
    if (key.isEmpty() || art.isEmpty()) return;
    // Record the whole bundle (urls + videos + audio + meta) so it survives offline even before downloads
    // finish; merge keeps any roles a previous provider already stored.
    QJsonObject blob = load(key).value(QStringLiteral("art")).toObject();
    const QJsonObject fresh = QJsonObject::fromVariantMap(art.toVariant());
    for (auto it = fresh.constBegin(); it != fresh.constEnd(); ++it) blob.insert(it.key(), it.value());
    merge(key, { { QStringLiteral("art"), blob } });
    // Prefetch the best image per role so posters/logos/box/fanart render with no network next time.
    for (auto it = art.images.constBegin(); it != art.images.constEnd(); ++it)
        if (!it.value().isEmpty()) cacheImage(key, it.key(), it.value().first());
    // The theme song + first directly-playable trailer -> disk in the background, so they play instantly and
    // offline next time (the video element streams the url meanwhile). YouTube ids are skipped by cacheMedia.
    if (!art.audio.isEmpty()) cacheMedia(key, QStringLiteral("audio0"), art.audio.first());
    for (const QString& v : art.videos)
        if (!isYoutube(v)) { cacheMedia(key, QStringLiteral("video0"), v); break; }
}

MediaArt MetaCache::loadArt(const QString& key)
{
    const QJsonObject art = load(key).value(QStringLiteral("art")).toObject();
    MediaArt a = MediaArt::fromJson(art); // same parser: images{role:[urls]} + videos + audio + meta
    // Offline-first: put the locally cached file (if any) at the front of each role's candidate list.
    QMap<QString, QStringList> resolved;
    for (auto it = a.images.constBegin(); it != a.images.constEnd(); ++it)
    {
        QStringList list;
        const QString local = imagePath(key, it.key());
        if (!local.isEmpty()) list << local;
        for (const QString& u : it.value()) if (!list.contains(u)) list << u;
        resolved.insert(it.key(), list);
    }
    a.images = resolved;
    // Offline-first for the trailer + theme song too: a cached local file plays before the remote url.
    const QString localVideo = mediaPath(key, QStringLiteral("video0"));
    if (!localVideo.isEmpty() && !a.videos.contains(localVideo)) a.videos.prepend(localVideo);
    const QString localAudio = mediaPath(key, QStringLiteral("audio0"));
    if (!localAudio.isEmpty() && !a.audio.contains(localAudio)) a.audio.prepend(localAudio);
    return a;
}

MediaDetail MetaCache::cachedDetail(const QString& key)
{
    MediaDetail d;
    const QJsonObject obj = load(key);
    const QJsonObject det = obj.value(QStringLiteral("detail")).toObject();
    const QJsonObject it  = obj.value(QStringLiteral("item")).toObject();
    // Prefer the saved detail card; fall back to the item's own fields so at least title/poster show.
    d.title    = det.value(QStringLiteral("title")).toString(it.value(QStringLiteral("title")).toString());
    d.subtitle = det.value(QStringLiteral("subtitle")).toString(it.value(QStringLiteral("subtitle")).toString());
    d.overview = det.value(QStringLiteral("overview")).toString();
    d.imdbStreamId = det.value(QStringLiteral("imdbStreamId")).toString();
    for (const QJsonValue& v : det.value(QStringLiteral("facts")).toArray())
    {
        const QJsonObject f = v.toObject();
        d.facts.push_back({ f.value(QStringLiteral("label")).toString(), f.value(QStringLiteral("value")).toString() });
    }
    // Offline-first artwork: the locally cached poster (detail cover, else the grid thumb), else the urls.
    QString img = imagePath(key, QStringLiteral("poster"));
    if (img.isEmpty()) img = imagePath(key, QStringLiteral("thumb"));
    if (img.isEmpty()) img = det.value(QStringLiteral("imageUrl")).toString(it.value(QStringLiteral("thumbnailUrl")).toString());
    d.imageUrl = img;
    d.art = loadArt(key); // rich artwork/videos/audio/meta, resolved to local files where cached
    d.valid = !d.title.isEmpty() || !d.art.isEmpty();
    return d;
}

QString MetaCache::imagePath(const QString& key, const QString& role)
{
    if (key.isEmpty()) return {};
    const QString file = load(key).value(QStringLiteral("images")).toObject().value(role).toString();
    if (file.isEmpty()) return {};
    const QString abs = dirFor(key) + QLatin1Char('/') + file;
    return QFile::exists(abs) ? abs : QString();
}

QString MetaCache::displayImage(const QString& key, const QString& url)
{
    QString img = imagePath(key, QStringLiteral("thumb"));
    if (img.isEmpty()) img = imagePath(key, QStringLiteral("poster"));
    return img.isEmpty() ? url : img;
}

void MetaCache::cacheImage(const QString& key, const QString& role, const QString& url)
{
    if (key.isEmpty() || url.isEmpty() || !url.startsWith(QStringLiteral("http"))) return;
    if (!imagePath(key, role).isEmpty()) return; // already cached
    const QString tag = key + QLatin1Char('|') + role;
    if (inflight().contains(tag)) return;
    inflight().insert(tag);

    QNetworkRequest req{ QUrl(url) };
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam()->get(req);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, key, role, tag] {
        inflight().remove(tag);
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return; // offline/404: keep the url fallback
        const QByteArray data = reply->readAll();
        if (data.isEmpty()) return;
        const QString ext = imageExt(reply->url(),
                                     reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower());
        const QString file = role + QLatin1Char('.') + ext;
        QDir().mkpath(MetaCache::dirFor(key));
        QSaveFile f(MetaCache::dirFor(key) + QLatin1Char('/') + file);
        if (!f.open(QIODevice::WriteOnly)) return;
        f.write(data);
        if (!f.commit()) return;
        // Record it under "images" (merge keeps any other roles already saved).
        QJsonObject images = MetaCache::load(key).value(QStringLiteral("images")).toObject();
        images.insert(role, file);
        MetaCache::merge(key, { { QStringLiteral("images"), images } });
    });
}

void MetaCache::storeImage(const QString& key, const QString& role, const QString& url,
                           const QString& contentType, const QByteArray& data)
{
    if (key.isEmpty() || role.isEmpty() || data.isEmpty()) return;
    if (!imagePath(key, role).isEmpty()) return; // already cached
    const QString ext = imageExt(QUrl(url), contentType.toLower());
    const QString file = role + QLatin1Char('.') + ext;
    QDir().mkpath(dirFor(key));
    QSaveFile f(dirFor(key) + QLatin1Char('/') + file);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(data);
    if (!f.commit()) return;
    // Record it under "images" (merge keeps any other roles already saved).
    QJsonObject images = load(key).value(QStringLiteral("images")).toObject();
    images.insert(role, file);
    merge(key, { { QStringLiteral("images"), images } });
}

static bool isYoutube(const QString& url)
{
    return url.contains(QStringLiteral("youtube"), Qt::CaseInsensitive)
        || url.contains(QStringLiteral("youtu.be"), Qt::CaseInsensitive);
}

QString MetaCache::mediaPath(const QString& key, const QString& role)
{
    if (key.isEmpty()) return {};
    const QString file = load(key).value(QStringLiteral("media")).toObject().value(role).toString();
    if (file.isEmpty()) return {};
    const QString abs = dirFor(key) + QLatin1Char('/') + file;
    return QFile::exists(abs) ? abs : QString();
}

void MetaCache::cacheMedia(const QString& key, const QString& role, const QString& url)
{
    if (key.isEmpty() || url.isEmpty() || !url.startsWith(QStringLiteral("http")) || isYoutube(url)) return;
    if (!mediaPath(key, role).isEmpty()) return; // already cached
    const QString tag = key + QLatin1Char('|') + QStringLiteral("media|") + role;
    if (inflight().contains(tag)) return;
    inflight().insert(tag);

    QNetworkRequest req{ QUrl(url) };
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam()->get(req);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, key, role, tag] {
        inflight().remove(tag);
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return; // offline/404: keep streaming the url next time
        const QByteArray data = reply->readAll();
        if (data.isEmpty()) return;
        QString ext = QFileInfo(reply->url().path()).suffix().toLower();
        static const QStringList media = { QStringLiteral("mp4"), QStringLiteral("webm"), QStringLiteral("mkv"),
                                           QStringLiteral("mov"), QStringLiteral("mp3"), QStringLiteral("m4a"),
                                           QStringLiteral("ogg"), QStringLiteral("opus") };
        if (!media.contains(ext)) ext = role.startsWith(QStringLiteral("audio")) ? QStringLiteral("mp3")
                                                                                 : QStringLiteral("mp4");
        const QString file = role + QLatin1Char('.') + ext;
        QDir().mkpath(MetaCache::dirFor(key));
        QSaveFile f(MetaCache::dirFor(key) + QLatin1Char('/') + file);
        if (!f.open(QIODevice::WriteOnly)) return;
        f.write(data);
        if (!f.commit()) return;
        QJsonObject m = MetaCache::load(key).value(QStringLiteral("media")).toObject();
        m.insert(role, file);
        MetaCache::merge(key, { { QStringLiteral("media"), m } });
    });
}

void MetaCache::remove(const QString& key)
{
    if (key.isEmpty()) return;
    QDir(dirFor(key)).removeRecursively();
}
