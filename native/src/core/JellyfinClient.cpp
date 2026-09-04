#include "JellyfinClient.h"

#include "AppBrand.h"
#include "JellyfinServerStore.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace {

// The four fields Jellyfin's Authorization header wants. `Device` and `DeviceId` are what the server shows
// in its own Dashboard > Devices list, so they are the app and this install — a user looking at their
// server's session list should recognise the box in their living room.
QString clientName()  { return QString::fromLatin1(AppBrand::kName); }
QString deviceName()  { return QString::fromLatin1(AppBrand::kDisplayName); }
QString appVersion()
{
    const QString v = QCoreApplication::applicationVersion();
    return v.isEmpty() ? QStringLiteral("0") : v;
}

// The transport sentences. Rendered from the NetworkError ENUM, never from errorString(), because Qt's own
// text embeds the url — and this app's Jellyfin urls carry a token when they are stream urls, and the
// request that produced this error carried one in a header regardless. JellyfinClient.h has the rule.
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
    case QNetworkReply::ContentNotFoundError:
        return QObject::tr("That address answered, but it is not a Jellyfin server.");
    default:
        return QObject::tr("That server could not be read.");
    }
}

void applyCommonHeaders(QNetworkRequest& req, const QString& token)
{
    req.setRawHeader("Accept", "application/json");
    // THE ONE PLACE A TOKEN IS SPELLED INTO A STRING in this file, and it goes straight into the request.
    // It is not returned, not logged and not put into any message.
    req.setRawHeader("Authorization",
                     Jellyfin::authHeader(clientName(), deviceName(), Settings::deviceId(),
                                          appVersion(), token).toUtf8());
    // A redirect to another host would carry the Authorization header — and therefore the token — to a
    // server the user never configured.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QVariant::fromValue(QNetworkRequest::SameOriginRedirectPolicy));
}

// Arm a deadline on one reply. Returns the timer so the caller can stop it once the reply settles.
//
// A LEG'S DEADLINE IS ITS OWN. abort() makes the reply finish with OperationCanceledError, so there is
// exactly one completion path per leg however it ends — which is what lets the fan-out below count legs
// rather than track states.
QTimer* armDeadline(QNetworkReply* reply, int budgetMs)
{
    auto* t = new QTimer(reply);
    t->setSingleShot(true);
    QObject::connect(t, &QTimer::timeout, reply, [reply] { reply->abort(); });
    t->start(budgetMs > 0 ? budgetMs : 15000);
    return t;
}

} // namespace

JellyfinClient::JellyfinClient(QObject* parent) : QObject(parent) {}

JellyfinClient& JellyfinClient::instance()
{
    static JellyfinClient c;
    return c;
}

QNetworkAccessManager* JellyfinClient::nam()
{
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    return nam_;
}

// ---- Step one: who is this server? ---------------------------------------------------------------------

void JellyfinClient::fetchPublicInfo(const QString& url, bool allowPlainHttp, int budgetMs, InfoDone done)
{
    const QString root = Jellyfin::normalizeRoot(url, allowPlainHttp);
    if (root.isEmpty())
    {
        // Refused before a socket is opened, and the two refusals are told apart because they need different
        // fixes — the plain-HTTP one is a question the user can answer, the malformed one is not.
        const Jellyfin::UrlVerdict v = Jellyfin::checkUrl(url, allowPlainHttp);
        if (done) done({}, v == Jellyfin::UrlVerdict::InsecureRefused
                             ? tr("That address is plain HTTP. Allow it for this server, or use https://.")
                             : tr("That is not a server address."));
        return;
    }

    QNetworkRequest req{ QUrl(root + Jellyfin::publicInfoPath()) };
    applyCommonHeaders(req, QString());                 // unauthenticated: this call is what precedes a token
    QNetworkReply* reply = nam()->get(req);
    QTimer* t = armDeadline(reply, budgetMs);
    connect(reply, &QNetworkReply::finished, this, [reply, t, done] {
        t->stop();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            if (done) done({}, transportSentence(reply->error()));
            return;
        }
        const Jellyfin::PublicInfo info = Jellyfin::readPublicInfo(reply->readAll());
        if (done) done(info, info.ok ? QString()
                                     : tr("That address answered, but it is not a Jellyfin server."));
    });
}

// ---- Step two: the sign-in ------------------------------------------------------------------------------

