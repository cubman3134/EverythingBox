// Google Drive sign-in + sync backend. Uses the OAuth 2.0 "loopback" flow for desktop apps (browser consent ->
// a redirect to a temporary 127.0.0.1 port we listen on -> token exchange, PKCE). Scope: drive.file, so
// the app can only touch files IT creates (a "EverythingBox" folder). The refresh token is stored on-device.
//
// This is the Google Drive implementation of the SyncBackend transport seam: sign in/out, token refresh, and
// the six Drive primitives (find-or-create folder, find/upload/download a file, rename, folder lookup).
// CloudSync composes one of these and layers the state bundle + carve-outs + merge on top of these primitives.
#pragma once
#include "SyncBackend.h"
#include "PendingPush.h"   // #34: the Auth classification the token refresh reports
#include <QObject>
#include <QString>
#include <QByteArray>
#include <functional>

class QNetworkAccessManager;
class QTcpServer;

class DriveSyncBackend : public SyncBackend
{
    Q_OBJECT
public:
    explicit DriveSyncBackend(QObject* parent = nullptr);

    static bool isConfigured();          // an OAuth client id/secret is available (embedded or in settings)
    bool isSignedIn() const override;    // we hold a refresh token
    QString accountEmail() const override;  // the signed-in Google account (cached), or empty

    // Why the LAST token refresh could not produce an access token — the one fact that separates "the account
    // needs a human" from "the network needs patience" (#34). isSignedIn() cannot answer it: a revoked grant
    // leaves cloud/refreshToken in place, so this device still reads as signed in while every Drive call fails.
    // Ok until a refresh has actually failed; reset to Ok by any successful refresh and by a fresh sign-in.
    // Reports a CLASSIFICATION, never any part of a token — the refresh body is neither logged nor stored.
    PendingPush::Auth lastAuth() const override { return lastAuth_; }

    // Whether interactive Drive sign-in works on this platform: true on desktop, false under Q_OS_ANDROID (the
    // OAuth-on-Android follow-up is pending). The onboarding Restore action stays VISIBLE everywhere and consults
    // this on tap so an Android user gets a graceful "not available yet" decline, never a dead end.
    static bool signInAvailable();
    void signIn() override;              // run the browser consent flow; emits signedIn()/signInFailed()
    void signOut() override;             // forget the tokens; emits signedOut()

    // ---- Drive primitives (callbacks fire on the GUI thread) ----
    void ensureFolder(std::function<void(const QString& folderId)> cb) override;
    void findFile(const QString& folderId, const QString& name,
                  std::function<void(bool listOk, const QString& id, const QString& modifiedIso, const QString& stateHash)> cb) override;
    void uploadFile(const QString& folderId, const QString& existingId, const QString& name,
                    const QString& mimeType, const QByteArray& data, const QString& stateHash,
                    std::function<void(const QString& id)> cb) override;
    void downloadFile(const QString& fileId, std::function<void(bool ok, const QByteArray& data)> cb) override;
    void findFolderNamed(const QString& name, std::function<void(bool queryOk, const QString& id)> cb) override;
    void renameFile(const QString& fileId, const QString& newName, std::function<void(bool ok)> cb) override;

    // Escape a value being interpolated into a Drive `q=` string literal. Drive's query grammar quotes with
    // apostrophes and escapes with backslash, so a name carrying either — "Link's Awakening" is an ordinary
    // ROM title — otherwise terminates the literal early and the query fails or, worse, matches something
    // else. Backslashes first, or the escape we just added gets escaped again.
    static QString driveQueryQuote(const QString& value);

private:
    void exchangeCode(const QString& code, const QString& verifier, const QString& redirectUri);
    void fetchAccountEmail();
    // Ensure a fresh access token, then call cb(true) — or cb(false) if we can't (forces re-sign-in).
    void withAccessToken(std::function<void(bool ok)> cb);

    QNetworkAccessManager* nam_ = nullptr;
    QTcpServer* loopback_ = nullptr;
    QString accessToken_;
    qint64 accessExpiryMs_ = 0;          // epoch ms when accessToken_ expires
    PendingPush::Auth lastAuth_ = PendingPush::Auth::Ok;
    QString pendingVerifier_, pendingState_, redirectUri_;
};
