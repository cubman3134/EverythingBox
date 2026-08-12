#include "DriveSyncBackend.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "BrandMigration.h"  // the Drive lookups tolerate the previous brand until its flag is set

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
#include <chrono>

// The shared "EverythingBox" Google OAuth client (Desktop-app type). For desktop/installed apps Google
// treats the client secret as non-confidential (it can't be hidden in a distributed binary; security comes
// from PKCE + user consent), so it's embedded here. A settings override (cloud/clientId/clientSecret) wins.
static const char* kClientId = "993265781329-4n8gj4fgjo96qu01pbdbpg3s26a8ssnh.apps.googleusercontent.com";
static const char* kClientSecret = "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";

static const char* kAuthUrl  = "https://accounts.google.com/o/oauth2/v2/auth";
static const char* kTokenUrl = "https://oauth2.googleapis.com/token";
static const char* kUserInfo = "https://www.googleapis.com/oauth2/v3/userinfo";
static const char* kDrive    = "https://www.googleapis.com/drive/v3";
static const char* kDriveUp  = "https://www.googleapis.com/upload/drive/v3";
static const char* kScopes   = "openid email https://www.googleapis.com/auth/drive.file";
static const char* kFolder   = AppBrand::kDriveFolder;

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
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

DriveSyncBackend::DriveSyncBackend(QObject* parent) : SyncBackend(parent)
{
    nam_ = new QNetworkAccessManager(this);
    // EVERY request this class makes gets a transfer timeout (#34 review, minor 6). Qt 6 ships this DISABLED by
    // default, so without it a reply that stops mid-transfer — a captive portal that accepts the connection and
    // then says nothing, a handheld whose Wi-Fi drops between the request and the response — never finishes and
    // never errors. Every caller here is a callback chain, so a reply that never completes is a callback that
    // never runs: the push funnel's in-flight guard is held forever and every later attempt, automatic AND the
    // user's own Retry, silently returns until the app restarts. It is an INACTIVITY timeout, not a deadline,
    // so a slow upload of a multi-megabyte bundle is unaffected as long as bytes keep moving.
    nam_->setTransferTimeout(std::chrono::milliseconds(60000));
}

QString DriveSyncBackend::driveQueryQuote(const QString& value)
{
    QString out = value;
    out.replace(QLatin1Char('\\'), QLatin1String("\\\\"));   // first: else the escapes below get re-escaped
    out.replace(QLatin1Char('\''), QLatin1String("\\'"));
    return out;
}

bool DriveSyncBackend::isConfigured() { return !clientId().isEmpty() && !clientSecret().isEmpty(); }
bool DriveSyncBackend::isSignedIn() const { return !store().value(QStringLiteral("cloud/refreshToken")).toString().isEmpty(); }
QString DriveSyncBackend::accountEmail() const { return store().value(QStringLiteral("cloud/email")).toString(); }

void DriveSyncBackend::signOut()
{
    store().remove(QStringLiteral("cloud/refreshToken"));
    store().remove(QStringLiteral("cloud/email"));
    store().sync();
    accessToken_.clear();
    accessExpiryMs_ = 0;
    lastAuth_ = PendingPush::Auth::Ok;   // no account is not a failed one — the owed push is cleared with it (#34)
    emit signedOut();
}

// ---- OAuth loopback flow ----------------------------------------------------------------------------

// Interactive sign-in uses the desktop loopback OAuth flow (a local QTcpServer + the system browser), which
// isn't wired on Android yet — so the platform gate is compile-time. The onboarding layer consults this before
// attempting signIn() so an unsupported platform declines gracefully instead of dead-ending.
bool DriveSyncBackend::signInAvailable()
{
#ifdef Q_OS_ANDROID
    return false;
#else
    return true;
#endif
}

