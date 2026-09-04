// MUSIC SCROBBLING (issue #192, increment 1) — the rules that are COMMON TO EVERY SERVICE, and nothing else.
//
// WHY THIS FILE EXISTS, AND WHY IT IS NOT INSIDE A PROVIDER. The app is getting two scrobble targets:
// ListenBrainz now, Last.fm next. Almost nothing that decides WHETHER and WHEN a track is scrobbled is a
// property of either service — the threshold, what counts as a track, what an audiobook is, whose play it is —
// and every one of those rules put inside a provider would be written twice and would drift the first time one
// of them was corrected. So the seam (ScrobbleProvider.h) carries only "post this listen to this service", and
// everything above it lives here, as pure functions over plain structs.
//
// NOTHING HERE TOUCHES THE WORLD. No Qt beyond QString, no network, no settings, no clock: `nowUnix` and the
// player's position are PARAMETERS, for the same reason trakt::planMissed takes its own clock — a rule that
// read the clock itself could only be tested by waiting. probe_scrobble drives every arm below with no
// account, no socket and no audio device.
//
// ==================================================================================================
// THE THRESHOLD — half the track, or four minutes, whichever comes first
// ==================================================================================================
// This is the convention BOTH services expect, and it is the one number in the feature that fails silently
// when it is wrong: too low and the app scrobbles tracks the listener skipped, which quietly corrupts a
// listening history that people have been accumulating for a decade; too high and long tracks never scrobble
// at all, which looks like the network being down. Neither shows up as an error anywhere.
//
// Three clauses, each load-bearing:
//
//   * HALF THE LENGTH. A three-minute track needs 90 seconds; a ten-minute one needs five minutes... except
//   * ...FOUR MINUTES CAPS IT. Half of a forty-minute live set is twenty minutes, and nobody who has listened
//     to a side of a record for four minutes has "skipped" it. The cap is what makes long-form music
//     scrobbleable at all.
//   * AN UNKNOWN LENGTH FALLS BACK TO THE CAP, because it is the only arm that can still be evaluated. Note
//     which way that errs: the alternative (treat unknown as "scrobble immediately") would scrobble every
//     stream the moment it started, which is the skipped-track failure at its worst.
//
// And the floor: a one-second track's half is zero, and a zero threshold is reached before a single sample has
// played — the exact off-by-one this rule exists to prevent, arriving from the bottom rather than the top. So
// the half arm is clamped to at least one second: everything must be PLAYED for some time to count.
//
// WHAT IS DELIBERATELY NOT HERE: Last.fm's additional "a track must be longer than 30 seconds" rule. That is a
// property of ONE service's API contract, not of scrobbling, and ListenBrainz has no such rule. It belongs in
// the Last.fm provider's own accept test in increment 2, which is exactly the kind of thing the seam's
// per-provider gate exists for. Putting it here would make this app refuse to send ListenBrainz a listen that
// ListenBrainz would have accepted.
//
// ==================================================================================================
// PLAY TIME IS ACCUMULATED, NOT READ OFF THE CLOCK OR THE POSITION
// ==================================================================================================
// "Passed the threshold" means the listener HEARD that much of it. Three ways to get that wrong:
//
//   * READING THE POSITION. Seek to 3:00 of a 4:00 track and the position says the threshold is passed while
//     nothing has been heard. A scrobbler that trusts the position scrobbles a whole album by dragging the
//     seek bar across it.
//   * READING THE WALL CLOCK. A paused track accrues wall time and no sound. So does a track whose audio has
//     stalled — and on this app's classic layout mpv's audio does not advance at all while the window is
//     occluded, so wall time and heard time diverge by minutes.
//   * SUMMING RAW POSITION DELTAS. Correct while playing, and a forward seek arrives as one enormous delta
//     that credits the whole jump.
//
// So: sum the position deltas, and CREDIT NOTHING for a delta that is negative (a rewind, or a gapless
// boundary resetting the position to zero) or larger than kMaxCreditStepSec (a seek). A paused track's delta
// is zero and credits zero without any pause flag having to be plumbed in. The step bound is generous against
// the player's event rate and far under any seek a person makes — see the constant.
//
// ==================================================================================================
// THE BOUNDARY THAT INVOLVES NO LOAD
// ==================================================================================================
// Gapless is on by default, and under it mpv walks its OWN playlist: the next track begins with no reload, no
// file-open, and no trip through the host's play sink. Increment 4 of #193 hit exactly this — a per-track hook
// wired to the play sink misses every gapless advance — so the accumulator is designed to be driven from the
// ONE signal that fires across every kind of boundary (PlaybackSession::trackChanged) plus a position feed
// that is a property of the player rather than of any page. `begin()` resets the position anchor to a sentinel
// so the first tick of a new track credits nothing, which is what makes a boundary safe whether the position
// went 210 -> 0 (gapless), stayed put (a re-open of the same file), or jumped anywhere at all.
//
// ==================================================================================================
// PER-PROFILE KEYS, AND WHICH HALF OF THEM A SETTINGS DISCARD MAY REVERT
// ==================================================================================================
// The two key families at the bottom are here rather than in Settings.cpp so that CloudSync's device-local
// carve-out and SettingsTxn's transaction scope can both be written in terms of the SAME constants the writers
// use, the way trakt::backfillKeyPrefix() already is. A carve-out that names a literal drifts from the writer
// silently, and the drift is invisible until a token is uploaded to somebody's Drive.
#pragma once
#include <QString>

