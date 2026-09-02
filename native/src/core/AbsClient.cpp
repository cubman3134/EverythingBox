#include "AbsClient.h"
#include "AppBrand.h"
#include "MetaCache.h"
#include "RemoteAudiobook.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

// ==================================================================================================
// TRANSPORT FAILURES, IN OUR OWN WORDS
// ==================================================================================================
// NOT QNetworkReply::errorString(). Qt's text embeds the URL, and for the two token-bearing URLs this
// feature mints that would put the credential into a status line — see the header. These sentences are
// built from the ENUM and nothing else, so there is no path by which a token can reach a message.
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
        case QNetworkReply::ContentAccessDenied:
        case QNetworkReply::ContentOperationNotPermittedError:
            return QObject::tr("That server refused the sign-in. Add the server again to sign in afresh.");
        case QNetworkReply::ContentNotFoundError:
            return QObject::tr("That server answered, but not like an Audiobookshelf server. "
                               "Check the address.");
        default:
            return QObject::tr("Could not reach that server.");
    }
}

// The message for a URL this client will not send a request to. The verdict says WHICH problem it is; an
// "InsecureRefused" that read as "could not connect" would send the user looking at their network.
QString urlMessage(Abs::UrlVerdict v)
{
    switch (v)
    {
        case Abs::UrlVerdict::InsecureRefused:
            return QObject::tr("That server's address is plain HTTP. Turn on \"Allow plain HTTP\" for it if "
                               "you really want your sign-in sent unencrypted.");
        case Abs::UrlVerdict::NotHttp:
            return QObject::tr("An audiobook server address has to start with https:// (or http://).");
        default:
            return QObject::tr("That server's address is not a valid URL.");
    }
}

} // namespace

AbsClient& AbsClient::instance()
{
    static AbsClient c;
    return c;
}

AbsClient::AbsClient(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
}

AbsClient::ServerCache& AbsClient::cacheFor(const QString& serverId)
{
    return caches_[serverId];
}

// ==================================================================================================
// One request
// ==================================================================================================
void AbsClient::request(const AbsServer& srv, const QString& path, const QByteArray& verb,
                        const QByteArray& body, std::function<void(const QByteArray&, const Result&)> then)
{
    const QString root = Abs::normalizeRoot(srv.url, srv.allowPlainHttp);
    if (root.isEmpty())
    {
        Result bad;
        bad.message = urlMessage(Abs::checkUrl(srv.url, srv.allowPlainHttp));
        then(QByteArray(), bad);
        return;
    }

    QNetworkRequest req{ QUrl(root + path) };
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // THE TOKEN, AS A HEADER. Built here, at the moment of use, out of the store — never held in a member,
    // never copied into a diagnostic, never returned. See the header for why it is not in the query.
    if (!srv.token.isEmpty())
        req.setRawHeader("Authorization", QByteArray("Bearer ") + srv.token.toUtf8());
    // SAME ORIGIN. A redirect to another host would carry this Authorization header to a server the user
    // never configured.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);

    QNetworkReply* reply = verb.isEmpty() ? nam_->get(req)
                                          : nam_->sendCustomRequest(req, verb, body);
    connect(reply, &QNetworkReply::finished, this, [reply, then] {
        reply->deleteLater();
        Result r;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError)
        {
            // Note what is NOT read here: reply->errorString(). See the header.
            // 401/403 is the one failure a retry cannot fix, and the only one whose remedy is a sentence
            // about signing in again rather than about the network.
            r.auth    = (status == 401 || status == 403);
            r.message = transportMessage(reply->error());
            then(QByteArray(), r);
            return;
        }
        r.ok = true;
        then(reply->readAll(), r);
    });
}