void DriveSyncBackend::signIn()
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

            const QString err = q.queryItemValue(QStringLiteral("error"));
            const QString code = q.queryItemValue(QStringLiteral("code"));
            const QString state = q.queryItemValue(QStringLiteral("state"));

            // Work out the OUTCOME before replying, so the page the user lands on matches what
            // actually happened. This used to write one "Signed in" body unconditionally and only
            // then check for failure — so a declined or mismatched sign-in told the user it had
            // worked while the app reported an error.
            QString landing = QString::fromLatin1(AppBrand::kAuthSuccessUrl);
            QString failure;                    // empty => success
            if (!err.isEmpty()) {
                failure = err;
                landing = QStringLiteral("%1?reason=%2").arg(QString::fromLatin1(AppBrand::kAuthErrorUrl),
                                                             QString::fromUtf8(QUrl::toPercentEncoding(err)));
            } else if (code.isEmpty()) {
                failure = tr("Sign-in was cancelled.");
                landing = QStringLiteral("%1?reason=cancelled").arg(QString::fromLatin1(AppBrand::kAuthErrorUrl));
            } else if (state != pendingState_) {
                failure = tr("Sign-in state mismatch.");
                landing = QStringLiteral("%1?reason=state_mismatch").arg(QString::fromLatin1(AppBrand::kAuthErrorUrl));
            }

            // Hand the browser to the website rather than rendering a bare line here.
            //
            // This request's URL carries the authorization code in its query string, so the
            // hand-off must not pass it on. Measured, rather than assumed (Chromium, top-level
            // navigation out of this loopback URL):
            //
            //   302                     -> no Referer sent at all
            //   302 + Referrer-Policy   -> no Referer sent at all
            //   HTML page + JS redirect -> Referer: http://127.0.0.1:<port>/  (origin only)
            //
            // So a 302 does not leak the code, and neither does the scripted variant, because
            // current browsers default to strict-origin-when-cross-origin and drop the query.
            // The header is therefore belt-and-braces: it costs one line and makes the guarantee
            // explicit instead of resting on a default that a future browser or a user's
            // hardened config could change. It is NOT patching a demonstrated leak.
            //
            // The 302 is preferred over serving HTML that redirects for a plainer reason: it is
            // the variant that sends no Referer whatsoever, and there is no page to flash.
            //
            // Cache-Control: a cached 302 would send a later, unrelated request for this loopback
            // port straight to the success page.
            const QByteArray redirect =
                "HTTP/1.1 302 Found\r\n"
                "Location: " + landing.toUtf8() + "\r\n"
                "Referrer-Policy: no-referrer\r\n"
                "Cache-Control: no-store\r\n"
                "Connection: close\r\n"
                "Content-Length: 0\r\n\r\n";
            sock->write(redirect);
            sock->flush();
            sock->disconnectFromHost();
            if (loopback_) { loopback_->close(); loopback_->deleteLater(); loopback_ = nullptr; }

            // The redirect is only what the human sees. Sign-in itself succeeds or fails on the
            // exchange below, so an unreachable website cannot break it — the app's own UI stays
            // the source of truth for whether cloud sync is on.
            if (!failure.isEmpty()) { emit signInFailed(failure); return; }
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

void DriveSyncBackend::exchangeCode(const QString& code, const QString& verifier, const QString& redirectUri)
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
        lastAuth_ = PendingPush::Auth::Ok;   // a fresh grant un-parks a retry that gave up on the old one (#34)
        store().setValue(QStringLiteral("cloud/refreshToken"), rt);
        store().sync();
        fetchAccountEmail();
    });
}

void DriveSyncBackend::fetchAccountEmail()
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

