// Steam achievements for an installed PC game (the Hydra-launcher approach). The appid comes from the game's
// own steam_appid.txt; the achievement definitions (names + icons) come from the Steam web API (needs a Steam
// web API key); and which ones are unlocked comes from the local Steam-emulator save (Goldberg / GSE), since
// DRM-free repacks have no real Steam login. Only works for games that are installed locally.
#pragma once
#include <QObject>
#include <QString>
#include <QList>
#include <QSet>
#include <functional>

class QNetworkAccessManager;

class SteamAchievements : public QObject
{
    Q_OBJECT
public:
    explicit SteamAchievements(QObject* parent = nullptr);

    struct Ach { QString title; QString icon; bool earned = false; };

    static bool configured();  // a Steam web API key (steam/apikey) is set
    // gameDir = the installed game's folder (holds steam_appid.txt + the emu's unlock file). Empty -> nothing.
    void fetch(const QString& title, const QString& gameDir, std::function<void(QList<Ach>)> cb);

private:
    QSet<QString> readUnlocked(int appid, const QString& gameDir) const; // apinames the emu marks earned
    QNetworkAccessManager* nam_ = nullptr;
};