void JellyfinClient::authenticate(const QString& url, bool allowPlainHttp, const QString& username,
                                  const QString& password, int budgetMs, AuthDone done)
{
    const QString root = Jellyfin::normalizeRoot(url, allowPlainHttp);
    if (root.isEmpty()) { if (done) done({}, tr("That is not a server address.")); return; }

    QNetworkRequest req{ QUrl(root + Jellyfin::authenticatePath()) };
    applyCommonHeaders(req, QString());
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // The body carries the PASSWORD. It is built here, handed to the transport, and not held anywhere.
    QNetworkReply* reply = nam()->post(req, Jellyfin::authenticateBody(username, password));
    QTimer* t = armDeadline(reply, budgetMs);
    connect(reply, &QNetworkReply::finished, this, [reply, t, done] {
        t->stop();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            if (done) done({}, transportSentence(reply->error()));
            return;
        }
        const Jellyfin::AuthResult r = Jellyfin::readAuthResult(reply->readAll());
        if (done) done(r, r.ok ? QString() : tr("That server refused the sign-in."));
    });
}

// ---- The fan-out ----------------------------------------------------------------------------------------

void JellyfinClient::fetchLibrary(int budgetMs, LibraryDone done)
{
    const QList<JellyfinServer> servers = JellyfinServerStore::enabled();
    if (servers.isEmpty()) { if (done) done({}, {}); return; }

    // One slot per leg, filled in place, plus a countdown. The union is built from the SLOTS, in the order
    // the servers were listed — so the merged shelf's order does not depend on which server answered first,
    // which would make it reshuffle between two refreshes for no reason the user can see.
    struct Fan
    {
        QVector<Jellyfin::ServerReply> replies;
        int                            outstanding = 0;
        LibraryDone                    done;
    };
    auto fan = std::make_shared<Fan>();
    fan->replies.resize(servers.size());
    fan->outstanding = int(servers.size());
    fan->done = std::move(done);

    auto settle = [fan] {
        if (--fan->outstanding > 0) return;
        QStringList notes;
        for (const Jellyfin::ServerReply& r : fan->replies)
        {
            const QString n = Jellyfin::unavailableNote(r);
            if (!n.isEmpty()) notes << n;
        }
        if (fan->done) fan->done(Jellyfin::unionOf(fan->replies), notes);
    };

    for (int i = 0; i < servers.size(); ++i)
    {
        const JellyfinServer& s = servers.at(i);
        Jellyfin::ServerReply& slot = fan->replies[i];
        slot.serverId   = s.id;
        slot.serverName = s.name;

        const QString root = Jellyfin::normalizeRoot(s.url, s.allowPlainHttp);
        if (root.isEmpty() || s.userId.isEmpty() || s.token.isEmpty())
        {
            // Configured but not usable — an address that no longer passes the transport check, or a
            // half-finished sign-in. It contributes nothing and says so, exactly like an unreachable box.
            slot.outcome = Jellyfin::Outcome::Failed;
            settle();
            continue;
        }

        QUrl u(root + Jellyfin::itemsPath(s.userId));
        u.setQuery(QStringLiteral("Recursive=true&IncludeItemTypes=Movie,Series&SortBy=SortName"
                                  "&Fields=ProductionYear&EnableTotalRecordCount=false"));
        QNetworkRequest req{ u };
        applyCommonHeaders(req, s.token);
        QNetworkReply* reply = nam()->get(req);
        QTimer* t = armDeadline(reply, budgetMs);
        connect(reply, &QNetworkReply::finished, this, [reply, t, fan, i, settle] {
            t->stop();
            reply->deleteLater();
            Jellyfin::ServerReply& slot = fan->replies[i];
            if (reply->error() == QNetworkReply::OperationCanceledError)
            {
                // The deadline fired. NOT an error the user is shown as a failure: the shelf is drawn from
                // whoever did answer, and this server gets one line saying it was left out of this view.
                slot.outcome = Jellyfin::Outcome::TimedOut;
            }
            else if (reply->error() != QNetworkReply::NoError)
            {
                slot.outcome = Jellyfin::Outcome::Failed;
            }
            else
            {
                bool ok = false;
                slot.items   = Jellyfin::readItems(reply->readAll(), &ok);
                slot.outcome = ok ? Jellyfin::Outcome::Ok : Jellyfin::Outcome::Failed;
            }
            settle();
        });
    }
}

// ---- Playback -------------------------------------------------------------------------------------------