// ==================================================================================================
// Signing in
// ==================================================================================================
void AbsClient::login(const QString& url, const QString& username, const QString& password,
                      bool allowPlainHttp, const QString& displayName, LoggedIn done)
{
    AbsServer draft;
    draft.name           = displayName;
    draft.url            = url;
    draft.username       = username;
    draft.allowPlainHttp = allowPlainHttp;

    const QByteArray body = QJsonDocument(Abs::loginBody(username, password))
                                .toJson(QJsonDocument::Compact);
    // The password exists in exactly this local, for exactly this call. Nothing captures it: the lambda
    // below closes over `draft` (which has no password field to hold one) and over nothing else typed.
    request(draft, Abs::loginPath(), QByteArray("POST"), body,
            [this, draft, done](const QByteArray& reply, const Result& res) mutable {
        if (!res.ok) { if (done) done(res, AbsServer{}); return; }
        const Abs::Login in = Abs::readLogin(reply);
        if (!in.ok)
        {
            Result bad; bad.auth = true;
            bad.message = tr("That server did not accept those details.");
            if (done) done(bad, AbsServer{});
            return;
        }
        draft.token = in.token;
        if (!in.username.isEmpty()) draft.username = in.username;

        // ...and now, WITH the token, ask the server who it is. The id it publishes (if any) is what the
        // saved row is qualified by for the rest of its life — Audiobookshelf.h explains why this is
        // settled once, at add time, and never rewritten.
        request(draft, Abs::authorizePath(), QByteArray("POST"), QByteArray("{}"),
                [draft, done](const QByteArray& who, const Result& r2) mutable {
            if (r2.ok)
            {
                const QString sid = Abs::serverIdOf(who);
                if (!sid.isEmpty()) draft.id = sid;
            }
            // A server that answers /login but not /api/authorize is still signed in; the id simply stays
            // the uuid AbsServerStore will mint. Failing the whole add over an identity we have a perfectly
            // good substitute for would be refusing to work for no user-visible gain.
            Result ok; ok.ok = true;
            if (done) done(ok, draft);
        });
    });
}

// ==================================================================================================
// Browse
// ==================================================================================================
void AbsClient::fetchLibraries(const QString& serverId, Done done)
{
    AbsServer srv;
    if (!AbsServerStore::get(serverId, srv))
    {
        // Removed out from under the row. An empty level, never a crash.
        if (done) done(Result{ false, false, tr("That audiobook server is no longer set up.") });
        return;
    }
    const QString tag = QStringLiteral("libs|") + serverId;
    if (done) waiting_[tag].push_back(done);
    if (inflight_.contains(tag)) return;      // coalesce: one request, every caller answered
    inflight_.insert(tag);

    request(srv, Abs::librariesPath(), QByteArray(), QByteArray(),
            [this, serverId, tag](const QByteArray& body, const Result& res) {
        if (res.ok)
        {
            ServerCache& c = cacheFor(serverId);
            c.libs = Abs::readLibraries(body);
            c.libsLoaded = true;
            emit cacheChanged(serverId);
        }
        inflight_.remove(tag);
        const QVector<Done> cbs = waiting_.take(tag);
        for (const Done& d : cbs) d(res);
    });
}

