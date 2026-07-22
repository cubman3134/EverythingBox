// Reads the locally-installed Steam library (no API key / network needed): finds the Steam install, walks
// its library folders, and lists installed games from the appmanifest_*.acf files. Games launch through the
// steam:// protocol. Cross-platform install-path detection (Windows registry / macOS / Linux defaults).
//
// The owned-library helpers below DO use the network (QtNetwork) and a user-supplied Steam Web API key +
// SteamID — they surface owned-but-not-installed games on the Steam console. Everything else stays QtCore-only.
#pragma once
#include <QString>
#include <QVector>
#include <QByteArray>
#include <functional>

class QObject;

struct SteamGame
{
    QString appid; // Steam application id
    QString name;  // display name
};

namespace SteamLibrary
{
    bool isAvailable();                       // a Steam install was found on this machine
    QVector<SteamGame> installedGames();      // installed apps (deduped, sorted by name; tools filtered out)
    QString posterUrl(const QString& appid);  // local librarycache art if present, else the Steam CDN URL
    QString launchUrl(const QString& appid);  // steam://rungameid/<appid>
    QString installUrl(const QString& appid); // steam://install/<appid>  (the owned-not-installed handoff)

    // Owned library via the Steam Web API (IPlayerService/GetOwnedGames). Requires a user-supplied Web API key
    // and 64-bit SteamID (NEVER embedded). Owned = {appid,name} per owned game (sorted by name), TTL-cached
    // (~30 min) per (key,steamId). Split into an INSTANT read + a background FETCH so the console never blocks
    // the GUI thread on the network (the CatalogPrefetcher precedent): passive console-open reads the cache,
    // an async fetch fills it, and a re-present picks it up.

    // Instant, network-free: the TTL-fresh cached owned list for (key,steamId), or {} if never fetched / stale /
    // not configured. Safe to call on every console-open — it NEVER touches the network.
    QVector<SteamGame> ownedGamesCached(const QString& apiKey, const QString& steamId);

    // Background fetch (GUI thread, QNetworkAccessManager async — the existing 8s bound is now the reply
    // timeout). No-ops when not configured OR when the cache is already TTL-fresh (nothing new to fetch). On a
    // real fetch it stores the result (even an empty one — a silent, TTL-bounded fallback so a bad/offline key
    // isn't hammered), then invokes onReady on the GUI thread with the owned list ({} on error/empty). `context`
    // guards the callback: if it is destroyed before the reply lands, onReady never fires. Outcome-only logging;
    // the key / request URL are NEVER logged. Callers should further guard staleness (still at the console).
    void ownedGamesFetch(const QString& apiKey, const QString& steamId, QObject* context,
                         std::function<void(const QVector<SteamGame>&)> onReady);

    // Pure state-machine core (I/O-free, probe-tested): given the current cache identity/timestamp and the
    // requested (key,id) at `now`, decide what ownedGamesFetch does — nothing (unconfigured), reuse the cache
    // (a TTL-fresh hit for the same key+id), or hit the network. Empty key or id -> NotConfigured.
    enum class OwnedFetch { NotConfigured, CacheHit, Fetch };
    OwnedFetch ownedFetchDecision(const QString& cacheKey, const QString& cacheId, qint64 cacheTs,
                                  const QString& reqKey, const QString& reqId, qint64 now, int ttlSecs);

    // Pure parse of a GetOwnedGames JSON body -> games (response.games[].{appid,name}); a game with no name is
    // kept with its appid as the label. Malformed/empty JSON -> {}. Exposed for headless probe coverage.
    QVector<SteamGame> parseOwnedGames(const QByteArray& json);

    // TTL helper (pure): is a list cached at cachedTs still fresh at `now`, given ttlSecs? (false for a zero or
    // future timestamp.) Exposed so the owned-games TTL semantics are probe-testable without touching a clock.
    bool ownedCacheFresh(qint64 cachedTs, qint64 now, int ttlSecs);
}
