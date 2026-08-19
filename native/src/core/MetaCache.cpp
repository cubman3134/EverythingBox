#include "MetaCache.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "MetaOverrides.h"  // the user's corrections composite over everything this cache holds (issue #24)

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
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
#include <QSettings>
#include <QThreadPool>
#include <QUrl>
#include <algorithm>
#include <atomic>

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

const QStringList& imageExts()
{
    static const QStringList known = { QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
                                       QStringLiteral("webp"), QStringLiteral("gif"), QStringLiteral("svg") };
    return known;
}

// Image file extension from the url path, else from a content type, else jpg.
QString imageExt(const QUrl& url, const QString& contentType)
{
    const QString fromPath = QFileInfo(url.path()).suffix().toLower();
    if (imageExts().contains(fromPath)) return fromPath;
    if (contentType.contains(QStringLiteral("png")))  return QStringLiteral("png");
    if (contentType.contains(QStringLiteral("webp"))) return QStringLiteral("webp");
    if (contentType.contains(QStringLiteral("svg")))  return QStringLiteral("svg");
    if (contentType.contains(QStringLiteral("gif")))  return QStringLiteral("gif");
    return QStringLiteral("jpg");
}

// Same ini the other stores use; here it holds the cap ("cache/imageCapMB") and a cheap running byte
// total ("cache/imageBytes") so browsing never pays a directory sweep just to know if eviction is due.
QSettings& ini()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile), QSettings::IniFormat);
    return s;
}
const QString kImageBytesKey = QStringLiteral("cache/imageBytes");

// Keys whose art must never be evicted (downloaded/favorited items). Installed by the app at startup;
// unset (headless probes, early calls) means nothing is pinned.
std::function<QSet<QString>()>& pinnedProvider()
{
    static std::function<QSet<QString>()> p;
    return p;
}

QString hashedName(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
}

// LRU-ish recency for the cap: bump a served file's mtime the first time it's used each run, so
// eviction targets art the user hasn't seen for the longest time, not merely the oldest-fetched.
void touchServed(const QString& absPath, const QString& tag)
{
    static QSet<QString> touched;
    if (touched.contains(tag)) return;
    touched.insert(tag);
    QFile f(absPath);
    if (f.open(QIODevice::ReadWrite))
        f.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
}

// The scan-based sweep itself: walk every bundle folder for the true image byte total, evict
// least-recently-served unpinned thumbs until 90% of the cap (or until candidates run out — detail art
// and pinned bundles are never evicted, so an over-cap cache may stay over-cap). Filesystem-only, NO
// QSettings access: safe to run on a worker thread (QFile/QDir are reentrant; the ini writes stay on
// the GUI thread with every other ini() use).
struct SweepResult { int evicted = 0; qint64 total = 0; };
SweepResult sweepImageCache(qint64 capBytes, const QSet<QString>& pinnedDirs)
{
    struct Candidate { QString file; QString dir; qint64 size; QDateTime served; };
    QVector<Candidate> candidates;
    SweepResult r;
    QDirIterator dirs(metaRoot(), QDir::Dirs | QDir::NoDotAndDotDot);
    while (dirs.hasNext())
    {
        const QString dir = dirs.next();
        const bool pinned = pinnedDirs.contains(dirs.fileName());
        const QFileInfoList files = QDir(dir).entryInfoList(QDir::Files);
        for (const QFileInfo& fi : files)
        {
            if (!imageExts().contains(fi.suffix().toLower())) continue;
            r.total += fi.size();
            if (!pinned && fi.fileName().startsWith(QStringLiteral("thumb.")))
                candidates.push_back({ fi.absoluteFilePath(), dir, fi.size(), fi.lastModified() });
        }
    }

    if (r.total > capBytes)
    {
        // Least recently served first; stop at 90% of the cap so the next poster doesn't re-trigger.
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.served < b.served; });
        const qint64 target = capBytes * 9 / 10;
        for (const Candidate& c : candidates)
        {
            if (r.total <= target) break;
            if (!QFile::remove(c.file)) continue;
            r.total -= c.size;
            ++r.evicted;
            // Drop the bundle's "images" record too (directly — merge() would stamp fresh savedAt onto
            // a bundle the user hasn't actually touched). The rest of the bundle stays: item/detail
            // text is tiny and the thumb re-caches on the next scroll-past.
            QFile mf(c.dir + QStringLiteral("/meta.json"));
            if (!mf.open(QIODevice::ReadOnly)) continue;
            QJsonObject obj = QJsonDocument::fromJson(mf.readAll()).object();
            mf.close();
            QJsonObject images = obj.value(QStringLiteral("images")).toObject();
            images.remove(QStringLiteral("thumb"));
            obj.insert(QStringLiteral("images"), images);
            QSaveFile out(c.dir + QStringLiteral("/meta.json"));
            if (!out.open(QIODevice::WriteOnly)) continue;
            out.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
            out.commit();
        }
    }
    return r;
}

