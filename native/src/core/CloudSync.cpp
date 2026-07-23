#include "CloudSync.h"
#include "AppPaths.h"
#include "Settings.h"   // deviceId() — stamped into meta.json (mdsync T4)

#include <QCoreApplication>
#include <QSet>
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
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSysInfo>
#include <cstring>
#include "miniz.h"

// The shared "My Media Vault" Google OAuth client (Desktop-app type). For desktop/installed apps Google
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
static const char* kFolder   = "MyMediaVault";

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
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

            // Try to auto-close the tab (works when the browser allows it); otherwise it shows a one-liner.
            const QByteArray body = "<html><body style='font-family:sans-serif;padding:30px'>"
                                    "Signed in to My Media Vault — you can close this tab."
                                    "<script>window.close();</script></body></html>";
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
                         std::function<void(const QString&, const QString&, const QString&)> cb)
{
    withAccessToken([this, folderId, name, cb](bool ok) {
        if (!ok) { cb(QString(), QString(), QString()); return; }
        QUrl u(QString::fromLatin1(kDrive) + QStringLiteral("/files"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("q"), QStringLiteral("name='%1' and '%2' in parents and trashed=false")
                                                .arg(name, folderId));
        // appProperties carries the bundle's own content hash, so we can detect "another device changed it"
        // without depending on Drive's modifiedTime (which our own uploads bump).
        q.addQueryItem(QStringLiteral("fields"), QStringLiteral("files(id,modifiedTime,appProperties)"));
        u.setQuery(q);
        QNetworkRequest req(u);
        req.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
        QNetworkReply* reply = nam_->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, cb] {
            reply->deleteLater();
            const QJsonArray files = QJsonDocument::fromJson(reply->readAll()).object()
                                         .value(QStringLiteral("files")).toArray();
            if (files.isEmpty()) { cb(QString(), QString(), QString()); return; }
            const QJsonObject f = files.first().toObject();
            const QString hash = f.value(QStringLiteral("appProperties")).toObject()
                                     .value(QStringLiteral("stateHash")).toString();
            cb(f.value(QStringLiteral("id")).toString(), f.value(QStringLiteral("modifiedTime")).toString(), hash);
        });
    });
}

void CloudSync::uploadFile(const QString& folderId, const QString& existingId, const QString& name,
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

// ---- state bundle (a zip of the synced settings + local addons + themes) ----------------------------

static const char* kBundleName = "mymediavault-sync.zip";

// First-party addon folders (manifest id "com.mymediavault.*"). These ship with the app build and are
// updated by install/deploy, so they're kept OUT of cloud sync - otherwise the cloud snapshot would clobber
// a freshly-deployed update on the next startup. Third-party addons (other ids) still sync. Addon *config*
// lives in settings.json, which is synced regardless, so API keys still travel across devices.
static QSet<QString> firstPartyAddonDirs()
{
    QSet<QString> out;
    const QString root = AppPaths::dataDir() + QStringLiteral("/addons");
    const QFileInfoList subs = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& d : subs)
    {
        QFile mf(d.absoluteFilePath() + QStringLiteral("/manifest.json"));
        if (!mf.open(QIODevice::ReadOnly)) continue;
        const QJsonObject m = QJsonDocument::fromJson(mf.readAll()).object();
        if (m.value(QStringLiteral("id")).toString().startsWith(QStringLiteral("com.mymediavault.")))
            out.insert(d.fileName());
    }
    return out;
}

// Top-level subfolder of a relative path, e.g. "aiocatalog/main.js" -> "aiocatalog".
static QString topSegment(const QString& rel)
{
    const int s = rel.indexOf(QLatin1Char('/'));
    return s < 0 ? rel : rel.left(s);
}

static void zipAddDir(mz_zip_archive& z, const QString& dir, const QString& prefix,
                      const QSet<QString>& excludeTop = {})
{
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        const QString rel = QDir(dir).relativeFilePath(path);
        if (!excludeTop.isEmpty() && excludeTop.contains(topSegment(rel))) continue; // skip first-party addons
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray data = f.readAll();
        const QString arch = prefix + QStringLiteral("/") + rel;
        mz_zip_writer_add_mem(&z, arch.toUtf8().constData(), data.constData(), data.size(), MZ_DEFAULT_COMPRESSION);
    }
}

