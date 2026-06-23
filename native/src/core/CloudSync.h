// Google Drive sign-in + sync. Uses the OAuth 2.0 "loopback" flow for desktop apps (browser consent ->
// a redirect to a temporary 127.0.0.1 port we listen on -> token exchange, PKCE). Scope: drive.file, so
// the app can only touch files IT creates (a "MyMediaVault" folder). The refresh token is stored on-device.
//
// Slice 1 (here): sign in/out, token refresh, and the Drive primitives (find-or-create folder, upload,
// download, metadata). Slice 2 layers the state bundle + automatic sync on top of these.
#pragma once
#include <QObject>
#include <QString>
#include <QByteArray>
#include <functional>

class QNetworkAccessManager;
class QTcpServer;

class CloudSync : public QObject
{
    Q_OBJECT
public:
    explicit CloudSync(QObject* parent = nullptr);

    static bool isConfigured();          // an OAuth client id/secret is available (embedded or in settings)
    bool isSignedIn() const;             // we hold a refresh token
    QString accountEmail() const;        // the signed-in Google account (cached), or empty

    void signIn();                       // run the browser consent flow; emits signedIn()/signInFailed()
    void signOut();                      // forget the tokens; emits signedOut()

    // ---- Drive primitives (callbacks fire on the GUI thread) ----
    // Find (or create) the "MyMediaVault" folder; returns its file id ("" on failure).
    void ensureFolder(std::function<void(const QString& folderId)> cb);
    // Find a file by name inside a folder; cb gets {id, modifiedTimeIso} ("" id if absent).
    void findFile(const QString& folderId, const QString& name,
                  std::function<void(const QString& id, const QString& modifiedIso)> cb);
    // Create or update a file's binary content; cb gets the file id ("" on failure).
    void uploadFile(const QString& folderId, const QString& existingId, const QString& name,
                    const QString& mimeType, const QByteArray& data,
                    std::function<void(const QString& id)> cb);
    void downloadFile(const QString& fileId, std::function<void(bool ok, const QByteArray& data)> cb);

    // ---- sync (slice 2) ----
    // Pull: download + apply the Drive bundle if it's newer than what we last applied. result is one of
    // "applied" | "current" | "none" | "error". Push: zip the local state and upload it.
    void pull(std::function<void(const QString& result)> cb);
    void push(std::function<void(bool ok, const QString& message)> cb);

signals:
    void signedIn(const QString& email);
    void signInFailed(const QString& error);
    void signedOut();

private:
    void exchangeCode(const QString& code, const QString& verifier, const QString& redirectUri);
    void fetchAccountEmail();
    // Ensure a fresh access token, then call cb(true) — or cb(false) if we can't (forces re-sign-in).
    void withAccessToken(std::function<void(bool ok)> cb);

    QNetworkAccessManager* nam_ = nullptr;
    QTcpServer* loopback_ = nullptr;
    QString accessToken_;
    qint64 accessExpiryMs_ = 0;          // epoch ms when accessToken_ expires
    QString pendingVerifier_, pendingState_, redirectUri_;
};
