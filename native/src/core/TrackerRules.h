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
    // ================= THE ONE SEND POLICY (issue #326) ==================================================
    //
    // WHAT A FAILED PUSH MEANS, decided ONCE for every tracker. Increment 2 gave MyAnimeList a rule AniList
    // never had — 400/404/422 is a refusal that can never become a success, so the row is DROPPED rather than
    // left at the head of an ORDERED queue where it blocks every update behind it for ever. AniList retried
    // everything, so one deleted list entry stopped that account syncing altogether and said nothing about
    // it. #326 is that rule, moved out of `mal` into one place both providers ask, because two answers to
    // "is this row deliverable?" is exactly one too many.
    //
    // A RESPONSE IS ONE OF THREE THINGS and never two of them:
    //   * ACCEPTED   — the provider really applied the write. The row goes, the debounce stamp is set, the
    //                  status line clears. What "really applied" MEANS stays with the provider: AniList
    //                  answers a refused mutation with HTTP 200 (see anilist::saveAccepted), MAL does not.
    //   * RETRYABLE  — waiting could fix it. The row STAYS. The delay doubles from `baseMs`, is capped at
    //                  `maxMs`, and a Retry-After asking for LONGER wins. `reauth` is the retryable case
    //                  where the token is the problem and refreshing is part of the waiting.
    //   * PERMANENT  — waiting can never fix it. The row is dropped AND SAID OUT LOUD. A queue that quietly
    //                  discards somebody's progress is worse than one that wedges, because at least a wedge
    //                  is eventually noticed.
    //
    // WHAT EACH PROVIDER SUPPLIES is the SendPolicy below and nothing else: the status codes its own API
    // actually uses. The arithmetic, the cap, the Retry-After comparison and the drop/keep decision are
    // shared, because there is no version of them that is right for one service and wrong for the other.
    // The Retry-After READING stays per provider by construction — each tracker's transport hands the number
    // in — and both services happen to spell it the same way today, so neither of them writes it twice.
    //
    // THE DEFAULT IS RETRY. A status no policy names is retried, never dropped: nobody's progress is thrown
    // away on a status nobody has thought about.
    struct SendPolicy
    {
        QVector<int> permanent;   // never becomes a success — DROP the row, and say so
        QVector<int> reauth;      // the token is the problem — refresh, keep the row, retry
        QVector<int> throttle;    // rate limited — retry, honouring a longer Retry-After
        qint64 baseMs = 60000;    // the first wait, and the floor under every later one
        qint64 maxMs  = 1800000;  // the ceiling on the doubling
    };

    struct SendVerdict
    {
        bool   retry = false;      // leave the row queued and try again after delayMs
        bool   reauth = false;     // ...and refresh the token before that attempt
        bool   permanent = false;  // the provider will never accept this row; drop it
        qint64 delayMs = 0;
    };

    // `retryAfterSec` is the Retry-After header in seconds, or 0 when it was absent or was the HTTP-date
    // form (unparsed, and answering it with 0 falls back to `baseMs`, which is never shorter than a minute).
    // `consecutiveFailures` is 1 for the first failure. A 2xx decides nothing and comes back all-false, so a
    // caller may ask about any response without classifying it first.
    SendVerdict classifySend(const SendPolicy& p, int httpStatus, qint64 retryAfterSec,
                             int consecutiveFailures);

    // ================= THE OAUTH LOOPBACK CALLBACK (issue #326) ==========================================
    //
    // Both trackers redeem their authorization code through the app's loopback listener, and each had its
    // OWN copy of the four lines that pick the query out of a raw HTTP request and the seven that answer it.
    // The bytes were identical and there is nothing per-provider in either, so they are here — where a probe
    // can drive them with no socket, and where the Referer/Cache-Control posture is written once.
    struct LoopbackCallback
    {
        QString code;    // the authorization code, or "" when the callback carried none
        QString error;   // the error CODE the provider named, or ""
        QString state;   // whatever `state` came back, or "". COMPARED by the caller; never trusted here.
    };
    // TOTAL: a truncated read, a request with no query, or bytes that are not an HTTP request at all give an
    // empty result rather than a partly-built one.
    LoopbackCallback parseLoopbackRequest(const QByteArray& httpRequest);

    // The whole reply, headers and body. A PLAIN PAGE rather than a redirect to a landing page: sending the
    // browser anywhere would hand a third party a request whose Referer names this loopback port.
    // Content-Length is explicit so the browser does not sit waiting on a connection close.
    QByteArray loopbackResponse(bool signedIn);

    // Retry-After, in SECONDS, or 0 when the header was absent, unparseable, not positive, or was the
    // HTTP-date form (which this deliberately does not parse: answering an unparsed date with 0 falls back
    // to the policy's base wait, which is never shorter than a minute). Each tracker's transport reads the
    // header off its own reply and hands the number to classifySend — that is the seam a service which
    // spells its rate limit differently would use — but AniList and MyAnimeList both send the delta-seconds
    // form, so today they share this one reader instead of writing it twice.
    qint64 retryAfterSeconds(const QByteArray& headerValue);

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

        // ---- what a push RESPONSE means (issue #326) ---------------------------------------------------
        //
        // ANILIST ANSWERS A REFUSED MUTATION WITH HTTP 200. GraphQL carries its failures in an `errors`
        // array beside a null `data`, so transport success is not acceptance and never was: the only proof
        // the write landed is a SaveMediaListEntry payload in the body. This is increment 1's test, moved
        // here so the shared drain loop can ask it — not a new one.
        bool saveAccepted(int httpStatus, const QByteArray& body);

        // The status the send policy should judge. Normally the HTTP one; but when the transport succeeded
        // and the body was NOT an acceptance, it is the `status` AniList puts inside its own error object
        // (a media the account cannot write answers 404 there). 0 when there is none — and 0 is RETRYABLE,
        // which is exactly what this tracker did with every unrecognised failure before #326, so an error
        // shape we cannot read costs nobody their queue.
        int effectiveStatus(int httpStatus, const QByteArray& body);

        // The retry curve, spelled for AniList. THE BASE IS THE 60 SECONDS INCREMENT 1 ALREADY WAITED, kept
        // to the millisecond: the first retry after a failed push is the one it always was. What #326 adds
        // is the SECOND one — AniList publishes a per-minute rate limit and answers a breach with 429, and a
        // flat 60-second retry against that is the tight loop MAL's backoff exists to prevent.
        constexpr qint64 kBackoffBaseMs = 60000;      // 1 minute
        constexpr qint64 kBackoffMaxMs  = 1800000;    // 30 minutes — the ceiling on the doubling

        // THE POLICY. 400 and 404 are the two AniList really uses for a write it will never accept: 400 for
        // a mutation its schema refuses (a retry sends the identical bytes and gets the identical answer)
        // and 404 for a media or a list entry that is gone. 422 is NOT here, and that is the one genuine
        // difference from MAL's set — 422 is a REST validation status and this GraphQL endpoint does not
        // emit it, so naming it would be a rule about a response that cannot arrive. 403 is absent for the
        // reason MAL's is: a temporarily-refused client and a suspended account share it, and dropping a
        // whole queue for the first of those is the worse mistake.
        SendPolicy sendPolicy();
    }

    // ================= the MyAnimeList wire (issue #156, increment 2) ====================================
    //
    // A SECOND provider behind the SAME seam. Nothing in Tracker.h changed to admit it — see the report —
    // and nothing above this namespace changed either: AniList's bodies, keys and decisions are byte-for-byte
    // what increment 1 shipped, which §16 of probe_tracker asserts directly.
    //
    // MAL IS NOT ANILIST, and the four differences below are the whole of why this namespace exists rather
    // than a parameter on the AniList one:
    //   1. REST, not GraphQL. Three different URLs (search / read / write) instead of one endpoint, and the
    //      URL is therefore a wire format: it is built here, where a probe can read it.
    //   2. The token endpoint and the write both take **application/x-www-form-urlencoded**, not JSON.
    //   3. Its vocabulary is KIND-DEPENDENT. `watching` vs `reading`; `num_episodes` vs `num_chapters`;
    //      and — MAL's own asymmetry, not ours — it REPORTS `num_episodes_watched` but ACCEPTS
    //      `num_watched_episodes`. Getting that pair the wrong way round is a silent no-op write: MAL
    //      answers 200 and stores nothing.
    //   4. Its score is 0..10, while the seam (and AniList) carry 0..100. The conversion is here, in one
    //      place, and both directions are pinned — an unconverted 85 is refused by MAL as out of range, and
    //      a truncating conversion loses half a point in one direction and not the other.
    //
    // FIXTURES, NOT AN ACCOUNT. Every shape here is written from MyAnimeList's published API v2 reference.
    // No MyAnimeList account was created, no API client was registered and nothing in this work contacted
    // MyAnimeList — the probe and the live drive were both answered by a local fixture server.
    namespace mal
    {
        // The API host and the OAuth host are DIFFERENT hosts on MAL (unlike AniList, where both are
        // anilist.co). Overridable at RUN TIME through EB_MAL_ENDPOINT / EB_MAL_AUTH so a fixture stub can
        // stand in for the real service in a live drive — read in MyAnimeListTracker.cpp, not here, so a
        // probe asserting the DEFAULT cannot be satisfied by an environment variable.
        inline QString defaultApiUrl()   { return QStringLiteral("https://api.myanimelist.net/v2"); }
        inline QString defaultAuthBase() { return QStringLiteral("https://myanimelist.net/v1/oauth2"); }

        // ---- PKCE ----------------------------------------------------------------------------------
        // MAL's authorization-code flow REQUIRES PKCE and accepts only the `plain` method, so the challenge
        // IS the verifier and the verifier is the only thing standing between a stolen redirect and a token.
        // It is generated per attempt from the system CSPRNG, held in memory for the length of one sign-in,
        // and never written to disk.
        constexpr int kVerifierMinChars = 43;    // RFC 7636's floor
        constexpr int kVerifierMaxChars = 128;   // ...and its ceiling
        // Length AND alphabet. RFC 7636's unreserved set is [A-Za-z0-9-._~]; a verifier outside it is
        // rejected by MAL as a whole request, which presents to the user as "sign-in failed" with no clue.
        bool isValidCodeVerifier(const QString& v);
        // A fresh verifier. The ONE non-deterministic function in this layer; it is here rather than in the
        // socket because what MAL accepts is a wire format, and a probe can then assert that what we
        // generate is what we would accept.
        QString makeCodeVerifier();

        // The browser URL. `state` is carried and MUST be compared on the way back: without it, anything
        // that can reach the loopback listener can feed us an authorization code of its choosing.
        QString authorizeUrl(const QString& authBase, const QString& clientId, const QString& redirectUri,
                             const QString& codeVerifier, const QString& state);

        // The two grants. FORM-ENCODED, not JSON — MAL's token endpoint rejects a JSON body outright.
        QByteArray tokenExchangeBody(const QString& clientId, const QString& clientSecret,
                                     const QString& redirectUri, const QString& code,
                                     const QString& codeVerifier);
        QByteArray tokenRefreshBody(const QString& clientId, const QString& clientSecret,
                                    const QString& refreshToken);

        // What MAL's token endpoint hands back. Deliberately its OWN struct rather than a reuse of
        // anilist::TokenReply: the two services are free to diverge (MAL rotates the refresh token on every
        // refresh, AniList does not), and sharing the type would make the day one of them adds a field a
        // change to the other one's parser.
        struct TokenReply
        {
            bool    ok = false;
            QString accessToken;
            QString refreshToken;
            qint64  expiresInSec = 0;
        };
        // TOTAL, and gated on a NON-EMPTY access token for the reason anilist::parseTokenReply is: MAL
        // answers a refused grant with {"error":"invalid_request","message":"…"} — a 200-shaped object that
        // a caller would otherwise store over the live tokens, permanently unlinking the account.
        TokenReply parseTokenReply(const QByteArray& json);

        // ---- search --------------------------------------------------------------------------------
        // MAL's `q` must be at least three characters; anything shorter is a 400. It is refused HERE rather
        // than sent, because a two-character query is also not a query anybody could be confident about —
        // see the low-confidence rule below.
        constexpr int kMinQueryChars = 3;
        bool searchable(const QString& title);

        // GET .../anime?q=…&limit=…&fields=… (or …/manga). The field list is part of the wire: MAL returns
        // ONLY the fields asked for, so a missing `num_episodes` here is a missing COMPLETED rule later.
        // Returns "" when `title` is not searchable, so "we did not ask" and "MAL said nothing" are the
        // same empty result to the caller and neither is an error.
        QString searchUrl(const QString& apiBase, const QString& title, Kind kind, int limit);
        // TOTAL. `asked` is the kind the caller searched for, and is what a row carrying no count at all is
        // filed under. A row with no id, or no title, is skipped without costing the rest.
        QVector<Match> parseSearch(const QByteArray& json, Kind asked);
        // MAL paginates with an ABSOLUTE next URL in `paging.next`. It is followed only when it is on the
        // SAME ORIGIN as `apiBase` — an absolute URL in a response body is attacker-controlled input, and
        // following one blindly would send the account's bearer token to whatever host it named. Returns ""
        // for absent, malformed, or off-origin.
        QString nextPageUrl(const QByteArray& json, const QString& apiBase);

        // ---- the account's entry ---------------------------------------------------------------------
        // GET .../anime/{id}?fields=num_episodes,my_list_status. KIND-DEPENDENT in both the path and the
        // fields, which is why the seam passes Kind to fetchEntry at all (AniList ignores it; MAL cannot).
        QString entryUrl(const QString& apiBase, const QString& mediaId, Kind kind);
        // ok=false = "this body was not an entry reply". exists=false = "it was, and the account has no row".
        bool parseEntry(const QByteArray& json, const QString& mediaId, Kind kind, Entry& out);

        // ---- the push --------------------------------------------------------------------------------
        // PATCH .../anime/{id}/my_list_status (or …/manga/…).
        QString saveUrl(const QString& apiBase, const QString& mediaId, Kind kind);
        // The form body, and THE SAME THREE SAFETY RULES anilist::saveBody carries, restated against MAL's
        // spellings because they are decisions about content and not about syntax:
        //   * `score` is present ONLY when u.hasScore. MAL reads 0 as "no score", so sending it would clear
        //     a rating the user set by hand — the same damage, arrived at from the opposite convention.
        //   * `status` is `completed` only when u.completes AND the unit really is the last one by
        //     `totalUnits`. Otherwise `watching`/`reading`.
        //   * the progress field is never below 1, and it is the WRITE spelling (`num_watched_episodes`),
        //     not the one a read answers with.
        QByteArray saveBody(const Update& u, int totalUnits);

        // MAL's list statuses. KIND-DEPENDENT: `watching`/`plan_to_watch` for anime, `reading`/`plan_to_read`
        // for manga; `completed`, `on_hold` and `dropped` are shared. An unknown token reads back as
        // Current, for the reason AniList's does.
        QString statusToken(Status s, Kind kind);
        Status  statusFromToken(const QString& token);

        // ---- the score conversion --------------------------------------------------------------------
        // The seam carries 0..100 (AniList's POINT_100 raw). MAL's is an integer 0..10. Both directions, in
        // one place, ROUNDING rather than truncating: 85 is a 9 (not an 8), and a 9 read back is 90. 0 is
        // "unrated" on both sides and maps to 0 either way, which is why neither direction is ever called
        // for an update that has no score.
        int scoreToMal(int hundred);
        int scoreFromMal(int ten);

        // ---- rate limits ------------------------------------------------------------------------------
        // MAL publishes a rate limit and answers a breach with 429; an outage answers 5xx; an expired token
        // answers 401; and a request it will never accept answers 400/403/404. Those four families need four
        // different responses, and getting them wrong is how an integration gets its client banned.
        //
        // BASE is a full minute, not a second: every failure this can see is answered by waiting, and a
        // tight retry against a rate limit is the one behaviour that turns a throttle into a ban.
        constexpr qint64 kBackoffBaseMs = 60000;      // 1 minute
        constexpr qint64 kBackoffMaxMs  = 1800000;    // 30 minutes — the ceiling on the doubling
        struct Backoff
        {
            bool   retry = false;      // leave the row queued and try again after delayMs
            bool   reauth = false;     // the token is the problem: refresh before the next attempt
            bool   permanent = false;  // MAL will never accept this row; DROP it (see below)
            qint64 delayMs = 0;
        };
        // `retryAfterSec` is the Retry-After header's value, or 0 when it was absent. MAL's own number wins
        // over ours whenever it is LARGER; a header asking for less than our base is not honoured downward,
        // because the base exists to protect the account and not to be the smallest legal wait.
        //
        // `consecutiveFailures` is 1 for the first failure. The delay doubles per failure and is capped.
        //
        // WHY `permanent` DROPS THE ROW. A 400 (a media id the account cannot write) or a 404 (a deleted
        // entry) never becomes acceptable by waiting. Leaving it queued wedges the head of the queue
        // FOREVER, and every later chapter behind it is lost — a worse outcome than losing the one row that
        // could not be delivered. It is recorded in the last-error line rather than dropped silently.
        //
        // MOVED, NOT REWRITTEN (#326). The arithmetic and every verdict now live in tracker::classifySend,
        // which both providers ask; this stays the MAL-shaped forwarder it always was, so every increment-2
        // assertion about it still reads the same function and still reads the same answer.
        Backoff backoffFor(int httpStatus, qint64 retryAfterSec, int consecutiveFailures);

        // THE SAME POLICY, in the shared shape. MAL adds 422 to AniList's 400/404 because its REST
        // endpoints really do answer a rejected field with it; everything else about the two is identical
        // and is therefore not written twice.
        SendPolicy sendPolicy();
    }

    // ================= the Kitsu wire (issue #156, increment 3) ==========================================
    //
    // A THIRD provider behind the SAME seam, and the test of what #326 hoisted. Kitsu supplies its status
    // codes, its wire format and its auth, and consumes the shared queue, the shared credential store, the
    // shared drain loop and the ONE classification of a failure. Nothing above this namespace changed to
    // admit it — §21 of probe_tracker asserts AniList's and MyAnimeList's bytes are untouched.
    //
    // KITSU IS NOT MYANIMELIST EITHER, and these five differences are why this namespace exists rather than
    // a parameter on MAL's:
    //   1. THERE IS NO BROWSER. Kitsu's OAuth 2 offers the **password grant** to ordinary users: the app
    //      posts the account's own email and password once and gets a token pair back. No client is
    //      registered, no client id or secret exists to type, no loopback listener is opened and no
    //      `state` is carried, because nothing round-trips through a third party that could be spoofed.
    //      The shared loopback helpers (tracker::parseLoopbackRequest / loopbackResponse) are therefore
    //      simply UNUSED here — offered, not imposed.
    //   2. JSON:API, not plain REST. Bodies are {"data":{"type":…,"attributes":{…}}} and travel under
    //      application/vnd.api+json; filters are filter[…] query parameters.
    //   3. A LIST ENTRY IS ITS OWN RESOURCE with its own id, and it belongs to a USER. So a write is
    //      either a PATCH of an existing library-entry or a POST creating one, and the create needs the
    //      signed-in user's id. That id is derived from the token (users?filter[self]=true), held in
    //      memory for the session and NEVER written to disk — which is also what makes the write
    //      idempotent: a replayed update finds the entry and PATCHes it rather than creating a second row.
    //   4. Its score is `ratingTwenty`, an integer 2..20, while the seam (and AniList) carry 0..100.
    //   5. Its progress field is `progress` for BOTH kinds — no watched/read asymmetry to get wrong — but
    //      the unit COUNT is `episodeCount` or `chapterCount`, so the kind still reaches the wire.
    //
    // FIXTURES, NOT AN ACCOUNT. Every shape here is written from Kitsu's published JSON:API reference. No
    // Kitsu account was created, no API client was registered and nothing in this work contacted Kitsu —
    // the probe and the live drive were both answered by a local fixture server.
    namespace kitsu
    {
        // The API host and the OAuth host, named once. Overridable at RUN TIME through EB_KITSU_ENDPOINT /
        // EB_KITSU_AUTH so a fixture stub can stand in for the real service in a live drive — read in
        // KitsuTracker.cpp, not here, so a probe asserting the DEFAULT cannot be satisfied by an
        // environment variable. Kitsu's pre-rebrand host was kitsu.io and still redirects here; the
        // override is the escape hatch if that ever stops being true.
        inline QString defaultApiUrl()   { return QStringLiteral("https://kitsu.app/api/edge"); }
        inline QString defaultAuthBase() { return QStringLiteral("https://kitsu.app/api/oauth"); }

        // ---- auth: the password grant -----------------------------------------------------------------
        // THE WHOLE SIGN-IN, in one POST. There is no authorize URL to open and no code to redeem, so the
        // pair below is the entirety of what a caller has to build.
        //
        // FORM-ENCODED. Kitsu's token endpoint takes application/x-www-form-urlencoded, like MAL's.
        //
        // THE PASSWORD IS AN ARGUMENT AND NEVER A STORED VALUE. It exists for the length of this call and
        // of the request it builds; KitsuTracker holds it in memory for one sign-in and clears it, and
        // §20's byte-scan asserts it reaches the ini ZERO times — a stronger claim than the "exactly once"
        // the other two secrets get, and the right one, because this credential is never stored at all.
        QByteArray passwordGrantBody(const QString& username, const QString& password);
        // The refresh. Kitsu issues a refresh token with every grant and rotates it, like MAL.
        QByteArray tokenRefreshBody(const QString& refreshToken);

        // What Kitsu's token endpoint hands back. Its OWN struct, for the reason mal::TokenReply is its
        // own: the three services are free to diverge, and sharing the type would make the day one of them
        // adds a field a change to the other two's parsers.
        struct TokenReply
        {
            bool    ok = false;
            QString accessToken;
            QString refreshToken;
            qint64  expiresInSec = 0;
        };
        // TOTAL, and gated on a NON-EMPTY access token for the reason the other two are: Kitsu answers a
        // refused grant with {"error":"invalid_grant","error_description":"…"} — a JSON object a caller
        // would otherwise store over the live tokens, permanently unlinking the account.
        TokenReply parseTokenReply(const QByteArray& json);

        // ---- who the token belongs to -----------------------------------------------------------------
        // GET .../users?filter[self]=true. The signed-in user's id is needed to READ a library entry (it is
        // a filter) and to CREATE one (it is a relationship). It is NOT a credential and NOT stored: it is
        // derived from the token, so caching it on disk is the one way it could ever go stale against a
        // re-linked account.
        QString selfUrl(const QString& apiBase);
        // The id out of that reply, or "" for anything that is not one.
        QString parseSelfId(const QByteArray& json);

        // ---- search -----------------------------------------------------------------------------------
        // Kitsu's filter[text] is a full-text search and answers a two-character query with noise, so the
        // same floor MAL has applies here — and for the same reason, it is refused rather than sent.
        constexpr int kMinQueryChars = 3;
        bool searchable(const QString& title);

        // GET .../anime?filter[text]=…&page[limit]=… (or …/manga). `year` narrows on the media's start year
        // when non-zero and is OMITTED when 0 — omitted, not sent as 0, because filter[year]=0 matches
        // nothing rather than everything. Returns "" when `title` is not searchable, so "we did not ask"
        // and "Kitsu said nothing" are the same empty result to the caller and neither is an error.
        QString searchUrl(const QString& apiBase, const QString& title, int year, Kind kind, int limit);
        // TOTAL. `asked` is the kind the caller searched for and is what a row carrying no count at all is
        // filed under, exactly as MAL's is. A row with no id, or no title, is skipped without costing the
        // rest.
        QVector<Match> parseSearch(const QByteArray& json, Kind asked);
        // Kitsu paginates with an ABSOLUTE next URL in `links.next`. Followed only when it is on the SAME
        // ORIGIN as `apiBase` — an absolute URL in a response body is attacker-controlled input, and
        // following one blindly would send the account's bearer token to whatever host it named. Returns ""
        // for absent, malformed, or off-origin.
        QString nextPageUrl(const QByteArray& json, const QString& apiBase);

        // ---- the account's entry ----------------------------------------------------------------------
        // GET .../library-entries?filter[user_id]=…&filter[kind]=…&filter[media_id]=…&include=…
        // KIND-DEPENDENT in the filter AND in the include, which is why the seam passes Kind to fetchEntry:
        // the include is what brings the unit COUNT back with the entry, and a missing count is a missing
        // COMPLETED rule later. Empty when either id is empty — a request with a blank filter would return
        // somebody else's whole library.
        QString entryUrl(const QString& apiBase, const QString& userId, const QString& mediaId, Kind kind);
        // ok=false = "this body was not a library-entries reply". exists=false = "it was, and the account
        // has no row for this media" — Kitsu says that with an EMPTY `data` array, which is a success.
        // `entryIdOut` receives the library entry's OWN id, which is what a PATCH is addressed to; it is
        // "" when there is no row, and that is precisely what selects the create path.
        bool parseEntry(const QByteArray& json, const QString& mediaId, Kind kind, Entry& out,
                        QString* entryIdOut);

        // ---- the push ---------------------------------------------------------------------------------
        // PATCH .../library-entries/{entryId} when the account already has a row, POST .../library-entries
        // when it does not. Both spelled off the SAME emptiness test, so the URL and the method can never
        // disagree about which of the two this is.
        QString saveUrl(const QString& apiBase, const QString& entryId);
        QByteArray saveMethod(const QString& entryId);   // "PATCH" or "POST"
        // The JSON:API document, and THE SAME THREE SAFETY RULES the other two carry, restated against
        // Kitsu's spellings because they are decisions about content and not about syntax:
        //   * `ratingTwenty` is present ONLY when u.hasScore. Kitsu reads an explicit rating as a rating;
        //     sending one the user never gave overwrites the one they did.
        //   * `status` is "completed" only when u.completes AND the unit really is the last one by
        //     `totalUnits`. Otherwise "current".
        //   * `progress` is never below 1.
        // A create additionally carries the user and media RELATIONSHIPS; a PATCH carries the entry's id
        // and no relationships, because re-stating them on an update is how an entry gets re-pointed at
        // another user's library.
        QByteArray saveBody(const Update& u, int totalUnits, const QString& entryId, const QString& userId);

        // Kitsu's list statuses. NOT kind-dependent (unlike MAL's): "current" and "planned" for both.
        // An unknown token reads back as Current, for the reason the other two do.
        QString statusToken(Status s);
        Status  statusFromToken(const QString& token);

        // ---- the score conversion ---------------------------------------------------------------------
        // The seam carries 0..100 (AniList's POINT_100 raw). Kitsu's is `ratingTwenty`, an integer 2..20 —
        // note the FLOOR: 2, not 0. Kitsu has no "zero" rating; the absence of a rating is the absence of
        // the field, which is why neither direction is ever called for an update that has no score, and
        // why scoreToKitsu clamps UP to 2 rather than sending a 0 the API refuses.
        // ROUNDING rather than truncating, for mal::scoreToMal's reason: 85 is a 17, and a 17 read back
        // is 85.
        int scoreToKitsu(int hundred);
        int scoreFromKitsu(int twenty);

        // ---- rate limits ------------------------------------------------------------------------------
        // The base and the ceiling are the shared ones; only the STATUS SET is Kitsu's, and it is MAL's
        // rather than AniList's because Kitsu is JSON:API over REST and really does answer a rejected
        // attribute with 422. That is the whole of what this provider adds to #326's classification.
        constexpr qint64 kBackoffBaseMs = 60000;      // 1 minute
        constexpr qint64 kBackoffMaxMs  = 1800000;    // 30 minutes
        SendPolicy sendPolicy();
    }

    // ================= how sure we are of a match (issue #156's conservatism rule) =======================
    //
    // "A match we are not sure of is not written." A wrong link writes somebody's progress onto the wrong
    // series in a list they curate by hand, which is worse than no sync at all — so nothing here ever links
    // anything; it only decides what is worth OFFERING and whether an offer is unambiguous.
    //
    // Provider-agnostic on purpose (it works off tracker::Match), but applied on the MAL path only. AniList
    // is asked for SEARCH_MATCH-sorted results and already answers in relevance order; MAL's `q=` is a fuzzy
    // full-text search that will happily return five loosely-related shows for a title it does not have, and
    // a controller user scrolling that list is one press away from linking the wrong one.

    // How well `m` answers `query`, 0..100, over BOTH of the match's titles. Normalised (case, punctuation,
    // whitespace) so "My Hero Academia!" and "my hero academia" are one string. 100 = an exact normalised
    // hit on either title; 0 = not one word in common, which is the threshold for "this row is noise".
    int titleConfidence(const QString& query, const Match& m);

    // `ms` sorted best-first, with the pure-noise rows removed. STABLE, so equally-confident rows keep the
    // provider's own order. A list where EVERY row scores 0 is returned UNCHANGED rather than emptied: a
    // title in a different script legitimately shares no word with its English name, and answering "nothing
    // found" there would make those series permanently unlinkable.
    QVector<Match> rankMatches(const QString& query, const QVector<Match>& ms);

    // The index of the ONE match we would be willing to link without asking, or -1. Requires an exact
    // normalised hit AND no other candidate that is even close, so an ambiguous field always falls through
    // to the user. Nothing in this increment auto-links: this exists so that the "are we sure?" question has
    // one answer, in one place, that a probe can hold — and so a later increment cannot invent a looser one
    // somewhere else.
    int confidentMatchIndex(const QString& query, const QVector<Match>& ms);

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
