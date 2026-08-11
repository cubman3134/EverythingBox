// Per-profile list of saved OPDS book catalogs (#146): a self-hosted book server the user adds by URL, so it
// appears as a browsable "Reading" shelf rather than something re-entered per session. Mirrors the shape of
// IptvSourceStore / FavoritesStore — a QSettings + JSON wrapper over the shared everythingbox.ini, keyed by
// the active profile so each user has their own catalogs. QtCore-only, so it links into a headless probe in a
// few lines and touches no UI.
//
// CREDENTIALS ARE DEVICE-LOCAL. A catalog carries an optional HTTP basic-auth username + password, and the
// server URL itself is machine/network specific (a LAN address, a private host). The store writes under the
// "opds/" ini prefix, which CloudSync::isDeviceLocalKey carves OUT of the synced settings bundle — so a
// credential-bearing URL never travels to another device. probe_cloudmerge pins that carve-out. The password
// is NEVER logged; build the request header with opdsBasicAuth (ebook/OpdsFeed.h) at fetch time only.
#pragma once
#include <QList>
#include <QString>
#include <functional>

struct OpdsCatalog
{
    QString id;        // stable per-catalog identity (a uuid); the browse row keys off it
    QString name;      // the user-facing shelf name ("My Calibre")
    QString url;       // the OPDS root-feed url (http(s))
    QString username;  // HTTP basic-auth user, or empty for an open catalog
    QString password;  // HTTP basic-auth password, or empty. DEVICE-LOCAL — never synced, never logged.
};

namespace OpdsCatalogStore
{
    QList<OpdsCatalog> list();                       // for the active profile, in insertion order

    // Add a catalog. If cat.id is empty a stable uuid is minted; returns the id the catalog now has. De-duped
    // by id (a re-add carrying an existing id updates in place rather than duplicating).
    QString add(const OpdsCatalog& cat);

    // Replace the catalog with this id (name/url/username/password). A no-op if no catalog has that id.
    void update(const OpdsCatalog& cat);

    void remove(const QString& id);
    bool get(const QString& id, OpdsCatalog& out);   // false if no such catalog

    // Multi-device sync trigger (parity with IptvSourceStore): a change-callback fired after every mutation,
    // set once by MainWindow to (re)arm the debounced push. Unset in headless probes (fires nothing). Note the
    // catalog list is device-local, so this arms only the machinery, never a credential upload.
    void setChangeHook(std::function<void()> hook);
}