namespace Scrobble
{
    // ---- What is playing ------------------------------------------------------------------------------

    // Music, or something spoken. The distinction is not about the file — it is about whether a "listen" is a
    // meaningful unit. A twelve-hour audiobook submitted as one track is noise in a listening history, and an
    // audiobook queue submitted per chapter is worse. Both services agree; neither enforces it.
    enum class Kind { Music, Spoken };

    // WHO ELSE MIGHT ALREADY BE COUNTING THIS PLAY. This is the coordination hazard the issue names: if
    // EverythingBoxServer's Subsonic endpoint ever forwards its own plays upstream, a server that forwards AND
    // a client that scrobbles means every play is counted twice, and the user's only symptom is a history that
    // says they listened to everything exactly twice.
    //
    // The arm exists NOW, unused in anger, because the alternative is a redesign later: the moment the server
    // grows forwarding, the only change this side needs is for the play's origin to be reported as `Server`
    // and for Policy::serverForwards to be set from the server's own capability answer. No new parameter, no
    // new call site, no reshaping of the seam. See verdictFor().
    enum class Origin
    {
        LocalLibrary,   // a file in this machine's music library (#74)
        Remote,         // an addon/stream source that carries its own tags
        Server          // an EverythingBoxServer-backed source, which may one day forward its own plays
    };

    // One track, as both services want it described. `albumArtist` is carried separately from `artist`
    // because on a compilation they differ and the two services use them for different things; a scrobble
    // sends the TRACK artist, which is what the listener actually heard.
    struct Track
    {
        QString artist;        // the track artist — REQUIRED
        QString title;         // the track title  — REQUIRED
        QString album;         // may be empty (a single, a loose file)
        QString albumArtist;   // may be empty
        int     trackNumber = 0;   // 0 == untagged
        int     durationSec = 0;   // 0 == unknown (see thresholdSec)
        Kind    kind   = Kind::Music;
        Origin  origin = Origin::LocalLibrary;
    };

    // A COMPLETED listen, waiting to be delivered. `listenedAt` is the moment the track STARTED, in unix
    // seconds, and it is captured at the start and carried unchanged through the offline queue — see
    // ScrobbleQueue.h for why a submission stamped with the time it was finally delivered is worse than one
    // that was never delivered at all.
    struct Play
    {
        Track  track;
        qint64 listenedAt = 0;
    };

    // ---- The threshold --------------------------------------------------------------------------------

    // Four minutes, in seconds. The cap arm of the rule, and the answer for a track of unknown length.
    constexpr int kCapSec = 240;

    // The floor on the half arm. See the header: a zero threshold is passed before anything has played.
    constexpr int kMinSec = 1;

    // How much play time a single position report may credit, in seconds. The player reports position several
    // times a second, so a real step is well under one; anything above this is a seek, and a seek is not
    // listening. Deliberately generous (a stutter, a slow frame, a 2x playback rate) and deliberately far
    // below the smallest seek any transport in this app performs (the jump buttons start at 10 seconds).
    constexpr double kMaxCreditStepSec = 4.0;

    // Half the track, or four minutes, whichever comes first; the cap for an unknown length; never below
    // kMinSec. See the header for every clause. Total over nonsense: a negative duration reads as unknown.
    inline int thresholdSec(int durationSec)
    {
        if (durationSec <= 0) return kCapSec;      // unknown length: only the cap arm can be evaluated
        const int half = durationSec / 2;
        if (half > kCapSec) return kCapSec;
        return half < kMinSec ? kMinSec : half;
    }

    // ---- What counts ----------------------------------------------------------------------------------

