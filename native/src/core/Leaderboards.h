// RetroAchievements LEADERBOARDS and RICH PRESENCE — the pure half (issue #94, increment 2).
//
// rcheevos' rc_client already fetches every leaderboard with the game's achievement set and raises an event
// for each stage of an attempt; nothing here talks to the network, and nothing here includes a rcheevos
// header. This file holds the parts that DECIDE something — the rcheevos-event-id -> kind map, the tracker
// overlay's whole state machine, the hardcore-only submission rule and the user-facing copy that states it —
// so a probe can drive all of them against a hand-built (fake) event source with no account, no login and no
// socket. `Achievements` is the thin adapter: it decodes rc_client_event_t into ra::Event and hands it to
// ra::dispatch(), which is the ONE place an event becomes a UI-visible fact.
//
// Deliberately QtCore-only (QString/QVector) and free of QObject: the signal shapes live on `Achievements`,
// the rules live here.
#pragma once

#include <QString>
#include <QVector>

namespace ra
{
    // ---- the rcheevos event ids this feature routes on ----------------------------------------------------
    // Transcribed BY HAND from third_party/rcheevos/include/rc_client.h (the RC_CLIENT_EVENT_* enum) so this
    // header stays free of the C headers and the probe can pin the mapping without linking rcheevos.
    // Achievements.cpp static_asserts every one of these against the real symbol, so a rcheevos bump that
    // renumbers the enum fails the BUILD rather than silently routing a submit into the "attempt failed" arm.
    enum RcEventId : unsigned
    {
        kRcLeaderboardStarted   = 2,   // RC_CLIENT_EVENT_LEADERBOARD_STARTED
        kRcLeaderboardFailed    = 3,   // RC_CLIENT_EVENT_LEADERBOARD_FAILED
        kRcLeaderboardSubmitted = 4,   // RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED
        kRcTrackerShow          = 10,  // RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW
        kRcTrackerHide          = 11,  // RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE
        kRcTrackerUpdate        = 12,  // RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE
        kRcScoreboard           = 13,  // RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD
    };

    // The four attempt stages the issue names (started / failed / submitted / the submit RESULT), plus the
    // three tracker-overlay lifecycle events. Anything else rc_client raises maps to None and is ignored here.
    enum class EventKind
    {
        None,
        AttemptStarted,     // the player entered a leaderboard's start condition
        AttemptFailed,      // the attempt's cancel condition fired — nothing is sent
        AttemptSubmitted,   // the attempt completed and a value went to the server (hardcore only)
        SubmitResult,       // the server answered: submitted/best value, new rank, entry count
        TrackerShow,        // an on-screen running value should appear
        TrackerUpdate,      // ... and now reads differently
        TrackerHide,        // ... and should go away
    };

    // rcheevos event id -> the kind we route on. Unknown/unrelated ids are None (rc_client raises achievement,
    // reset, server-error and connection events through the same handler).
    EventKind kindForRcEvent(unsigned rcEventType);

    // One leaderboard as the UI sees it. `trackerValue` is rc_client's live/last formatted value for the
    // leaderboard (the same 24-byte display string the tracker shows), empty when there is no attempt running.
    struct Leaderboard
    {
        unsigned id = 0;
        QString  title;
        QString  description;
        QString  trackerValue;
        bool     lowerIsBetter = false;
        bool     active = false;   // rc_client has this leaderboard armed for the current session
    };

    // The server's answer to a submitted attempt (RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD).
    //
    // `title` is the BOARD'S NAME, and it does not come from the scoreboard event: rcheevos' scoreboard
    // carries the leaderboard's ID and its values, and leaves event->leaderboard null (issue #312). It is
    // looked up by id at the point the notice is built (Achievements::leaderboardTitle ->
    // rc_client_get_leaderboard_info) and is EMPTY when the lookup finds nothing — no game loaded, or a board
    // rc_client does not know — in which case the notice falls back to its generic heading rather than
    // inventing a name. Nothing here is logged: a board's name says what is being played and how well.
    struct Scoreboard
    {
        unsigned id = 0;
        QString  title;       // the board this result is FOR; empty -> the notice's generic heading
        QString  submitted;   // the value that was sent
        QString  best;        // the player's best value on this board
        unsigned newRank = 0;
        unsigned numEntries = 0;
    };

