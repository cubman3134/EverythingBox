#pragma once
#include "PendingPush.h"
#include <QObject>
#include <QString>
#include <QByteArray>
#include <functional>

// The transport seam for cloud sync. Google Drive is one implementation (DriveSyncBackend); a
// self-hosted server backend is another (Increment C). CloudSync orchestrates ABOVE this — bundle,
// merge, carve-outs — and reaches a backend only through these primitives.
class SyncBackend : public QObject
{
    Q_OBJECT
public:
    explicit SyncBackend(QObject* parent = nullptr) : QObject(parent) {}
    ~SyncBackend() override = default;

    // ---- auth / account (Drive uses OAuth; a token-URL backend reports differently) ----
    virtual bool isSignedIn() const = 0;
    virtual QString accountEmail() const = 0;
    virtual PendingPush::Auth lastAuth() const = 0;
    virtual void signIn() = 0;
    virtual void signOut() = 0;

    // ---- the six primitives (callbacks fire on the GUI thread) ----
    virtual void ensureFolder(std::function<void(const QString& folderId)> cb) = 0;
    virtual void findFile(const QString& folderId, const QString& name,
        std::function<void(bool listOk, const QString& id, const QString& modifiedIso, const QString& stateHash)> cb) = 0;
    virtual void uploadFile(const QString& folderId, const QString& existingId, const QString& name,
        const QString& mimeType, const QByteArray& data, const QString& stateHash,
        std::function<void(const QString& id)> cb) = 0;
    virtual void downloadFile(const QString& fileId, std::function<void(bool ok, const QByteArray& data)> cb) = 0;
    virtual void findFolderNamed(const QString& name, std::function<void(bool queryOk, const QString& id)> cb) = 0;
    virtual void renameFile(const QString& fileId, const QString& newName, std::function<void(bool ok)> cb) = 0;

signals:
    void signedIn(const QString& email);
    void signInFailed(const QString& error);
    void signedOut();
};