    // The user's answer to "scrobble my listening", plus the two questions the answer alone cannot settle.
    struct Policy
    {
        bool enabled       = false;  // the visible per-profile on/off
        bool includeSpoken = false;  // the per-source toggle for anyone who DOES want audiobooks + podcasts
        bool serverForwards = false; // a server-sourced play is already being forwarded upstream by the server
    };

    // Why a play was or was not submitted. An ENUM rather than a bool because "nothing happened" is the
    // complaint every scrobbling client has ever attracted: the surface can say which of these it was, and the
    // probe can tell "off" from "untagged" from "an audiobook" instead of asserting one lump of false.
    enum class Verdict
    {
        Submit,
        SkipOff,             // scrobbling is switched off for this profile
        SkipUntagged,        // no usable artist or title — never send "Unknown Artist"
        SkipSpoken,          // an audiobook or podcast, and the per-source toggle is off
        SkipServerForwards   // the server that served this play is forwarding it itself
    };

    // The one gate. Ordered so the most specific true statement wins: a user who has scrobbling switched off
    // is told that, not that their file is untagged.
    //
    // THE UNTAGGED CLAUSE IS NOT A NICETY. A library where half the files are named "01.mp3" would otherwise
    // submit hundreds of listens for "Unknown Artist — 01", to a service that will happily accept every one of
    // them, into a history that cannot be cleaned up from this app. Skipping is recoverable (tag the files and
    // play them again); submitting is not.
    inline Verdict verdictFor(const Track& t, const Policy& p)
    {
        if (!p.enabled)                                     return Verdict::SkipOff;
        if (t.artist.trimmed().isEmpty()
            || t.title.trimmed().isEmpty())                 return Verdict::SkipUntagged;
        if (t.kind == Kind::Spoken && !p.includeSpoken)     return Verdict::SkipSpoken;
        if (t.origin == Origin::Server && p.serverForwards) return Verdict::SkipServerForwards;
        return Verdict::Submit;
    }

    inline bool eligible(const Track& t, const Policy& p) { return verdictFor(t, p) == Verdict::Submit; }

    // ---- The accumulator ------------------------------------------------------------------------------

    // One track's worth of listening, in progress. Owned by the host, advanced by the position feed, and read
    // at the boundary. Copyable and comparable by value so a probe can drive a whole album through it.
    struct Watch
    {
        Track  track;
        qint64 startedAt   = 0;      // unix seconds; what a completed Play is stamped with
        double playedSec   = 0.0;    // accumulated HEARD seconds — see the header
        double lastPos     = -1.0;   // the previous position report; < 0 means "no anchor yet"
        bool   counts      = false;  // verdictFor said Submit when this track began
        bool   submitted   = false;  // the threshold has already been crossed and reported once
        bool   active      = false;  // there is a track here at all
    };

    // Start watching a track. The position anchor is reset to the sentinel so the FIRST report of the new
    // track credits nothing, whatever the position was before it — which is what makes a gapless boundary
    // (210 -> 0), a manual jump (210 -> 400) and a re-open of the same file (210 -> 210) all safe with one
    // rule instead of three.
    inline void begin(Watch& w, const Track& t, qint64 nowUnix, const Policy& p)
    {
        w.track     = t;
        w.startedAt = nowUnix;
        w.playedSec = 0.0;
        w.lastPos   = -1.0;
        w.counts    = eligible(t, p);
        w.submitted = false;
        w.active    = true;
    }

    inline void clear(Watch& w) { w = Watch{}; }

    // Has this watch heard enough? Read separately from advance() so a boundary can ask the question about a
    // track whose last position report never arrived.
    inline bool reached(const Watch& w)
    {
        return w.active && w.counts && w.playedSec >= double(thresholdSec(w.track.durationSec));
    }

    // Feed one position report. Returns TRUE exactly once per track: on the report that carries it across the
    // threshold. The `submitted` latch is what makes "exactly once" true — without it every later tick of a
    // long track would report a fresh scrobble, and an album left playing would submit one track hundreds of
    // times.
    inline bool advance(Watch& w, double positionSec)
    {
        if (!w.active) return false;
        const double prev = w.lastPos;
        w.lastPos = positionSec;
        if (prev >= 0.0)
        {
            const double d = positionSec - prev;
            // Negative (a rewind, or a gapless boundary's reset) and oversized (a seek) both credit nothing.
            if (d > 0.0 && d <= kMaxCreditStepSec) w.playedSec += d;
        }
        if (w.submitted) return false;
        if (!reached(w)) return false;
        w.submitted = true;
        return true;
    }

