// PER-PROFILE LIST OF CONNECTED JELLYFIN SERVERS (issue #160). "My box, plus the one a friend shares with
// me" is an ordinary Jellyfin setup, and app-switching between them is exactly the friction a hub exists to
// remove — so this store holds N of them from the first line, the way SubsonicServerStore does, and for the
// reason that file states: supporting one is a decision that cannot be undone.
//
// It mirrors SubsonicServerStore exactly — a QSettings + JSON wrapper over the shared everythingbox.ini,
// keyed by the active profile — and is QtCore-only, so probe_jellyfin links it in a few lines and it touches
// no UI.
//
// ==================================================================================================
// THE ID IS THE SERVER'S OWN `Id`, NOT A UUID WE MINT AND NOT ITS URL
// ==================================================================================================
// This is the one place this store differs from its Subsonic twin, and it is deliberate. Subsonic's protocol
// exposes no server identity at all, so that store mints a uuid of its own. Jellyfin DOES expose one —
// /System/Info/Public's `Id` — and it is strictly better than anything we could mint, because it is the same
// value from every device. Two EverythingBox installs that both connect to the same server therefore agree
// about what its rows are called, so a resume position banked on the television is found by the phone.
// A minted uuid would be per-install, and the two devices would key the same item two different ways.
//
// It is not the URL, for the reasons Jellyfin.h's section 1 gives at length: a URL is where the server is
// answering from THIS device on THIS network today, and keying on it would re-key every stored row the day
// the user put a certificate in front of the box.
//
// Adding a server therefore begins with an unauthenticated /System/Info/Public call, and a server whose
// identity cannot be read is NOT ADDED — there would be nothing to qualify its rows with.
//
// ==================================================================================================
// THE TOKEN IS DEVICE-LOCAL, AND `enabled` IS NOT A DELETION
// ==================================================================================================
// Each server carries an access token. The store writes under the "jellyfin/" ini prefix, which
// CloudSync::isDeviceLocalKey carves OUT of the synced settings bundle — a synced bundle is a zip in
// somebody's Drive folder, and a token in it is a credential on a third party's disk, for a server the user
// signed into on one machine. probe_cloudmerge pins that carve-out. The token is never logged and never
// rendered; it is read at request-build time and put in a header (Jellyfin::authHeader), never in a message.
//
// `enabled` hides a server's rows without forgetting anything: the issue asks for per-server enable so the
// friend's 8,000 films can be got out of the way for an evening, and a switch that deleted the sign-in would
// be a different verb. REMOVING a server drops its token and its row here, and deliberately LEAVES ITS
// STORED DATA ALONE — the resume positions and marks under `jf:<thatServerId>:…` are harmless (nothing
// resolves them) and the server may well come back, at which point they are all still there.
#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

struct JellyfinServer
{
    QString id;         // THE SERVER'S OWN `Id` (/System/Info/Public). See the header: not a minted uuid,
                        // not the URL. It is in every id this server's rows ever produce.
    QString name;       // the user-facing display name, which is what a merged row is tagged with
    QString url;        // the server root, e.g. https://jf.example.com — no trailing slash needed
    QString userId;     // the signed-in user's id, needed to address /Users/<id>/Items
    QString userName;   // display only
    QString token;      // DEVICE-LOCAL — never synced, never logged, never shown
    bool    enabled = true;
    // The explicit choice, per server, rather than a silent downgrade: a Jellyfin sign-in POSTs the
    // password, so plain HTTP is refused by Jellyfin::checkUrl unless the user has said otherwise for this
    // server, and the question is asked BEFORE the password is sent.
    bool    allowPlainHttp = false;
};

namespace JellyfinServerStore
{
    QList<JellyfinServer> list();            // for the active profile, in insertion order
    QList<JellyfinServer> enabled();         // the subset the merged library fans out to
    QStringList           ids();             // every configured server's id — the migration's "which server"

    bool hasServers();

    // Add or update. The id is the server's OWN id and is REQUIRED: a server whose identity could not be
    // read cannot qualify a single row, so adding it would write ids nothing can ever resolve. Returns
    // false, and stores nothing, for an empty or malformed id. De-duped by id — re-adding a server the user
    // already has (a friend re-shares it, the url changed) updates in place rather than duplicating.
    bool add(const JellyfinServer& s);

    void update(const JellyfinServer& s);
    void setEnabled(const QString& id, bool on);

    // Forget the sign-in. Drops the token with the row. Stored ITEM data under this server's qualified ids
    // is deliberately untouched — see the header.
    void remove(const QString& id);

    bool get(const QString& id, JellyfinServer& out);   // false if no such server

    // Fired after every mutation. MainWindow sets it to the same refresh the library-folder setting
    // triggers. Unset in headless probes (fires nothing).
    void setChangeHook(std::function<void()> hook);

#ifdef EB_JELLYFIN_TEST_SEAM
    // Test-only ini redirect, the same macro and the same rule as JellyfinMigrate's: without the define the
    // symbol does not exist, so a production call is a compile error rather than a silent process-wide
    // redirect. Load-bearing here for a second reason — this store holds TOKENS, and a probe run against the
    // app's real ini would write a fixture credential into the user's own settings file.
    void setIniPathForTesting(const QString& path);
#endif
}
