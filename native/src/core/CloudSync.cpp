#include "CloudSync.h"

#include <QCoreApplication>
#include <QSettings>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QDesktopServices>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHostAddress>
#include <QDateTime>

// The shared "My Media Vault" Google OAuth client (Desktop type). Paste the values here once the client is
// created; a settings override (cloud/clientId, cloud/clientSecret) takes precedence for testing.
static const char* kClientId = "";
static const char* kClientSecret = "";

static const char* kAuthUrl  = "https://accounts.google.com/o/oauth2/v2/auth";
static const char* kTokenUrl = "https://oauth2.googleapis.com/token";
static const char* kUserInfo = "https://www.googleapis.com/oauth2/v3/userinfo";
static const char* kDrive    = "https://www.googleapis.com/drive/v3";
static const char* kDriveUp  = "https://www.googleapis.com/upload/drive/v3";
static const char* kScopes   = "openid email https://www.googleapis.com/auth/drive.file";
static const char* kFolder   = "MyMediaVault";

static QSettings& store()
{
    static QSettings s(QCoreApplication::applicationDirPath() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}
static QString clientId()
{
    const QString s = store().value(QStringLiteral("cloud/clientId")).toString();
    return s.isEmpty() ? QString::fromLatin1(kClientId) : s;
}
static QString clientSecret()
{
    const QString s = store().value(QStringLiteral("cloud/clientSecret")).toString();
    return s.isEmpty() ? QString::fromLatin1(kClientSecret) : s;
}
static QString randomToken(int n)
{
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    QString out;
    for (int i = 0; i < n; ++i) out += QLatin1Char(a[QRandomGenerator::system()->bounded(64)]);
    return out;
}

CloudSync::CloudSync(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
}

bool CloudSync::isConfigured() { return !clientId().isEmpty() && !clientSecret().isEmpty(); }
bool CloudSync::isSignedIn() const { return !store().value(QStringLiteral("cloud/refreshToken")).toString().isEmpty(); }
QString CloudSync::accountEmail() const { return store().value(QStringLiteral("cloud/email")).toString(); }

void CloudSync::signOut()
{
    store().remove(QStringLiteral("cloud/refreshToken"));
    store().remove(QStringLiteral("cloud/email"));
    store().sync();
    accessToken_.clear();
    accessExpiryMs_ = 0;
    emit signedOut();
}

// ---- OAuth loopback flow ----------------------------------------------------------------------------

void CloudSync::signIn()
{
    if (!isConfigured()) { emit signInFailed(tr("No Google sign-in client is configured yet.")); return; }

    pendingVerifier_ = randomToken(64);
    pendingState_ = randomToken(24);
    const QByteArray challenge = QCryptographicHash::hash(pendingVerifier_.toUtf8(), QCryptographicHash::Sha256)
                                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

    if (loopback_) { loopback_->deleteLater(); loopback_ = nullptr; }
    loopback_ = new QTcpServer(this);
    if (!loopback_->listen(QHostAddress::LocalHost, 0))
    { emit signInFailed(tr("Couldn't open a local port for sign-in.")); return; }
    redirectUri_ = QStringLiteral("http://127.0.0.1:%1").arg(loopback_->serverPort());

    connect(loopback_, &QTcpServer::newConnection, this, [this] {
        QTcpSocket* sock = loopback_->nextPendingConnection();
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
            const QByteArray req = sock->readAll();
            const QByteArray line = req.left(req.indexOf('\r'));
            const int sp1 = line.indexOf(' '), sp2 = line.indexOf(' ', sp1 + 1);
            const QString target = QString::fromUtf8(line.mid(sp1 + 1, sp2 - sp1 - 1));
            const QUrlQuery q(QUrl::fromEncoded(("http://localhost" + target.toUtf8())).query());

            const QByteArray body = "<html><body style='font-family:sans-serif;padding:40px'>"
                                    "<h3>My Media Vault</h3>You're signed in. You can close this tab.</body></html>";
            sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n" + body);
            sock->flush();
            sock->disconnectFromHost();
            if (loopback_) { loopback_->close(); loopback_->deleteLater(); loopback_ = nullptr; }

            const QString err = q.queryItemValue(QStringLiteral("error"));
            const QString code = q.queryItemValue(QStringLiteral("code"));
            const QString state = q.queryItemValue(QStringLiteral("state"));
            if (!err.isEmpty()) { emit signInFailed(err); return; }
            if (code.isEmpty()) { emit signInFailed(tr("Sign-in was cancelled.")); return; }
            if (state != pendingState_) { emit signInFailed(tr("Sign-in state mismatch.")); return; }
            exchangeCode(code, pendingVerifier_, redirectUri_);
        });
    });

    QUrl u(QString::fromLatin1(kAuthUrl));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("client_id"), clientId());
    q.addQueryItem(QStringLiteral("redirect_uri"), redirectUri_);
    q.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    q.addQueryItem(QStringLiteral("scope"), QString::fromLatin1(kScopes));
    q.addQueryItem(QStringLiteral("code_challenge"), QString::fromUtf8(challenge));
    q.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    q.addQueryItem(QStringLiteral("state"), pendingState_);
    q.addQueryItem(QStringLiteral("access_type"), QStringLiteral("offline"));
    q.addQueryItem(QStringLiteral("prompt"), QStringLiteral("consent"));
    u.setQuery(q);
    QDesktopServices::openUrl(u);
}

