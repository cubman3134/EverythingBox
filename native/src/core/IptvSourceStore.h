// Per-profile IPTV/Live-TV sources (#75, increment 2): a SAVED playlist — a URL or a local file path — that
// appears as a browsable "Live TV" shelf rather than something re-pasted per session. Stored as a JSON array
// in everythingbox.ini, keyed by the active profile (so each user has their own), mirroring FavoritesStore /
// PlaylistStore's per-profile shape. QtCore-only (a QSettings + JSON wrapper), so it links into a headless
// probe in a few lines and touches no UI.
//
// The channel data itself is NOT stored — a source is refreshed from its url on open (a stale channel list
// across sessions is exactly what this feature exists to avoid). Only the SOURCE definition persists here.
#pragma once
#include <QList>
#include <QString>
#include <functional>

struct IptvSource
{
    QString id;      // stable per-source identity (a uuid); the browse row + favourites key off it
    QString name;    // the user-facing name shown on the "Live TV" shelf ("My Provider")
    QString url;     // the playlist location: an http(s) URL or a local .m3u/.m3u8 file path
    // Per-source XMLTV EPG url. RESERVED for increment 3 (the guide) — it is written and round-tripped now, but
    // is EMPTY for every increment-2 source, so increment 3 can fill it without a store migration.
    QString epgUrl;
};

namespace IptvSourceStore
{
    QList<IptvSource> list();                       // for the active profile, in insertion order

    // Add a source. If src.id is empty a stable uuid is minted; returns the id the source now has. De-duped by
    // id (a re-add with an existing id updates in place).
    QString add(const IptvSource& src);

    // Replace the source with this id (name/url/epgUrl). A no-op if no source has that id.
    void update(const IptvSource& src);

    void remove(const QString& id);
    bool get(const QString& id, IptvSource& out);   // false if no such source

    // Multi-device sync trigger (mdsync T2, parity with FavoritesStore/PlaylistStore): a change-callback fired
    // after every mutation, set once by MainWindow to (re)arm the debounced push. QtCore-clean (a std::function,
    // not a Qt signal). Unset in headless probes (fires nothing).
    void setChangeHook(std::function<void()> hook);
}
