#include "SubsonicScrobbleProvider.h"
#include "AppBrand.h"
#include "Subsonic.h"
#include "SubsonicServerStore.h"
#include "SubsonicTransport.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>

namespace {

// The `c` parameter. Servers log it and show it in their own "now playing" surfaces — which is half the
// point of reporting back at all, so it is the app's name rather than something generic.
QString clientName() { return QString::fromLatin1(AppBrand::kDisplayName); }

// The message a FAILED envelope produces: the server's own words, or the code when it gave none. Nothing
// here is built from the request — see the header, and note that this string is persisted verbatim as
// ScrobbleQueue's lastError.
QString envelopeMessage(const Subsonic::Envelope& env)
{
    if (!env.message.isEmpty()) return env.message;
    return QObject::tr("That server refused the request (error %1).").arg(env.code);
}

} // namespace

QString SubsonicScrobbleProvider::idPrefix() { return QStringLiteral("subsonic:"); }

QString SubsonicScrobbleProvider::idFor(const QString& serverId) { return idPrefix() + serverId; }

SubsonicScrobbleProvider::SubsonicScrobbleProvider(const QString& serverId, QObject* parent)
    : QObject(parent), serverId_(serverId)
{
    nam_ = new QNetworkAccessManager(this);
}

QString SubsonicScrobbleProvider::id() const { return idFor(serverId_); }

QString SubsonicScrobbleProvider::displayName() const
{
    SubsonicServer srv;
    if (SubsonicServerStore::get(serverId_, srv) && !srv.name.trimmed().isEmpty()) return srv.name;
    return tr("your music server");
}

bool SubsonicScrobbleProvider::configured() const
{
    // THE STORE IS THE ANSWER. A server the user removed stops being configured the moment they remove it,
    // so this provider stops pumping, stops being offered listens and drops out of the status line with
    // nothing having to hunt it down. A blank url or username is not configured either: the request would
    // be built and refused, and a refusal the user cannot act on is worse than silence.
    SubsonicServer srv;
    if (!SubsonicServerStore::get(serverId_, srv)) return false;
    if (srv.url.trimmed().isEmpty() || srv.username.trimmed().isEmpty()) return false;
    return !Subsonic::normalizeRoot(srv.url, srv.allowPlainHttp).isEmpty();
}

bool SubsonicScrobbleProvider::accepts(const Scrobble::Track& track) const
{
    // The play's own id says which server served it, and only this server's plays can be described to this
    // server. Note what is NOT used: the artist and title. A track with the same name on two servers is two
    // different records, and matching on the words would report a play of one against the other.
    const Subsonic::Ref r = Subsonic::parse(track.sourceId);
    return r.ok && r.kind == Subsonic::Kind::Track && r.serverId == serverId_;
}

bool SubsonicScrobbleProvider::ownsSource(const Scrobble::Track& track) const { return accepts(track); }

void SubsonicScrobbleProvider::nowPlaying(const Scrobble::Track& track)
{
    if (!accepts(track) || !configured()) return;
    const Subsonic::Ref r = Subsonic::parse(track.sourceId);
    // NO TIME on a now-playing hint: it is about this moment by definition, every server treats it as such,
    // and a hint carrying the moment the track STARTED would be a stale claim as soon as it arrived.
    call(QStringLiteral("scrobble"),
         Subsonic::scrobbleParams({ r.remoteId }, {}, /*submission=*/false),
         // NO CALLBACK. Ephemeral, never queued, never retried — ScrobbleProvider.h states the rule and the
         // seam's shape is what enforces it.
         {});
}