// One library, in three requests that land independently: the items, the series and the authors. The
// callback fires when the ITEMS land — that is the one a level cannot be drawn without — and the other two
// re-emit cacheChanged when they arrive, so the Series and Authors doors appear a moment later rather than
// holding the whole level behind the slowest of three.
void AbsClient::fetchLibrary(const QString& qualifiedLibraryId, Done done)
{
    const Abs::Ref ref = Abs::parse(qualifiedLibraryId);
    AbsServer srv;
    if (!ref.ok || !AbsServerStore::get(ref.serverId, srv))
    {
        if (done) done(Result{ false, false, tr("That audiobook server is no longer set up.") });
        return;
    }
    const QString tag = QStringLiteral("lib|") + qualifiedLibraryId;
    if (done) waiting_[tag].push_back(done);
    if (inflight_.contains(tag)) return;
    inflight_.insert(tag);

    const QString serverId = ref.serverId;
    const QString libId    = ref.itemId;

    // The library's NAME, out of the server listing if it is already cached. It is what the level's title
    // reads, and a level reached directly (a Back into a saved route) has no listing above it to take one
    // from — so it is filled in here rather than passed down.
    {
        ServerCache& c = cacheFor(serverId);
        LibraryCache& lc = c.libraries[qualifiedLibraryId];
        if (lc.name.isEmpty())
            for (const Abs::Library& l : c.libs)
                if (l.id == libId) { lc.name = l.name; break; }
    }

    request(srv, Abs::libraryItemsPath(libId, /*limit*/ 0), QByteArray(), QByteArray(),
            [this, serverId, qualifiedLibraryId, tag](const QByteArray& body, const Result& res) {
        if (res.ok)
        {
            LibraryCache& lc = cacheFor(serverId).libraries[qualifiedLibraryId];
            lc.items  = Abs::readLibraryItems(body);
            lc.loaded = true;
            emit cacheChanged(serverId);
        }
        inflight_.remove(tag);
        const QVector<Done> cbs = waiting_.take(tag);
        for (const Done& d : cbs) d(res);
    });

    request(srv, Abs::librarySeriesPath(libId), QByteArray(), QByteArray(),
            [this, serverId, qualifiedLibraryId](const QByteArray& body, const Result& res) {
        if (!res.ok) return;   // a library with no series endpoint simply has no Series door
        cacheFor(serverId).libraries[qualifiedLibraryId].series = Abs::readSeries(body);
        emit cacheChanged(serverId);
    });

    request(srv, Abs::libraryAuthorsPath(libId), QByteArray(), QByteArray(),
            [this, serverId, qualifiedLibraryId](const QByteArray& body, const Result& res) {
        if (!res.ok) return;
        cacheFor(serverId).libraries[qualifiedLibraryId].authors = Abs::readAuthors(body);
        emit cacheChanged(serverId);
    });
}

void AbsClient::fetchItem(const QString& qualifiedItemId, Done done)
{
    const Abs::Ref ref = Abs::parse(qualifiedItemId);
    AbsServer srv;
    if (!ref.ok || !AbsServerStore::get(ref.serverId, srv))
    {
        if (done) done(Result{ false, false, tr("That audiobook server is no longer set up.") });
        return;
    }
    const QString tag = QStringLiteral("item|") + qualifiedItemId;
    if (done) waiting_[tag].push_back(done);
    if (inflight_.contains(tag)) return;
    inflight_.insert(tag);

    const QString serverId = ref.serverId;
    request(srv, Abs::itemPath(ref.itemId), QByteArray(), QByteArray(),
            [this, serverId, qualifiedItemId, tag](const QByteArray& body, const Result& res) {
        Result r = res;
        if (r.ok)
        {
            const Abs::ItemDetail d = Abs::readItem(body);
            if (d.ok) { cacheFor(serverId).items.insert(qualifiedItemId, d); emit cacheChanged(serverId); }
            else { r.ok = false; r.message = tr("That server answered, but not like an Audiobookshelf "
                                                "server. Check the address."); }
        }
        inflight_.remove(tag);
        const QVector<Done> cbs = waiting_.take(tag);
        for (const Done& d : cbs) d(r);
    });
}

QVector<Abs::Library> AbsClient::libraries(const QString& serverId) const
{
    const auto it = caches_.constFind(serverId);
    return it == caches_.constEnd() ? QVector<Abs::Library>{} : it->libs;
}

bool AbsClient::librariesLoaded(const QString& serverId) const
{
    const auto it = caches_.constFind(serverId);
    return it != caches_.constEnd() && it->libsLoaded;
}

