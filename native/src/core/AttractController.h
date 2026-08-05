// Attract mode (issue #54): the idle screensaver that fades into a full-screen slideshow of library art after
// N minutes of no input. This header is the PURE, windowless heart of it — no Qt widgets, no real clock, no
// player — so every decision it makes is unit-testable under the offscreen QPA (see probe_attract). MainWindow
// owns the QWidget overlay, the idle QTimer and the Settings reads; this owns the three things that are actually
// hard to get right:
//
//   1. WHEN attract mode enters. It may only fire when it is enabled, NOT suppressed by active playback (a
//      screensaver over a running game or video is a bug), the idle time is past the timeout, there is at
//      least one slide to show, and it is not already showing. `poll()` is the timer tick that decides this.
//
//   2. WHAT it shows. The rotation is built from the library's art, and a game with NO usable art is SKIPPED
//      rather than shown blank. `buildSlides()`/`slideFor()` are pure and pin that. The rotation advances and
//      WRAPS.
//
//   3. The ENTER -> input -> RESTORE round-trip, which is the #1 correctness property. An attract overlay that
//      captures input and does not release it cleanly STRANDS the user, which is worse than no attract mode at
//      all. So: entering captures the caller's opaque "prior view" token; the FIRST input after entering both
//      DISMISSES attract and is SWALLOWED (standard screensaver etiquette — the first press only wakes); and
//      every input after that is NOT swallowed, so it flows to the restored view exactly as normal. `noteInput`
//      returns precisely that decision, and the round-trip is what probe_attract drives end to end.
//
// The class is deliberately clock-injected: `poll()` and `noteInput()` take the current monotonic time in
// milliseconds, so a test drives the whole idle/fire/dismiss timeline with fabricated times and no waiting.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <QPair>
#include <QtGlobal>

struct MediaArt;

// One slide in the slideshow: the artwork to Ken-Burns over, and the game title to overlay on it.
struct AttractSlide
{
    QString title;
    QString art;    // resolved local path (offline-first) or url of the artwork to display
    bool isValid() const { return !art.isEmpty(); }
};

class AttractController
{
public:
    // The art roles a FULL-SCREEN slideshow prefers, best first. These are the wide, screen-filling roles the
    // issue names (fanart / hero / screenshot) plus their common synonyms; a poster/box is a portrait grid
    // image and is a poor last resort, so it is deliberately NOT here — a game with only a box poster is
    // skipped rather than shown letterboxed. Exposed so probe_attract pins the contract.
    static QStringList preferredRoles();

    // Build a slide for one game, or an INVALID slide (isValid()==false) when the game has none of the
    // preferred roles — the caller drops those. Picks the first preferred role the art actually carries.
    static AttractSlide slideFor(const QString& title, const MediaArt& art);

    // Build the whole rotation from (title, art) pairs, dropping every game with no usable art. Order is
    // preserved; the caller may start the rotation at any index (poll/enter's startIndex) to randomise it.
    static QVector<AttractSlide> buildSlides(const QVector<QPair<QString, MediaArt>>& library);

    // --- Configuration (MainWindow pushes these from Settings) ---
    void setEnabled(bool on);
    void setTimeoutMs(qint64 ms);
    void setPlaybackActive(bool on);   // true while content is on screen: blocks entry and dismisses if active
    void setSlides(const QVector<AttractSlide>& slides);

    bool    enabled() const        { return enabled_; }
    qint64  timeoutMs() const      { return timeoutMs_; }
    bool    playbackActive() const { return playbackActive_; }
    bool    active() const         { return active_; }
    bool    hasSlides() const      { return !slides_.isEmpty(); }
    int     slideCount() const     { return slides_.size(); }

    // Seed the idle clock (call when a fresh menu screen appears, or on startup) so the FIRST timeout is
    // measured from a real "now", not from 0. Without it, a large monotonic `now` on the first poll would look
    // like the app had been idle since the epoch and fire immediately.
    void resetIdle(qint64 nowMs) { lastInputMs_ = nowMs; }

    // What feeding one input decides.
    struct InputResult
    {
        bool    swallow = false;    // true => the caller must NOT dispatch this input (attract consumed it)
        bool    dismissed = false;  // true => attract JUST left; restore `restoreToken`
        QString restoreToken;       // the view token captured at enter(), valid only when dismissed
    };
    // Feed one input event (call on EVERY pad/key/remote press, BEFORE dispatching it). Always resets the idle
    // clock. When attract is active it DISMISSES and SWALLOWS this one input; when it is not, it does neither,
    // so ordinary navigation is untouched.
    InputResult noteInput(qint64 nowMs);

    // The timer tick. Returns true when it JUST entered attract mode this call, having captured `viewToken` as
    // the view to restore on dismiss and started the rotation at `startIndex` (wrapped). Entry needs: enabled,
    // not playback-suppressed, has slides, not already active, and idle >= timeout. While suppressed it keeps
    // the idle clock pinned to `now`, so the FULL timeout must elapse after playback ends before it can fire.
    bool poll(qint64 nowMs, const QString& viewToken, int startIndex = 0);

    // Force entry regardless of the idle clock — the "preview attract now" / test hook. Same capture and
    // rotation-start semantics as poll's entry. No-op (returns false) when there are no slides or it is
    // already active.
    bool enter(const QString& viewToken, int startIndex = 0);

    // The slide currently on screen (INVALID when not active or empty). currentIndex() is where in the
    // rotation we are; advance() steps to the next slide, WRAPPING, and returns the new index.
    AttractSlide currentSlide() const;
    int currentIndex() const { return index_; }
    int advance();

    QString restoreToken() const { return restoreToken_; }

private:
    bool canEnter(qint64 nowMs) const;

    bool    enabled_ = false;
    qint64  timeoutMs_ = qint64(10) * 60 * 1000;   // 10 minutes
    bool    playbackActive_ = false;
    bool    active_ = false;
    qint64  lastInputMs_ = 0;
    QVector<AttractSlide> slides_;
    int     index_ = 0;
    QString restoreToken_;
};
