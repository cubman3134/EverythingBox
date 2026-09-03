// THE TRACKER RULES LAYER (issue #156) — every wire format AniList speaks, and every decision the push/pull
// machinery makes. PURE: QByteArray/structs in, QByteArray/structs out. No network, no GUI, no ini, no clock
// (the debounce takes its "now" as an argument). probe_tracker pins the whole of it against fixtures with no
// socket and no account, which is the only way the offline, rate-limited and "the tracker is ahead of us"
// cases are reachable at all.
//
// The relationship to Tracker.h is the relationship TraktRead has to TraktClient: the seam declares the
// vocabulary, this declares what the bytes look like, and only AniListTracker owns a socket.
//
// WHY THE ANILIST BODIES ARE BUILT HERE RATHER THAN INLINE AT THE REQUEST SITE. Three of them are the places
// this feature can do irreversible damage to somebody's tracker account: a mutation that sends `scoreRaw` when
// the app has no rating overwrites a score the user set by hand; one that sends status COMPLETED off a
// miscounted total marks a series finished that is not; one that sends a progress LOWER than the account's
// regresses it. All three are decisions about the body's CONTENT, so the body is built by a function a probe
// can call.
#pragma once
#include "Tracker.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace tracker
{
    // ================= the AniList wire =================================================================
    namespace anilist
    {
        // The two hosts, named once. The GraphQL endpoint is overridable at RUN TIME through the
        // EB_ANILIST_ENDPOINT / EB_ANILIST_AUTH environment variables so a fixture stub can stand in for the
        // real service in a live drive — see AniListTracker.cpp. They are read there, not here: this layer
        // stays pure, and a probe asserting the DEFAULT must not be able to be satisfied by an environment.
        inline QString defaultApiUrl()  { return QStringLiteral("https://graphql.anilist.co"); }
        inline QString defaultAuthBase() { return QStringLiteral("https://anilist.co/api/v2/oauth"); }

        // The browser URL that starts the authorization-code flow. `redirectUri` is the app's loopback
        // listener ("http://127.0.0.1:<port>"), exactly as the Drive sign-in uses — the redirect URI
        // registered on AniList's developer page must match it, which is why the docs tell the user to
        // register the loopback form.
        QString authorizeUrl(const QString& authBase, const QString& clientId, const QString& redirectUri);

        // POST bodies for the two token grants. JSON, because AniList's token endpoint accepts JSON and a
        // form encoding would put the SECRET through percent-encoding for no benefit.
        QByteArray tokenExchangeBody(const QString& clientId, const QString& clientSecret,
                                     const QString& redirectUri, const QString& code);
        QByteArray tokenRefreshBody(const QString& clientId, const QString& clientSecret,
                                    const QString& refreshToken);

        // What a token endpoint hands back.
        struct TokenReply
        {
            bool    ok = false;       // the body WAS a token reply carrying a non-empty access token
            QString accessToken;
            QString refreshToken;     // "" when the reply carried none (a refresh may omit it)
            qint64  expiresInSec = 0;
        };
        // TOTAL: any body that is not a token reply — an error object, an HTML captive-portal page, a
        // truncated read — comes back ok=false with every field empty. The caller MUST NOT store an
        // unsuccessful reply: writing its empty strings over the live tokens is the failure mode that
        // permanently unlinks an account on a transient 502 (TraktRead §13 documents the same hazard).
        TokenReply parseTokenReply(const QByteArray& json);

        // ---- the three GraphQL operations -------------------------------------------------------------
        // Search. `year` narrows on the media's START year when non-zero and is omitted entirely when 0 —
        // omitted, not sent as 0, because AniList reads startDate_like="0%" as a filter that matches nothing.
        QByteArray searchBody(const QString& title, int year, Kind kind);
        // TOTAL, like every parser here: a non-JSON body, a GraphQL `errors` payload, or a media row missing
        // its id all yield an empty list rather than a partly-built one.
        QVector<Match> parseSearch(const QByteArray& json);

        // Read the signed-in account's list entry for one media.
        QByteArray entryBody(const QString& mediaId);
        // ok=false = "this body was not an entry reply". An entry with exists=false = "it was, and the
        // account has no row". `mediaId` is echoed onto the result so a caller holding several in flight
        // does not have to correlate by request.
        bool parseEntry(const QByteArray& json, const QString& mediaId, Entry& out);

        // The push. `totalUnits` is what the tracker says the series HAS (0 = unknown); it is passed in
        // rather than trusted from the Update because the COMPLETED decision belongs to the tracker's count,
        // not to the app's guess about it.
        //
        // THE THREE SAFETY RULES, all of them assertable off the returned bytes:
        //   * `scoreRaw` is present ONLY when u.hasScore. AniList reads 0 as "rated zero", not "unrated".
        //   * `status` is COMPLETED only when u.completes AND the unit really is the last one by
        //     `totalUnits` (or totalUnits is unknown and the caller has already decided). Otherwise CURRENT.
        //   * `progress` is never negative and never below 1 for a real completion event.
        QByteArray saveBody(const Update& u, int totalUnits);

        // AniList's MediaListStatus spellings, both ways. An unknown token reads back as Current — the
        // safest wrong answer, because it is the one status a push would overwrite with the same value.
        QString statusToken(Status s);
        Status  statusFromToken(const QString& token);
    }

    // ================= the push machinery ================================================================

    // One mutation per item per 30 seconds. A binge-read turns pages fast enough to fire several completion
    // events a minute, and AniList's rate limit is per MINUTE — the debounce is what keeps a reader from
    // spending it. The value is a constant rather than a setting because a user has no way to know what a
    // safe number is, and the wrong one gets the account throttled rather than merely being slow.
    constexpr qint64 kDebounceMs = 30000;

    // May an update for an item whose last mutation went out at `lastSentMs` be sent at `nowMs`?
    // TRUE when nothing has been sent yet (lastSentMs <= 0) — a first push is never delayed.
    // TRUE for a clock that has gone BACKWARDS (nowMs < lastSentMs): a system clock correction, or an ini
    // written by a machine whose clock ran fast, must not silently suspend pushing for hours. The stamp is
    // wall clock because it has to survive a restart, so this clause is not theoretical.
    bool debounceAllows(qint64 lastSentMs, qint64 nowMs);

    // FURTHEST WINS, applied to the PENDING queue. Adding an update for an item the queue already holds one
    // for REPLACES it when the new one is further along, and is dropped when it is not — the queue never
    // grows a row per page turn, and it can never deliver a lower progress after a higher one. Returns true
    // when `q` changed.
    //
    // "Further" compares `unit` first and then `completes` — an update that completes the series at the same
    // unit is further than one that does not, because it carries the status transition.
    bool coalesce(QVector<Update>& q, const Update& u);

    // The most updates kept on disk. Far past any plausible offline binge; a queue that grew without bound
    // would write an unbounded row into the ini. Drops from the FRONT (the oldest) on overflow, for
    // ScrobbleQueue's reason: the newest progress is the progress still worth delivering.
    constexpr int kMaxQueued = 500;
    int applyQueueCap(QVector<Update>& q);   // returns how many were dropped

    // Round-trips through JSON, exposed so the replay-after-restart property is assertable without an ini.
    // decode is TOTAL: a malformed row is skipped, a malformed document yields an empty queue.
    QByteArray encodeQueue(const QVector<Update>& q);
    QVector<Update> decodeQueue(const QByteArray& json);

    // ================= the pull machinery ================================================================

    // #136's rule, made explicit. Neither side is ever regressed.
    enum class Reconcile
    {
        Nothing,       // the two agree
        AdvanceLocal,  // the tracker is ahead — move the app's completion marks up to it
        PushRemote,    // the app is ahead — send our progress to the tracker
    };
    // `localUnits` / `remoteUnits` are counts of finished episodes/chapters. Negative inputs are clamped to
    // 0 rather than trusted: a negative "how far through" is not a direction, it is corrupt state, and
    // treating it as behind would push a wrong number into somebody's account.
    Reconcile reconcile(int localUnits, int remoteUnits);

    // ================= identity helpers =================================================================

    // The chapter NUMBER a run entry's title names ("Vol. 2 · Ch. 14" -> 14, "Chapter 7" -> 7, "007" -> 7),
    // or `fallback` when the title names none. Used because a run's list POSITION is not a chapter number:
    // a run captured from a partial listing starts at whatever the provider returned first, and pushing the
    // index would report chapter 1 for chapter 340.
    //
    // Deliberately prefers a "ch"-marked number over a bare one, so "Vol. 2 · Ch. 14" is 14 and not 2.
    int chapterNumberFromTitle(const QString& title, int fallback);

    // The episode number inside an app episode stream id ("ttShow:season:episode"), or 0 when the id is not
    // one. The video completion path already holds this string, so the tracker hook needs no new plumbing
    // through the player.
    int episodeFromStreamId(const QString& streamId);
    // The series part of the same id ("ttShow"), or "" — what a link is keyed against for a series, so
    // every episode of a show reuses one link and one prompt.
    QString seriesFromStreamId(const QString& streamId);

    // THE KEY A LINK IS STORED AGAINST, computed in ONE place so the comic reader, the video player and the
    // detail verb cannot disagree about what "this series" means - which they would, since each holds a
    // different handle on it. An episode stream id ("ttShow:s:e") collapses to its SHOW part, which is also
    // the key the Trakt watched-history import writes marks under, so the two integrations agree about a
    // show. A manga has no such id, so it falls back to a normalised title prefixed "title:" - the same
    // string the comic reader and the series detail page both hold, which is what makes those two agree.
    // Empty in, empty out: an item with no identity has nowhere to remember anything.
    QString itemKeyFor(const QString& imdbStreamId, const QString& title);

    // ---- the state keys, built from Tracker.h's prefixes so the carve-outs cannot drift from the writers.
    QString queueKey(const QString& profileId, Id id);
    QString lastSentKey(const QString& profileId, Id id, const QString& itemKey);
    QString lastErrorKey(const QString& profileId, Id id);
    // An empty profile id means "no profile chosen yet" and maps to "default", exactly as Scrobble's and the
    // Trakt backfill cursor's slots do.
    QString profileSlot(const QString& profileId);
}
