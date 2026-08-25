// Per-profile list of saved SUBSONIC music servers (issue #193): a Navidrome / Airsonic / Gonic / Ampache /
// Astiga box the user adds once, so it appears as a browsable supplier inside the Music category rather than
// something re-entered per session. Mirrors OpdsCatalogStore exactly — a QSettings + JSON wrapper over the
// shared everythingbox.ini, keyed by the active profile — and is QtCore-only, so probe_subsonic links it in
// a few lines and it touches no UI.
//
// WHY SEVERAL SERVERS FROM THE START. Not because many people run two, but because supporting one is a
// decision that cannot be undone: a store written against a single server has no field naming WHICH server a
// row came from, so every id it has ever written is unqualifiable after the fact. See Subsonic.h — the id
// scheme is the whole point, and it needs a server identity to qualify with.
//
// CREDENTIALS ARE DEVICE-LOCAL. Each server carries a username and a password, and the URL itself is
// machine/network specific (a LAN address, a private host). The store writes under the "subsonic/" ini
// prefix, which CloudSync::isDeviceLocalKey carves OUT of the synced settings bundle — a synced bundle is a
// zip in somebody's Drive folder, and a password in it is a password on a third party's disk. probe_cloudmerge
// pins that carve-out.
//
// The password is NEVER logged and never rendered: it is read at request-build time by SubsonicClient and
// turned into a per-request salted token there. Note what that means for diagnostics, because it is the trap
// this protocol sets — a log line containing a request URL contains `t` and `s` together, which is the
// interesting half of the password. Subsonic.h says it again where the token is computed.
#pragma once
#include <QList>
#include <QString>
#include <functional>

struct SubsonicServer
{
    QString id;         // stable per-server identity (a uuid) — what Subsonic::qualify() qualifies WITH,
                        // so it is in every id this server's rows ever produce and must never be reused
    QString name;       // the user-facing shelf name ("Navidrome")
    QString url;        // the server root, e.g. https://music.example.com — no trailing slash needed
    QString username;
    QString password;   // DEVICE-LOCAL — never synced, never logged, never shown unmasked
    // The explicit choice, per server, rather than a silent downgrade. HTTPS is required unless this is set,
    // and Subsonic::checkUrl answers InsecureRefused rather than quietly sending the credential in clear.
    bool    allowPlainHttp = false;
    // The legacy plaintext `p` parameter, for the old servers that do not accept the token scheme. An opt-in
    // and never a fallback: a client that retries a REFUSED token as a plaintext password has just handed
    // the password to a server that may have refused the token because it is not the server the user thinks.
    bool    legacyAuth = false;
};

namespace SubsonicServerStore
{
    QList<SubsonicServer> list();                       // for the active profile, in insertion order

    // Cheap enough to ask on every home refresh — see HomeView's Music tab gate. One QSettings read of one
    // string plus a JSON parse of a handful of objects; no network, no disk walk, no per-server work. That
    // bound is deliberate: this decides whether the Music category is drawn at all, and a gate that reached
    // a server to answer would make the whole home screen wait on a box that may be switched off.
    bool hasServers();

    // Add a server. If s.id is empty a stable uuid is minted; returns the id the server now has. De-duped by
    // id (a re-add carrying an existing id updates in place rather than duplicating).
    QString add(const SubsonicServer& s);

    void update(const SubsonicServer& s);
    void remove(const QString& id);
    bool get(const QString& id, SubsonicServer& out);   // false if no such server

    // Fired after every mutation. MainWindow sets it to the same refresh the music-folder setting triggers,
    // which is what makes the Music tab appear the moment the first server is added and disappear when the
    // last one is removed — on every layout, with no restart. Unset in headless probes (fires nothing).
    void setChangeHook(std::function<void()> hook);
}
