#include "ServerMusicClient.h"

#include "AppBrand.h"
#include "CoverFetch.h"
#include "MetaCache.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace {

// Rendered from the NetworkError ENUM, never from errorString(): Qt's text embeds the url, and a shelf's
// urls can be signed. ServerMusicClient.h has the rule.
QString transportSentence(QNetworkReply::NetworkError e)
{
    switch (e)
    {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
        return QObject::tr("That server could not be reached from this device.");
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return QObject::tr("That server did not answer in time.");
    case QNetworkReply::SslHandshakeFailedError:
        return QObject::tr("That server's security certificate could not be verified.");
    default:
        return QObject::tr("That server's music could not be read.");
    }
}

} // namespace

ServerMusicClient& ServerMusicClient::instance()
{
    static ServerMusicClient c;
    return c;
}

ServerMusicClient::ServerMusicClient(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
}

void ServerMusicClient::setShelves(const QVector<Shelf>& shelves)
{
    shelves_ = shelves;
    // A shelf that is gone loses its session urls at once: a url signed for a server the user has just
    // disconnected is not something to keep in memory, and nothing can route to it any more anyway.
    for (auto it = trackUrls_.begin(); it != trackUrls_.end();)
        it = has(ServerMusic::sourceOf(it.key())) ? ++it : trackUrls_.erase(it);
}

bool ServerMusicClient::has(const QString& sourceId) const
{
    for (const Shelf& s : shelves_) if (s.id == sourceId) return true;
    return false;
}

QString ServerMusicClient::nameOf(const QString& sourceId) const
{
    for (const Shelf& s : shelves_) if (s.id == sourceId) return s.name;
    return QString();
}

bool ServerMusicClient::shelfFor(const QString& sourceId, Shelf& out) const
{
    for (const Shelf& s : shelves_) if (s.id == sourceId) { out = s; return true; }
    return false;
}

const MusicLibrary::Index& ServerMusicClient::index(const QString& sourceId) const
{
    static const MusicLibrary::Index empty;
    const auto it = caches_.constFind(sourceId);
    return it == caches_.constEnd() ? empty : it->idx;
}

bool ServerMusicClient::artistsLoaded(const QString& sourceId) const
{
    const auto it = caches_.constFind(sourceId);
    return it != caches_.constEnd() && it->artistsLoaded;
}

bool ServerMusicClient::artistLoaded(const QString& artistKey) const
{
    const auto it = caches_.constFind(ServerMusic::sourceOf(artistKey));
    return it != caches_.constEnd() && it->loadedArtists.contains(artistKey);
}

bool ServerMusicClient::albumTracksLoaded(const QString& albumKey) const
{
    const auto it = caches_.constFind(ServerMusic::sourceOf(albumKey));
    return it != caches_.constEnd() && it->loadedAlbums.contains(albumKey);
}

void ServerMusicClient::request(const Shelf& shelf, const QString& path, int budgetMs,
                                std::function<void(const QByteArray&, const Result&)> then)
{
    QNetworkRequest req{ QUrl(shelf.baseUrl + path) };
    req.setRawHeader("Accept", "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    if (!shelf.configHeader.isEmpty()) req.setRawHeader(AppBrand::kConfigHeader, shelf.configHeader);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QVariant::fromValue(QNetworkRequest::SameOriginRedirectPolicy));
    QNetworkReply* reply = nam_->get(req);

    auto* t = new QTimer(reply);
    t->setSingleShot(true);
    connect(t, &QTimer::timeout, reply, [reply] { reply->abort(); });
    t->start(budgetMs > 0 ? budgetMs : 15000);

    connect(reply, &QNetworkReply::finished, this, [reply, t, then] {
        t->stop();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            then({}, Result{ false, transportSentence(reply->error()) });
            return;
        }
        then(reply->readAll(), Result{ true, QString() });
    });
}

