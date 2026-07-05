#include "TraktClient.h"
#include "Settings.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QDateTime>
#include <QUrl>

namespace {
constexpr const char* kBase = "https://api.trakt.tv";

QNetworkRequest req(const QString& path, bool auth)
{
    QNetworkRequest r{ QUrl(QString::fromLatin1(kBase) + path) };
    r.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    r.setRawHeader("trakt-api-version", "2");
    r.setRawHeader("trakt-api-key", Settings::traktClientId().toUtf8());
    r.setTransferTimeout(20000);
    if (auth) r.setRawHeader("Authorization", QByteArray("Bearer ") + Settings::traktAccessToken().toUtf8());
    return r;
}

// Build the scrobble media object from an IMDB stream id:
//   "tt123"        -> { "movie": { "ids": { "imdb": "tt123" } } }
//   "ttShow:s:e"   -> { "show": { "ids": { "imdb": "ttShow" } }, "episode": { "season": s, "number": e } }
QJsonObject mediaJson(const QString& imdbStreamId)
{
    const QStringList p = imdbStreamId.split(QLatin1Char(':'));
    if (p.size() >= 3)
        return { { QStringLiteral("show"), QJsonObject{ { QStringLiteral("ids"), QJsonObject{ { QStringLiteral("imdb"), p.value(0) } } } } },
                 { QStringLiteral("episode"), QJsonObject{ { QStringLiteral("season"), p.value(1).toInt() },
                                                           { QStringLiteral("number"), p.value(2).toInt() } } } };
    return { { QStringLiteral("movie"), QJsonObject{ { QStringLiteral("ids"), QJsonObject{ { QStringLiteral("imdb"), imdbStreamId } } } } } };
}
} // namespace

TraktClient::TraktClient(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
}

bool TraktClient::configured()
{
    return !Settings::traktClientId().isEmpty() && !Settings::traktClientSecret().isEmpty();
}

bool TraktClient::connected() { return !Settings::traktAccessToken().isEmpty(); }

void TraktClient::connectAccount()
{
    if (!configured()) { emit connectError(tr("Enter your Trakt client id and secret first.")); return; }
    const QJsonObject body{ { QStringLiteral("client_id"), Settings::traktClientId() } };
    QNetworkReply* r = nam_->post(req(QStringLiteral("/oauth/device/code"), false),
                                  QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) { emit connectError(tr("Couldn't reach Trakt (%1).").arg(r->errorString())); return; }
        const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
        const QString code = o.value(QStringLiteral("device_code")).toString();
        const QString userCode = o.value(QStringLiteral("user_code")).toString();
        const QString url = o.value(QStringLiteral("verification_url")).toString();
        pollExpiresIn_ = o.value(QStringLiteral("expires_in")).toInt(600);
        const int interval = o.value(QStringLiteral("interval")).toInt(5);
        if (code.isEmpty() || userCode.isEmpty()) { emit connectError(tr("Trakt didn't return a device code.")); return; }
        emit deviceCode(userCode, url.isEmpty() ? QStringLiteral("https://trakt.tv/activate") : url);
        pollDeviceCode_ = code; pollElapsed_ = 0;
        pollDeviceToken(code, qMax(2, interval));
    });
}