QString JellyfinClient::playUrlFor(const QString& qualifiedId) const
{
    const Jellyfin::Ref ref = Jellyfin::parse(qualifiedId);
    if (!ref.ok) return QString();
    JellyfinServer s;
    if (!JellyfinServerStore::get(ref.serverId, s)) return QString();  // removed: the row is unavailable
    if (!s.enabled) return QString();                                  // switched off: hidden, not deleted
    const QString root = Jellyfin::normalizeRoot(s.url, s.allowPlainHttp);
    if (root.isEmpty() || s.token.isEmpty()) return QString();
    return Jellyfin::streamUrl(root, ref.itemId, s.token);
}

bool JellyfinClient::isAvailable(const QString& qualifiedId) const
{
    const Jellyfin::Ref ref = Jellyfin::parse(qualifiedId);
    if (!ref.ok) return false;
    JellyfinServer s;
    return JellyfinServerStore::get(ref.serverId, s) && s.enabled;
}

// =========================================================================================================
// BROWSE, PLAY AND PROGRESS (issue #83)
// =========================================================================================================

namespace {

// Where a qualified id resolves to, right now, on this device: the owning server, its usable root, and the
// item half. `error` is one of OUR sentences and never contains the url.
//
// THREE DIFFERENT SITUATIONS WITH ONE HONEST ANSWER, exactly as playUrlFor already has it: the id is not
// qualified, its server is not configured any more, or that server is switched off. A caller renders all
// three as "unavailable" rather than erroring at play.
struct Resolved
{
    JellyfinServer server;
    QString        root;
    QString        itemId;
    bool           ok = false;
    QString        error;
};

Resolved resolveRef(const QString& qualifiedId)
{
    Resolved r;
    const Jellyfin::Ref ref = Jellyfin::parse(qualifiedId);
    if (!ref.ok)
    { r.error = QObject::tr("That item does not name a Jellyfin server."); return r; }
    if (!JellyfinServerStore::get(ref.serverId, r.server) || !r.server.enabled)
    { r.error = QObject::tr("That item's Jellyfin server is not set up on this device."); return r; }
    r.root = Jellyfin::normalizeRoot(r.server.url, r.server.allowPlainHttp);
    if (r.root.isEmpty() || r.server.token.isEmpty() || r.server.userId.isEmpty())
    { r.error = QObject::tr("That item's Jellyfin server is not signed in."); return r; }
    r.itemId = ref.itemId;
    r.ok = true;
    return r;
}

} // namespace

// ---- One server, one list of items ----------------------------------------------------------------------
// The shared tail of fetchLibraryItems / fetchSeasons / fetchEpisodes: they differ only in the path and the
// query, and writing the reply handling three times is how the three would come to disagree about what an
// empty answer means.
void JellyfinClient::fetchItemList(const QString& qualifiedId, const QString& path, const QString& query,
                                   int budgetMs, ItemsDone done)
{
    const Resolved r = resolveRef(qualifiedId);
    if (!r.ok) { if (done) done({}, r.error); return; }

    QUrl u(r.root + path);
    u.setQuery(query);
    QNetworkRequest req{ u };
    applyCommonHeaders(req, r.server.token);
    QNetworkReply* reply = nam()->get(req);
    QTimer* t = armDeadline(reply, budgetMs);
    const QString serverId = r.server.id;
    const QString serverName = r.server.name;
    connect(reply, &QNetworkReply::finished, this, [reply, t, done, serverId, serverName] {
        t->stop();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        { if (done) done({}, transportSentence(reply->error())); return; }
        bool ok = false;
        const QVector<Jellyfin::RemoteItem> items = Jellyfin::readItems(reply->readAll(), &ok);
        if (!ok)
        { if (done) done({}, QObject::tr("That server's answer could not be read.")); return; }
        // THROUGH THE UNION, EVEN FOR ONE SERVER. It is what qualifies the ids, and a second qualifying
        // path here would be a second place for a bare id to escape from - the one thing #160 exists to
        // make unspellable.
        Jellyfin::ServerReply sr;
        sr.serverId = serverId;
        sr.serverName = serverName;
        sr.outcome = Jellyfin::Outcome::Ok;
        sr.items = items;
        if (done) done(Jellyfin::unionOf({ sr }), QString());
    });
}

