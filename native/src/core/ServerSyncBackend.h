// The self-hosted-server implementation of the SyncBackend transport seam (Increment C). Where DriveSyncBackend
// speaks Google Drive's REST API behind OAuth, this speaks a small versioned object store over plain HTTP: a
// per-namespace list, and get/put/delete on a percent-encoded key. Auth is not a flow — it is configuration
// (a server URL, an optional access token, a namespace), read lazily from the app's own ini. CloudSync composes
// one of these exactly as it composes a DriveSyncBackend, and everything layered above the six primitives (the
// state bundle, SaveSync's index compare-and-swap, the progress merge) runs unchanged.
//
// The contract, per primitive:
//   endpoint(key) = serverBase [ + "/" + token ] + "/sync/" + ns [ + "/" + percent-encode(key) ]
//   - the token is a URL PATH PREFIX, never a header, and is never logged.
//   - the object list is GET endpoint("")  -> {objects:[{key,version,meta,size,deleted,modifiedUtc}]}
//   - a blob is        GET/PUT/DELETE endpoint(key)
//   - `meta` is the client's content-hash CAS token (Drive's stateHash), carried on PUT as `X-Sync-Meta`.
#pragma once
#include "SyncBackend.h"
#include "PendingPush.h"
#include <QObject>
#include <QString>
#include <QByteArray>
#include <functional>

class QNetworkAccessManager;

class ServerSyncBackend : public SyncBackend
{
    Q_OBJECT
public:
    explicit ServerSyncBackend(QObject* parent = nullptr);

    // ---- auth / account: configuration, not an OAuth flow ----
    bool isSignedIn() const override;        // a server URL is configured
    QString accountEmail() const override;   // the server URL stands in for the "account" in the UI
    // A server backend never refreshes a token, so a push is never parked behind a dead grant: always Ok.
    PendingPush::Auth lastAuth() const override { return PendingPush::Auth::Ok; }
    void signIn() override;                   // emits signedIn(serverBase()) if a URL is set, else signInFailed()
    void signOut() override;                  // forget the URL + token; emits signedOut()

    // ---- the six primitives (callbacks fire on the GUI thread) ----
    void ensureFolder(std::function<void(const QString& folderId)> cb) override;
    void findFile(const QString& folderId, const QString& name,
                  std::function<void(bool listOk, const QString& id, const QString& modifiedIso, const QString& stateHash)> cb) override;
    void uploadFile(const QString& folderId, const QString& existingId, const QString& name,
                    const QString& mimeType, const QByteArray& data, const QString& stateHash,
                    std::function<void(const QString& id)> cb) override;
    void downloadFile(const QString& fileId, std::function<void(bool ok, const QByteArray& data)> cb) override;
    void findFolderNamed(const QString& name, std::function<void(bool queryOk, const QString& id)> cb) override;
    void renameFile(const QString& fileId, const QString& newName, std::function<void(bool ok)> cb) override;

private:
    QString serverBase() const;   // cloud/server/url (trimmed); "" => not configured
    QString token() const;        // cloud/server/token (trimmed); "" => no token prefix in the path
    QString ns() const;           // cloud/server/namespace, else ProfileStore::currentId()
    QString endpoint(const QString& key) const;

    QNetworkAccessManager* nam_ = nullptr;
};