    // A decoded rcheevos leaderboard event. Only the fields its kind uses are populated.
    struct Event
    {
        EventKind  kind = EventKind::None;
        Leaderboard leaderboard;    // AttemptStarted / AttemptFailed / AttemptSubmitted
        unsigned   trackerId = 0;   // TrackerShow / TrackerUpdate / TrackerHide
        QString    trackerDisplay;
        Scoreboard scoreboard;      // SubmitResult
    };

    // ---- the tracker overlay's state ---------------------------------------------------------------------
    // rc_client can run several attempts at once and gives each ONE tracker id (attempts with an identical
    // value definition share a tracker). This holds the visible set, oldest first, and is the only thing the
    // overlay paints from. It takes no input and owns no widget by construction — it is data.
    class TrackerModel
    {
    public:
        void show(unsigned trackerId, const QString& display);    // new tracker, or replace an existing id
        void update(unsigned trackerId, const QString& display);  // value moved; an unknown id is ignored
        void hide(unsigned trackerId);                            // one attempt ended (others may continue)
        void clear();                                             // game unloaded / session reset: show nothing

        bool    visible() const { return !entries_.isEmpty(); }
        int     count() const   { return entries_.size(); }
        bool    has(unsigned trackerId) const;          // asked separately from displayFor(): a tracker whose
                                                        // value is legitimately "" is still showing

        QString display() const;                        // every visible tracker, oldest first, one per line
        QString displayFor(unsigned trackerId) const;   // a single tracker's value ("" if not visible)

    private:
        struct Entry { unsigned id = 0; QString display; };
        QVector<Entry> entries_;
    };

    // ---- the honesty rules -------------------------------------------------------------------------------
    // RetroAchievements' rule, not ours: a leaderboard attempt is only SUBMITTED from a hardcore session, and
    // only for a signed-in player. Softcore attempts still start, still track and still show their value —
    // they simply do not reach the server, and the UI says so rather than failing quietly.
    bool willSubmit(bool hardcoreActive, bool loggedIn);

    // The marking on every row of the per-game leaderboard list. Never empty: a leaderboard that cannot submit
    // must READ as such, and one that can should say so too, so the two states are told apart at a glance.
    QString submissionNote(bool hardcoreActive, bool loggedIn);

    // The one-line STATUS shown for an attempt event - "Attempt started", "Attempt failed", "Submitted 1'02"
    // and, crucially, the softcore "- not submitting". It deliberately does NOT repeat the board's name: the
    // notice card carries that on its own line, and a single combined string was long enough that the elide
    // ate the "not submitting" clause off the end - the one part that must never be lost.
    // `willSubmit` is the verdict willSubmit() already reached (the signals carry it, so a start and its
    // submit cannot disagree). Empty for a kind that has no notice - the tracker lifecycle events change a
    // running value rather than announce anything.
    QString attemptNotice(EventKind kind, const Leaderboard& lb, bool willSubmit);

    // The one-line notice for the server's answer to a submit.
    QString scoreboardNotice(const Scoreboard& sb);

    // ---- dispatch ----------------------------------------------------------------------------------------
    // Everything the UI layer learns from a leaderboard event. `Achievements` implements this by emitting its
    // signals; a probe implements it by recording — so both drive the SAME dispatch code below.
    struct Sink
    {
        virtual ~Sink() = default;
        virtual void attemptStarted(const Leaderboard& lb) = 0;
        virtual void attemptFailed(const Leaderboard& lb) = 0;
        virtual void attemptSubmitted(const Leaderboard& lb) = 0;
        virtual void submitResult(const Scoreboard& sb) = 0;
        // Collapsed to one call on purpose: the overlay only ever needs "is anything showing, and what does it
        // read" — show/update/hide are three ways to reach that, not three things to draw.
        virtual void trackerChanged(bool visible, const QString& display) = 0;
    };

    // The ONE dispatcher: apply a decoded event to the tracker model and make at most one sink call. A tracker
    // event that changes nothing (an update for an id that is not showing) makes no call at all, so the overlay
    // is not repainted for nothing.
    void dispatch(const Event& e, TrackerModel& tracker, Sink& sink);
}
