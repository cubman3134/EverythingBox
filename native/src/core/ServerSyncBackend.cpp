#include "ServerSyncBackend.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QSettings>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <chrono>

// The backend reads its own configuration from the app's ini — the same file DriveSyncBackend and CloudSync
// keep their `cloud/*` keys in — so switching backends is a matter of settings, not of separate storage. Its
// own file-static, matching the DriveSyncBackend pattern (issue #42: under a probe AppPaths::dataDir() is a
// per-process scratch dir, so this resolves to the isolated ini automatically).
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

ServerSyncBackend::ServerSyncBackend(QObject* parent) : SyncBackend(parent)
{
    nam_ = new QNetworkAccessManager(this);
    // The same inactivity timeout DriveSyncBackend applies (its comment, in short): Qt 6 ships this disabled, so
    // a reply that stalls mid-transfer would never finish and never error — and every caller here is a callback
    // chain, so a reply that never completes is a callback that never runs and an in-flight guard held forever.
    nam_->setTransferTimeout(std::chrono::milliseconds(60000));
}

// ---- configuration --------------------------------------------------------------------------------------

QString ServerSyncBackend::serverBase() const
{
    // Trim whitespace, then strip any trailing '/' — a pasted URL like "http://host:8080/" would otherwise
    // yield "http://host:8080//sync/..." and 404. The Increment-C pairing UI has users pasting this by hand.
    QString base = store().value(QStringLiteral("cloud/server/url")).toString().trimmed();
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    return base;
}
QString ServerSyncBackend::token() const
{ return store().value(QStringLiteral("cloud/server/token")).toString().trimmed(); }
QString ServerSyncBackend::ns() const
{
    // The namespace must be the SAME across every device of one user, or two devices would compute different
    // namespaces and never sync each other. The old fallback to ProfileStore::currentId() (key
    // `profiles/current`) was DEVICE-LOCAL and broke exactly that. Fall back to a fixed, shared "default"
    // instead — the per-user token already scopes access, so a shared namespace is safe.
    const QString configured = store().value(QStringLiteral("cloud/server/namespace")).toString();
    return configured.isEmpty() ? QStringLiteral("default") : configured;
}

// serverBase [ + "/" + token ] + "/sync/" + ns [ + "/" + percent-encode(key) ]. The token is a PATH PREFIX,
// deliberately not a header — and it is never logged from here.
QString ServerSyncBackend::endpoint(const QString& key) const
{
    QString base = serverBase();
    if (!token().isEmpty()) base += QStringLiteral("/") + token();
    QString u = base + QStringLiteral("/sync/") + ns();
    if (!key.isEmpty()) u += QStringLiteral("/") + QString::fromUtf8(QUrl::toPercentEncoding(key));
    return u;
}

// ---- auth / account -------------------------------------------------------------------------------------

bool ServerSyncBackend::isSignedIn() const { return !serverBase().isEmpty(); }
QString ServerSyncBackend::accountEmail() const { return serverBase(); }

void ServerSyncBackend::signIn()
{
    // There is no interactive flow: "sign in" is "a server is configured". Report success or a config gap
    // through the SAME signals the OAuth flow uses, so onboarding/MainWindow need no server-specific branch.
    if (serverBase().isEmpty()) { emit signInFailed(tr("Set the server URL first.")); return; }
    emit signedIn(serverBase());
}

void ServerSyncBackend::signOut()
{
    store().remove(QStringLiteral("cloud/server/url"));
    store().remove(QStringLiteral("cloud/server/token"));
    store().sync();
    emit signedOut();
}

// ---- the six primitives ---------------------------------------------------------------------------------

void ServerSyncBackend::ensureFolder(std::function<void(const QString&)> cb)
{
    // The namespace IS the folder: there is nothing to create. A non-empty id means "ready" (reachability is
    // proven later by findFile's listOk, per the contract); with no server configured there is no namespace.
    cb(serverBase().isEmpty() ? QString() : ns());
}

void ServerSyncBackend::findFile(const QString& /*folderId*/, const QString& name,
                                 std::function<void(bool, const QString&, const QString&, const QString&)> cb)
{
    QNetworkRequest req{ QUrl(endpoint(QString())) };
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, name, cb] {
        reply->deleteLater();
        // A network error yields no list. Surface it (listOk=false) so a caller never mistakes "couldn't reach
        // the server" for "the namespace is empty" — the latter would let a restore clobber the backup.
        if (reply->error() != QNetworkReply::NoError) { cb(false, QString(), QString(), QString()); return; }
        const QJsonArray objs = QJsonDocument::fromJson(reply->readAll()).object()
                                    .value(QStringLiteral("objects")).toArray();
        for (const QJsonValue& v : objs) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("key")).toString() == name && !o.value(QStringLiteral("deleted")).toBool()) {
                cb(true, name, o.value(QStringLiteral("modifiedUtc")).toString(),
                   o.value(QStringLiteral("meta")).toString());   // meta == Drive's stateHash
                return;
            }
        }
        cb(true, QString(), QString(), QString());   // genuinely absent (the list itself succeeded)
    });
}

void ServerSyncBackend::uploadFile(const QString& /*folderId*/, const QString& existingId, const QString& name,
                                   const QString& mimeType, const QByteArray& data, const QString& stateHash,
                                   std::function<void(const QString&)> cb)
{
    QNetworkRequest req{ QUrl(endpoint(name)) };
    if (!mimeType.isEmpty()) req.setHeader(QNetworkRequest::ContentTypeHeader, mimeType);
    req.setRawHeader("X-Sync-Meta", stateHash.toUtf8());   // the content-hash CAS token, echoed back on the list
    // Create-only when we believe the object does not exist yet: If-None-Match:* makes the server reject a PUT
    // that would overwrite (412), so two devices creating the same key don't silently clobber each other. A 412
    // is a failure here; the next cycle re-plans it as an update (with a non-empty existingId).
    if (existingId.isEmpty()) req.setRawHeader("If-None-Match", "*");
    QNetworkReply* reply = nam_->sendCustomRequest(req, "PUT", data);
    connect(reply, &QNetworkReply::finished, this, [reply, name, cb] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        cb(status == 204 ? name : QString());
    });
}

void ServerSyncBackend::downloadFile(const QString& fileId, std::function<void(bool, const QByteArray&)> cb)
{
    QNetworkRequest req{ QUrl(endpoint(fileId)) };
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, cb] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) { cb(false, {}); return; }
        cb(true, reply->readAll());
    });
}

void ServerSyncBackend::findFolderNamed(const QString& name, std::function<void(bool, const QString&)> cb)
{
    // The server has no folders — the namespace is implicit. Only Drive/brand-migration ever calls this, and
    // brand migration is forced onto the Drive backend, so this path is unused on the server. Answer "found".
    cb(true, name);
}

void ServerSyncBackend::renameFile(const QString& /*fileId*/, const QString& /*newName*/, std::function<void(bool)> cb)
{
    // Unused on the server path (brand migration is Drive-only). A faithful PUT-new + DELETE-old is possible
    // later; a no-op success is correct for every caller that reaches a server backend today.
    cb(true);
}
