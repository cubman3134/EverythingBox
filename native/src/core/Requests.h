// ASKING FOR SOMETHING YOU DO NOT HAVE (issue #109) — the pure half: the id path, the vocabulary a
// request surface speaks, the failure sentences, and the shelf's grouping. No network, no settings, no
// clock, no UI, exactly as Jellyfin.h and Subsonic.h are, so probe_requests drives every arm of it with no
// server and no account.
//
// ==========================================================================================================
// 1. THIS FILE NAMES NO BACKEND, ON PURPOSE
// ==========================================================================================================
// EverythingBoxServer#16 grows an equivalent queue, and the issue is explicit that whichever lands second
// must EXTEND this surface rather than grow a parallel one. The way to make that true is not to guess at
// that server's API — it is to keep everything that is not a per-service act off the seam and in here:
//
//   * WHICH IDS AN ITEM CARRIES and how they are read off a catalogue row — refFor() below. Written once,
//     applied whatever answers the request.
//   * WHAT THE STATES ARE CALLED. `Availability` is OUR vocabulary, not Jellyseerr's integers: a backend
//     translates its own status codes into it and no caller ever sees a number it would have to know a
//     service to interpret.
//   * WHAT A FAILURE SAYS. `failureSentence` is a fixed table of our own sentences. A backend reports a
//     `Failure` and stops; it never hands up a message it built out of a request, because
//     QNetworkReply::errorString() embeds the url and a request to a request service carries the API key
//     in its headers. SubsonicClient.h states the same rule at length for the same reason.
//   * HOW THE SHELF IS GROUPED and in what order — shelfGroups(). A property of the surface, and the
//     surface is shared.
//
// What is left for a backend is two verbs, and RequestBackend.h states them.
//
// ==========================================================================================================
// 2. THE ID PATH, AND WHY AN ITEM WITH NEITHER ID SHOWS NOTHING
// ==========================================================================================================
// A request is keyed on a title, and the only stable names for a title this app already carries end to end
// are TMDB's and IMDB's:
//
//   * `MediaItem::id` of the shape "tmdb:movie:{N}" / "tmdb:tv:{N}" / "tmdb:episode:{N}:{s}:{e}" — what the
//     bundled AIO Catalog mints (native/addons/aiocatalog/main.js);
//   * an IMDB id, either as `MediaItem::id` itself ("tt0111161", or "tt0903747:1:2" for an episode — the
//     Stremio stream-id shape) or, for a TMDB-keyed catalogue that has resolved one, as
//     `MediaItem::imdbStreamId`.
//
// Anything else — a local file, a ROM, a Subsonic key, a qualified Jellyfin id, an addon's opaque id — is
// NOT a reference this app can hand to an acquisition pipeline, and refFor() answers IdKind::None for it.
// That arm is the important one: the surface offers no Request action at all rather than one that would
// submit a request nobody can fulfil. The issue asks for the action on "any movie or series detail view,
// not just Jellyfin-sourced items", and the honest boundary of "any" is "any that carries one of these
// two ids".
//
// AN EPISODE IS A REQUEST FOR ITS SEASON. There is no such thing as requesting one episode from an *arr
// stack — Sonarr's unit is a season (or the series) — so an episode reference collapses to its series plus
// a season number, and the surface says so rather than pretending otherwise.
//
// ==========================================================================================================
// 3. NOTHING HERE SUBMITS ANYTHING
// ==========================================================================================================
// Every function in this file is a reader, a builder or a classifier. A submission causes somebody's server
// to go and acquire content, so it is only ever the result of an explicit press — never automatic, never
// retried silently, and never a side effect of viewing an item. That rule lives at the one call site that
// can press the button (MainWindowRequests.cpp); this file simply gives it nothing it could call by
// accident.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

namespace requests
{
    // ---- The reference ---------------------------------------------------------------------------------

    enum class IdKind
    {
        None,   // this item cannot be requested; the surface shows no action at all
        Imdb,
        Tmdb
    };

    // What kind of thing is being asked for. Deliberately two values: an acquisition pipeline knows films
    // and it knows series, and everything else this app browses (a game, an album, a book) is not something
    // Jellyseerr or its successor has any concept of.
    inline QString kMovie() { return QStringLiteral("movie"); }
    inline QString kTv()    { return QStringLiteral("tv"); }

