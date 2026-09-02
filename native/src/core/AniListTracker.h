// THE ANILIST TRACKER (issue #156, increment 1) — the only implementation of the Tracker seam so far, and
// the only part of this feature that owns a socket. Everything it decides is decided in TrackerRules; this
// file is the impure half: OAuth, the network access manager, the retry timer and the ini.
//
// AUTH is OAuth AUTHORIZATION CODE, not Trakt's device code: AniList issues no device codes. The redirect is
// the app's existing loopback listener — a QTcpServer on 127.0.0.1 and the system browser, the shape
// DriveSyncBackend::signIn() already uses — so the URI the user registers on AniList's developer page is the
// loopback form. `authUrlReady` carries the URL out to whatever surface is up, so a TV that cannot open a
// browser can still show it (see the note on that signal).
//
// CREDENTIALS live in the device-local secrets carve-out (tracker::settingsKeyPrefix()) and are NEVER logged.
// The `log` signal is deliberately fed exception/status text only: no request body, no Authorization header,
// no query string. A fixture token appearing in a probe transcript would be a failure of this file.
//
// PUSHING is debounced (one mutation per item per 30 s) and QUEUED on disk, so an offline session syncs when
// the app comes back rather than dropping the progress. The queue is a tracker::Update list — see
// TrackerRules — and it coalesces per item, so a fast reader does not grow a row per page turn.
#pragma once
#include "SingleFlight.h"   // ensureValidToken's one-refresh-many-waiters queue, shared with TraktClient
#include "Tracker.h"
#include "TrackerRules.h"

#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QTcpServer;
class QTimer;

class AniListTracker : public QObject, public tracker::Tracker
{
    Q_OBJECT
public:
    explicit AniListTracker(QObject* parent = nullptr);
    ~AniListTracker() override;

    // ---- statics, for the settings surfaces (which have no instance to ask) --------------------------
    static bool isConfigured();   // a client id + secret are present
    static bool isConnected();    // an access token is stored
    // The user's typed client id / secret. The #81 BuiltinSecrets follow-up is a ONE-LINE change inside each
    // of these two — "return typed value, else the embedded slot" — which is why nothing else in this file
    // reads the Settings keys directly. Slot names: tracker::builtinSecretIdSlot() / …SecretSlot().
    static QString clientId();
    static QString clientSecret();
    // The two setters the settings surfaces call. They live HERE rather than in Settings.cpp, unlike the
    // Trakt pair, for the reason the getters do: the #81 follow-up has to be able to change what "the
    // client id" MEANS in one place, and a Settings accessor writing the same ini key from another file
    // would be a second definition of it. The key spellings are tracker::clientIdKey/clientSecretKey, so
    // the sync carve-out and the settings transaction still classify them without knowing about this class.
    static void setClientId(const QString& v);
    static void setClientSecret(const QString& v);

    // ---- the Tracker seam -----------------------------------------------------------------------------
    tracker::Id id() const override { return tracker::Id::AniList; }
    QString displayName() const override { return QStringLiteral("AniList"); }
    bool configured() const override { return isConfigured(); }
    bool connected() const override { return isConnected(); }

    void search(const QString& title, int year, tracker::Kind kind,
                std::function<void(QVector<tracker::Match>)> cb) override;
    void fetchEntry(const QString& mediaId, tracker::Kind kind,
                    std::function<void(bool ok, tracker::Entry)> cb) override;
    void pushProgress(const tracker::Update& u) override;
    void flushQueue() override;

    // ---- account linking ------------------------------------------------------------------------------
    // Open the loopback listener and hand the browser to AniList. Emits authUrlReady immediately (so a
    // surface can show the URL even where no browser opened), then connectedChanged or connectError.
    void connectAccount();
    // Forget the tokens AND this device's pending queue. The queue is per-ACCOUNT progress that the next
    // account has not agreed to receive, so it cannot outlive the link that produced it. The per-item LINKS
    // are deliberately KEPT: they describe the media, not the account, and re-linking a whole library by
    // hand is the cost of getting that wrong.
    void disconnectAccount();

    // How many updates are still waiting to go out (this profile, this tracker). Shown in the settings
    // status line: a queue that silently stopped draining looks identical to one that was always empty.
    static int queuedCount();
    // The last thing that went wrong, for the same line. Empty = nothing has failed since the last success.
    // NEVER contains a credential — see the file header.
    static QString lastError();

signals:
    // The authorization URL, for a surface to display. Emitted for EVERY connect attempt, including ones
    // where QDesktopServices did open a browser, because on a TV the browser may have opened somewhere the
    // user cannot see it. A caller may render it as text or as a code; it carries no secret (the client id
    // is public by design, and there is no token in it).
    void authUrlReady(const QString& url);
    void connectedChanged(bool connected);
    void connectError(const QString& message);
    // Something was accepted by the tracker. `itemKey` is the app's key, `unit` what the account now holds.
    void progressPushed(const QString& itemKey, int unit);
    void queueChanged();
    void log(const QString& line);

private:
    // Refresh if expired, then call done. SINGLE-FLIGHT, for TraktClient's reason: two overlapping refreshes
    // against a rotating refresh token can permanently break the link.
    void ensureValidToken(std::function<void(bool ok)> done);
    void exchangeCode(const QString& code);
    void storeTokenReply(const tracker::anilist::TokenReply& r);
    void closeLoopback();

    // One authenticated GraphQL POST. `cb` receives the raw body and whether the transport succeeded; it is
    // NEVER called with a body when the token gate refused, so no caller has to distinguish "off" from
    // "empty reply" by inspecting bytes.
    void post(const QByteArray& body, std::function<void(bool ok, QByteArray)> cb);

    // The endpoints, read from the environment ONCE per call so a fixture stub can stand in during a live
    // drive (EB_ANILIST_ENDPOINT / EB_ANILIST_AUTH). Absent, both are the real AniList hosts.
    static QString apiUrl();
    static QString authBase();

    // Queue persistence (this profile, this tracker).
    static QVector<tracker::Update> loadQueue();
    static void saveQueue(const QVector<tracker::Update>& q);
    static void setLastError(const QString& message);

    // Send the first queued update whose item passes the debounce; re-arm the timer for the rest. Called on
    // every push, on connect, and when the retry timer fires. Re-entrant-safe through `sending_`.
    void drain();

    QNetworkAccessManager* nam_ = nullptr;
    QTcpServer*            loopback_ = nullptr;
    QString                redirectUri_;
    QTimer*                retry_ = nullptr;
    bool                   sending_ = false;
    SingleFlight           tokenRefresh_;
};
