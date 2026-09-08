// THE JELLYSEERR PROTOCOL, AS PURE FUNCTIONS (issue #109) — the paths, the bodies, the readers and the
// status mapping. No network, no settings, no clock, no UI, exactly as Jellyfin.h is, so probe_requests
// drives every arm of it against recorded fixture payloads with no server and no account.
//
// Everything backend-NEUTRAL lives in Requests.h — the id path, the state vocabulary, the failure
// sentences, the shelf. This file is the one place that knows Jellyseerr's own spelling, and it is
// deliberately the only file in the tree that does.
//
// ==========================================================================================================
// THE API KEY IS A CREDENTIAL, AND A REQUEST IS NOT A DIAGNOSTIC
// ==========================================================================================================
// Jellyseerr authenticates with an `X-Api-Key` header. That is better than a query parameter — it cannot
// end up in a proxy's access log or a recents row — but only if nothing ever renders the request. So the
// rule Jellyfin.h states, restated once here because this file mints the header: NOTHING BUILDS A
// USER-VISIBLE MESSAGE, OR A LOG LINE, OUT OF A REQUEST. QNetworkReply::errorString() embeds the url and
// there is no call to it in JellyseerrClient.cpp; transport failures are classified from the NetworkError
// enum and HTTP failures from the STATUS CODE, and both are rendered by requests::failureSentence.
//
// A key that authenticates every call is a standing grant over somebody's whole *arr stack — it can add
// requests and, for an admin key, approve them. It therefore rides the device-local settings carve-out
// (JellyseerrStore) and is read at request-build time, never held anywhere a message could reach.
//
// ==========================================================================================================
// WHAT WE ASK FOR, AND WHAT WE DELIBERATELY DO NOT
// ==========================================================================================================
//   GET  /api/v1/movie/{tmdbId}   — a film's details, including `mediaInfo` (what the linked server has)
//   GET  /api/v1/tv/{tmdbId}      — a series' details, including per-season status
//   GET  /api/v1/search?query=    — the id bridge: Jellyseerr resolves an IMDB id here, so an item that
//                                   carries only "tt…" still reaches the two calls above
//   POST /api/v1/request          — the ONE call that costs somebody bandwidth
//
// Not asked for, and not in this increment: /api/v1/request/{id}/approve and /decline (that is an admin
// surface and it is Jellyseerr's own web UI's job), /api/v1/user (user management, likewise), and the
// quality-profile fields on a request body — v1 leaves the profile to the server's default, which is what
// the issue asks for and is also the only choice that cannot silently fetch somebody a 60 GB remux.
#pragma once
#include "Requests.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace jellyseerr
{
    // ---- Transport safety ------------------------------------------------------------------------------
    // Subsonic's and Jellyfin's verdicts, unchanged and for the same reason they are an enum there: "that
    // is not a URL" and "that URL is plain HTTP and you have not allowed plain HTTP" are different problems
    // with different fixes, and collapsing them is how a downgrade becomes silent. The API key rides a
    // header on EVERY call, so the plain-HTTP question is asked BEFORE the key is stored, never after.
    enum class UrlVerdict { Ok, Malformed, NotHttp, InsecureRefused };
    UrlVerdict checkUrl(const QString& url, bool allowPlainHttp);

    // The root with any trailing slashes removed, so every caller concatenates without thinking. Empty for
    // a url checkUrl refuses — there is no fallback, because there is no other service this could mean.
    QString normalizeRoot(const QString& url, bool allowPlainHttp);

    // ---- Paths ----------------------------------------------------------------------------------------
    // Spelled once each, so a builder and a reader cannot drift.
    QString mediaPath(const QString& mediaType, const QString& tmdbId);  // /api/v1/{movie|tv}/{id}
    QString searchPath();                                                // /api/v1/search
    QString requestPath();                                               // /api/v1/request
    QString statusPath();                                                // /api/v1/status — the reachability probe

    // The header NAME the key rides on. The VALUE is never spelled in this file: JellyseerrClient reads it
    // from the device-local store at request-build time and puts it straight into a QNetworkRequest.
    inline const char* apiKeyHeader() { return "X-Api-Key"; }

    // ---- Readers ---------------------------------------------------------------------------------------

    // /api/v1/{movie|tv}/{id}. `ok` is false for a body that is not one of these envelopes at all — a
    // proxy's HTML error page, a truncated body, an empty reply — which is NOT the same as a title nobody
    // has asked for, and the two must not be confused: the first is Failure::Malformed and the second is
    // Availability::NotRequested.
    struct MediaStatus
    {
        bool                   ok = false;
        requests::Availability availability = requests::Availability::Unknown;
        QString                tmdbId;
        // The linked Jellyfin server's OWN item id for this title, when the service has one. Unqualified —
        // requests::libraryRefFor is the one place it becomes an id this app may store or open.
        QString                serverItemId;
        QVector<int>           availableSeasons;   // series only
        int                    seasonCount = 0;    // series only; specials (season 0) are not counted
    };
    MediaStatus readMediaStatus(const QByteArray& body, const QString& mediaType);

    // /api/v1/search?query=tt0111161 — the IMDB bridge. Returns the FIRST result whose media type matches
    // `wantType`, because Jellyseerr's external-id search answers with the one title that id names and the
    // type gate is what stops a series result being requested as a film. `ok` false = no usable match,
    // which the surface renders as Failure::NotFound rather than as a request for nothing.
    struct SearchHit
    {
        bool    ok = false;
        QString tmdbId;
        QString mediaType;   // requests::kMovie() / requests::kTv()
    };
    SearchHit readSearchHit(const QByteArray& body, const QString& wantType);

    // POST /api/v1/request's answer. A 2xx with a body we cannot read is NOT a success: the request may or
    // may not exist on the service, and reporting it as made would leave a row on the shelf that refreshes
    // to "unknown" for ever.
    struct Ack
    {
        bool                   ok = false;
        requests::Availability availability = requests::Availability::Pending;
    };
    Ack readRequestAck(const QByteArray& body);

    // ---- Builders --------------------------------------------------------------------------------------

    // The POST body. `mediaId` is a NUMBER in this API, so the tmdb id is emitted as one and a non-numeric
    // id yields an EMPTY body — which the client treats as "do not send", rather than posting something the
    // service would half-understand. `seasons` empty = the whole thing (a film, or a series asked for
    // entire, which this API spells as the string "all").
    //
    // NO QUALITY PROFILE, NO ROOT FOLDER, NO SERVER ID. All three are optional in the API and all three are
    // deliberately absent: leaving them out is what "the server's default" means, and it is the difference
    // between a request and a request that quietly overrides somebody's own configuration.
    QByteArray requestBody(const QString& mediaType, const QString& tmdbId, const QVector<int>& seasons);

    // ---- Failure classification -------------------------------------------------------------------------

    // An HTTP status (and, only where the status alone is ambiguous, the SHAPE of the body) turned into one
    // of our failure arms. THE BODY'S TEXT IS NEVER RENDERED — it is inspected for one known marker and
    // then dropped. Jellyseerr answers a duplicate request with 409, and some builds answer 500 with a
    // "Request for this media already exists" message; both must read as Duplicate, or pressing Request
    // twice reports a server fault for something that worked.
    requests::Failure failureForHttp(int status, const QByteArray& body);
}