// ---- the ONE device-local carve-out (mdsync T4) -----------------------------------------------------
// Applied in BOTH directions: buildBundle omits these from settings.json, applyBundle refuses to write them,
// and stateHash leaves them out of the sync fingerprint (so a device-local edit never reads as an unsynced
// change and never desyncs the cross-device baseline). Keep this the SINGLE source of truth for the list.
bool CloudSync::isDeviceLocalKey(const QString& key)
{
    // Exact device-local keys. Each has a SIBLING that DOES sync (profiles/list, sync/global/*,
    // library/showHidden) — so match the leaf exactly, never the group.
    static const QSet<QString> kExact = {
        QStringLiteral("roms/folder"),          // where THIS machine keeps its ROMs
        QStringLiteral("emulators/root"),        // this machine's standalone-emulator install root
        QStringLiteral("emulators/fullscreen"),  // per-machine display preference
        QStringLiteral("player/externalPath"),   // this machine's external-player exe path
        QStringLiteral("player/external"),        // this machine's external-player choice
        QStringLiteral("netplay/relay"),          // this machine's relay endpoint
        QStringLiteral("display/mode"),           // TV / desktop / mobile form factor of THIS device
        QStringLiteral("display/tvPromptDone"),   // one-shot per-device onboarding flag
        QStringLiteral("profiles/current"),       // the active profile is per-device (profiles/list SYNCS)
    };
    if (kExact.contains(key)) return true;
    // Prefix families that are wholly device-local.
    return key.startsWith(QStringLiteral("emu/virtualPad")) // emu/virtualPad* (the on-screen pad, per device)
        || key.startsWith(QStringLiteral("sync/files/"))     // per-file A/V sync offsets (sync/global/* SYNCS)
        || key.startsWith(QStringLiteral("device/"))         // device/* (this install's identity — device/id)
        || key.startsWith(QStringLiteral("cloud/"))          // cloud/* (this device's OAuth tokens / client id)
        || key.startsWith(QStringLiteral("downloads"))       // downloads* (this device's local download catalog)
        || key.startsWith(QStringLiteral("pcgames/"));       // pcgames/* (this device's installed PC games)
}

// The per-item stores the progress merge document (CloudMerge) owns. applyBundle must never write these from
// the heavy bundle (release-gating: a peer's stale copy of stats/<this-device>/... would clobber the live
// accumulator namespace and then propagate on the next push).
bool CloudSync::isPerItemStoreKey(const QString& key)
{
    return key.startsWith(QStringLiteral("resume/"))    || key.startsWith(QStringLiteral("recent/"))
        || key.startsWith(QStringLiteral("marks/"))     || key.startsWith(QStringLiteral("favorites/"))
        || key.startsWith(QStringLiteral("playlists/")) || key.startsWith(QStringLiteral("stats/"))
        || key.startsWith(QStringLiteral("playstats/")) || key.startsWith(QStringLiteral("deleted/"));
}

QByteArray CloudSync::buildSettingsJson()
{
    // Every setting except the device-local carve-out AND the per-item stores (mdsync T5 cadence fix). The
    // per-item stores are owned exclusively by the CloudMerge progress document, so they must NOT ride the
    // heavy bundle: applyBundle already refuses to write them inbound, and carrying them outbound only made a
    // per-item tick (a mark/favorite/playlist/stats accrual) flip the stateHash fingerprint and re-upload the
    // whole zip. Excluding them here (and in stateHash) keeps the heavy bundle quiet on per-item churn while the
    // lightweight merge doc still pushes on its own 15s debounce.
    QJsonObject so;
    for (const QString& k : store().allKeys())
        if (!isDeviceLocalKey(k) && !isPerItemStoreKey(k)) so.insert(k, store().value(k).toString());
    return QJsonDocument(so).toJson(QJsonDocument::Compact);
}

void CloudSync::applySettingsJson(const QByteArray& settingsJson)
{
    const QJsonObject so = QJsonDocument::fromJson(settingsJson).object();
    for (auto it = so.begin(); it != so.end(); ++it)
    {
        const QString& k = it.key();
        // Inbound carve-out: never overwrite a device-local key, and never write a per-item store key (the
        // merge document owns those — writing them here would clobber this device's live/merged state).
        if (isDeviceLocalKey(k) || isPerItemStoreKey(k)) continue;
        store().setValue(k, it.value().toString());
    }
    store().sync();
}

