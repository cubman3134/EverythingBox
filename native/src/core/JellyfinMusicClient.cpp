#include "JellyfinMusicClient.h"

#include "AppBrand.h"
#include "Jellyfin.h"
#include "JellyfinServerStore.h"
#include "MetaCache.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace {

QString clientName() { return QString::fromLatin1(AppBrand::kName); }
QString deviceName() { return QString::fromLatin1(AppBrand::kDisplayName); }
QString appVersion()
{
    const QString v = QCoreApplication::applicationVersion();
    return v.isEmpty() ? QStringLiteral("0") : v;
}

// The transport sentences. Rendered from the NetworkError ENUM, never from errorString() — Qt's text embeds
// the url, and every request in this file carries a token. JellyfinMusicClient.h has the rule.
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
    case QNetworkReply::AuthenticationRequiredError:
    case QNetworkReply::ContentAccessDenied:
        return QObject::tr("That server refused the sign-in.");
    default:
        return QObject::tr("That server's music could not be read.");
    }
}

bool isAuthError(QNetworkReply::NetworkError e)
{
    return e == QNetworkReply::AuthenticationRequiredError || e == QNetworkReply::ContentAccessDenied;
}

void applyCommonHeaders(QNetworkRequest& req, const QString& token)
{
    req.setRawHeader("Accept", "application/json");
    // The ONE place a token is spelled into a string here, and it goes straight into the request.
    req.setRawHeader("Authorization",
                     Jellyfin::authHeader(clientName(), deviceName(), Settings::deviceId(),
                                          appVersion(), token).toUtf8());
    // A redirect to another host would carry the Authorization header — and therefore the token — to a
    // server the user never configured.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QVariant::fromValue(QNetworkRequest::SameOriginRedirectPolicy));
}

// The server behind a qualified key, only when it is configured AND enabled. Three different situations —
// not a Jellyfin key, a server that was removed, a server switched off — with one honest answer, which is
// what lets a stale row render as unavailable rather than erroring at play.
bool liveServerFor(const QString& qualified, JellyfinServer& out, QString* itemId = nullptr)
{
    const Jellyfin::Ref r = Jellyfin::parse(qualified);
    if (!r.ok) return false;
    if (!JellyfinServerStore::get(r.serverId, out)) return false;
    if (!out.enabled) return false;
    if (itemId) *itemId = r.itemId;
    return true;
}

} // namespace

JellyfinMusicClient& JellyfinMusicClient::instance()
{
    static JellyfinMusicClient c;
    return c;
}

JellyfinMusicClient::JellyfinMusicClient(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
}

JellyfinMusicClient::Cache& JellyfinMusicClient::cacheFor(const QString& serverId)
{
    return caches_[serverId];
}

const MusicLibrary::Index& JellyfinMusicClient::index(const QString& serverId) const
{
    static const MusicLibrary::Index empty;
    const auto it = caches_.constFind(serverId);
    return it == caches_.constEnd() ? empty : it->idx;
}

bool JellyfinMusicClient::artistsLoaded(const QString& serverId) const
{
    const auto it = caches_.constFind(serverId);
    return it != caches_.constEnd() && it->artistsLoaded;
}

bool JellyfinMusicClient::artistLoaded(const QString& artistKey) const
{
    const auto it = caches_.constFind(Jellyfin::serverOf(artistKey));
    return it != caches_.constEnd() && it->loadedArtists.contains(artistKey);
}

bool JellyfinMusicClient::albumTracksLoaded(const QString& albumKey) const
{
    const auto it = caches_.constFind(Jellyfin::serverOf(albumKey));
    return it != caches_.constEnd() && it->loadedAlbums.contains(albumKey);
}

