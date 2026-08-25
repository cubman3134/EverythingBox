#include "SubsonicClient.h"
#include "AppBrand.h"
#include "MetaCache.h"
#include "MusicArt.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>

namespace {

// The `c` parameter. Servers log it and show it in their own "now playing" surfaces, so it is the app's
// name rather than something generic.
QString clientName() { return QString::fromLatin1(AppBrand::kDisplayName); }

// ==================================================================================================
// TRANSPORT FAILURES, IN OUR OWN WORDS
// ==================================================================================================
// NOT QNetworkReply::errorString(). Qt's text embeds the URL, and for this protocol the URL contains the
// user's token and salt — see the header. These sentences are built from the ENUM and nothing else, so
// there is no path by which a credential can reach a status line, a notification or a log.
QString transportMessage(QNetworkReply::NetworkError err)
{
    switch (err)
    {
        case QNetworkReply::HostNotFoundError:
            return QObject::tr("That server could not be found. Check the address.");
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::RemoteHostClosedError:
            return QObject::tr("That server refused the connection. Is it running?");
        case QNetworkReply::TimeoutError:
        case QNetworkReply::OperationCanceledError:
            return QObject::tr("That server took too long to answer.");
        case QNetworkReply::SslHandshakeFailedError:
            return QObject::tr("The secure connection to that server could not be established.");
        case QNetworkReply::AuthenticationRequiredError:
            return QObject::tr("That server refused the sign-in.");
        case QNetworkReply::ContentNotFoundError:
            return QObject::tr("That server answered, but not like a Subsonic server.");
        default:
            return QObject::tr("Could not reach that server.");
    }
}

} // namespace

SubsonicClient& SubsonicClient::instance()
{
    static SubsonicClient c;
    return c;
}

SubsonicClient::SubsonicClient(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
}

SubsonicClient::Cache& SubsonicClient::cacheFor(const QString& serverId)
{
    return caches_[serverId];
}

const MusicLibrary::Index& SubsonicClient::index(const QString& serverId) const
{
    static const MusicLibrary::Index kEmpty;
    const auto it = caches_.constFind(serverId);
    return it == caches_.constEnd() ? kEmpty : it->idx;
}

bool SubsonicClient::albumTracksLoaded(const QString& albumKey) const
{
    const Subsonic::Ref r = Subsonic::parse(albumKey);
    if (!r.ok) return false;
    const auto it = caches_.constFind(r.serverId);
    return it != caches_.constEnd() && it->loadedAlbums.contains(albumKey);
}

bool SubsonicClient::artistsLoaded(const QString& serverId) const
{
    const auto it = caches_.constFind(serverId);
    return it != caches_.constEnd() && it->artistsLoaded;
}

bool SubsonicClient::artistLoaded(const QString& artistKey) const
{
    const Subsonic::Ref r = Subsonic::parse(artistKey);
    if (!r.ok) return false;
    const auto it = caches_.constFind(r.serverId);
    return it != caches_.constEnd() && it->loadedArtists.contains(artistKey);
}

