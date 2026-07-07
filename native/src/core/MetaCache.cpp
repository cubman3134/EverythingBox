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
    d.valid = !d.title.isEmpty();
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

void MetaCache::remove(const QString& key)
{
    if (key.isEmpty()) return;
    QDir(dirFor(key)).removeRecursively();
}