void SubsonicScrobbleProvider::submit(const QVector<Scrobble::Play>& plays,
                                      std::function<void(ScrobbleResult)> cb)
{
    QVector<QString> ids;
    QVector<qint64>  times;
    for (const Scrobble::Play& p : plays)
    {
        const Subsonic::Ref r = Subsonic::parse(p.track.sourceId);
        // A row that is not this server's cannot be delivered here and never will be. REJECTED rather than
        // kept, because keeping it jams every listen behind it for ever — the queue's one fatal failure.
        // accepts() means this should be unreachable; it is handled anyway, because "unreachable" is what
        // every jammed queue was before it jammed.
        if (!r.ok || r.kind != Subsonic::Kind::Track || r.serverId != serverId_) continue;
        ids.push_back(r.remoteId);
        times.push_back(p.listenedAt);
    }
    if (ids.isEmpty())
    {
        if (cb) cb(ScrobbleResult::rejected(tr("Those listens are not this server's to record.")));
        return;
    }
    if (!configured())
    {
        if (cb) cb(ScrobbleResult::retryable(tr("That music server is no longer set up.")));
        return;
    }
    if (ids.size() != plays.size())
    {
        // A MIXED batch. Every id above IS deliverable, but the orchestrator drops exactly `plays.size()`
        // rows off the front when it is told Ok — so accepting here would silently discard the rows that
        // were skipped. Reject instead: the queue keeps moving and the foreign rows are dropped with it,
        // which is the outcome the queue's own rule already prescribes for rows that can never be sent.
        if (cb) cb(ScrobbleResult::rejected(tr("Those listens are not this server's to record.")));
        return;
    }
    call(QStringLiteral("scrobble"), Subsonic::scrobbleParams(ids, times, /*submission=*/true), cb);
}

void SubsonicScrobbleProvider::love(const Scrobble::Track& track, bool loved,
                                    std::function<void(ScrobbleResult)> cb)
{
    if (!accepts(track))
    {
        // Not this server's track. SILENT rather than an error: the local favourite stands, and there is
        // nothing for the user to fix about a row this server never held.
        if (cb) cb(ScrobbleResult::ok());
        return;
    }
    if (!configured())
    {
        if (cb) cb(ScrobbleResult::retryable(tr("That music server is no longer set up.")));
        return;
    }
    const Subsonic::Ref r = Subsonic::parse(track.sourceId);
    call(loved ? QStringLiteral("star") : QStringLiteral("unstar"),
         Subsonic::starParams(r.kind, r.remoteId), cb);
}

void SubsonicScrobbleProvider::call(const QString& method, const QList<QPair<QString, QString>>& extra,
                                    std::function<void(ScrobbleResult)> cb)
{
    SubsonicServer srv;
    if (extra.isEmpty() || !SubsonicServerStore::get(serverId_, srv))
    {
        if (cb) cb(ScrobbleResult::retryable(tr("That music server is no longer set up.")));
        return;
    }
    const QString root = Subsonic::normalizeRoot(srv.url, srv.allowPlainHttp);
    if (root.isEmpty())
    {
        // Plain HTTP without the per-server opt-in, or an address that is not a URL. RETRYABLE, not
        // rejected: the listens are kept, and they land the moment the address is corrected in Settings.
        if (cb) cb(ScrobbleResult::retryable(tr("That server's address cannot be used as it is set up.")));
        return;
    }

    QUrl u(root + QStringLiteral("/rest/") + method + QStringLiteral(".view"));
    QUrlQuery q;
    // Built HERE, at the moment of use, out of the store — never held in a member, never copied into a
    // diagnostic, never returned. A fresh salt per request, which is what a salt is for.
    const QString salt = Subsonic::saltFrom(QRandomGenerator::global()->generate64());
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
    connect(reply, &QNetworkReply::finished, this, [reply, cb] {
        reply->deleteLater();
        if (!cb) return;                      // the nowPlaying case: nothing is owed, nothing is read
        if (reply->error() != QNetworkReply::NoError)
        {
            // Note what is NOT read here: reply->errorString(). See the header.
            cb(ScrobbleResult::retryable(SubsonicTransport::message(reply->error())));
            return;
        }
        bool parsed = false;
        const Subsonic::Node root = Subsonic::parseBody(reply->readAll(), &parsed);
        const Subsonic::Envelope env = Subsonic::envelopeOf(root);
        // A 200 IS NOT A SUCCESS. Every Subsonic error arrives as one, with the failure inside — the trap
        // Subsonic.h opens with, and the reason a status-only client reports delivering listens it lost.
        switch (Subsonic::fateOf(env))
        {
            case Subsonic::Fate::Ok:        cb(ScrobbleResult::ok()); return;
            case Subsonic::Fate::Auth:      cb(ScrobbleResult::auth(envelopeMessage(env))); return;
            case Subsonic::Fate::Rejected:  cb(ScrobbleResult::rejected(envelopeMessage(env))); return;
            case Subsonic::Fate::Retryable: break;
        }
        cb(ScrobbleResult::retryable(env.status == Subsonic::Status::Unparsable
                                         ? tr("That server answered, but not like a Subsonic server.")
                                         : envelopeMessage(env)));
    });
}
