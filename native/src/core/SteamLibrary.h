// Reads the locally-installed Steam library (no API key / network needed): finds the Steam install, walks
// its library folders, and lists installed games from the appmanifest_*.acf files. Games launch through the
// steam:// protocol. Cross-platform install-path detection (Windows registry / macOS / Linux defaults).
#pragma once
#include <QString>
#include <QVector>

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
}