// Resolve the pinned keys (must run on the app thread — the provider reads app-side stores) into the
// sha1 folder names the sweep compares against.
QSet<QString> pinnedDirsNow()
{
    QSet<QString> dirs;
    if (pinnedProvider())
    {
        const QSet<QString> pinnedKeys = pinnedProvider()();
        for (const QString& k : pinnedKeys) dirs.insert(hashedName(k));
    }
    return dirs;
}

// The next running-total value that may trigger another sweep. After a sweep the disk truth stays as-is
// for a cache whose UNEVICTABLE art (pinned bundles + non-thumb roles) alone exceeds the cap — so
// re-arming at the bare cap made EVERY subsequent image store rescan the whole metadata tree (2-3k
// folders, ~300ms, on the GUI thread: the themed shelf's per-step selection stall). One slack step past
// the post-sweep truth keeps the cap live (a healthy cache still re-arms at the cap: 90% + 10% slack)
// while an over-cap-by-construction cache sweeps once per slack-worth of new downloads, not per file.
qint64 sweepFloor(qint64 capBytes, qint64 postSweepTotal)
{
    // Slack is RELATIVE only (cap/10). An absolute floor (an earlier 32MB term) exceeded small caps: any cap
    // under 320MB re-armed above its own limit, letting steady state oscillate over the configured cap. With
    // cap/10, a healthy cache (postSweep <= 90% of cap) re-arms exactly at the cap; an over-cap-by-
    // construction cache sweeps once per cap/10 of new downloads instead of once per file.
    return std::max(capBytes, postSweepTotal + capBytes / 10);
}
const QString kSweepFloorKey = QStringLiteral("cache/imageSweepFloor");
std::atomic_bool g_sweepRunning{ false };