static QByteArray buildBundle()
{
    mz_zip_archive z; std::memset(&z, 0, sizeof(z));
    mz_zip_writer_init_heap(&z, 0, 0);

    const QJsonObject meta{ { QStringLiteral("device"), QSysInfo::machineHostName() },
                            { QStringLiteral("deviceId"), Settings::deviceId() }, // stable per-install id (mdsync T4)
                            { QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate) } };
    const QByteArray metaJson = QJsonDocument(meta).toJson(QJsonDocument::Compact);
    mz_zip_writer_add_mem(&z, "meta.json", metaJson.constData(), metaJson.size(), MZ_DEFAULT_COMPRESSION);

    // All settings except the device-local carve-out (the ONE exclusion table).
    const QByteArray sJson = CloudSync::buildSettingsJson();
    mz_zip_writer_add_mem(&z, "settings.json", sJson.constData(), sJson.size(), MZ_DEFAULT_COMPRESSION);

    const QString app = AppPaths::dataDir();
    zipAddDir(z, app + QStringLiteral("/addons"), QStringLiteral("addons"), firstPartyAddonDirs());
    zipAddDir(z, app + QStringLiteral("/themes"), QStringLiteral("themes"));
    zipAddDir(z, app + QStringLiteral("/saves"),  QStringLiteral("saves"));   // emulator battery saves (.srm)
    zipAddDir(z, app + QStringLiteral("/states"), QStringLiteral("states"));  // emulator save states (.state)

    void* buf = nullptr; size_t sz = 0;
    mz_zip_writer_finalize_heap_archive(&z, &buf, &sz);
    QByteArray out(static_cast<const char*>(buf), int(sz));
    mz_zip_writer_end(&z);
    if (buf) mz_free(buf);
    return out;
}

static bool applyBundle(const QByteArray& data)
{
    mz_zip_archive z; std::memset(&z, 0, sizeof(z));
    if (!mz_zip_reader_init_mem(&z, data.constData(), data.size(), 0)) return false;
    const QString app = AppPaths::dataDir();
    const QSet<QString> firstParty = firstPartyAddonDirs(); // never let the cloud overwrite these
    const int n = int(mz_zip_reader_get_num_files(&z));
    for (int i = 0; i < n; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(&z, i)) continue;
        const QString name = QString::fromUtf8(st.m_filename);
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
        if (!p) continue;
        const QByteArray bytes(static_cast<const char*>(p), int(sz));
        mz_free(p);

        if (name == QStringLiteral("settings.json"))
        {
            // Device-local keys AND per-item store keys are held off here (the merge document owns per-item).
            CloudSync::applySettingsJson(bytes);
        }
        else if (name.startsWith(QStringLiteral("addons/")) || name.startsWith(QStringLiteral("themes/"))
                 || name.startsWith(QStringLiteral("saves/")) || name.startsWith(QStringLiteral("states/")))
        {
            // A first-party addon ships with the build; don't let an (older) cloud bundle overwrite it.
            if (name.startsWith(QStringLiteral("addons/")) && firstParty.contains(topSegment(name.mid(7))))
                continue;
            // Restrict to the app dir (defend against path traversal in archive names).
            const QString dest = QDir::cleanPath(app + QStringLiteral("/") + name);
            if (!dest.startsWith(QDir::cleanPath(app) + QStringLiteral("/"))) continue;
            QDir().mkpath(QFileInfo(dest).absolutePath());
            QFile f(dest);
            if (f.open(QIODevice::WriteOnly)) { f.write(bytes); f.close(); }
        }
    }
    mz_zip_reader_end(&z);
    return true;
}

// A deterministic fingerprint of the local synced state (settings + addon/theme files), independent of the
// zip's byte layout. Lets us tell "this device has unsynced edits" from "nothing changed".
static QByteArray stateHash()
{
    QCryptographicHash h(QCryptographicHash::Sha256);
    QStringList keys;
    // Same carve-out as buildBundle: a device-local key isn't synced, so it must not enter the fingerprint —
    // otherwise a purely-local edit reads as an unsynced change and cross-device baselines never converge.
    // The per-item stores are ALSO excluded (mdsync T5): they're owned by the merge document, and if they
    // entered this fingerprint every mark/favorite/playlist/stats tick would read as "local changed" and
    // re-upload the heavy bundle. Keeping them out means per-item churn is served solely by the merge doc's
    // own push cadence; the bundle only re-uploads when a genuinely bundle-synced setting or file changes.
    for (const QString& k : store().allKeys())
        if (!CloudSync::isDeviceLocalKey(k) && !CloudSync::isPerItemStoreKey(k)) keys << k;
    keys.sort();
    for (const QString& k : keys)
    { h.addData(k.toUtf8()); h.addData("="); h.addData(store().value(k).toString().toUtf8()); h.addData("\n"); }

    const QString app = AppPaths::dataDir();
    const QSet<QString> firstParty = firstPartyAddonDirs(); // not synced -> not part of the fingerprint
    for (const QString& sub : { QStringLiteral("addons"), QStringLiteral("themes"),
                                QStringLiteral("saves"), QStringLiteral("states") })
    {
        const QString dir = app + QStringLiteral("/") + sub;
        QStringList files;
        QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) files << it.next();
        files.sort();
        for (const QString& f : files)
        {
            const QString rel = QDir(dir).relativeFilePath(f);
            if (sub == QStringLiteral("addons") && firstParty.contains(topSegment(rel))) continue;
            QFile file(f);
            if (!file.open(QIODevice::ReadOnly)) continue;
            h.addData((sub + QStringLiteral("/") + rel).toUtf8());
            h.addData(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256));
        }
    }
    return h.result().toHex();
}

