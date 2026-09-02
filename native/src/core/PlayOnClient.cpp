#include "PlayOnClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace
{
    // The peer's own words for a refusal, when it sent any. Falls back to a generic line rather than to the
    // HTTP status, which means nothing to a user standing in front of a television.
    QString reasonOf(const QByteArray& body, const QString& fallback)
    {
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject())
        {
            const QString r = doc.object().value(QStringLiteral("reason")).toString();
            if (!r.isEmpty()) return r;
        }
        return fallback;
    }
}

PlayOnClient::PlayOnClient(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
    qRegisterMetaType<PlayOn::RemoteView>("PlayOn::RemoteView");
    qRegisterMetaType<PlayOn::Pull>("PlayOn::Pull");
}

QString PlayOnClient::base(const PlayOn::Peer& peer) const
{
    return QStringLiteral("http://%1:%2").arg(peer.host).arg(peer.port);
}

void PlayOnClient::post(const PlayOn::Peer& peer, const QString& path, const QByteArray& body,
                        const QString& token, std::function<void(int, const QByteArray&, bool)> done)
{
    QNetworkRequest req{ QUrl(base(peer) + path) };
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // The credential. One header, one request, and it appears nowhere else in this process's output.
    if (!token.isEmpty()) req.setRawHeader("X-EB-Token", token.toLatin1());

    QNetworkReply* r = nam_->post(req, body);
    QTimer::singleShot(kTimeoutMs, r, [r] { if (r->isRunning()) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [r, done] {
        r->deleteLater();
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = r->readAll();
        done(status, payload, r->error() == QNetworkReply::NoError);
    });
}

void PlayOnClient::get(const PlayOn::Peer& peer, const QString& path, const QString& token,
                       std::function<void(int, const QByteArray&, bool)> done)
{
    QNetworkRequest req{ QUrl(base(peer) + path) };
    if (!token.isEmpty()) req.setRawHeader("X-EB-Token", token.toLatin1());
    QNetworkReply* r = nam_->get(req);
    QTimer::singleShot(kTimeoutMs, r, [r] { if (r->isRunning()) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [r, done] {
        r->deleteLater();
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = r->readAll();
        done(status, payload, r->error() == QNetworkReply::NoError);
    });
}

// ---------------------------------------------------------------------------- pairing --------------------
void PlayOnClient::requestPairing(const PlayOn::Peer& peer)
{
    const QString id = peer.id;
    post(peer, QStringLiteral("/pair"), QByteArray("{}"), QString(),
         [this, id](int status, const QByteArray& body, bool ok) {
             if (ok && status == 200) emit pairingOffered(id);
             else emit pairingFailed(id, reasonOf(body, tr("That device did not answer.")));
         });
}

void PlayOnClient::redeemPairing(const PlayOn::Peer& peer, const QString& code)
{
    const QString id = peer.id;
    QJsonObject o;
    o.insert(QStringLiteral("code"), PlayOn::normalizeCode(code));
    post(peer, QStringLiteral("/pair"), QJsonDocument(o).toJson(QJsonDocument::Compact), QString(),
         [this, id](int status, const QByteArray& body, bool ok) {
             if (ok && status == 200)
             {
                 const QString token = QJsonDocument::fromJson(body).object()
                                           .value(QStringLiteral("token")).toString();
                 if (!token.isEmpty()) { emit paired(id, token); return; }
             }
             // A wrong code is the ordinary case here, so it gets the ordinary sentence rather than an error.
             emit pairingFailed(id, reasonOf(body, tr("That code was not accepted.")));
         });
}

// ---------------------------------------------------------------------------- hand-off -------------------
void PlayOnClient::handOff(const PlayOn::Peer& peer, const QString& token, const PlayOn::Handoff& h)
{
    const QString id = peer.id;
    const QString name = peer.name;
    post(peer, QStringLiteral("/open"), PlayOn::handoffJson(h), token,
         [this, id, name](int status, const QByteArray& body, bool ok) {
             if (ok && status == 200) { emit handedOff(id); return; }
             PlayOn::OpenResult r;
             r.httpStatus = status;
             r.reason = reasonOf(body, tr("it did not say why"));
             r.outcome = status == 409 ? PlayOn::OpenOutcome::Unresolvable
                       : status == 403 ? PlayOn::OpenOutcome::Gated
                                       : PlayOn::OpenOutcome::BadRequest;
             if (status == 401)
                 emit handOffRefused(id, tr("%1 needs pairing again.").arg(name));
             else if (!ok && status == 0)
                 emit handOffRefused(id, tr("%1 did not answer.").arg(name));
             else
                 emit handOffRefused(id, PlayOn::describeRefusal(r, name));
         });
}

// ---------------------------------------------------------------------------- remote mode ----------------
void PlayOnClient::pollState(const PlayOn::Peer& peer)
{
    const QString id = peer.id;
    get(peer, QStringLiteral("/state"), QString(), [this, id](int status, const QByteArray& body, bool ok) {
        emit stateArrived(id, PlayOn::remoteView(body, ok && status == 200));
    });
}

void PlayOnClient::sendPlayerCommand(const PlayOn::Peer& peer, const QByteArray& body)
{
    post(peer, QStringLiteral("/player"), body, QString(), [](int, const QByteArray&, bool) {});
}

// ---------------------------------------------------------------------------- continue here --------------
void PlayOnClient::pullState(const PlayOn::Peer& peer)
{
    const QString id = peer.id;
    get(peer, QStringLiteral("/state"), QString(), [this, id](int status, const QByteArray& body, bool ok) {
        emit pullArrived(id, PlayOn::continueHere(body, ok && status == 200));
    });
}