// Called after each committed image write: accrue the running total, and only when it crosses both the
// cap and the last sweep's floor (see sweepFloor) run the real scan-based eviction — on a worker
// thread, because the scan walks the whole metadata tree and this is called from network-reply
// handlers on the GUI thread (it was the per-selection-step ~300ms freeze of the themed game shelf).
void maybeEnforceCap(qint64 justWrote)
{
    const qint64 cap = MetaCache::imageCacheCapBytes();
    if (cap <= 0) return;
    const qint64 approx = ini().value(kImageBytesKey).toLongLong() + justWrote;
    ini().setValue(kImageBytesKey, approx);
    if (approx <= cap || approx < ini().value(kSweepFloorKey).toLongLong()) return;
    if (g_sweepRunning.exchange(true)) return;     // one sweep at a time; the total re-syncs when it lands
    const QSet<QString> pinnedDirs = pinnedDirsNow(); // resolved HERE: the provider is not thread-safe
    QThreadPool::globalInstance()->start([cap, pinnedDirs] {
        const SweepResult r = sweepImageCache(cap, pinnedDirs);
        // ini() lives on the app thread (shared QSettings) — marshal the bookkeeping back there.
        if (auto* app = QCoreApplication::instance())
            QMetaObject::invokeMethod(app, [cap, r] {
                ini().setValue(kImageBytesKey, r.total);
                ini().setValue(kSweepFloorKey, sweepFloor(cap, r.total));
                g_sweepRunning.store(false);
            }, Qt::QueuedConnection);
        else
            g_sweepRunning.store(false);
    });
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
    return metaRoot() + QLatin1Char('/') + hashedName(key);
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
    // Prefetch the best image per role so posters/logos/box/fanart render with no network next time. An
    // on-demand role (a game manual — megabytes, issue #89) is deliberately NOT fetched here: its URL is now
    // recorded in the bundle above, but the file is pulled only when the user opens it (fetchManual). This is
    // the hover / console-entry prefetch path, so excluding it here is what keeps a manual off every hover.
    for (auto it = art.images.constBegin(); it != art.images.constEnd(); ++it)
        if (!it.value().isEmpty() && !isOnDemandRole(it.key())) cacheImage(key, it.key(), it.value().first());
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
    // The composited "miximage" card (issue #90) is generated art, not a scraped URL role, so it lives only
    // in the "images" record — surface it as a selectable role here so a theme's `role: "miximage"` resolves
    // it exactly like a provider role. Absent (never generated) -> the role is simply not there, and a theme
    // binding to it falls through to its own fallback. Generation is a display-path concern (Miximage::
    // ensureForKey), deliberately kept out of this read so a background prefetch that only calls loadArt does
    // not composite the whole library.
    const QString mix = imagePath(key, QStringLiteral("miximage"));
    if (!mix.isEmpty()) a.images.insert(QStringLiteral("miximage"), QStringList{ mix });
    // Offline-first for the trailer + theme song too: a cached local file plays before the remote url.
    const QString localVideo = mediaPath(key, QStringLiteral("video0"));
    if (!localVideo.isEmpty() && !a.videos.contains(localVideo)) a.videos.prepend(localVideo);
    const QString localAudio = mediaPath(key, QStringLiteral("audio0"));
    if (!localAudio.isEmpty() && !a.audio.contains(localAudio)) a.audio.prepend(localAudio);
    MetaOverrides::applyTo(MetaOverrides::get(key), a); // the user's correction outranks anything scraped
    return a;
}

MediaDetail MetaCache::cachedDetail(const QString& key)
{
    MediaDetail d = cachedDetailScraped(key);
    // The override layer composites LAST, over everything the providers wrote (issue #24). It is the only
    // layer here that cannot be re-derived, so it is also the only one that must outrank a fresh scrape.
    MetaOverrides::applyTo(MetaOverrides::get(key), d);
    return d;
}

MediaDetail MetaCache::cachedDetailScraped(const QString& key)
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
    if (!QFile::exists(abs)) return {};
    touchServed(abs, key + QLatin1Char('|') + role); // recency for the cap's LRU-ish eviction
    return abs;
}

QString MetaCache::scrapedImage(const QString& key, const QString& url)
{
    // #183: the composited miximage card (issue #90) is the preferred tile art wherever it exists — this is
    // the host-fed tile-role pick every grid/shelf goes through (displayImage → here), so preferring it here
    // is what turns the grid into the uniform "shelf" the compositor is for. It is opt-in BY PRESENCE, not by
    // theme: Miximage::ensureForKey only records this role once an item has the game art to composite from, so
    // an unscraped item (or one whose card has not been generated yet) has no "miximage" role and falls
    // straight through to exactly today's tile — its own thumb, then poster, then the remote url. Absent a
    // card, nothing changes; there is no blank slot and no forced look change.
    QString img = imagePath(key, QStringLiteral("miximage"));
    if (img.isEmpty()) img = imagePath(key, QStringLiteral("thumb"));
    if (img.isEmpty()) img = imagePath(key, QStringLiteral("poster"));
    return img.isEmpty() ? url : img;
}

