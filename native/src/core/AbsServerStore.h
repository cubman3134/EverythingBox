// Per-profile list of saved AUDIOBOOKSHELF servers (issue #197). Mirrors SubsonicServerStore exactly — a
// QSettings + JSON wrapper over the shared everythingbox.ini, keyed by the active profile, QtCore-only, no
// UI — because it is the same object with a different protocol behind it, and two shapes for one idea is
// how the two drift.
//
// SEVERAL SERVERS FROM THE START, for the reason SubsonicServerStore.h gives at length and #160 sets as
// the rule: supporting one is a decision that cannot be undone. A store written against a single server
// has no field naming WHICH server a row came from, so every id it has ever written is unqualifiable
// after the fact — and an Audiobookshelf item id is unique on one server only. Audiobookshelf.h's id
// scheme is what qualifies with `AbsServer::id`.
//
// ==================================================================================================
// WHAT IS STORED IS A TOKEN, AND THERE IS NOWHERE TO PUT A PASSWORD
// ==================================================================================================
// The user types a password once, into the add-a-server prompt. It goes into the body of ONE request
// (`POST /login`), which answers with an API token, and then it is gone: it is not written here, not held
// in a member, not logged, and — the structural half of that promise — THERE IS NO PASSWORD FIELD ON THIS
// STRUCT. A later change cannot quietly start storing one without adding a field, which is a visible edit
// in a file whose header says not to.
//
// The token itself is DEVICE-LOCAL. Everything is keyed under the "audiobookshelf/" ini prefix, which
// CloudSync::isDeviceLocalKey carves OUT of the synced settings bundle — a synced bundle is a zip in
// somebody's Drive folder, and an API token in it is an API token on a third party's disk, for a server
// the user signed into on one machine. probe_cloudmerge pins that carve-out; probe_absclient byte-scans a
// fixture token against everything this feature writes.
//
// The URL is device-local for the second reason the same carve-out exists for OPDS and Subsonic: a
// self-hosted server's address is a LAN address or a private host, which means nothing on another machine.
#pragma once
#include <QList>
#include <QString>
#include <functional>

struct AbsServer
{
    QString id;         // stable per-server identity — what Abs::qualify() qualifies WITH, so it is in
                        // every id this server's rows ever produce and must never be reused or rewritten.
                        // A uuid, unless /api/authorize published a distinguishing id of the server's own
                        // at add time (Audiobookshelf.h explains why it almost never does).
    QString name;       // the user-facing shelf name ("Audiobookshelf")
    QString url;        // the server root, e.g. https://books.example.com — no trailing slash needed
    QString username;   // shown on the settings row so two accounts on one box are tellable apart
    QString token;      // DEVICE-LOCAL — never synced, never logged, never rendered. NO PASSWORD FIELD.
    // Off keeps the server configured but out of the browse levels — the "I am away from home and that box
    // is not reachable" case, which is otherwise a remove-and-re-add (and a re-typed password).
    bool    enabled = true;
    // The explicit choice, per server, rather than a silent downgrade. HTTPS is required unless this is
    // set, and Abs::checkUrl answers InsecureRefused rather than quietly posting the password in clear.
    bool    allowPlainHttp = false;
};

namespace AbsServerStore
{
    QList<AbsServer> list();          // for the active profile, in insertion order
    QList<AbsServer> enabledList();   // …the subset a browse level actually shows

    // Cheap enough to ask on every home refresh — this decides whether the Audiobooks category is drawn at
    // all when there is no local audiobook folder. One QSettings read of one string plus a JSON parse of a
    // handful of objects; no network, no disk walk. SubsonicServerStore::hasServers states why that bound
    // is deliberate: a gate that reached a server to answer would make the home screen wait on a box that
    // may be switched off.
    bool hasServers();

    // Add a server. If s.id is empty a stable uuid is minted; returns the id the server now has. De-duped
    // by id (a re-add carrying an existing id updates in place rather than duplicating).
    QString add(const AbsServer& s);

    void update(const AbsServer& s);
    void remove(const QString& id);
    bool get(const QString& id, AbsServer& out);   // false if no such server

    // Fired after every mutation. MainWindow sets it to the same refresh the audiobook-folder setting
    // triggers, which is what makes the Audiobooks tab appear the moment the first server is added and
    // disappear when the last one is removed — on every layout, with no restart. Unset in headless probes.
    void setChangeHook(std::function<void()> hook);
}