    // The boundary's question: this track is ending (a gapless advance, a skip, the end of the queue, the page
    // being left). Does it owe a scrobble? TRUE only for a track that passed the threshold and has not already
    // been reported — so a track that crossed mid-play and was submitted then is not submitted twice, and a
    // track skipped one second short of the threshold is not submitted at all.
    inline bool finish(Watch& w)
    {
        if (!w.active) return false;
        const bool owed = !w.submitted && reached(w);
        if (owed) w.submitted = true;
        return owed;
    }

    // ---- Where the state lives ------------------------------------------------------------------------
    //
    // Two families, and the split is not cosmetic — it is which half of the feature a settings Discard is
    // allowed to revert.
    //
    //   settingsKeyPrefix()  the things the user TYPES or TOGGLES: the on/off, the audiobook opt-in, the token,
    //                        the custom API URL. IN a settings transaction's scope, so opening Settings,
    //                        pasting the wrong token and pressing Discard puts the old one back.
    //   stateKeyPrefix()     the things PLAYBACK writes: the confidence counter, the offline queue, the last
    //                        error. Written continuously while a settings panel is open (an album is playing
    //                        behind it — #193 made that ordinary), so in scope they would inflate the exit
    //                        prompt with "3 settings changed" the user never touched, and a Discard would
    //                        throw away queued listens. Same rule, and the same reasons, as the Trakt token
    //                        and calendar-cache exclusions beside them.
    //
    // BOTH are device-local for sync: the token is a secret that must never ride a bundle to another machine
    // (the whole point of the carve-out), and the queue and counter are this device's own accumulators — two
    // machines merging their counters would report a number neither of them scrobbled.
    //
    // Distinct TOP-LEVEL groups rather than "scrobble/state/…" so no profile id can ever collide with the
    // discriminator; profile ids are minted uuids and "state" would be an absurd one, but a carve-out that is
    // only correct because of what an id happens to look like is not a carve-out.
    inline QString settingsKeyPrefix() { return QStringLiteral("scrobble/"); }
    inline QString stateKeyPrefix()    { return QStringLiteral("scrobblestate/"); }

    // Every key this feature owns. What CloudSync::isDeviceLocalKey asks.
    inline bool isDeviceLocalKey(const QString& key)
    {
        return key.startsWith(settingsKeyPrefix()) || key.startsWith(stateKeyPrefix());
    }

    // The half a settings transaction must not snapshot or revert. What SettingsTxn::inScope asks.
    inline bool isBackgroundStateKey(const QString& key) { return key.startsWith(stateKeyPrefix()); }

    // ...AND THE OTHER HALF IT MUST NOT REVERT, which is NOT the same rule and was learned the hard way.
    //
    // settingsKeyPrefix() was described above as "the things the user TYPES or TOGGLES", and for increment 1
    // that was the whole truth: a ListenBrainz token is pasted, so pasting the wrong one and pressing Discard
    // has to put the old one back. Last.fm's credential is not typed at all. It is a SESSION KEY the service
    // hands back after the user has approved this app in a browser, and it arrives from a background poll
    // reply that can land in the middle of a settings visit the user is making about something else.
    //
    // In scope it is wrong twice over. The exit prompt says "2 setting(s) changed" about two values nobody
    // touched — observed, not theorised: it is what the #192 increment 2 live drive saw the first time it
    // connected an account and pressed Back — and Discard silently unlinks an account that cost a browser
    // round trip to link, which a typed token does not, because a typed token can simply be typed again.
    //
    // This is exactly the "ra/user" / "ra/token" case SettingsTxn.cpp already spells out beside it (sign in,
    // then Discard, and the stored token reverts while the live session stays signed in), and the same split
    // as trakt/access-vs-trakt/clientId. The group is matched through the writer's OWN prefix rather than a
    // literal, so an exclusion cannot drift from the key Settings.cpp writes.
    inline QString authorisedCredentialGroup() { return QStringLiteral("/lastfm/"); }
    inline bool isAuthorisedCredentialKey(const QString& key)
    {
        return key.startsWith(settingsKeyPrefix()) && key.contains(authorisedCredentialGroup());
    }

    // An empty profile id means "no profile chosen yet" and maps to "default", exactly as the Trakt backfill
    // cursor's slot does — so the keys a pre-profile launch writes are the ones the default profile reads.
    inline QString profileSlot(const QString& profileId)
    {
        return profileId.isEmpty() ? QStringLiteral("default") : profileId;
    }
}