// ==================================================================================================
// One request
// ==================================================================================================
void SubsonicClient::request(const SubsonicServer& srv, const QString& method,
                             const QList<QPair<QString, QString>>& extra,
                             std::function<void(const Subsonic::Node&, const Result&)> then)
{
    Result bad;
    const QString root = Subsonic::normalizeRoot(srv.url, srv.allowPlainHttp);
    if (root.isEmpty())
    {
        // The explicit choice, surfaced rather than downgraded. checkUrl's verdict says WHICH problem it is;
        // an "InsecureRefused" that read as "could not connect" would send the user looking at their network.
        switch (Subsonic::checkUrl(srv.url, srv.allowPlainHttp))
        {
            case Subsonic::UrlVerdict::InsecureRefused:
                bad.message = tr("That server's address is plain HTTP. Turn on \"Allow plain HTTP\" for it if "
                                 "you really want the password sent unencrypted.");
                break;
            case Subsonic::UrlVerdict::NotHttp:
                bad.message = tr("A music server address has to start with https:// (or http://).");
                break;
            default:
                bad.message = tr("That server's address is not a valid URL.");
                break;
        }
        then(Subsonic::Node{}, bad);
        return;
    }

    // A fresh salt per request — that is what a salt is for, and the reason saltFrom takes the randomness
    // rather than reading it, so the probe can pin the token against a known one.
    const QString salt = Subsonic::saltFrom(QRandomGenerator::global()->generate64());

    QUrl u(root + QStringLiteral("/rest/") + method + QStringLiteral(".view"));
    QUrlQuery q;
    // The auth parameters are built HERE, at the moment of use, out of the store — never held in a member,
    // never copied into a diagnostic, never returned.
    for (const auto& p : Subsonic::authParams(srv.username, srv.password, salt, srv.legacyAuth, clientName()))
        q.addQueryItem(p.first, p.second);
    for (const auto& p : extra) q.addQueryItem(p.first, p.second);
    u.setQuery(q);

    QNetworkRequest req{ u };
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    // SAME ORIGIN. A redirect to another host would carry this query — and therefore the credential — to a
    // server the user never configured.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);

    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, then] {
        reply->deleteLater();
        Result r;
        if (reply->error() != QNetworkReply::NoError)
        {
            // Note what is NOT read here: reply->errorString(). See the header.
            r.message = transportMessage(reply->error());
            then(Subsonic::Node{}, r);
            return;
        }
        bool parsed = false;
        const Subsonic::Node root = Subsonic::parseBody(reply->readAll(), &parsed);
        const Subsonic::Envelope env = Subsonic::envelopeOf(root);
        if (!parsed || env.status == Subsonic::Status::Unparsable)
        {
            // A 200 that is not a subsonic-response: a reverse proxy's HTML error page, a captive portal, a
            // URL that points at something else entirely.
            r.message = tr("That server answered, but not like a Subsonic server. Check the address.");
            then(Subsonic::Node{}, r);
            return;
        }
        if (env.status == Subsonic::Status::Failed)
        {
            // THE 200-WITH-A-FAILURE-INSIDE CASE, which is every Subsonic error. The server's own words are
            // what the user is shown; they are the only thing here that came off the wire.
            r.auth    = Subsonic::isAuthCode(env.code);
            r.message = env.message.isEmpty()
                            ? tr("That server refused the request (error %1).").arg(env.code)
                            : env.message;
            then(Subsonic::Node{}, r);
            return;
        }
        r.ok = true;
        then(root, r);
    });
}

// ==================================================================================================
// The three browse fetches
// ==================================================================================================
void SubsonicClient::fetchArtists(const QString& serverId, Done done)
{
    SubsonicServer srv;
    if (!SubsonicServerStore::get(serverId, srv))
    {
        // Removed out from under the row. An empty level, never a crash.
        if (done) done(Result{ false, false, tr("That music server is no longer set up.") });
        return;
    }
    const QString tag = QStringLiteral("artists|") + serverId;
    if (done) waiting_[tag].push_back(done);
    if (inflight_.contains(tag)) return;      // coalesce: one request, every caller answered
    inflight_.insert(tag);

    request(srv, QStringLiteral("getArtists"), {}, [this, serverId, tag](const Subsonic::Node& root,
                                                                        const Result& res) {
        if (res.ok)
        {
            Cache& c = cacheFor(serverId);
            c.idx = Subsonic::indexOfArtists(serverId, Subsonic::readArtists(root));
            c.albumCoverId.clear();
            c.loadedAlbums.clear();
            c.loadedArtists.clear();
            c.artistsLoaded = true;
            emit indexChanged(serverId);
        }
        inflight_.remove(tag);
        const QVector<Done> cbs = waiting_.take(tag);
        for (const Done& d : cbs) d(res);
    });
}