QVector<Abs::Item> AbsClient::libraryItems(const QString& qualifiedLibraryId) const
{
    const auto it = caches_.constFind(Abs::serverOf(qualifiedLibraryId));
    if (it == caches_.constEnd()) return {};
    return it->libraries.value(qualifiedLibraryId).items;
}

QVector<Abs::SeriesRow> AbsClient::series(const QString& qualifiedLibraryId) const
{
    const auto it = caches_.constFind(Abs::serverOf(qualifiedLibraryId));
    if (it == caches_.constEnd()) return {};
    return it->libraries.value(qualifiedLibraryId).series;
}

QVector<Abs::AuthorRow> AbsClient::authors(const QString& qualifiedLibraryId) const
{
    const auto it = caches_.constFind(Abs::serverOf(qualifiedLibraryId));
    if (it == caches_.constEnd()) return {};
    return it->libraries.value(qualifiedLibraryId).authors;
}

bool AbsClient::libraryLoaded(const QString& qualifiedLibraryId) const
{
    const auto it = caches_.constFind(Abs::serverOf(qualifiedLibraryId));
    return it != caches_.constEnd() && it->libraries.value(qualifiedLibraryId).loaded;
}

QString AbsClient::libraryNameOf(const QString& qualifiedLibraryId) const
{
    const auto it = caches_.constFind(Abs::serverOf(qualifiedLibraryId));
    if (it == caches_.constEnd()) return QString();
    const QString cached = it->libraries.value(qualifiedLibraryId).name;
    if (!cached.isEmpty()) return cached;
    const QString libId = Abs::itemOf(qualifiedLibraryId);
    for (const Abs::Library& l : it->libs) if (l.id == libId) return l.name;
    return QString();
}

// A series' / an author's books, filtered out of the library listing. BY NAME, because that is the only
// join the listing offers: a library item carries `seriesName` and `authorName`, not the ids the series
// and author endpoints are keyed by. The caller passes the NAME as the bucket, and browse/AbsCatalogs.cpp
// keys its rows by the name for exactly this reason — the first live drive of this feature keyed them by
// the server's `ser_1` and every series opened an empty shelf over a library that plainly had the books.
QVector<Abs::Item> AbsClient::seriesBooks(const QString& qualifiedLibraryId, const QString& seriesId) const
{
    QVector<Abs::Item> out;
    for (const Abs::Item& b : libraryItems(qualifiedLibraryId))
        if (!b.series.isEmpty() && b.series == seriesId) out.push_back(b);
    return out;
}

QVector<Abs::Item> AbsClient::authorBooks(const QString& qualifiedLibraryId, const QString& authorId) const
{
    QVector<Abs::Item> out;
    for (const Abs::Item& b : libraryItems(qualifiedLibraryId))
        if (!b.author.isEmpty() && b.author == authorId) out.push_back(b);
    return out;
}

Abs::ItemDetail AbsClient::item(const QString& qualifiedItemId) const
{
    const auto it = caches_.constFind(Abs::serverOf(qualifiedItemId));
    if (it == caches_.constEnd()) return {};
    return it->items.value(Abs::itemIdOf(qualifiedItemId));
}

bool AbsClient::itemLoaded(const QString& qualifiedItemId) const
{
    return item(qualifiedItemId).ok;
}