void DriveSyncBackend::withAccessToken(std::function<void(bool)> cb)
{
    if (!accessToken_.isEmpty() && QDateTime::currentMSecsSinceEpoch() < accessExpiryMs_)
    { lastAuth_ = PendingPush::Auth::Ok; cb(true); return; }
    const QString rt = store().value(QStringLiteral("cloud/refreshToken")).toString();
    // No stored grant: nothing to refresh and no trip to make. Expired, not Offline — the fix is a sign-in.
    if (rt.isEmpty())
    { lastAuth_ = PendingPush::classifyRefresh(false, /*httpStatus*/0, QString(), false); cb(false); return; }

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
        // #34: separate "the grant is dead" from "this failure is transient". BOTH facts the classifier needs
        // are on the reply and both are read here: the HTTP status (0 when the request never got a response at
        // all) and the OAuth `error` code. What is deliberately NOT the test any more is "the body parsed as
        // JSON" — Google answers rate limits and internal errors with JSON too, and reading those as a revoked
        // grant parks the device behind a "sign in again" it cannot act on (review round 1). The body is
        // classified and dropped: never logged, never stored, and no part of it reaches the pending record.
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        const QString at = o.value(QStringLiteral("access_token")).toString();
        lastAuth_ = PendingPush::classifyRefresh(/*haveRefreshToken*/true, status,
                                                 o.value(QStringLiteral("error")).toString(),
                                                 /*haveAccessToken*/!at.isEmpty());
        if (at.isEmpty()) { cb(false); return; }
        accessToken_ = at;
        accessExpiryMs_ = QDateTime::currentMSecsSinceEpoch() + (o.value(QStringLiteral("expires_in")).toInt(3600) - 60) * 1000LL;
        cb(true);
    });
}

// ---- Drive primitives ------------------------------------------------------------------------------

void DriveSyncBackend::findFolderNamed(const QString& name, std::function<void(bool, const QString&)> cb)
{
    withAccessToken([this, name, cb](bool ok) {
        if (!ok) { cb(false, QString()); return; }
        QUrl u(QString::fromLatin1(kDrive) + QStringLiteral("/files"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("q"), QStringLiteral("mimeType='application/vnd.google-apps.folder' and "
            "name='%1' and trashed=false").arg(driveQueryQuote(name)));
        q.addQueryItem(QStringLiteral("fields"), QStringLiteral("files(id)"));
        u.setQuery(q);
        QNetworkRequest req(u);
        req.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
        QNetworkReply* reply = nam_->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, cb] {
            reply->deleteLater();
            // A network error on the folder list-GET must NOT be read as "no folder" — that would POST a DUPLICATE
            // empty folder, and a subsequent findFile in that fresh-empty folder reads listReached=true/hasRemote=false
            // ("proven-empty") -> Seed -> a later pushLocal (resolving the ORIGINAL folder) overwrites the real backup.
            // Surfacing queryOk=false (folderId.isEmpty() -> st.reached=false -> Retry) means a query failure can't
            // launder into a Seed, and no duplicate folder is ever minted on a transient error.
            if (reply->error() != QNetworkReply::NoError) { cb(false, QString()); return; }
            const QJsonArray files = QJsonDocument::fromJson(reply->readAll()).object()
                                         .value(QStringLiteral("files")).toArray();
            if (files.isEmpty()) { cb(true, QString()); return; } // proven-absent (the query itself succeeded)
            cb(true, files.first().toObject().value(QStringLiteral("id")).toString());
        });
    });
}

void DriveSyncBackend::renameFile(const QString& fileId, const QString& newName, std::function<void(bool)> cb)
{
    withAccessToken([this, fileId, newName, cb](bool ok) {
        if (!ok) { cb(false); return; }
        QUrl u(QString::fromLatin1(kDrive) + QStringLiteral("/files/") + fileId);
        QUrlQuery q; q.addQueryItem(QStringLiteral("fields"), QStringLiteral("id"));
        u.setQuery(q);
        QNetworkRequest req(u);
        req.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        const QJsonObject meta{ { QStringLiteral("name"), newName } };
        QNetworkReply* reply = nam_->sendCustomRequest(req, "PATCH",
                                                       QJsonDocument(meta).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [reply, cb] {
            reply->deleteLater();
            cb(reply->error() == QNetworkReply::NoError);
        });
    });
}

