// RetroAchievements "browse" lookup: for a game we're only *looking at* (not playing), fetch its achievement
// set and which ones the signed-in user has earned, via the RetroAchievements web API. Distinct from the
// rcheevos runtime client (Achievements) which needs the ROM loaded in the emulator. Needs the user's web API
// key (ra/apikey) in addition to the login user (ra/user). Console game lists are cached per session; matching
// a catalog title to an RA game is fuzzy (title + console), so it can occasionally miss or mis-match.
#pragma once
#include <QObject>
#include <QString>
#include <QList>
#include <functional>

class QNetworkAccessManager;

class RaBrowse : public QObject
{
    Q_OBJECT
public:
    explicit RaBrowse(QObject* parent = nullptr);

    struct Ach { QString title; QString badge; bool earned = false; };

    static bool configured();                 // ra/user + ra/apikey both set
    // consoleId is an RC_CONSOLE_* / RA console id (see Achievements::consoleIdForExtension). 0 -> skipped.
    void fetch(const QString& title, unsigned consoleId, std::function<void(QList<Ach>)> cb);

private:
    QNetworkAccessManager* nam_ = nullptr;
};