// Test seam (mdsync T5): the same fingerprint the sync gate uses (checkStatus's st.localChanged). Exposed so
// the headless probe can assert per-item churn no longer flips it (i.e. the heavy bundle stays quiet).
QByteArray CloudSync::stateFingerprint() { return stateHash(); }

void CloudSync::checkStatus(std::function<void(const Status&)> cb)
{
    ensureFolder([this, cb](const QString& folderId) {
        Status st;
        if (folderId.isEmpty()) { cb(st); return; } // unreachable
        st.reached = true;
        const QByteArray synced = store().value(QStringLiteral("cloud/syncedHash")).toByteArray();
        st.localChanged = (stateHash() != synced);
        findFile(folderId, QString::fromLatin1(kBundleName),
                 [this, cb, st, synced](const QString& id, const QString& modIso, const QString& remoteHash) mutable {
            st.hasRemote = !id.isEmpty();
            st.fileId = id;
            st.modifiedIso = modIso;
            st.remoteHash = remoteHash;
            // The remote differs from our last-synced baseline -> another device pushed. Compare content
            // hashes (robust); fall back to modifiedTime only for a legacy bundle without the hash stamp.
            if (!st.hasRemote)
                st.remoteChanged = false;
            else if (!remoteHash.isEmpty())
                st.remoteChanged = (remoteHash.toUtf8() != synced);
            else
                st.remoteChanged = (modIso != store().value(QStringLiteral("cloud/appliedModified")).toString());
            cb(st);
        });
    });
}

void CloudSync::applyRemote(const QString& fileId, const QString& modifiedIso, const QString& remoteHash,
                            std::function<void(bool)> cb)
{
    if (fileId.isEmpty()) { cb(false); return; }
    downloadFile(fileId, [this, modifiedIso, remoteHash, cb](bool ok, const QByteArray& data) {
        if (!ok || !applyBundle(data)) { cb(false); return; }
        store().setValue(QStringLiteral("cloud/appliedModified"), modifiedIso);
        // Baseline = the remote we just took, so a re-check sees neither side changed (no false conflict).
        store().setValue(QStringLiteral("cloud/syncedHash"),
                         remoteHash.isEmpty() ? stateHash() : remoteHash.toUtf8());
        store().sync();
        cb(true);
    });
}

void CloudSync::pushLocal(std::function<void(bool, const QString&)> cb)
{
    ensureFolder([this, cb](const QString& folderId) {
        if (folderId.isEmpty()) { cb(false, tr("Couldn't reach Drive.")); return; }
        const QByteArray bundle = buildBundle();
        const QByteArray hash = stateHash();
        findFile(folderId, QString::fromLatin1(kBundleName), [this, folderId, bundle, hash, cb](const QString& id, const QString&, const QString&) {
            uploadFile(folderId, id, QString::fromLatin1(kBundleName), QStringLiteral("application/zip"), bundle,
                       QString::fromUtf8(hash),
                       [this, folderId, hash, cb](const QString& newId) {
                if (newId.isEmpty()) { cb(false, tr("Upload failed.")); return; }
                findFile(folderId, QString::fromLatin1(kBundleName), [this, hash, cb](const QString&, const QString& modIso, const QString&) {
                    if (!modIso.isEmpty()) store().setValue(QStringLiteral("cloud/appliedModified"), modIso);
                    store().setValue(QStringLiteral("cloud/syncedHash"), hash);
                    store().sync();
                    cb(true, tr("Backed up to Google Drive."));
                });
            });
        });
    });
}

static const char* kProgressName = "mymediavault-progress.json";

void CloudSync::pullProgress(std::function<void(bool, const QByteArray&)> cb)
{
    ensureFolder([this, cb](const QString& folderId) {
        if (folderId.isEmpty()) { cb(false, QByteArray()); return; }
        findFile(folderId, QString::fromLatin1(kProgressName), [this, cb](const QString& id, const QString&, const QString&) {
            if (id.isEmpty()) { cb(true, QByteArray()); return; } // no progress file yet — a valid "nothing to merge"
            downloadFile(id, [cb](bool ok, const QByteArray& data) { cb(ok, data); });
        });
    });
}

void CloudSync::pushProgress(const QByteArray& json, std::function<void(bool)> cb)
{
    ensureFolder([this, json, cb](const QString& folderId) {
        if (folderId.isEmpty()) { cb(false); return; }
        findFile(folderId, QString::fromLatin1(kProgressName), [this, folderId, json, cb](const QString& id, const QString&, const QString&) {
            uploadFile(folderId, id, QString::fromLatin1(kProgressName), QStringLiteral("application/json"), json,
                       QString(), [cb](const QString& newId) { cb(!newId.isEmpty()); });
        });
    });
}