void SubsonicClient::fetchArtistAlbums(const QString& artistKey, Done done)
{
    const Subsonic::Ref ref = Subsonic::parse(artistKey);
    SubsonicServer srv;
    if (!ref.ok || ref.kind != Subsonic::Kind::Artist || !SubsonicServerStore::get(ref.serverId, srv))
    {
        if (done) done(Result{ false, false, tr("That music server is no longer set up.") });
        return;
    }
    const QString tag = QStringLiteral("artist|") + artistKey;
    if (done) waiting_[tag].push_back(done);
    if (inflight_.contains(tag)) return;
    inflight_.insert(tag);

    const QString serverId = ref.serverId;
    request(srv, QStringLiteral("getArtist"), { { QStringLiteral("id"), ref.remoteId } },
            [this, serverId, artistKey, tag](const Subsonic::Node& root, const Result& res) {
        if (res.ok)
        {
            Cache& c = cacheFor(serverId);
            const QVector<Subsonic::RemoteAlbum> albums = Subsonic::readAlbums(root);
            Subsonic::fillArtistAlbums(c.idx, serverId, artistKey, albums);
            c.loadedArtists.insert(artistKey);
            // The cover art id is kept HERE rather than on the Album, so MusicLibrary::Album stays a struct
            // about music rather than about one supplier's URL scheme.
            for (const Subsonic::RemoteAlbum& b : albums)
            {
                const QString key = Subsonic::qualify(serverId, Subsonic::Kind::Album, b.id);
                if (!key.isEmpty() && !b.coverArt.isEmpty()) c.albumCoverId.insert(key, b.coverArt);
            }
            emit indexChanged(serverId);
        }
        inflight_.remove(tag);
        const QVector<Done> cbs = waiting_.take(tag);
        for (const Done& d : cbs) d(res);
    });
}

void SubsonicClient::fetchAlbumTracks(const QString& albumKey, Done done)
{
    const Subsonic::Ref ref = Subsonic::parse(albumKey);
    SubsonicServer srv;
    if (!ref.ok || ref.kind != Subsonic::Kind::Album || !SubsonicServerStore::get(ref.serverId, srv))
    {
        if (done) done(Result{ false, false, tr("That music server is no longer set up.") });
        return;
    }
    const QString tag = QStringLiteral("album|") + albumKey;
    if (done) waiting_[tag].push_back(done);
    if (inflight_.contains(tag)) return;
    inflight_.insert(tag);

    const QString serverId = ref.serverId;
    request(srv, QStringLiteral("getAlbum"), { { QStringLiteral("id"), ref.remoteId } },
            [this, serverId, albumKey, tag](const Subsonic::Node& root, const Result& res) {
        if (res.ok)
        {
            Cache& c = cacheFor(serverId);
            // adoptAlbum, not fillAlbumTracks: on a COLD cache (a Recents row from a previous session, a
            // favourite, a resumed queue) the index has never heard of this album, and filling an album that
            // is not there is a silent no-op. See Subsonic.h.
            const QVector<Subsonic::RemoteAlbum> info = Subsonic::readAlbums(root);
            if (!info.isEmpty()) Subsonic::adoptAlbum(c.idx, serverId, info.first(), Subsonic::readSongs(root));
            else                 Subsonic::fillAlbumTracks(c.idx, serverId, albumKey, Subsonic::readSongs(root));
            c.loadedAlbums.insert(albumKey);
            if (!c.albumCoverId.contains(albumKey))
            {
                // Some servers only carry coverArt on the album's own reply, not on the artist listing.
                if (const Subsonic::Node* a = root.find(QStringLiteral("album")))
                {
                    const QString ca = a->attr(QStringLiteral("coverArt"));
                    if (!ca.isEmpty()) c.albumCoverId.insert(albumKey, ca);
                }
            }
            emit indexChanged(serverId);
        }
        inflight_.remove(tag);
        const QVector<Done> cbs = waiting_.take(tag);
        for (const Done& d : cbs) d(res);
    });
}

// ==================================================================================================
// Playback and art
// ==================================================================================================
QString SubsonicClient::streamUrl(const QString& qualifiedTrackId) const
{
    const Subsonic::Ref ref = Subsonic::parse(qualifiedTrackId);
    if (!ref.ok || ref.kind != Subsonic::Kind::Track) return QString();
    SubsonicServer srv;
    if (!SubsonicServerStore::get(ref.serverId, srv)) return QString();
    const QString root = Subsonic::normalizeRoot(srv.url, srv.allowPlainHttp);
    if (root.isEmpty()) return QString();

    // THE ONE STABLE SALT in the feature. A stream url is this track's identity to the resume store, the
    // stats store and the queue-to-album map, so it must not change between plays — Subsonic.h sets out the
    // whole argument, including why it costs nothing.
    const QString salt = Subsonic::stableSalt(ref.serverId + QLatin1Char('|') + ref.remoteId);
    QUrl u(root + QStringLiteral("/rest/stream.view"));
    QUrlQuery q;
    for (const auto& p : Subsonic::authParams(srv.username, srv.password, salt, srv.legacyAuth, clientName()))
        q.addQueryItem(p.first, p.second);
    q.addQueryItem(QStringLiteral("id"), ref.remoteId);
    // No maxBitRate and no format: this increment streams whatever the server holds, so the bytes mpv gets
    // are the bytes on the server and gapless/ReplayGain behave as they do for a local file. The mobile
    // bitrate cap is deliberately a later increment (see the report).
    u.setQuery(q);
    return u.toString();
}

