// RetroAchievements integration (built on rcheevos' high-level rc_client). Login persists a token; loading
// a game identifies it by hash and pulls its achievement set; doFrame() evaluates unlock conditions against
// the emulator's memory each frame and emits achievementUnlocked. Softcore only for now (save states stay
// enabled). Only the full-screen emulator uses this (one client, one active game at a time).
#pragma once
#include <QObject>
#include <QString>

class LibretroCore;

class Achievements : public QObject
{
    Q_OBJECT
public:
    explicit Achievements(QObject* parent = nullptr);
    ~Achievements() override;

    bool isLoggedIn() const;
    QString username() const;

    void loginWithPassword(const QString& user, const QString& password); // -> loginResult
    void tryLoginWithStoredToken();   // silent re-login at startup if a token was saved
    void logout();

    // Map a ROM extension to an RC_CONSOLE_* id (0 = system RetroAchievements doesn't cover / we can't map).
    static unsigned consoleIdForExtension(const QString& extLower);

    // Called by the full-screen emulator around a game. `console` is an RC_CONSOLE_* id (0 -> skipped).
    void loadGame(LibretroCore* core, unsigned console, const QString& romPath);
    void unloadGame();
    void doFrame();

signals:
    void loginResult(bool ok, const QString& message);
    void gameLoaded(bool ok, const QString& title, int unlocked, int total);
    void achievementUnlocked(const QString& title, const QString& description, int points, const QString& badgeUrl);

private:
    void* impl_ = nullptr; // opaque rcheevos state (keeps the C headers out of this header)
};