// ==================================================================================================
// Play
// ==================================================================================================
void AbsClient::openSession(const QString& qualifiedId, Opened done)
{
    const Abs::Ref ref = Abs::parse(qualifiedId);
    AbsServer srv;
    if (!ref.ok || !AbsServerStore::get(ref.serverId, srv))
    {
        if (done) done(Result{ false, false, tr("That audiobook server is no longer set up.") },
                       Abs::Session{});
        return;
    }
    // The device the server files this session under. A fixed, honest description rather than something
    // unique per play: Audiobookshelf shows it in its own sessions list, and a client that invented a new
    // device on every press would fill that list with strangers.
    QJsonObject body;
    body.insert(QStringLiteral("deviceInfo"),
                QJsonObject{ { QStringLiteral("clientName"), QString::fromLatin1(AppBrand::kDisplayName) } });
    // No forceDirectPlay and no forceTranscode: the server decides how to serve its own files, which is the
    // same rule #83 sets for Jellyfin. What comes back is what we play.
    request(srv, Abs::playPath(ref.itemId, ref.episodeId), QByteArray("POST"),
            QJsonDocument(body).toJson(QJsonDocument::Compact),
            [this, qualifiedId, done](const QByteArray& reply, const Result& res) {
        if (!res.ok) { if (done) done(res, Abs::Session{}); return; }
        const Abs::Session s = Abs::readPlaySession(reply);
        if (!s.ok)
        {
            Result bad;
            bad.message = tr("That server did not give any audio for this item.");
            if (done) done(bad, Abs::Session{});
            return;
        }
        sessions_.insert(qualifiedId, s);
        if (done) done(res, s);
    });
}

Abs::Session AbsClient::session(const QString& qualifiedId) const
{
    return sessions_.value(qualifiedId);
}

QString AbsClient::partStreamUrl(const QString& qualifiedId, int partIndex) const
{
    const Abs::Session s = sessions_.value(qualifiedId);
    if (!s.ok || partIndex < 0 || partIndex >= s.tracks.size()) return QString();
    const Abs::Ref ref = Abs::parse(qualifiedId);
    AbsServer srv;
    if (!ref.ok || !AbsServerStore::get(ref.serverId, srv)) return QString();
    const QString root = Abs::normalizeRoot(srv.url, srv.allowPlainHttp);
    if (root.isEmpty()) return QString();
    // Minted HERE and handed straight to the player. Not returned to anything that writes: the whole of
    // RemoteAudiobook.h is the argument for why a queue holds names and never links.
    return Abs::streamUrl(root, s.tracks.at(partIndex).contentUrl, srv.token);
}

// ==================================================================================================
// Progress
// ==================================================================================================
void AbsClient::reportProgress(const QString& qualifiedId, double currentTime, double duration, bool force)
{
    const Abs::Ref ref = Abs::parse(qualifiedId);
    AbsServer srv;
    if (!ref.ok || !AbsServerStore::get(ref.serverId, srv)) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Reported& last = reported_[qualifiedId];
    if (!force && !Abs::shouldReport(last.ever, last.pos, last.atMs, currentTime, now)) return;
    // Recorded BEFORE the request rather than in its callback: two ticks a millisecond apart would
    // otherwise both pass the gate and send two PATCHes for one position.
    last.ever = true; last.pos = currentTime; last.atMs = now;

    const QByteArray body = QJsonDocument(Abs::progressBody(currentTime, duration))
                                .toJson(QJsonDocument::Compact);
    request(srv, Abs::progressPath(ref.itemId, ref.episodeId), QByteArray("PATCH"), body,
            [](const QByteArray&, const Result&) {
        // Nothing to do with the answer. A failed report is not worth a message: the listener is listening,
        // the next tick will try again, and a toast saying "could not tell the server where you are" every
        // ten seconds on a flaky link would be the whole experience of a flaky link.
    });
}

void AbsClient::fetchProgress(const QString& qualifiedId, GotProgress done)
{
    const Abs::Ref ref = Abs::parse(qualifiedId);
    AbsServer srv;
    if (!ref.ok || !AbsServerStore::get(ref.serverId, srv))
    {
        if (done) done(Result{ false, false, tr("That audiobook server is no longer set up.") },
                       Abs::Progress{});
        return;
    }
    request(srv, Abs::progressPath(ref.itemId, ref.episodeId), QByteArray(), QByteArray(),
            [done](const QByteArray& body, const Result& res) {
        // A 404 here means "this user has never opened that item", which is an ANSWER and not a failure —
        // readProgress carries it as `found == false` and the caller starts at the top of the book.
        if (done) done(res, res.ok ? Abs::readProgress(body) : Abs::Progress{});
    });
}