void TraktClient::pollDeviceToken(const QString& deviceCode, int intervalSec)
{
    if (!pollTimer_) { pollTimer_ = new QTimer(this); pollTimer_->setSingleShot(true); }
    pollTimer_->disconnect();
    connect(pollTimer_, &QTimer::timeout, this, [this, deviceCode, intervalSec] {
        pollElapsed_ += intervalSec;
        if (pollElapsed_ > pollExpiresIn_) { emit connectError(tr("Trakt activation timed out — try again.")); return; }
        const QJsonObject body{ { QStringLiteral("code"), deviceCode },
                                { QStringLiteral("client_id"), Settings::traktClientId() },
                                { QStringLiteral("client_secret"), Settings::traktClientSecret() } };
        QNetworkReply* r = nam_->post(req(QStringLiteral("/oauth/device/token"), false),
                                      QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(r, &QNetworkReply::finished, this, [this, r, deviceCode, intervalSec] {
            r->deleteLater();
            const int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (code == 200)
            {
                const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
                const qint64 created = o.value(QStringLiteral("created_at")).toVariant().toLongLong();
                const qint64 exp = (created ? created : QDateTime::currentSecsSinceEpoch())
                                 + o.value(QStringLiteral("expires_in")).toVariant().toLongLong();
                Settings::setTraktTokens(o.value(QStringLiteral("access_token")).toString(),
                                         o.value(QStringLiteral("refresh_token")).toString(), exp);
                emit connectedChanged(true);
                return;
            }
            if (code == 400) { pollDeviceToken(deviceCode, intervalSec); return; } // still pending -> keep polling
            emit connectError(code == 409 ? tr("This code was already used.")
                            : code == 410 ? tr("The code expired — try again.")
                            : code == 418 ? tr("Activation was denied.")
                                          : tr("Trakt activation failed (%1).").arg(code));
        });
    });
    pollTimer_->start(intervalSec * 1000);
}

void TraktClient::disconnectAccount()
{
    Settings::clearTraktTokens();
    if (pollTimer_) pollTimer_->stop();
    emit connectedChanged(false);
}

// Refresh the access token if it has (nearly) expired, then invoke done(ok).
void TraktClient::ensureValidToken(std::function<void(bool)> done)
{
    if (!connected()) { done(false); return; }
    if (QDateTime::currentSecsSinceEpoch() < Settings::traktTokenExpiry() - 60) { done(true); return; }
    const QJsonObject body{ { QStringLiteral("refresh_token"), Settings::traktRefreshToken() },
                            { QStringLiteral("client_id"), Settings::traktClientId() },
                            { QStringLiteral("client_secret"), Settings::traktClientSecret() },
                            { QStringLiteral("redirect_uri"), QStringLiteral("urn:ietf:wg:oauth:2.0:oob") },
                            { QStringLiteral("grant_type"), QStringLiteral("refresh_token") } };
    QNetworkReply* r = nam_->post(req(QStringLiteral("/oauth/token"), false),
                                  QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(r, &QNetworkReply::finished, this, [this, r, done] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) { emit log(QStringLiteral("trakt: token refresh failed")); done(false); return; }
        const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
        const qint64 exp = QDateTime::currentSecsSinceEpoch() + o.value(QStringLiteral("expires_in")).toVariant().toLongLong();
        Settings::setTraktTokens(o.value(QStringLiteral("access_token")).toString(),
                                 o.value(QStringLiteral("refresh_token")).toString(), exp);
        done(true);
    });
}

void TraktClient::scrobble(const QString& action, const QString& imdbStreamId, double pct)
{
    if (!configured() || !connected() || imdbStreamId.isEmpty()) return;
    ensureValidToken([this, action, imdbStreamId, pct](bool ok) {
        if (!ok) return;
        QJsonObject body = mediaJson(imdbStreamId);
        body.insert(QStringLiteral("progress"), qBound(0.0, pct, 100.0));
        QNetworkReply* r = nam_->post(req(QStringLiteral("/scrobble/") + action, true),
                                      QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(r, &QNetworkReply::finished, this, [this, r, action] {
            r->deleteLater();
            emit log(QStringLiteral("trakt: scrobble %1 -> HTTP %2").arg(action)
                         .arg(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
        });
    });
}

void TraktClient::scrobbleStart(const QString& id, double pct) { scrobble(QStringLiteral("start"), id, pct); }
void TraktClient::scrobblePause(const QString& id, double pct) { scrobble(QStringLiteral("pause"), id, pct); }
void TraktClient::scrobbleStop(const QString& id, double pct)  { scrobble(QStringLiteral("stop"),  id, pct); }