void ServerMusicClient::fetchArtists(const QString& sourceId, Done done)
{
    Shelf shelf;
    if (!shelfFor(sourceId, shelf))
    {
        if (done) done(Result{ false, tr("That server is not connected any more.") });
        return;
    }
    const QString tag = QStringLiteral("artists|") + sourceId;
    if (inflight_.contains(tag)) { if (done) waiting_[tag].push_back(done); return; }
    inflight_.insert(tag);
    if (done) waiting_[tag].push_back(done);

    request(shelf, ServerMusic::catalogPath(shelf.catalogId), 12000,
            [this, sourceId, tag](const QByteArray& body, const Result& r) {
        Result out = r;
        if (r.ok)
        {
            bool parsed = false;
            const QVector<ServerMusic::RemoteArtist> arts = ServerMusic::readArtists(body, &parsed);
            if (parsed)
            {
                Cache& c = caches_[sourceId];
                c.idx = ServerMusic::indexOfArtists(sourceId, arts);
                c.loadedArtists.clear();
                c.loadedAlbums.clear();
                c.artistsLoaded = true;
                emit indexChanged(sourceId);
            }
            else out = Result{ false, tr("That server's music could not be read.") };
        }
        inflight_.remove(tag);
        const QVector<Done> owed = waiting_.take(tag);
        for (const Done& d : owed) d(out);
    });
}

void ServerMusicClient::fetchArtistAlbums(const QString& artistKey, Done done)
{
    const ServerMusic::Ref ref = ServerMusic::parse(artistKey);
    Shelf shelf;
    if (!ref.ok || !shelfFor(ref.sourceId, shelf))
    {
        if (done) done(Result{ false, tr("That server is not connected any more.") });
        return;
    }
    const QString tag = QStringLiteral("albums|") + artistKey;
    if (inflight_.contains(tag)) { if (done) waiting_[tag].push_back(done); return; }
    inflight_.insert(tag);
    if (done) waiting_[tag].push_back(done);

    const QString sourceId = ref.sourceId;
    request(shelf, ServerMusic::detailPath(QString::fromLatin1(ServerMusic::kArtistType), ref.remoteId), 12000,
            [this, sourceId, artistKey, tag](const QByteArray& body, const Result& r) {
        Result out = r;
        if (r.ok)
        {
            bool parsed = false;
            const QVector<ServerMusic::RemoteAlbum> albs = ServerMusic::readAlbums(body, &parsed);
            if (parsed)
            {
                Cache& c = caches_[sourceId];
                ServerMusic::fillArtistAlbums(c.idx, sourceId, artistKey, albs);
                c.loadedArtists.insert(artistKey);
                // The cover url is kept BESIDE the index rather than in it, for the same reason the track
                // urls are: the index is copied into queues and item records, and a shelf's url can be
                // signed. ServerMusic::fillArtistAlbums never sees this field.
                for (const ServerMusic::RemoteAlbum& b : albs)
                {
                    const QString k = ServerMusic::qualify(sourceId, ServerMusic::Kind::Album, b.id);
                    if (!k.isEmpty() && !b.coverUrl.isEmpty()) c.albumCoverUrl.insert(k, b.coverUrl);
                }
                emit indexChanged(sourceId);
            }
            else out = Result{ false, tr("That server's music could not be read.") };
        }
        inflight_.remove(tag);
        const QVector<Done> owed = waiting_.take(tag);
        for (const Done& d : owed) d(out);
    });
}

void ServerMusicClient::fetchAlbumTracks(const QString& albumKey, Done done)
{
    const ServerMusic::Ref ref = ServerMusic::parse(albumKey);
    Shelf shelf;
    if (!ref.ok || !shelfFor(ref.sourceId, shelf))
    {
        if (done) done(Result{ false, tr("That server is not connected any more.") });
        return;
    }
    const QString tag = QStringLiteral("tracks|") + albumKey;
    if (inflight_.contains(tag)) { if (done) waiting_[tag].push_back(done); return; }
    inflight_.insert(tag);
    if (done) waiting_[tag].push_back(done);

    const QString sourceId = ref.sourceId;
    request(shelf, ServerMusic::detailPath(QString::fromLatin1(ServerMusic::kAlbumType), ref.remoteId), 12000,
            [this, sourceId, albumKey, tag](const QByteArray& body, const Result& r) {
        Result out = r;
        if (r.ok)
        {
            bool parsed = false;
            const QVector<ServerMusic::RemoteSong> songs = ServerMusic::readSongs(body, &parsed);
            if (parsed)
            {
                Cache& c = caches_[sourceId];
                ServerMusic::fillAlbumTracks(c.idx, sourceId, albumKey, songs);
                // LOADED ONLY WHEN THE INDEX HOLDS THE ALBUM (#368). fillAlbumTracks files the tracks under an
                // album the index already lists and does nothing otherwise — which is a starred track's case
                // after a restart: it fetches its album for the urls below before any level has listed it.
                // Marking that album loaded would tell its level, once listed, that the tracks were already
                // here, and it would open empty.
                if (c.idx.album(albumKey)) c.loadedAlbums.insert(albumKey);
                // THE URLS GO HERE, NOT INTO THE INDEX. See the header.
                for (const ServerMusic::RemoteSong& s : songs)
                {
                    const QString id = ServerMusic::qualify(sourceId, ServerMusic::Kind::Track, s.id);
                    if (!id.isEmpty() && !s.url.isEmpty()) trackUrls_.insert(id, s.url);
                }
                emit indexChanged(sourceId);
            }
            else out = Result{ false, tr("That server's music could not be read.") };
        }
        inflight_.remove(tag);
        const QVector<Done> owed = waiting_.take(tag);
        for (const Done& d : owed) d(out);
    });
}