void SubsonicClient::prefetchAlbumCover(const QString& albumKey, std::function<void()> then)
{
    const Subsonic::Ref ref = Subsonic::parse(albumKey);
    if (!ref.ok || ref.kind != Subsonic::Kind::Album) return;
    // ALREADY ON DISK: return WITHOUT firing `then`. The callback means "new artwork landed, re-render",
    // and a re-render re-runs this prefetch over the same albums — so firing it for a cached cover would
    // schedule a refresh that schedules a refresh, for ever.
    if (!MetaCache::imagePath(albumKey, QStringLiteral("cover")).isEmpty()) return;
    const auto it = caches_.constFind(ref.serverId);
    if (it == caches_.constEnd()) return;
    const QString coverId = it->albumCoverId.value(albumKey);
    if (coverId.isEmpty()) return;               // this record has no artwork on the server

    SubsonicServer srv;
    if (!SubsonicServerStore::get(ref.serverId, srv)) return;
    const QString root = Subsonic::normalizeRoot(srv.url, srv.allowPlainHttp);
    if (root.isEmpty()) return;

    const QString tag = QStringLiteral("cover|") + albumKey;
    if (inflight_.contains(tag)) return;
    inflight_.insert(tag);

    const QString salt = Subsonic::saltFrom(QRandomGenerator::global()->generate64());
    QUrl u(root + QStringLiteral("/rest/getCoverArt.view"));
    QUrlQuery q;
    for (const auto& p : Subsonic::authParams(srv.username, srv.password, salt, srv.legacyAuth, clientName()))
        q.addQueryItem(p.first, p.second);
    q.addQueryItem(QStringLiteral("id"), coverId);
    q.addQueryItem(QStringLiteral("size"), QStringLiteral("600"));
    u.setQuery(q);

    QNetworkRequest req{ u };
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, albumKey, tag, then] {
        reply->deleteLater();
        inflight_.remove(tag);
        if (reply->error() == QNetworkReply::NoError)
        {
            // storeImage records the FILE NAME under "images", never the url it came from — which is what
            // makes routing the cover through MetaCache safe at all, since that url carries the credential.
            // (Verified in MetaCache.cpp: only `file` is written into the json.)
            MetaCache::storeImage(albumKey, QStringLiteral("cover"),
                                  QStringLiteral("cover.jpg"),
                                  reply->header(QNetworkRequest::ContentTypeHeader).toString(),
                                  reply->readAll());
        }
        if (then) then();
    });
}

QString SubsonicClient::albumCoverPath(const QString& albumKey) const
{
    return MetaCache::imagePath(albumKey, QStringLiteral("cover"));
}

// ==================================================================================================
// MusicSupply — which supplier owns this key
// ==================================================================================================
const MusicLibrary::Index& MusicSupply::indexFor(const QString& key)
{
    const QString server = Subsonic::serverOf(key);
    if (server.isEmpty()) return MusicLibrary::index();      // an unqualified key is local by definition
    return SubsonicClient::instance().index(server);
}

QString MusicSupply::playUrl(const QString& path)
{
    if (!Subsonic::isQualified(path)) return path;           // a local file passes straight through
    return SubsonicClient::instance().streamUrl(path);
}

QString MusicSupply::albumArt(const MusicLibrary::Album& album)
{
    if (Subsonic::isQualified(album.key)) return SubsonicClient::instance().albumCoverPath(album.key);
    return MusicArt::albumCover(album, MusicArt::cacheDir());
}