void JellyfinMusicClient::request(const QString& serverId, const QString& pathAndQuery, int budgetMs,
                                  std::function<void(const QByteArray&, const Result&)> then)
{
    JellyfinServer srv;
    if (!JellyfinServerStore::get(serverId, srv) || !srv.enabled)
    {
        then({}, Result{ false, false, tr("That server is not connected any more.") });
        return;
    }
    const QString root = Jellyfin::normalizeRoot(srv.url, srv.allowPlainHttp);
    if (root.isEmpty())
    {
        // checkUrl refused it. Never a downgrade and never a guess — see Jellyfin.h.
        then({}, Result{ false, false, tr("That server's address cannot be used securely.") });
        return;
    }

    QNetworkRequest req{ QUrl(root + pathAndQuery) };
    applyCommonHeaders(req, srv.token);
    QNetworkReply* reply = nam_->get(req);

    // The deadline is the leg's own: abort() makes the reply finish with OperationCanceledError, so there is
    // exactly one completion path however it ends.
    auto* t = new QTimer(reply);
    t->setSingleShot(true);
    connect(t, &QTimer::timeout, reply, [reply] { reply->abort(); });
    t->start(budgetMs > 0 ? budgetMs : 15000);

    connect(reply, &QNetworkReply::finished, this, [reply, t, then] {
        t->stop();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            then({}, Result{ false, isAuthError(reply->error()), transportSentence(reply->error()) });
            return;
        }
        then(reply->readAll(), Result{ true, false, QString() });
    });
}

void JellyfinMusicClient::fetchArtists(const QString& serverId, Done done)
{
    JellyfinServer srv;
    if (serverId.isEmpty() || !JellyfinServerStore::get(serverId, srv))
    {
        if (done) done(Result{ false, false, tr("That server is not connected any more.") });
        return;
    }
    const QString tag = QStringLiteral("artists|") + serverId;
    if (inflight_.contains(tag)) { if (done) waiting_[tag].push_back(done); return; }
    inflight_.insert(tag);
    if (done) waiting_[tag].push_back(done);

    request(serverId, JellyfinMusic::artistsPath(srv.userId), 12000,
            [this, serverId, tag](const QByteArray& body, const Result& r) {
        Result out = r;
        if (r.ok)
        {
            bool parsed = false;
            const QVector<JellyfinMusic::RemoteArtist> arts = JellyfinMusic::readArtists(body, &parsed);
            if (parsed)
            {
                Cache& c = cacheFor(serverId);
                // A LATER FETCH REPLACES THE CACHE WHOLESALE, which is what keeps a bucket that was adopted
                // from an album listing from lingering beside the real one.
                c.idx = JellyfinMusic::indexOfArtists(serverId, arts);
                c.loadedArtists.clear();
                c.loadedAlbums.clear();
                c.artistsLoaded = true;
                emit indexChanged(serverId);
            }
            else
            {
                // Answered with something that is not an item envelope: a proxy's error page. NOT "your
                // server has no music" — see JellyfinMusic.h.
                out = Result{ false, false, tr("That server's music could not be read.") };
            }
        }
        inflight_.remove(tag);
        const QVector<Done> owed = waiting_.take(tag);
        for (const Done& d : owed) d(out);
    });
}

void JellyfinMusicClient::fetchArtistAlbums(const QString& artistKey, Done done)
{
    JellyfinServer srv;
    QString itemId;
    if (!liveServerFor(artistKey, srv, &itemId))
    {
        if (done) done(Result{ false, false, tr("That server is not connected any more.") });
        return;
    }
    const QString serverId = srv.id;
    const QString tag = QStringLiteral("albums|") + artistKey;
    if (inflight_.contains(tag)) { if (done) waiting_[tag].push_back(done); return; }
    inflight_.insert(tag);
    if (done) waiting_[tag].push_back(done);

    request(serverId, JellyfinMusic::albumsPath(srv.userId, itemId), 12000,
            [this, serverId, artistKey, tag](const QByteArray& body, const Result& r) {
        Result out = r;
        if (r.ok)
        {
            bool parsed = false;
            const QVector<JellyfinMusic::RemoteAlbum> albs = JellyfinMusic::readAlbums(body, &parsed);
            if (parsed)
            {
                Cache& c = cacheFor(serverId);
                JellyfinMusic::fillArtistAlbums(c.idx, serverId, artistKey, albs);
                c.loadedArtists.insert(artistKey);
                emit indexChanged(serverId);
            }
            else out = Result{ false, false, tr("That server's music could not be read.") };
        }
        inflight_.remove(tag);
        const QVector<Done> owed = waiting_.take(tag);
        for (const Done& d : owed) d(out);
    });
}