QString MetaCache::fixedImageRole(const QString& url)
{
    return QStringLiteral("fix-")
           + QString::fromLatin1(
               QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5).toHex().left(16));
}

QString MetaCache::displayImage(const QString& key, const QString& url)
{
    // The corrected poster outranks the cached file: that cached file IS the wrong art being corrected, so
    // consulting it first would leave every grid tile showing the mis-scrape the user just fixed.
    const QString fixed = MetaOverrides::get(key).image;
    if (fixed.isEmpty()) return scrapedImage(key, url);
    // Offline-first is not suspended for it, though — that is a stated principle of this cache, and the
    // correction was the one tile it did not hold to. Serve the corrected poster's own cached copy when we
    // have one, and fetch it (async, idempotent, no-op offline) the first time it is seen. Under its OWN
    // role: cacheImage's "already cached" guard reads imagePath(key, role), and thumb/poster hold the wrong
    // art, so under those roles the corrected poster was never downloaded and the fixed item rendered as
    // NOTHING offline — where the wrong-but-cached art at least used to render.
    const QString role = fixedImageRole(fixed);
    const QString local = imagePath(key, role);
    if (!local.isEmpty()) return local;
    cacheImage(key, role, fixed);
    return fixed;
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
        // Same persist path as a poster the UI fetched itself; the post-redirect url guesses the extension.
        MetaCache::storeImage(key, role, reply->url().toString(),
                              reply->header(QNetworkRequest::ContentTypeHeader).toString(),
                              reply->readAll());
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
    maybeEnforceCap(data.size()); // every persisted poster counts toward the disk cap
}

void MetaCache::recordLocalImage(const QString& key, const QString& role, const QString& fileName)
{
    if (key.isEmpty() || role.isEmpty() || fileName.isEmpty()) return;
    // Same record shape storeImage writes, minus the byte total: this file was composited from art already
    // on disk (the miximage card), so it is not new bytes to count against the download cache cap.
    QJsonObject images = load(key).value(QStringLiteral("images")).toObject();
    images.insert(role, fileName);
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

// ---- On-demand game manual (issue #89) -------------------------------------------------------------------

bool MetaCache::isOnDemandRole(const QString& role)
{
    // The one role saveArt must never eager-download: a manual is a PDF/CBZ scan of megabytes, not the
    // kilobytes of an artwork role, so it is fetched only on explicit open. Kept as a tiny pure classifier
    // so the prefetch exclusion is a single directly-assertable fact rather than an inline condition.
    return role == QStringLiteral("manual");
}

// File extension for a stored manual: honour the source's own container when it is one we can open
// (pdf via PdfView, cbz/cbr/zip via ComicView), else default to pdf — the format ScreenScraper serves.
static QString manualExt(const QString& url)
{
    static const QStringList known = { QStringLiteral("pdf"), QStringLiteral("cbz"),
                                       QStringLiteral("cbr"), QStringLiteral("zip") };
    const QString suffix = QFileInfo(QUrl(url).path()).suffix().toLower();
    return known.contains(suffix) ? suffix : QStringLiteral("pdf");
}

QString MetaCache::manualPath(const QString& key)
{
    if (key.isEmpty()) return {};
    const QString file = load(key).value(QStringLiteral("manual")).toString();
    if (file.isEmpty()) return {};
    const QString abs = dirFor(key) + QLatin1Char('/') + file;
    return QFile::exists(abs) ? abs : QString();
}

void MetaCache::fetchManual(const QString& key, const QString& url,
                            std::function<void(qint64, qint64)> onProgress,
                            std::function<void(const QString&)> onDone)
{
    auto finish = [onDone = std::move(onDone)](const QString& path) { if (onDone) onDone(path); };
    if (key.isEmpty()) { finish(QString()); return; }
    // Already on disk (a previous open, this or an earlier session): hand the file back with no network.
    const QString have = manualPath(key);
    if (!have.isEmpty()) { finish(have); return; }
    if (url.isEmpty()) { finish(QString()); return; }

    QNetworkRequest req{ QUrl(url) };
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam()->get(req);
    if (onProgress)
        QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                         [onProgress = std::move(onProgress)](qint64 r, qint64 t) { onProgress(r, t); });
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, key, url, finish] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) { finish(QString()); return; }
        const QByteArray data = reply->readAll();
        if (data.isEmpty()) { finish(QString()); return; }
        const QString file = QStringLiteral("manual.") + manualExt(url);
        QDir().mkpath(MetaCache::dirFor(key));
        QSaveFile f(MetaCache::dirFor(key) + QLatin1Char('/') + file);
        if (!f.open(QIODevice::WriteOnly)) { finish(QString()); return; }
        f.write(data);
        if (!f.commit()) { finish(QString()); return; }
        // Record the on-disk name so manualPath()/a later open resolve it (merge keeps the rest of the bundle).
        MetaCache::merge(key, { { QStringLiteral("manual"), file } });
        finish(MetaCache::dirFor(key) + QLatin1Char('/') + file);
    });
}