void JellyfinClient::fetchLibraryItems(const QString& qualifiedLibraryId, int budgetMs, ItemsDone done)
{
    const Jellyfin::Ref ref = Jellyfin::parse(qualifiedLibraryId);
    const Resolved r = resolveRef(qualifiedLibraryId);
    if (!r.ok) { if (done) done({}, r.error); return; }
    fetchItemList(qualifiedLibraryId, Jellyfin::itemsPath(r.server.userId),
                  Jellyfin::libraryItemsQuery(ref.itemId), budgetMs, std::move(done));
}

void JellyfinClient::fetchSeasons(const QString& qualifiedSeriesId, int budgetMs, ItemsDone done)
{
    const Jellyfin::Ref ref = Jellyfin::parse(qualifiedSeriesId);
    const Resolved r = resolveRef(qualifiedSeriesId);
    if (!r.ok) { if (done) done({}, r.error); return; }
    fetchItemList(qualifiedSeriesId, Jellyfin::seasonsPath(ref.itemId),
                  Jellyfin::seasonsQuery(r.server.userId), budgetMs, std::move(done));
}

void JellyfinClient::fetchEpisodes(const QString& qualifiedSeriesId, const QString& qualifiedSeasonId,
                                   int budgetMs, ItemsDone done)
{
    const Jellyfin::Ref series = Jellyfin::parse(qualifiedSeriesId);
    const Resolved r = resolveRef(qualifiedSeriesId);
    if (!r.ok) { if (done) done({}, r.error); return; }
    // The season is optional AND is read from its own qualified id - so a season belonging to a different
    // server than the series (which cannot happen, but would be a silent cross-server read if it did)
    // simply contributes no filter rather than addressing the wrong box's season.
    const Jellyfin::Ref season = Jellyfin::parse(qualifiedSeasonId);
    const QString seasonId = (season.ok && season.serverId == series.serverId) ? season.itemId : QString();
    fetchItemList(qualifiedSeriesId, Jellyfin::episodesPath(series.itemId),
                  Jellyfin::episodesQuery(r.server.userId, seasonId), budgetMs, std::move(done));
}

// ---- The two fan-outs -------------------------------------------------------------------------------------
// fetchLibrary's shape exactly: one slot per leg, filled in place, a countdown, and the union built from the
// SLOTS so the merged order does not depend on which server answered first.

void JellyfinClient::fetchLibraries(int budgetMs, LibrariesDone done)
{
    const QList<JellyfinServer> servers = JellyfinServerStore::enabled();
    if (servers.isEmpty()) { if (done) done({}, {}); return; }

    struct Fan
    {
        QVector<Jellyfin::LibraryReply> replies;
        int                             outstanding = 0;
        LibrariesDone                   done;
    };
    auto fan = std::make_shared<Fan>();
    fan->replies.resize(servers.size());
    fan->outstanding = int(servers.size());
    fan->done = std::move(done);

    auto settle = [fan] {
        if (--fan->outstanding > 0) return;
        QStringList notes;
        for (const Jellyfin::LibraryReply& r : fan->replies)
        {
            // unavailableNote takes a ServerReply; the two replies carry the same three fields it reads,
            // so the note is built from a ServerReply view of this one rather than from a second copy of
            // the sentences - which is what keeps the wording identical on both shelves.
            Jellyfin::ServerReply view;
            view.serverId = r.serverId; view.serverName = r.serverName; view.outcome = r.outcome;
            const QString n = Jellyfin::unavailableNote(view);
            if (!n.isEmpty()) notes << n;
        }
        if (fan->done) fan->done(Jellyfin::unionOfLibraries(fan->replies), notes);
    };

    for (int i = 0; i < servers.size(); ++i)
    {
        const JellyfinServer& s = servers.at(i);
        Jellyfin::LibraryReply& slot = fan->replies[i];
        slot.serverId = s.id;
        slot.serverName = s.name;

        const QString root = Jellyfin::normalizeRoot(s.url, s.allowPlainHttp);
        if (root.isEmpty() || s.userId.isEmpty() || s.token.isEmpty())
        { slot.outcome = Jellyfin::Outcome::Failed; settle(); continue; }

        QNetworkRequest req{ QUrl(root + Jellyfin::viewsPath(s.userId)) };
        applyCommonHeaders(req, s.token);
        QNetworkReply* reply = nam()->get(req);
        QTimer* t = armDeadline(reply, budgetMs);
        connect(reply, &QNetworkReply::finished, this, [reply, t, fan, i, settle] {
            t->stop();
            reply->deleteLater();
            Jellyfin::LibraryReply& slot = fan->replies[i];
            if (reply->error() == QNetworkReply::OperationCanceledError)
                slot.outcome = Jellyfin::Outcome::TimedOut;
            else if (reply->error() != QNetworkReply::NoError)
                slot.outcome = Jellyfin::Outcome::Failed;
            else
            {
                bool ok = false;
                slot.libraries = Jellyfin::readViews(reply->readAll(), &ok);
                slot.outcome = ok ? Jellyfin::Outcome::Ok : Jellyfin::Outcome::Failed;
            }
            settle();
        });
    }
}