    struct MediaRef
    {
        IdKind  kind = IdKind::None;
        QString imdb;        // "tt0111161" — the SERIES' id for an episode, never the episode's own
        QString tmdb;        // "1396", bare and numeric — the SERIES' id for an episode
        QString mediaType;   // kMovie() or kTv()
        // 0 means "the whole thing": a film, or a series asked for entire. A positive number is the season
        // an episode reference collapsed to. Season 0 as a SPECIALS season is deliberately not
        // representable — nobody requests specials, and conflating it with "the whole series" would submit
        // the wrong thing.
        int     season = 0;

        bool ok() const { return kind != IdKind::None && !mediaType.isEmpty(); }

        // A stable, backend-independent key for this title: "imdb:tt0111161" / "tmdb:tv:1396". It names the
        // TITLE, never the season — a shelf row and a status cache are per title, and two seasons of one
        // show are one thing that is being acquired.
        QString key() const;
    };

    // THE ONE READER. `itemId` is MediaItem::id, `imdbStreamId` is MediaItem::imdbStreamId (either may be
    // empty), `type` is MediaItem::type. Answers IdKind::None — not a half-formed ref — for anything that
    // is not a film or a series carrying one of the two ids. See the header's section 2.
    MediaRef refFor(const QString& itemId, const QString& imdbStreamId, const QString& type);

    // Is `type` a shape a request could ever be about? Cheap, no id needed — what a surface asks before it
    // bothers looking for ids at all.
    bool isRequestableType(const QString& type);

    // ---- The states ------------------------------------------------------------------------------------
    // OUR vocabulary. A backend maps its own codes onto this; nothing above the seam sees a service's
    // numbers. `Unknown` is a first-class answer and is NEVER a default for "we could not ask": the issue
    // is explicit that a request whose status cannot be fetched shows as unknown with a readable reason,
    // and never as pending, which is a claim we cannot make.
    enum class Availability
    {
        Unknown,             // we could not find out. Says so, with a reason. Never assumed.
        NotRequested,        // nobody has asked for this
        Pending,             // asked for, waiting for somebody to approve it
        Approved,            // approved, not yet being fetched
        Processing,          // being fetched right now
        PartiallyAvailable,  // some of it (some seasons) is on the server
        Available,           // it is in the library — "In your library", never a Request button
        Declined,            // somebody said no
        Failed               // the pipeline tried and could not
    };

    // What the badge says. Sentences of our own; no service is named and no number is shown.
    QString availabilityLabel(Availability a);

    // Would pressing Request on an item in this state do anything useful? False for Available (that is the
    // anti-duplicate rule — the action becomes "In your library" instead) and false for everything already
    // in flight. TRUE for PartiallyAvailable, because the seasons that are missing are exactly what a
    // second request is for.
    bool isRequestable(Availability a);

    // Is this state one the server has already been told about? Drives the shelf's grouping and the "you
    // have already asked for this" wording.
    bool isInFlight(Availability a);

    // The stable token an Availability is stored and published as ("available", "pending", …). Used by the
    // request store and by the themed action row, so neither carries an integer whose meaning could drift.
    QString    statusToken(Availability a);
    Availability availabilityFromToken(const QString& token);

    // ---- Failure ---------------------------------------------------------------------------------------
    // Every arm the surface has to render, and one sentence each. THE SENTENCES ARE OURS. Nothing here is
    // built out of a request, out of Qt's errorString(), or out of a server's own error body: the first
    // embeds the url, and a request to a request service carries the API key in its headers.
    enum class Failure
    {
        None,
        NotConfigured,  // no backend set up at all
        Unreachable,    // the address did not answer / the transport failed
        TimedOut,
        Unauthorized,   // 401 — the key is wrong or has been revoked
        Forbidden,      // 403 — the key is right and this account may not do that
        NotFound,       // the service has never heard of this title
        Duplicate,      // it has been asked for already
        Malformed,      // something answered, but not with anything we can read
        RateLimited,
        ServerError
    };

    QString failureSentence(Failure f);

