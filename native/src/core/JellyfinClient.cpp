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
