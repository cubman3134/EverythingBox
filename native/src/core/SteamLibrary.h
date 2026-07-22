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
    // and 64-bit SteamID (NEVER embedded). Returns {appid,name} for every owned game (sorted by name); the
    // result is TTL-cached (~30 min) per (key,steamId). An empty key/steamId does no network and returns {}; a
    // network error or an invalid/empty response also yields {} — the console silently falls back to
    // installed-only (logged as an outcome, never surfaced; the key is NEVER logged). Needs QtNetwork.
    QVector<SteamGame> ownedGames(const QString& apiKey, const QString& steamId);

    // Pure parse of a GetOwnedGames JSON body -> games (response.games[].{appid,name}); a game with no name is
    // kept with its appid as the label. Malformed/empty JSON -> {}. Exposed for headless probe coverage.
    QVector<SteamGame> parseOwnedGames(const QByteArray& json);

    // TTL helper (pure): is a list cached at cachedTs still fresh at `now`, given ttlSecs? (false for a zero or
    // future timestamp.) Exposed so the owned-games TTL semantics are probe-testable without touching a clock.
    bool ownedCacheFresh(qint64 cachedTs, qint64 now, int ttlSecs);
}