void CloudSync::exchangeCode(const QString& code, const QString& verifier, const QString& redirectUri)
{
    QNetworkRequest req((QUrl(QString::fromLatin1(kTokenUrl))));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("code"), code);
    body.addQueryItem(QStringLiteral("client_id"), clientId());
    body.addQueryItem(QStringLiteral("client_secret"), clientSecret());
    body.addQueryItem(QStringLiteral("code_verifier"), verifier);
    body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    body.addQueryItem(QStringLiteral("redirect_uri"), redirectUri);
    QNetworkReply* reply = nam_->post(req, body.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        const QString at = o.value(QStringLiteral("access_token")).toString();
        const QString rt = o.value(QStringLiteral("refresh_token")).toString();
        if (at.isEmpty() || rt.isEmpty())
        { emit signInFailed(tr("Sign-in failed (no token returned).")); return; }
        accessToken_ = at;
        accessExpiryMs_ = QDateTime::currentMSecsSinceEpoch() + (o.value(QStringLiteral("expires_in")).toInt(3600) - 60) * 1000LL;
        store().setValue(QStringLiteral("cloud/refreshToken"), rt);
        store().sync();
        fetchAccountEmail();
    });
}

void CloudSync::fetchAccountEmail()
{
    withAccessToken([this](bool ok) {
        if (!ok) { emit signInFailed(tr("Couldn't verify the account.")); return; }
        QNetworkRequest req((QUrl(QString::fromLatin1(kUserInfo))));
        req.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
        QNetworkReply* reply = nam_->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
            const QString email = o.value(QStringLiteral("email")).toString();
            if (!email.isEmpty()) { store().setValue(QStringLiteral("cloud/email"), email); store().sync(); }
            emit signedIn(email);
        });
    });
}

void CloudSync::withAccessToken(std::function<void(bool)> cb)
{
    if (!accessToken_.isEmpty() && QDateTime::currentMSecsSinceEpoch() < accessExpiryMs_) { cb(true); return; }
    const QString rt = store().value(QStringLiteral("cloud/refreshToken")).toString();
    if (rt.isEmpty()) { cb(false); return; }

    QNetworkRequest req((QUrl(QString::fromLatin1(kTokenUrl))));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("refresh_token"), rt);
    body.addQueryItem(QStringLiteral("client_id"), clientId());
    body.addQueryItem(QStringLiteral("client_secret"), clientSecret());
    body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    QNetworkReply* reply = nam_->post(req, body.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply, cb] {
        reply->deleteLater();
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        const QString at = o.value(QStringLiteral("access_token")).toString();
        if (at.isEmpty()) { cb(false); return; }
        accessToken_ = at;
        accessExpiryMs_ = QDateTime::currentMSecsSinceEpoch() + (o.value(QStringLiteral("expires_in")).toInt(3600) - 60) * 1000LL;
        cb(true);
    });
}

// ---- Drive primitives ------------------------------------------------------------------------------