QString ServerMusicClient::streamUrl(const QString& qualifiedTrackId) const
{
    const ServerMusic::Ref r = ServerMusic::parse(qualifiedTrackId);
    if (!r.ok || !has(r.sourceId)) return QString();
    return trackUrls_.value(qualifiedTrackId);
}

bool ServerMusicClient::hasStreamUrl(const QString& qualifiedTrackId) const
{
    const ServerMusic::Ref r = ServerMusic::parse(qualifiedTrackId);
    return r.ok && has(r.sourceId) && trackUrls_.contains(qualifiedTrackId);   // only non-empty urls go in
}

void ServerMusicClient::prefetchAlbumCover(const QString& albumKey, std::function<void()> then)
{
    // NOTHING LANDED, SO `then` DOES NOT FIRE (#370) — not for a cover already on disk, not for an album the
    // shelf sent no image for, not for a failure. `then` re-renders the level and the re-render re-runs this
    // prefetch, so firing it on any of those paths made the level reload itself every 400 ms for as long as
    // it was on screen. CoverFetch.h has the whole rule.
    if (!MetaCache::imagePath(albumKey, QStringLiteral("cover")).isEmpty()) return;
    if (coverMissing_.contains(albumKey)) return;   // the shelf already said "no picture" this session
    const ServerMusic::Ref ref = ServerMusic::parse(albumKey);
    Shelf shelf;
    if (!ref.ok || !shelfFor(ref.sourceId, shelf)) return;
    const QString url = caches_.value(ref.sourceId).albumCoverUrl.value(albumKey);
    if (url.isEmpty()) return;

    const QString tag = QStringLiteral("cover|") + albumKey;
    if (inflight_.contains(tag)) return;
    inflight_.insert(tag);

    QNetworkRequest req{ QUrl(url) };
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    if (!shelf.configHeader.isEmpty()) req.setRawHeader(AppBrand::kConfigHeader, shelf.configHeader);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QVariant::fromValue(QNetworkRequest::SameOriginRedirectPolicy));
    req.setTransferTimeout(CoverFetch::kTransferTimeoutMs);   // a timeout is Retry, never "no art"
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, albumKey, tag, then] {
        reply->deleteLater();
        inflight_.remove(tag);
        const bool       transportOk = reply->error() == QNetworkReply::NoError;
        const int        status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body   = transportOk ? reply->readAll() : QByteArray();
        switch (CoverFetch::classify(transportOk, status, body))
        {
            case CoverFetch::Answer::Image:
                MetaCache::storeImage(albumKey, QStringLiteral("cover"), QStringLiteral("cover.jpg"),
                                      reply->header(QNetworkRequest::ContentTypeHeader).toString(), body);
                break;
            case CoverFetch::Answer::Absent:
                coverMissing_.insert(albumKey);
                break;
            case CoverFetch::Answer::Retry:
                break;
        }
        if (MetaCache::imagePath(albumKey, QStringLiteral("cover")).isEmpty()) return;   // nothing landed
        if (then) then();
    });
}

QString ServerMusicClient::albumCoverPath(const QString& albumKey) const
{
    return MetaCache::imagePath(albumKey, QStringLiteral("cover"));
}
