// RetroAchievements integration (built on rcheevos' high-level rc_client). Login persists a token; loading
// a game identifies it by hash and pulls its achievement set; doFrame() evaluates unlock conditions against
// the emulator's memory each frame and emits achievementUnlocked. Softcore only for now (save states stay
// enabled). Only the full-screen emulator uses this (one client, one active game at a time).
#pragma once
#include "Leaderboards.h"   // ra::Event / ra::Leaderboard / ra::TrackerModel (#94 increment 2)

#include <QObject>
#include <QString>
#include <QVector>

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

    // Hardcore mode (issue #94). setHardcore flips rc_client's hardcore flag; ENABLING it with a game loaded
    // resets the achievement session (the site rule — softcore-earned progress cannot carry into hardcore) via
    // rc_client_reset, which also re-enables processing that toggling-on-with-a-game deliberately suspends.
    // hardcoreActive() is the live truth the emulator gates on: hardcore is enabled on the client AND a game
    // with an achievement session is loaded. Off (the default) => hardcoreActive() is false and every gate is
    // a no-op, so the softcore path is byte-for-byte unchanged.
    void setHardcore(bool on);
    bool hardcoreActive() const;

    // ---- Leaderboards (issue #94, increment 2) ------------------------------------------------------------
    // rc_client fetches every leaderboard with the achievement set, so none of this costs a network call.
    // hasLeaderboards() answers whether the loaded game has any at all; leaderboards() is the per-game list the
    // pause menu reads, in rc_client's own order.
    bool hasLeaderboards() const;
    QVector<ra::Leaderboard> leaderboards() const;
    // ONE board's name, by id, from the already-fetched game data (no network call). Empty when no game is
    // loaded or rc_client does not know the id — which is the fallback arm of #312, not an error.
    QString leaderboardTitle(unsigned id) const;
    // RetroAchievements' rule, not ours: an attempt is only sent from a hardcore session, by a signed-in
    // player. Softcore boards stay visible and readable and are MARKED as not submitting (ra::submissionNote).
    bool leaderboardsSubmit() const;

    // The C event trampoline decodes an rc_client_event_t into this POD and hands it here; handleLeaderboard-
    // Event() applies it to the tracker model and emits exactly one of the signals below (ra::dispatch is the
    // one mapping). Public because the trampoline is a free C function, and because it is the seam a probe
    // drives with a fake event source instead of an account.
    void handleLeaderboardEvent(const ra::Event& e);
    // Drop whatever the tracker overlay is showing and say so. Called on every boundary rc_client does not
    // raise a hide for: a game load, a game unload, and a hardcore-enable reset.
    void clearLeaderboardTracker();
    // What the tracker overlay is showing right now ("" when nothing is). Never outlives a game: unloadGame()
    // clears it and emits leaderboardTrackerChanged(false, "").
    QString leaderboardTracker() const;

    // ---- Rich presence (issue #94, increment 2) -----------------------------------------------------------
    // rc_client re-evaluates the presence script inside rc_client_do_frame(); this only READS the current
    // message, so it is a pull, not a stored value. It is NOT free (it re-runs the script against core RAM),
    // so call it when something is about to show it — the pause menu, the future #64/#66 hook surface — never
    // once per frame. Empty when no game is loaded or the game ships no presence script (rc_client would
    // otherwise synthesise "Playing <title>", which tells the user nothing they cannot already see).
    // Never logged: the string names what is being played and how far in.
    bool hasRichPresence() const;
    QString richPresence() const;

signals:
    void loginResult(bool ok, const QString& message);
    void gameLoaded(bool ok, const QString& title, int unlocked, int total);
    void achievementUnlocked(const QString& title, const QString& description, int points, const QString& badgeUrl);

    // The four leaderboard-attempt stages, in the shape achievementUnlocked established, so the UI layer
    // learns nothing new about rcheevos. `willSubmit` on the start signal carries the hardcore-only rule to
    // the overlay so it can say so at the moment the attempt begins.
    void leaderboardAttemptStarted(unsigned id, const QString& title, const QString& description, bool willSubmit);
    void leaderboardAttemptFailed(unsigned id, const QString& title);
    void leaderboardAttemptSubmitted(unsigned id, const QString& title, const QString& value, bool willSubmit);
    // `title` is the board's name, looked up by id when the event was decoded (#312) — the scoreboard event
    // itself carries no leaderboard. Empty when the lookup found nothing, and the notice then falls back to
    // its generic heading; it never guesses.
    void leaderboardSubmitResult(unsigned id, const QString& title, const QString& submitted,
                                 const QString& best, unsigned newRank, unsigned numEntries);
    // The tracker overlay's whole contract: is anything showing, and what does it read. show / update / hide
    // all arrive here, so the overlay has one thing to draw and no lifecycle of its own to get wrong.
    void leaderboardTrackerChanged(bool visible, const QString& display);

private:
    void* impl_ = nullptr; // opaque rcheevos state (keeps the C headers out of this header)
    ra::TrackerModel tracker_; // the leaderboard tracker overlay's state (pure; see Leaderboards.h)
};
