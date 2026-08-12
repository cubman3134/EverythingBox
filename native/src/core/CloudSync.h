// Cloud sync ORCHESTRATOR: the state bundle, the CloudMerge progress doc, the device-local carve-out,
// and the sync fingerprint — all transport-neutral. It reaches a cloud backend only through the six
// primitives declared below, which forward to an owned SyncBackend (Increment B). Google Drive sign-in
// (OAuth loopback + PKCE, scope drive.file, on-device refresh token) and the Drive REST primitives now
// live in DriveSyncBackend behind that seam; a self-hosted server backend is Increment C. CloudSync's
// public API (the six virtuals, the sign-in surface, the signals) is unchanged — it forwards.
#pragma once
#include "PendingPush.h"   // #34: the Auth classification the token refresh reports
#include "SyncBackend.h"   // Increment B: the transport seam CloudSync composes (Drive is one backend)
#include <QObject>
#include <QString>
#include <QByteArray>
#include <functional>

class DriveSyncBackend;

class CloudSync : public QObject
{
    Q_OBJECT
public:
    explicit CloudSync(QObject* parent = nullptr);

    static bool isConfigured();          // an OAuth client id/secret is available (embedded or in settings)
    bool isSignedIn() const;             // we hold a refresh token
    QString accountEmail() const;        // the signed-in Google account (cached), or empty

    // Why the LAST token refresh could not produce an access token — the one fact that separates "the account
    // needs a human" from "the network needs patience" (#34). isSignedIn() cannot answer it: a revoked grant
    // leaves cloud/refreshToken in place, so this device still reads as signed in while every Drive call fails.
    // Ok until a refresh has actually failed; reset to Ok by any successful refresh and by a fresh sign-in.
    // Reports a CLASSIFICATION, never any part of a token — the refresh body is neither logged nor stored.
    PendingPush::Auth lastAuth() const;

    // ---- the device-local carve-out (mdsync T4) ----
    // The ONE exclusion table for sync (applied in BOTH directions). A device-local key never travels in the
    // synced settings.json (it's meaningful only to this machine — rom folders, emulator roots, the on-screen
    // pad, this device's identity/cloud tokens, the local downloads/pc-games catalogs). Note the deliberate
    // SIBLING carve-outs: sync/global/* and profiles/list and library/showHidden DO sync.
    static bool isDeviceLocalKey(const QString& key);
    // A per-item store key (resume/recent/marks/favorites/playlists/stats/playstats/deleted). The progress
    // merge document (CloudMerge) owns these exclusively, so applyBundle must NEVER write them from the heavy
    // bundle — a stale peer copy would clobber this device's live accumulator namespace and then propagate.
    static bool isPerItemStoreKey(const QString& key);
    // The bundle's settings.json content (device-local excluded) — the exact bytes buildBundle embeds. Exposed
    // so the headless probe exercises the real carve-out without the zip/network.
    static QByteArray buildSettingsJson();
    // Apply a bundle's settings.json the way applyBundle does: write only keys that are neither device-local
    // NOR per-item-store (the merge file owns those). Exposed for the probe's hands-off assertion.
    static void applySettingsJson(const QByteArray& settingsJson);
    // The sync fingerprint (checkStatus's localChanged gate). Excludes both the device-local carve-out and the
    // per-item stores, so per-item churn does NOT re-upload the heavy bundle (mdsync T5). Exposed for the probe.
    static QByteArray stateFingerprint();
    // checkStatus's st.localChanged, answered WITHOUT a network trip: does the local state still match the
    // baseline the last successful sync recorded? The push funnel consults it so a failed push that had
    // nothing to send does not inflate the pending record (#34 review, minor 4), and the probe uses it to
    // assert the fixed point below.
    static bool localChangedSinceSync();
    // Adopt a remote as the synced baseline: cloud/appliedModified + cloud/syncedHash. FACTORED OUT of
    // applyRemote so the property the whole no-oscillation argument rests on — after a pull, localChanged is
    // FALSE, so the next resolve() answers NothingToSend — is assertable without a socket. `remoteHash` empty
    // means a legacy bundle with no hash stamp, in which case the state we just applied IS the baseline.
    static void adoptSyncedBaseline(const QString& modifiedIso, const QString& remoteHash);

    // Whether interactive Drive sign-in works on this platform: true on desktop, false under Q_OS_ANDROID (the
    // OAuth-on-Android follow-up is pending). The onboarding Restore action stays VISIBLE everywhere and consults
    // this on tap so an Android user gets a graceful "not available yet" decline, never a dead end.
    static bool signInAvailable();
    void signIn();                       // run the browser consent flow; emits signedIn()/signInFailed()
    void signOut();                      // forget the tokens; emits signedOut()