    // ---- What the action IS ----------------------------------------------------------------------------
    // THE ANTI-DUPLICATE RULE, as a pure function, so it is asserted rather than described. It takes what
    // the backend SAID and not WHICH backend said it — no backend id, no service name, no status code — so
    // this is also where "the surface never learns which implementation answered" is pinned.
    enum class ActionKind
    {
        None,       // no action at all: nothing to key on, or nothing configured to ask
        Request,    // the button that asks. The ONLY kind a press may submit under.
        InLibrary,  // "In your library" — deep-links to playback, and can never create a duplicate
        Waiting,    // already asked for: a badge, not a button
        Unknown     // we could not find out; the press is still offered, with the reason beside it
    };

    // `backendConfigured` is RequestBackend::configured() for whichever backend the chooser returned;
    // `lookupOk` is whether the status fetch answered at all. A ref that cannot be keyed yields None
    // whatever the rest says — the issue's "an item carrying neither id shows no Request action rather than
    // a broken one".
    ActionKind actionFor(const MediaRef& ref, bool backendConfigured, bool lookupOk, Availability a);

    // The pill / button text for an action. Unknown carries its own word rather than borrowing "Request",
    // because the two mean different things to the person pressing.
    QString actionLabel(ActionKind kind, Availability a);

    // Everything a detail surface needs to DRAW the action, and nothing about how it was obtained. It lives
    // here rather than inside a view class because BOTH layouts read it and the window that fetches it has
    // to pass it between them — and because there is nothing view-shaped in it.
    struct UiState
    {
        QString token;       // statusToken() of the last answer; EMPTY means "no action at all on this item"
        QString label;       // actionLabel() — what the button says, which is the state made visible
        QString libraryRef;  // a QUALIFIED id to open, for the InLibrary branch; "" when it cannot be named
        bool    press = false;   // may pressing this do anything: a Request, or an open-in-library
    };

    // ---- The shelf -------------------------------------------------------------------------------------

    // One row of the profile's own request list, as it is stored. It holds NO credential and NO url — a
    // stored row names a title and the backend that was asked, and the address that backend lives at is
    // read from the device-local settings at request time.
    struct StoredRequest
    {
        QString key;         // MediaRef::key() — the identity, and the store's primary key
        QString backendId;   // which backend was asked ("jellyseerr"); a row is never refreshed by another
        QString title;
        QString thumb;
        QString mediaType;   // kMovie() / kTv()
        QString imdb;
        QString tmdb;
        QVector<int> seasons;    // empty = the whole thing
        qint64  requestedAt = 0; // epoch seconds, for ordering
        QString status;          // statusToken() of the LAST thing we were told. "unknown" until refreshed.
    };

    struct ShelfGroup
    {
        QString               header;
        QVector<StoredRequest> items;
    };

    // THE SHELF'S GROUPING, in the order it is drawn. Four groups, and they are four because they are four
    // different things for the user to do:
    //
    //   Ready to watch     it arrived — this is the row worth pressing
    //   On the way         asked for, approved or being fetched: nothing to do but wait
    //   Needs attention    declined or failed: it is not coming unless somebody acts
    //   Status unknown     we could not ask. NOT folded into "On the way", because that would be a claim
    //                      about somebody else's server that we have no basis for.
    //
    // An empty group is omitted entirely. Within a group, newest request first — the order a person scans.
    // Stable and total: rows with the same stamp keep their input order.
    QVector<ShelfGroup> shelfGroups(const QVector<StoredRequest>& rows);

    // ---- The already-available deep link ---------------------------------------------------------------

    // "In your library" has to lead somewhere. A request service that is linked to a Jellyfin server reports
    // that server's OWN item id for a title it already has — but not WHICH server, because it only knows
    // one and we may know several. So:
    //
    //   * exactly one Jellyfin server configured -> the qualified id for it (Jellyfin::qualify), which is
    //     something playback can open;
    //   * none, or more than one -> "" — an honest "I cannot tell you which of your servers this is on",
    //     which the surface renders as a badge without a deep link rather than as a link that would open
    //     the wrong server's copy. #160's whole point is that a bare item id is a corruption bug, and that
    //     rule does not weaken because it would be convenient here.
    QString libraryRefFor(const QString& serverItemId, const QStringList& configuredServerIds);
}