void JellyfinClient::fetchContinueWatching(int budgetMs, ContinueDone done)
{
    const QList<JellyfinServer> servers = JellyfinServerStore::enabled();
    if (servers.isEmpty()) { if (done) done({}, {}); return; }

    struct Fan
    {
        QVector<Jellyfin::ServerReply> replies;
        int                            outstanding = 0;
        ContinueDone                   done;
    };
    auto fan = std::make_shared<Fan>();
    fan->replies.resize(servers.size());
    fan->outstanding = int(servers.size());
    fan->done = std::move(done);

    auto settle = [fan] {
        if (--fan->outstanding > 0) return;
        QStringList notes;
        for (const Jellyfin::ServerReply& r : fan->replies)
        {
            const QString n = Jellyfin::unavailableNote(r);
            if (!n.isEmpty()) notes << n;
        }
        if (fan->done) fan->done(Jellyfin::unionOf(fan->replies), notes);
    };

    for (int i = 0; i < servers.size(); ++i)
    {
        const JellyfinServer& s = servers.at(i);
        Jellyfin::ServerReply& slot = fan->replies[i];
        slot.serverId = s.id;
        slot.serverName = s.name;

        const QString root = Jellyfin::normalizeRoot(s.url, s.allowPlainHttp);
        if (root.isEmpty() || s.userId.isEmpty() || s.token.isEmpty())
        { slot.outcome = Jellyfin::Outcome::Failed; settle(); continue; }

        QUrl u(root + Jellyfin::resumeItemsPath(s.userId));
        u.setQuery(Jellyfin::resumeItemsQuery());
        QNetworkRequest req{ u };
        applyCommonHeaders(req, s.token);
        QNetworkReply* reply = nam()->get(req);
        QTimer* t = armDeadline(reply, budgetMs);
        connect(reply, &QNetworkReply::finished, this, [reply, t, fan, i, settle] {
            t->stop();
            reply->deleteLater();
            Jellyfin::ServerReply& slot = fan->replies[i];
            if (reply->error() == QNetworkReply::OperationCanceledError)
                slot.outcome = Jellyfin::Outcome::TimedOut;
            else if (reply->error() != QNetworkReply::NoError)
                slot.outcome = Jellyfin::Outcome::Failed;
            else
            {
                bool ok = false;
                slot.items = Jellyfin::readItems(reply->readAll(), &ok);
                slot.outcome = ok ? Jellyfin::Outcome::Ok : Jellyfin::Outcome::Failed;
            }
            settle();
        });
    }
}

// ---- The open ---------------------------------------------------------------------------------------------
// TWO ROUND TRIPS, IN THIS ORDER, and the order is the point: the user's state is read FIRST so that the
// PlaybackInfo request can carry the real start position. Jellyfin uses StartTimeTicks to decide where a
// transcode begins, so asking for playback at zero and then seeking would make the server transcode from
// the beginning of a file the user is fifty minutes into.

