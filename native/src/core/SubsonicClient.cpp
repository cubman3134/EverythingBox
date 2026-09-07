#include "SubsonicClient.h"
#include "AppBrand.h"
#include "JellyfinMusicClient.h"   // issue #194 increment 3: the other two suppliers MusicSupply routes to
#include "MetaCache.h"
#include "MusicArt.h"
#include "ServerMusicClient.h"
#include "SubsonicTransport.h"

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
// user's token and salt — see the header. The sentences live in SubsonicTransport.h, built from the ENUM and
// nothing else, and they are in a shared header because the scrobble provider (#193 increment 6) needs the
// same table: two copies would have drifted, and a drift here is the door a call to errorString() gets
// added through.
QString transportMessage(QNetworkReply::NetworkError err) { return SubsonicTransport::message(err); }

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

const MusicLibrary::Index& SubsonicClient::sectionIndex(const QString& serverId) const
{
    static const MusicLibrary::Index kEmpty;
    const auto it = caches_.constFind(serverId);
    return it == caches_.constEnd() ? kEmpty : it->sections;
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
            // Note what is NOT cleared: `sections`. A getArtists replaces this index wholesale, which is
            // what keeps a bucket adoptAlbum invented from lingering beside the real one - and it is exactly
            // why the containers this app invented live in an index of their own. Otherwise an ordinary
            // refresh would destroy the record every starred track row on screen is queued behind.
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
            // adoptArtist, not fillArtistAlbums, for the same cold-cache reason the album level uses
            // adoptAlbum: a STARRED artist row can be opened in a session where the server's artist list was
            // never fetched, and filling an artist that is not in the index is a silent no-op - an
            // expandable row that opens on nothing at all.
            const QVector<Subsonic::RemoteArtist> who = Subsonic::readArtists(root);
            if (!who.isEmpty()) Subsonic::adoptArtist(c.idx, serverId, who.first(), albums);
            else                Subsonic::fillArtistAlbums(c.idx, serverId, artistKey, albums);
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
    // TWO KINDS ARRIVE HERE, AND THE ID SAYS WHICH (#193 increment 6). A playlist renders as an album and is
    // fetched with getPlaylist, so this is one entry point that ROUTES rather than two that would each have
    // to be wired into the album level. Kind::Virtual is refused along with everything else: the starred
    // record is one this app invented, its tracks are already in the index, and there is no endpoint to ask.
    const bool isPlaylist = ref.ok && ref.kind == Subsonic::Kind::Playlist;
    if (!ref.ok || (ref.kind != Subsonic::Kind::Album && !isPlaylist)
        || !SubsonicServerStore::get(ref.serverId, srv))
    {
        if (done) done(Result{ false, false, tr("That music server is no longer set up.") });
        return;
    }
    const QString tag = QStringLiteral("album|") + albumKey;
    if (done) waiting_[tag].push_back(done);
    if (inflight_.contains(tag)) return;
    inflight_.insert(tag);

    const QString serverId = ref.serverId;
    if (isPlaylist)
    {
        request(srv, QStringLiteral("getPlaylist"), { { QStringLiteral("id"), ref.remoteId } },
                [this, serverId, albumKey, tag](const Subsonic::Node& root, const Result& res) {
            if (res.ok)
            {
                Cache& c = cacheFor(serverId);
                // adoptPlaylist, not fillAlbumTracks, for the cold-cache reason adoptAlbum exists: a Recents
                // row can open a playlist in a session where the playlists level was never visited.
                const QVector<Subsonic::RemotePlaylist> info = Subsonic::readPlaylists(root);
                Subsonic::RemotePlaylist p = info.isEmpty() ? Subsonic::RemotePlaylist{} : info.first();
                if (p.id.isEmpty()) p.id = Subsonic::parse(albumKey).remoteId;
                Subsonic::adoptPlaylist(c.sections, serverId, p, Subsonic::readSongs(root));
                c.loadedAlbums.insert(albumKey);
                if (!p.coverArt.isEmpty() && !c.albumCoverId.contains(albumKey))
                    c.albumCoverId.insert(albumKey, p.coverArt);
                emit indexChanged(serverId);
            }
            inflight_.remove(tag);
            const QVector<Done> cbs = waiting_.take(tag);
            for (const Done& d : cbs) d(res);
        });
        return;
    }
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
// The three levels the server already has an answer for (issue #193, increment 6)
// ==================================================================================================
const QVector<MusicLibrary::Album>& SubsonicClient::playlists(const QString& serverId) const
{
    static const QVector<MusicLibrary::Album> kEmpty;
    const auto it = caches_.constFind(serverId);
    return it == caches_.constEnd() ? kEmpty : it->playlists;
}

const Subsonic::Starred& SubsonicClient::starred(const QString& serverId) const
{
    static const Subsonic::Starred kEmpty;
    const auto it = caches_.constFind(serverId);
    return it == caches_.constEnd() ? kEmpty : it->starredRows;
}

const QVector<MusicLibrary::Album>& SubsonicClient::newest(const QString& serverId) const
{
    static const QVector<MusicLibrary::Album> kEmpty;
    const auto it = caches_.constFind(serverId);
    return it == caches_.constEnd() ? kEmpty : it->newestAlbums;
}

bool SubsonicClient::playlistsLoaded(const QString& serverId) const
{
    const auto it = caches_.constFind(serverId);
    return it != caches_.constEnd() && it->playlistsLoaded;
}

bool SubsonicClient::starredLoaded(const QString& serverId) const
{
    const auto it = caches_.constFind(serverId);
    return it != caches_.constEnd() && it->starredLoaded;
}

bool SubsonicClient::newestLoaded(const QString& serverId) const
{
    const auto it = caches_.constFind(serverId);
    return it != caches_.constEnd() && it->newestLoaded;
}

void SubsonicClient::fetchPlaylists(const QString& serverId, Done done)
{
    SubsonicServer srv;
    if (!SubsonicServerStore::get(serverId, srv))
    {
        if (done) done(Result{ false, false, tr("That music server is no longer set up.") });
        return;
    }
    const QString tag = QStringLiteral("playlists|") + serverId;
    if (done) waiting_[tag].push_back(done);
    if (inflight_.contains(tag)) return;
    inflight_.insert(tag);

    request(srv, QStringLiteral("getPlaylists"), {}, [this, serverId, tag](const Subsonic::Node& root,
                                                                          const Result& res) {
        if (res.ok)
        {
            Cache& c = cacheFor(serverId);
            const QVector<Subsonic::RemotePlaylist> pls = Subsonic::readPlaylists(root);
            c.playlists = Subsonic::playlistRows(serverId, pls);
            c.playlistsLoaded = true;
            // The cover id, kept beside the rows for the reason the album listing keeps it: MusicLibrary
            // stays a struct about music rather than about one supplier's url scheme.
            for (const Subsonic::RemotePlaylist& p : pls)
            {
                const QString key = Subsonic::qualify(serverId, Subsonic::Kind::Playlist, p.id);
                if (!key.isEmpty() && !p.coverArt.isEmpty()) c.albumCoverId.insert(key, p.coverArt);
            }
            emit indexChanged(serverId);
        }
        inflight_.remove(tag);
        const QVector<Done> cbs = waiting_.take(tag);
        for (const Done& d : cbs) d(res);
    });
}

void SubsonicClient::fetchStarred(const QString& serverId, Done done)
{
    SubsonicServer srv;
    if (!SubsonicServerStore::get(serverId, srv))
    {
        if (done) done(Result{ false, false, tr("That music server is no longer set up.") });
        return;
    }
    const QString tag = QStringLiteral("starred|") + serverId;
    if (done) waiting_[tag].push_back(done);
    if (inflight_.contains(tag)) return;
    inflight_.insert(tag);

    // getStarred2, the ID3 form - the same choice getArtists/getAlbum make. getStarred (no 2) answers with
    // folder rows, whose ids are in a different namespace from every other id this client holds.
    request(srv, QStringLiteral("getStarred2"), {}, [this, serverId, tag](const Subsonic::Node& root,
                                                                         const Result& res) {
        if (res.ok)
        {
            Cache& c = cacheFor(serverId);
            c.starredRows   = Subsonic::readStarred(serverId, root);
            c.starredLoaded = true;
            // The loose tracks need a record to be queued behind, and the starred artists need buckets to
            // fill their albums into. Both go into the browsing index; the starred ALBUMS deliberately do
            // not (Subsonic.h says why).
            Subsonic::adoptStarred(c.sections, serverId, c.starredRows);
            for (const Subsonic::RemoteAlbum& b : Subsonic::readAlbums(root))
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

void SubsonicClient::fetchNewest(const QString& serverId, Done done)
{
    SubsonicServer srv;
    if (!SubsonicServerStore::get(serverId, srv))
    {
        if (done) done(Result{ false, false, tr("That music server is no longer set up.") });
        return;
    }
    const QString tag = QStringLiteral("newest|") + serverId;
    if (done) waiting_[tag].push_back(done);
    if (inflight_.contains(tag)) return;
    inflight_.insert(tag);

    // A BOUNDED page. getAlbumList2 defaults to 10 and tops out at 500; "recently added" is a glance at what
    // has arrived, not a second copy of the library, and asking for the maximum would make the level slow to
    // draw for a list nobody scrolls to the end of.
    request(srv, QStringLiteral("getAlbumList2"),
            { { QStringLiteral("type"), QStringLiteral("newest") },
              { QStringLiteral("size"), QStringLiteral("100") } },
            [this, serverId, tag](const Subsonic::Node& root, const Result& res) {
        if (res.ok)
        {
            Cache& c = cacheFor(serverId);
            const QVector<Subsonic::RemoteAlbum> albums = Subsonic::readAlbums(root);
            c.newestAlbums = Subsonic::albumRows(serverId, albums);
            c.newestLoaded = true;
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

void SubsonicClient::setStarred(const QString& qualifiedId, bool starred, Done done)
{
    const Subsonic::Ref ref = Subsonic::parse(qualifiedId);
    const QList<QPair<QString, QString>> params =
        ref.ok ? Subsonic::starParams(ref.kind, ref.remoteId) : QList<QPair<QString, QString>>{};
    SubsonicServer srv;
    if (params.isEmpty() || !SubsonicServerStore::get(ref.serverId, srv))
    {
        // Not something this server can be told about. The local favourite stands; nothing is said, because
        // there is nothing the user could do about a row that was never a server's to begin with.
        if (done) done(Result{ false, false, QString() });
        return;
    }
    // NOT COALESCED, and deliberately not: star and unstar are the same target with opposite meanings, so
    // folding a second press onto the first in-flight request would drop the press that reversed it and
    // leave the server holding the state the user just undid.
    request(srv, starred ? QStringLiteral("star") : QStringLiteral("unstar"), params,
            [done](const Subsonic::Node&, const Result& res) { if (done) done(res); });
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
    // Subsonic::streamPath(), not a literal: #203's reader has to recognise this url again to name the track
    // it came from, and two spellings of the endpoint is exactly how that stops working.
    QUrl u(root + Subsonic::streamPath());
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
    // A PLAYLIST HAS A COVER TOO (#193 increment 6): servers that render one for a playlist serve it through
    // the same getCoverArt endpoint under the same kind of id, so this is one route rather than two.
    if (!ref.ok || (ref.kind != Subsonic::Kind::Album && ref.kind != Subsonic::Kind::Playlist)) return;
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
// FOUR SUPPLIERS NOW, AND THE ROUTE IS STILL STRUCTURAL (issue #194, increment 3). Each family's own
// parser answers "is this mine", and the families are mutually unreadable BY CONSTRUCTION rather than by
// the order of these tests — Subsonic.h, Jellyfin.h and ServerMusic.h each state the property, and
// probe_musicsources drives every pair of them. So the order below is for cheapness, never for correctness:
// no key can satisfy two of these, and an unqualified key is local by definition because a qualified one
// carries its server.
const MusicLibrary::Index& MusicSupply::indexFor(const QString& key)
{
    // TWO INDEXES PER SUBSONIC SERVER, and the key says which - structurally, by its Kind, never by a
    // lookup that could miss. A Playlist or a Virtual container is one this app invented and lives in the
    // sections index; everything else the server itself minted and lives in the browse index. See
    // Subsonic.h at adoptStarred for why they are apart.
    const Subsonic::Ref ref = Subsonic::parse(key);
    if (ref.ok)
        return (ref.kind == Subsonic::Kind::Playlist || ref.kind == Subsonic::Kind::Virtual)
                   ? SubsonicClient::instance().sectionIndex(ref.serverId)
                   : SubsonicClient::instance().index(ref.serverId);
    const QString jf = Jellyfin::serverOf(key);
    if (!jf.isEmpty()) return JellyfinMusicClient::instance().index(jf);
    const QString shelf = ServerMusic::sourceOf(key);
    if (!shelf.isEmpty()) return ServerMusicClient::instance().index(shelf);
    return MusicLibrary::index();
}

QString MusicSupply::playUrl(const QString& path)
{
    // THE ONE PLACE A CREDENTIAL ENTERS A QUEUE, for every supplier that has one. A local file passes
    // straight through; each remote id becomes a url minted at this moment and stored nowhere.
    if (Subsonic::isQualified(path)) return SubsonicClient::instance().streamUrl(path);
    if (Jellyfin::isQualified(path))  return JellyfinMusicClient::instance().streamUrl(path);
    if (ServerMusic::isQualified(path)) return ServerMusicClient::instance().streamUrl(path);
    return path;
}

QString MusicSupply::albumArt(const MusicLibrary::Album& album)
{
    if (Subsonic::isQualified(album.key)) return SubsonicClient::instance().albumCoverPath(album.key);
    if (Jellyfin::isQualified(album.key)) return JellyfinMusicClient::instance().albumCoverPath(album.key);
    if (ServerMusic::isQualified(album.key)) return ServerMusicClient::instance().albumCoverPath(album.key);
    return MusicArt::albumCover(album, MusicArt::cacheDir());
}