void CloudSync::ensureFolder(std::function<void(const QString&)> cb)
{
    withAccessToken([this, cb](bool ok) {
        if (!ok) { cb(QString()); return; }
        QUrl u(QString::fromLatin1(kDrive) + QStringLiteral("/files"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("q"), QStringLiteral("mimeType='application/vnd.google-apps.folder' and "
            "name='%1' and trashed=false").arg(QString::fromLatin1(kFolder)));
        q.addQueryItem(QStringLiteral("fields"), QStringLiteral("files(id)"));
        u.setQuery(q);
        QNetworkRequest req(u);
        req.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
        QNetworkReply* reply = nam_->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, cb] {
            reply->deleteLater();
            const QJsonArray files = QJsonDocument::fromJson(reply->readAll()).object()
                                         .value(QStringLiteral("files")).toArray();
            if (!files.isEmpty()) { cb(files.first().toObject().value(QStringLiteral("id")).toString()); return; }
            // Not found -> create it.
            QNetworkRequest cr((QUrl(QString::fromLatin1(kDrive) + QStringLiteral("/files"))));
            cr.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
            cr.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            const QJsonObject meta{ { QStringLiteral("name"), QString::fromLatin1(kFolder) },
                                    { QStringLiteral("mimeType"), QStringLiteral("application/vnd.google-apps.folder") } };
            QNetworkReply* cre = nam_->post(cr, QJsonDocument(meta).toJson(QJsonDocument::Compact));
            connect(cre, &QNetworkReply::finished, this, [cre, cb] {
                cre->deleteLater();
                cb(QJsonDocument::fromJson(cre->readAll()).object().value(QStringLiteral("id")).toString());
            });
        });
    });
}

void CloudSync::findFile(const QString& folderId, const QString& name,
                         std::function<void(const QString&, const QString&)> cb)
{
    withAccessToken([this, folderId, name, cb](bool ok) {
        if (!ok) { cb(QString(), QString()); return; }
        QUrl u(QString::fromLatin1(kDrive) + QStringLiteral("/files"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("q"), QStringLiteral("name='%1' and '%2' in parents and trashed=false")
                                                .arg(name, folderId));
        q.addQueryItem(QStringLiteral("fields"), QStringLiteral("files(id,modifiedTime)"));
        u.setQuery(q);
        QNetworkRequest req(u);
        req.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
        QNetworkReply* reply = nam_->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, cb] {
            reply->deleteLater();
            const QJsonArray files = QJsonDocument::fromJson(reply->readAll()).object()
                                         .value(QStringLiteral("files")).toArray();
            if (files.isEmpty()) { cb(QString(), QString()); return; }
            const QJsonObject f = files.first().toObject();
            cb(f.value(QStringLiteral("id")).toString(), f.value(QStringLiteral("modifiedTime")).toString());
        });
    });
}

void CloudSync::uploadFile(const QString& folderId, const QString& existingId, const QString& name,
                           const QString& mimeType, const QByteArray& data,
                           std::function<void(const QString&)> cb)
{
    withAccessToken([this, folderId, existingId, name, mimeType, data, cb](bool ok) {
        if (!ok) { cb(QString()); return; }
        const QByteArray boundary = "mmvb" + randomToken(16).toUtf8();
        QJsonObject meta{ { QStringLiteral("name"), name } };
        if (existingId.isEmpty()) meta.insert(QStringLiteral("parents"), QJsonArray{ folderId });
        QByteArray body;
        body += "--" + boundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n";
        body += QJsonDocument(meta).toJson(QJsonDocument::Compact) + "\r\n";
        body += "--" + boundary + "\r\nContent-Type: " + mimeType.toUtf8() + "\r\n\r\n";
        body += data + "\r\n--" + boundary + "--\r\n";

        QUrl u(QString::fromLatin1(kDriveUp) + QStringLiteral("/files")
               + (existingId.isEmpty() ? QString() : (QStringLiteral("/") + existingId)));
        QUrlQuery q; q.addQueryItem(QStringLiteral("uploadType"), QStringLiteral("multipart"));
        q.addQueryItem(QStringLiteral("fields"), QStringLiteral("id"));
        u.setQuery(q);
        QNetworkRequest req(u);
        req.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
        req.setHeader(QNetworkRequest::ContentTypeHeader, "multipart/related; boundary=" + boundary);
        QNetworkReply* reply = existingId.isEmpty() ? nam_->post(req, body)
                                                    : nam_->sendCustomRequest(req, "PATCH", body);
        connect(reply, &QNetworkReply::finished, this, [reply, cb] {
            reply->deleteLater();
            cb(QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("id")).toString());
        });
    });
}

void CloudSync::downloadFile(const QString& fileId, std::function<void(bool, const QByteArray&)> cb)
{
    withAccessToken([this, fileId, cb](bool ok) {
        if (!ok) { cb(false, {}); return; }
        QUrl u(QString::fromLatin1(kDrive) + QStringLiteral("/files/") + fileId);
        QUrlQuery q; q.addQueryItem(QStringLiteral("alt"), QStringLiteral("media"));
        u.setQuery(q);
        QNetworkRequest req(u);
        req.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = nam_->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, cb] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) { cb(false, {}); return; }
            cb(true, reply->readAll());
        });
    });
}