void JellyfinMusicClient::fetchAlbumTracks(const QString& albumKey, Done done)
{
    JellyfinServer srv;
    QString itemId;
    if (!liveServerFor(albumKey, srv, &itemId))
    {
        if (done) done(Result{ false, false, tr("That server is not connected any more.") });
        return;
    }
    const QString serverId = srv.id;
    const QString tag = QStringLiteral("tracks|") + albumKey;
    if (inflight_.contains(tag)) { if (done) waiting_[tag].push_back(done); return; }
    inflight_.insert(tag);
    if (done) waiting_[tag].push_back(done);

    request(serverId, JellyfinMusic::songsPath(srv.userId, itemId), 12000,
            [this, serverId, albumKey, tag](const QByteArray& body, const Result& r) {
        Result out = r;
        if (r.ok)
        {
            bool parsed = false;
            const QVector<JellyfinMusic::RemoteSong> songs = JellyfinMusic::readSongs(body, &parsed);
            if (parsed)
            {
                Cache& c = cacheFor(serverId);
                JellyfinMusic::fillAlbumTracks(c.idx, serverId, albumKey, songs);
                c.loadedAlbums.insert(albumKey);
                emit indexChanged(serverId);
            }
            else out = Result{ false, false, tr("That server's music could not be read.") };
        }
        inflight_.remove(tag);
        const QVector<Done> owed = waiting_.take(tag);
        for (const Done& d : owed) d(out);
    });
}

QString JellyfinMusicClient::streamUrl(const QString& qualifiedTrackId) const
{
    JellyfinServer srv;
    QString itemId;
    if (!liveServerFor(qualifiedTrackId, srv, &itemId)) return QString();
    const QString root = Jellyfin::normalizeRoot(srv.url, srv.allowPlainHttp);
    if (root.isEmpty()) return QString();
    // CARRIES THE TOKEN. Minted here, handed to the player, never stored.
    return JellyfinMusic::audioStreamUrl(root, itemId, srv.token);
}

void JellyfinMusicClient::prefetchAlbumCover(const QString& albumKey, std::function<void()> then)
{
    if (!MetaCache::imagePath(albumKey, QStringLiteral("cover")).isEmpty()) { if (then) then(); return; }
    JellyfinServer srv;
    QString itemId;
    if (!liveServerFor(albumKey, srv, &itemId)) { if (then) then(); return; }
    const QString root = Jellyfin::normalizeRoot(srv.url, srv.allowPlainHttp);
    if (root.isEmpty()) { if (then) then(); return; }

    const QString tag = QStringLiteral("cover|") + albumKey;
    if (inflight_.contains(tag)) return;
    inflight_.insert(tag);

    // The image endpoint takes the same Authorization header the rest of this file uses, so no token goes
    // into this url at all — which is what makes it safe for MetaCache to be handed the album key as its
    // cache key and nothing else.
    QNetworkRequest req{ QUrl(root + QStringLiteral("/Items/") + itemId
                              + QStringLiteral("/Images/Primary?maxHeight=600")) };
    applyCommonHeaders(req, srv.token);
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, albumKey, tag, then] {
        reply->deleteLater();
        inflight_.remove(tag);
        if (reply->error() == QNetworkReply::NoError)
            MetaCache::storeImage(albumKey, QStringLiteral("cover"), QStringLiteral("cover.jpg"),
                                  reply->header(QNetworkRequest::ContentTypeHeader).toString(),
                                  reply->readAll());
        if (then) then();
    });
}

QString JellyfinMusicClient::albumCoverPath(const QString& albumKey) const
{
    return MetaCache::imagePath(albumKey, QStringLiteral("cover"));
}