    // ---- Drive primitives (callbacks fire on the GUI thread) ----
    // These four are THE test seam for everything layered on them (SaveSync's index compare-and-swap, its
    // torn-write guard, its listOk guards, its Drive-name mapping): a headless probe subclasses CloudSync,
    // substitutes an in-memory Drive, and every caller above runs for real. Nothing in production overrides
    // them — a seam any higher up would replace the very code whose failure modes are unrecoverable.
    // Find (or create) the "EverythingBox" folder; returns its file id ("" on failure).
    virtual void ensureFolder(std::function<void(const QString& folderId)> cb);
    // Find a file by name inside a folder; cb gets {listOk, id, modifiedTimeIso, stateHash}. listOk is false
    // when the query (or its token refresh) had a network error — the caller must NOT read an empty id as
    // "no such file" in that case (it's "couldn't reach Drive"). "" id with listOk==true means genuinely absent.
    virtual void findFile(const QString& folderId, const QString& name,
                  std::function<void(bool listOk, const QString& id, const QString& modifiedIso, const QString& stateHash)> cb);
    // Create or update a file's binary content; stateHash is stamped into appProperties (may be empty).
    // cb gets the file id ("" on failure).
    virtual void uploadFile(const QString& folderId, const QString& existingId, const QString& name,
                    const QString& mimeType, const QByteArray& data, const QString& stateHash,
                    std::function<void(const QString& id)> cb);
    virtual void downloadFile(const QString& fileId, std::function<void(bool ok, const QByteArray& data)> cb);

    // ---- brand-migration support (see core/BrandMigration.h) ----
    // Look up a FOLDER by exact name at the Drive root. cb(queryOk, id): queryOk==false means the query (or
    // its token refresh) failed, and the caller must NOT read the empty id as "no such folder" — same rule as
    // findFile's listOk, for the same reason (mistaking unreachable for absent is how a duplicate folder or a
    // clobbered backup happens). Never creates anything; ensureFolder owns creation.
    virtual void findFolderNamed(const QString& name, std::function<void(bool queryOk, const QString& id)> cb);
    // Rename a Drive file or folder in place (a metadata PATCH — the id, contents and sharing are unchanged).
    virtual void renameFile(const QString& fileId, const QString& newName, std::function<void(bool ok)> cb);

    // Escape a value being interpolated into a Drive `q=` string literal. Drive's query grammar quotes with
    // apostrophes and escapes with backslash, so a name carrying either — "Link's Awakening" is an ordinary
    // ROM title — otherwise terminates the literal early and the query fails or, worse, matches something
    // else. Backslashes first, or the escape we just added gets escaped again.
    static QString driveQueryQuote(const QString& value);

    // ---- sync ----
    struct Status {
        bool reached = false;       // we could reach Drive (folder query resolved)
        bool listReached = false;   // the bundle file-query itself had NO network error (else "empty" is unproven)
        bool hasRemote = false;     // a bundle exists on Drive
        bool remoteChanged = false; // Drive's bundle differs from what we last applied (another device pushed)
        bool localChanged = false;  // local state differs from what we last synced (this device has edits)
        QString fileId, modifiedIso, remoteHash;
    };
    void checkStatus(std::function<void(const Status&)> cb);                  // query Drive + compare hashes
    void applyRemote(const QString& fileId, const QString& modifiedIso,       // download + apply a bundle (pull)
                     const QString& remoteHash, std::function<void(bool ok)> cb);
    void pushLocal(std::function<void(bool ok, const QString& message)> cb);  // zip + upload the local state

    // ---- "continue watching" progress (a small JSON file synced far more often than the heavy state bundle) ----
    // Merge-based: the caller serializes/merges, these just move the bytes. Empty json on pull => none yet.
    void pullProgress(std::function<void(bool ok, const QByteArray& json)> cb);
    void pushProgress(const QByteArray& json, std::function<void(bool ok)> cb);

    // Locate one of the two brand-named sync documents, tolerating the PREVIOUS brand's file name until the
    // DriveFiles migration step has confirmed the rename. Retires itself the moment that flag is set. Same
    // {listOk,id,modifiedIso,stateHash} contract as findFile, and the same conservatism: an error on EITHER
    // query yields listOk=false, so "unreachable" can never launder into "proven empty".
    void findBrandedFile(const QString& folderId, const QString& name, const QString& legacyName,
                         std::function<void(bool listOk, const QString& id, const QString& modifiedIso,
                                            const QString& stateHash)> cb);

signals:
    void signedIn(const QString& email);
    void signInFailed(const QString& error);
    void signedOut();

private:
    // The transport backend the orchestration above reaches through. In production this is a DriveSyncBackend;
    // the six primitive methods and the auth surface are one-line forwarders to it. Orchestration still calls
    // the six via `this` (they stay virtual), so the headless probes' FakeCloud substitutes at the same seam.
    SyncBackend* backend_ = nullptr;
};