// ==================================================================================================
// Art
// ==================================================================================================
void AbsClient::prefetchCover(const QString& qualifiedId, std::function<void()> then)
{
    const QString itemKey = Abs::itemIdOf(qualifiedId);
    if (itemKey.isEmpty()) return;
    // ALREADY ON DISK: return WITHOUT firing `then`. The callback means "new artwork landed, re-render",
    // and a re-render re-runs this prefetch over the same rows — so firing it for a cached cover would
    // schedule a refresh that schedules a refresh, for ever. (SubsonicClient makes the same note.)
    if (!MetaCache::imagePath(itemKey, QStringLiteral("cover")).isEmpty()) return;

    const Abs::Ref ref = Abs::parse(itemKey);
    AbsServer srv;
    if (!ref.ok || !AbsServerStore::get(ref.serverId, srv)) return;
    const QString root = Abs::normalizeRoot(srv.url, srv.allowPlainHttp);
    if (root.isEmpty()) return;

    const QString tag = QStringLiteral("cover|") + itemKey;
    if (inflight_.contains(tag)) return;
    // ...and ASKED ONCE. A server that answers a cover request with something MetaCache cannot store — an
    // empty body, a 200 that is really an error page — leaves imagePath() empty, so the "already on disk"
    // test above says no on the next pass and this refetches. With `then` wired to a debounced re-render
    // (which is what a browse level does with it) that is a fetch every few hundred milliseconds, for ever,
    // against a server that has already said no. Found on the first live drive of this feature, where the
    // fixture's empty cover produced exactly that loop.
    if (coverMissing_.contains(itemKey)) return;
    inflight_.insert(tag);

    QNetworkRequest req{ QUrl(Abs::coverUrl(root, ref.itemId, srv.token)) };
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, itemKey, tag, then] {
        reply->deleteLater();
        inflight_.remove(tag);
        if (reply->error() == QNetworkReply::NoError)
        {
            // storeImage records the FILE NAME, never the url it came from — which is what makes routing
            // the cover through MetaCache safe at all, since that url carries the token.
            MetaCache::storeImage(itemKey, QStringLiteral("cover"), QStringLiteral("cover.jpg"),
                                  reply->header(QNetworkRequest::ContentTypeHeader).toString(),
                                  reply->readAll());
        }
        // DID ANYTHING LAND? `then` means "new artwork is on disk, re-render", and firing it when nothing
        // was stored is what makes the loop above possible. A cover that did not land is remembered as
        // absent for this session rather than asked for again on every repaint.
        if (MetaCache::imagePath(itemKey, QStringLiteral("cover")).isEmpty())
        {
            coverMissing_.insert(itemKey);
            return;
        }
        if (then) then();
    });
}

QString AbsClient::coverPath(const QString& qualifiedId) const
{
    const QString itemKey = Abs::itemIdOf(qualifiedId);
    return itemKey.isEmpty() ? QString() : MetaCache::imagePath(itemKey, QStringLiteral("cover"));
}

// ==================================================================================================
// AbsSupply — which supplier owns this key
// ==================================================================================================
QString AbsSupply::bookIdOf(const QString& queueEntryOrToken)
{
    // A multi-file book's queue entry is a PART TOKEN whose book key is the qualified id.
    if (RemoteAudiobook::isPartToken(queueEntryOrToken))
    {
        const QString key = RemoteAudiobook::bookKeyOfToken(queueEntryOrToken);
        return Abs::isQualified(key) ? key : QString();
    }
    // A single-file book or a podcast episode is played under its qualified id directly.
    return Abs::isQualified(queueEntryOrToken) ? queueEntryOrToken : QString();
}

bool AbsSupply::isAbsEntry(const QString& queueEntryOrToken)
{
    return !bookIdOf(queueEntryOrToken).isEmpty();
}