void JellyfinClient::prepareOpen(const QString& qualifiedId, double localResumeSeconds,
                                 int audioStreamIndex, int subtitleStreamIndex, int budgetMs, OpenDone done)
{
    const Resolved r = resolveRef(qualifiedId);
    if (!r.ok) { OpenPlan p; p.error = r.error; if (done) done(p); return; }

    const QString root = r.root;
    const QString token = r.server.token;
    const QString userId = r.server.userId;
    const QString itemId = r.itemId;

    // Step one: what does the server say about this item for this user?
    QNetworkRequest req{ QUrl(root + Jellyfin::itemPath(userId, itemId)) };
    applyCommonHeaders(req, token);
    QNetworkReply* reply = nam()->get(req);
    QTimer* t = armDeadline(reply, budgetMs);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, t, done, root, token, userId, itemId, localResumeSeconds,
             audioStreamIndex, subtitleStreamIndex, budgetMs] {
        t->stop();
        reply->deleteLater();
        Jellyfin::UserState state;
        // A FAILED USER-STATE READ IS NOT A FAILED OPEN. state.ok stays false, Jellyfin::resumeSeconds
        // therefore keeps the LOCAL mark, and the playback goes ahead - which is the right trade: the
        // server being briefly unreachable for one read should not stop the film.
        if (reply->error() == QNetworkReply::NoError)
            state = Jellyfin::readUserState(reply->readAll());
        const double start = Jellyfin::resumeSeconds(state, localResumeSeconds);

        // Step two: how may this be played?
        QNetworkRequest pi{ QUrl(root + Jellyfin::playbackInfoPath(itemId)) };
        applyCommonHeaders(pi, token);
        pi.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        const QByteArray body = Jellyfin::playbackInfoBody(userId, Jellyfin::ticksFromSeconds(start),
                                                           audioStreamIndex, subtitleStreamIndex);
        QNetworkReply* pr = nam()->post(pi, body);
        QTimer* pt = armDeadline(pr, budgetMs);
        connect(pr, &QNetworkReply::finished, this,
                [pr, pt, done, root, token, itemId, start, state] {
            pt->stop();
            pr->deleteLater();
            OpenPlan plan;
            plan.resumeSeconds      = start;
            plan.serverKnewPosition = state.ok;
            plan.played             = state.played;
            if (pr->error() != QNetworkReply::NoError)
            { plan.error = transportSentence(pr->error()); if (done) done(plan); return; }

            const Jellyfin::PlaybackChoice choice = Jellyfin::readPlaybackInfo(pr->readAll());
            plan.playSessionId = choice.playSessionId;
            plan.mediaSourceId = choice.mediaSourceId;
            plan.transcoding   = choice.mode == Jellyfin::PlaybackChoice::Mode::Transcode;
            // THE URL, MINTED HERE AND NOWHERE ELSE. It carries the token; the caller hands it to the
            // player and drops it.
            plan.url = Jellyfin::playbackUrl(root, itemId, token, choice);
            if (plan.url.isEmpty())
                plan.error = QObject::tr("That server would not offer a way to play this item.");
            if (done) done(plan);
        });
    });
}

// ---- Progress ------------------------------------------------------------------------------------------

void JellyfinClient::reportProgress(const QString& qualifiedId, Jellyfin::ProgressEvent ev,
                                    double positionSeconds, const QString& playSessionId,
                                    const QString& mediaSourceId)
{
    const Resolved r = resolveRef(qualifiedId);
    if (!r.ok) return;     // a removed or switched-off server: there is nobody to tell, and that is fine

    QNetworkRequest req{ QUrl(r.root + Jellyfin::progressPath(ev)) };
    applyCommonHeaders(req, r.server.token);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = nam()->post(req, Jellyfin::progressBody(r.itemId, playSessionId, mediaSourceId,
                                                                  positionSeconds, ev));
    // FIRE AND FORGET. The reply is consumed only to free it: nothing is read from it, nothing is logged
    // and nothing is shown. See the header for why a failed progress report is not worth a word.
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

// ---- Media segments ------------------------------------------------------------------------------------

void JellyfinClient::fetchMediaSegments(const QString& qualifiedId, int budgetMs, SegmentsDone done)
{
    const Resolved r = resolveRef(qualifiedId);
    if (!r.ok) { if (done) done({}); return; }

    QUrl u(r.root + Jellyfin::mediaSegmentsPath(r.itemId));
    u.setQuery(Jellyfin::mediaSegmentsQuery());
    QNetworkRequest req{ u };
    applyCommonHeaders(req, r.server.token);
    QNetworkReply* reply = nam()->get(req);
    QTimer* t = armDeadline(reply, budgetMs);
    connect(reply, &QNetworkReply::finished, this, [reply, t, done] {
        t->stop();
        reply->deleteLater();
        // NO ERROR CHANNEL, DELIBERATELY. A pre-10.10 server answers 404 here and that is not a fault the
        // user should read about - it is simply one fewer provider tier, exactly like a file with no .edl
        // beside it.
        if (reply->error() != QNetworkReply::NoError) { if (done) done({}); return; }
        if (done) done(Jellyfin::readMediaSegments(reply->readAll()));
    });
}