void DriveSyncBackend::ensureFolder(std::function<void(const QString&)> cb)
{
    // Create the app folder under the CURRENT name. Reached only when a query proved no folder exists under
    // either name — never on a query error, or a transient failure would mint a duplicate.
    auto create = [this, cb] {
        withAccessToken([this, cb](bool ok) {
            if (!ok) { cb(QString()); return; }
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
    };

    findFolderNamed(QString::fromLatin1(kFolder), [this, cb, create](bool queryOk, const QString& id) {
        if (!queryOk) { cb(QString()); return; }  // unreachable -> Retry (never "absent", never a duplicate)
        if (!id.isEmpty()) { cb(id); return; }
        // The folder is provably not under the current name. Until BrandMigration has CONFIRMED the rename,
        // it may still be under the previous one — and creating a second, empty folder here is exactly the
        // "proven-empty -> Seed -> overwrite the real backup" path above, only caused by a rebrand instead of
        // a dropped packet. Once the flag is set this second query is skipped and the tolerance is gone.
        if (BrandMigration::done(BrandMigration::Step::DriveFolder)) { create(); return; }
        findFolderNamed(QString::fromLatin1(AppBrand::Legacy::kDriveFolder),
                        [cb, create](bool legacyOk, const QString& legacyId) {
            if (!legacyOk) { cb(QString()); return; }
            if (!legacyId.isEmpty()) { cb(legacyId); return; }
            create();
        });
    });
}

void DriveSyncBackend::findFile(const QString& folderId, const QString& name,
                         std::function<void(bool, const QString&, const QString&, const QString&)> cb)
{
    withAccessToken([this, folderId, name, cb](bool ok) {
        if (!ok) { cb(false, QString(), QString(), QString()); return; } // token/auth failure — Drive not reached
        QUrl u(QString::fromLatin1(kDrive) + QStringLiteral("/files"));
        QUrlQuery q;
        // driveQueryQuote, not raw: the name is interpolated INSIDE a Drive query string literal, and an
        // apostrophe or backslash in it ("Link's Awakening.srm") would end that literal early — the query
        // then fails and the caller reads a file that exists as absent.
        q.addQueryItem(QStringLiteral("q"), QStringLiteral("name='%1' and '%2' in parents and trashed=false")
                                                .arg(driveQueryQuote(name), folderId));
        // appProperties carries the bundle's own content hash, so we can detect "another device changed it"
        // without depending on Drive's modifiedTime (which our own uploads bump).
        q.addQueryItem(QStringLiteral("fields"), QStringLiteral("files(id,modifiedTime,appProperties)"));
        u.setQuery(q);
        QNetworkRequest req(u);
        req.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
        QNetworkReply* reply = nam_->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, cb] {
            reply->deleteLater();
            // A network error here yields no file list. Surface it (listOk=false) so callers never mistake
            // "couldn't reach Drive" for "the cloud is empty" — the latter would let a restore clobber the backup.
            if (reply->error() != QNetworkReply::NoError) { cb(false, QString(), QString(), QString()); return; }
            const QJsonArray files = QJsonDocument::fromJson(reply->readAll()).object()
                                         .value(QStringLiteral("files")).toArray();
            if (files.isEmpty()) { cb(true, QString(), QString(), QString()); return; } // proven-empty (no error)
            const QJsonObject f = files.first().toObject();
            const QString hash = f.value(QStringLiteral("appProperties")).toObject()
                                     .value(QStringLiteral("stateHash")).toString();
            cb(true, f.value(QStringLiteral("id")).toString(), f.value(QStringLiteral("modifiedTime")).toString(), hash);
        });
    });
}

void DriveSyncBackend::uploadFile(const QString& folderId, const QString& existingId, const QString& name,
                           const QString& mimeType, const QByteArray& data, const QString& stateHash,
                           std::function<void(const QString&)> cb)
{
    withAccessToken([this, folderId, existingId, name, mimeType, data, stateHash, cb](bool ok) {
        if (!ok) { cb(QString()); return; }
        const QByteArray boundary = "mmvb" + randomToken(16).toUtf8();
        QJsonObject meta{ { QStringLiteral("name"), name } };
        if (!stateHash.isEmpty())
            meta.insert(QStringLiteral("appProperties"), QJsonObject{ { QStringLiteral("stateHash"), stateHash } });
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

void DriveSyncBackend::downloadFile(const QString& fileId, std::function<void(bool, const QByteArray&)> cb)
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