QVector<QPair<QString, MediaArt>> MetaCache::allArt()
{
    QVector<QPair<QString, MediaArt>> out;
    QDirIterator dirs(metaRoot(), QDir::Dirs | QDir::NoDotAndDotDot);
    while (dirs.hasNext())
    {
        const QString dir = dirs.next();
        QFile f(dir + QStringLiteral("/meta.json"));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
        // The folder name is sha1(key); the key we resolve art with is stored in the file itself.
        const QString key = obj.value(QStringLiteral("key")).toString();
        if (key.isEmpty()) continue;
        const QJsonObject item = obj.value(QStringLiteral("item")).toObject();
        const QJsonObject det  = obj.value(QStringLiteral("detail")).toObject();
        const QString title = det.value(QStringLiteral("title"))
                                  .toString(item.value(QStringLiteral("title")).toString());
        out.push_back(qMakePair(title, loadArt(key)));
    }
    return out;
}

void MetaCache::remove(const QString& key)
{
    if (key.isEmpty()) return;
    QDir(dirFor(key)).removeRecursively();
}

void MetaCache::setPinnedKeysProvider(std::function<QSet<QString>()> provider)
{
    pinnedProvider() = std::move(provider);
}

qint64 MetaCache::imageCacheCapBytes()
{
    return ini().value(QStringLiteral("cache/imageCapMB"), 500).toLongLong() * 1024 * 1024;
}

int MetaCache::enforceImageCacheCap(qint64 capBytes)
{
    if (capBytes < 0) capBytes = imageCacheCapBytes();
    if (capBytes <= 0) return 0; // unlimited

    // Synchronous by contract (Settings' "clear now" + probe_meta assert on the return value). The scan +
    // eviction is the shared sweep core; only thumbs of unpinned bundles are candidates — they land for
    // every scrolled page, while detail art only lands for items the user deliberately opened. (Bundle
    // folders are sha1(key), so the pinned keys map straight to folder names — no meta.json reads.)
    // Claim the one-sweep-at-a-time guard so this can't interleave file removals + meta.json rewrites with a
    // background sweep (and so the worker's queued bookkeeping can't later clobber the fresh totals below).
    // A background sweep already in flight makes this a no-op — its result lands moments later anyway.
    if (g_sweepRunning.exchange(true)) return 0;
    const SweepResult r = sweepImageCache(capBytes, pinnedDirsNow());
    g_sweepRunning.store(false);
    ini().setValue(kImageBytesKey, r.total); // re-sync the cheap running total with the disk truth
    ini().setValue(kSweepFloorKey, sweepFloor(capBytes, r.total)); // re-arm the store-path trigger (see sweepFloor)
    ini().sync();
    return r.evicted;
}
