// THE JELLYFIN PROTOCOL, AS PURE FUNCTIONS (issue #160, increment 1) — item ids, the auth handshake, the
// payload readers and the merge across servers. No network, no settings, no clock, no UI: everything here
// takes its inputs as parameters, exactly as Subsonic.h does, so probe_jellyfin can drive every arm of it
// with no server, no socket and no account.
//
// #160 asks for SEVERAL servers ("my box, plus the one a friend shares with me"), and #83 scoped a single
// one. This file is written for several from the first line, because the one thing in it that cannot be
// retrofitted is the id.
//
// ==================================================================================================
// 1. SERVER-QUALIFIED IDS, BEFORE ANY UI
// ==================================================================================================
// A Jellyfin item id is a GUID the SERVER minted. It is unique inside that server's database and means
// nothing outside it. Two servers hand out ids from the same space, and — this is the part that surprises
// people — a library restored from a backup, or two servers seeded from the same media, can hold literally
// the same GUID for two different items. An app that stores a bare item id therefore has a CORRUPTION bug
// rather than a display bug: the resume position banked against your friend's copy of a film is read back
// against yours, the "watched" tick lands on somebody else's episode, and every downstream thing that keys
// on it (a favourite, a playlist entry, a recents row, a play count) is filed against the wrong thing for
// ever. No later change can separate them, because nothing in the stored row says which server it came from.
//
// So no id leaves this file unqualified. qualify() is the ONE minter and parse() the ONE reader:
//
//     "jf" ":" <server id> ":" <the server's own item id>
//
// THE SERVER ID IS THE SERVER'S OWN `Id`, FROM /System/Info/Public — NOT ITS URL. This is a decision and it
// is the one the issue turns on. A URL is where a server is answering from THIS DEVICE, on THIS network,
// TODAY: the same box is `http://10.0.0.4:8096` in the living room, `https://jf.example.com` from a phone,
// and something else again after the user puts it behind a reverse proxy. Keying on the URL would give one
// server two identities (so the phone cannot read what the TV banked), and would silently RE-KEY every row
// the day a certificate arrived. The server's own `Id` is stable across all of that and is identical from
// every device, which is exactly the property an identity needs.
//
// Three properties, each load-bearing and each pinned by the probe:
//
//   * A ROUND TRIP IS EXACT, including an item id that itself contains a colon — the item half is
//     "everything after the second colon", never a section() split.
//   * AN ID FROM SERVER A NEVER RESOLVES AGAINST SERVER B. The server id is IN the key, so the lookup that
//     would have collided cannot even be spelled: two servers' rows for the same raw GUID differ in their
//     second field.
//   * A QUALIFIED ID IS NEVER MISTAKEN FOR THE LEGACY SINGLE-SERVER SHAPE, or the reverse. That is
//     structural rather than lucky, and section 2 is why it has to be.
//
// ==================================================================================================
// 2. THE LEGACY SHAPE, AND WHY IT IS PARSED AT ALL
// ==================================================================================================
// The single-server design (#83) spells an item reference `jf:<itemId>` — two fields. This build has never
// shipped that shape, so on today's installs there is nothing to migrate; the reader and the migration
// beside it exist so that the shape can never be written by accident and can always be repaired if it was.
// JellyfinMigrate.h states that plainly rather than dressing it up as an upgrade path.
//
// The two shapes are told apart by FIELD COUNT and by the SECOND FIELD'S FORM, never by one alone:
//   * a qualified id has at least three fields AND its second field is a well-formed server id (32 hex
//     digits, dashes ignored — the shape Jellyfin's own `Id` takes);
//   * a legacy id has exactly two fields;
//   * anything else is not a Jellyfin reference at all, and every caller must LEAVE IT ALONE. That last arm
//     is not politeness. It is what stops the migration from touching a local file path, an addon item id or
//     a Subsonic key that happens to sit in the same store.
//
// ==================================================================================================
// 3. AUTH: THE TOKEN IS A CREDENTIAL, AND A REQUEST IS NOT A DIAGNOSTIC
// ==================================================================================================
// Jellyfin authenticates with an access token, obtained once (Quick Connect, or username/password against
// /Users/AuthenticateByName) and then sent on every request. It goes in a HEADER — `Authorization:
// MediaBrowser ... Token="..."` — which is better than Subsonic's query string, but only if nothing ever
// renders the header. So, the same rule ListenBrainzClient states and Subsonic.h restates from the other
// direction: NOTHING BUILDS A USER-VISIBLE MESSAGE, OR A LOG LINE, OUT OF A REQUEST. Transport failures are
// rendered from Qt's NetworkError enum into fixed sentences of our own; server failures are rendered from
// the server's own words. authHeader() below is the one place a token is spelled into a string, and that
// string is handed straight to QNetworkRequest and never returned to a caller that could log it.
//
// A stream URL is the other half of the same hazard: Jellyfin will accept `?api_key=<token>` on a stream
// URL, which is the only way to hand a file to mpv, and that URL is therefore a CREDENTIAL. It is minted at
// the moment the player is handed it and is never stored — the same rule SubsonicClient.h arrives at, for
// the same reason: a stored url ends up in a queue, a playlist and a recents row, and no later change takes
// it back out of the files already written.
//
// ==================================================================================================
// 4. THE UNION ACROSS SERVERS, AND WHAT A SLOW SERVER MAY COST
// ==================================================================================================
// The merged library is the UNION of the enabled servers, each row carrying the display name of the server
// it came from. Two rules, both decided in the issue and both pinned here:
//
//   * A SERVER THAT DOES NOT ANSWER CONTRIBUTES NOTHING AND BLOCKS NOTHING. The friend's box being switched
//     off must not hold up your own shelf. unionOf() takes the per-server OUTCOMES, not futures, so the
//     policy — a timed-out or failed or disabled server is simply absent — is a pure function the probe
//     drives over a table, and the socket half above it only has to deliver outcomes.
//   * NO CROSS-SERVER DEDUPE. The same film on two servers is two rows, distinguished by server name. The
//     issue argues this at length and it is deliberately shallow: matching by provider ids gets subtly
//     wrong (different cuts, different quality, one with subtitles) and a wrong merge HIDES content the
//     user asked to see.
//
// Ordering is stable and total: servers in the order given, items in the order that server gave them. A
// shelf whose order changed between two refreshes for no reason the user can see is its own bug.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

namespace Jellyfin
{
    // ---- Ids -------------------------------------------------------------------------------------------

    // The scheme's prefix and separator, spelled once. Nothing else in the tree writes these.
    inline QString idPrefix() { return QStringLiteral("jf"); }
    inline QChar   idSep()    { return QLatin1Char(':'); }

    // Is this the shape Jellyfin's own `Id` takes? 32 hex digits, dashes tolerated and ignored (a server
    // reports the compact form; a .NET GUID round-trip can produce the dashed one, and they are the same
    // identity). Used to tell a qualified id from a legacy one — see the header's section 2.
    bool isServerId(const QString& s);

    // The ONE minter. An empty serverId or itemId, or a serverId that is not a server id, yields an EMPTY
    // string: an unqualifiable reference must be absent rather than half-formed, so no caller can
    // accidentally mint "jf::4f2" and file a row under it.
    QString qualify(const QString& serverId, const QString& itemId);

    // The ONE reader. `ok` is false for anything that is not a qualified id — which includes every legacy
    // id, every file path, every addon item id and every Subsonic key — so callers route on it rather than
    // on a prefix test.
    struct Ref
    {
        QString serverId;
        QString itemId;
        bool    ok = false;
    };
    Ref parse(const QString& qualified);

    inline bool isQualified(const QString& s) { return parse(s).ok; }

    // The server a qualified id belongs to, or an empty string. The routing question playback asks.
    inline QString serverOf(const QString& s) { const Ref r = parse(s); return r.ok ? r.serverId : QString(); }

    // The item id inside a LEGACY single-server reference ("jf:<itemId>"), or an empty string when this is
    // not one — which includes every qualified id. The migration's only input. See the header's section 2
    // for why "not one" is by far the most important arm.
    QString legacyItemId(const QString& s);
    inline bool isLegacy(const QString& s) { return !legacyItemId(s).isEmpty(); }

    // ---- Transport safety ------------------------------------------------------------------------------
    // Subsonic's verdicts, unchanged, and for the same reason they are an enum there: "that is not a URL"
    // and "that URL is plain HTTP and you have not allowed plain HTTP for this server" are different
    // problems with different fixes, and collapsing them is how a downgrade becomes silent. A Jellyfin
    // sign-in posts the password, so the plain-HTTP question is asked before the password is sent, never
    // after.
    enum class UrlVerdict { Ok, Malformed, NotHttp, InsecureRefused };
    UrlVerdict checkUrl(const QString& url, bool allowPlainHttp);

    // The root with any trailing slashes removed, so every caller concatenates without thinking. Empty for
    // a url checkUrl refuses — there is no fallback, because there is no other server this could mean.
    QString normalizeRoot(const QString& url, bool allowPlainHttp);

    // ---- Auth ------------------------------------------------------------------------------------------

    // The `Authorization: MediaBrowser ...` header value. `token` may be empty — that is the pre-sign-in
    // form, which /System/Info/Public and /Users/AuthenticateByName both accept, and it is why this is one
    // function rather than two.
    //
    // THE RETURN VALUE CARRIES A CREDENTIAL WHEN `token` IS SET. Hand it to QNetworkRequest and nothing
    // else: it must not reach a status line, a log, or an exception's text. See the header's section 3.
    QString authHeader(const QString& client, const QString& device, const QString& deviceId,
                       const QString& version, const QString& token);

    // The POST body for /Users/AuthenticateByName. Compact JSON, exactly the two fields the endpoint reads.
    // It carries the password, so it is built at request time and never held.
    QByteArray authenticateBody(const QString& username, const QString& password);

    // /System/Info/Public — the ONE call that establishes a server's IDENTITY, and the reason it is the
    // first thing the add-a-server flow does: the id it returns is what every row from this server will be
    // qualified with for ever. `ok` is false unless a well-formed server id came back.
    struct PublicInfo
    {
        QString serverId;      // `Id` — the identity. See the header's section 1 for why not the URL.
        QString serverName;    // `ServerName` — the DISPLAY name, and only that
        QString version;
        bool    ok = false;
    };
    PublicInfo readPublicInfo(const QByteArray& body);

    // /Users/AuthenticateByName's answer. `token` is a credential: it goes to the device-local store and
    // nowhere else.
    struct AuthResult
    {
        QString token;
        QString userId;
        QString userName;
        bool    ok = false;
    };
    AuthResult readAuthResult(const QByteArray& body);

    // ---- Items -----------------------------------------------------------------------------------------

    // One row as the server describes it. `id` is the server's OWN id, unqualified — qualification happens
    // at the union, where the server it came from is known, so a reader cannot mint a half-formed id.
    struct RemoteItem
    {
        QString id;
        QString name;
        QString type;         // "Movie", "Series", "Episode", "Audio", …
        QString seriesName;   // episodes only; empty otherwise
        int     year = 0;
        qint64  runTimeTicks = 0;
        bool    played = false;
        // #83. An episode's number and the season it is in, so a season's rows can be ordered and titled
        // the way the user's server orders and titles them. 0 means "the server did not say", which is a
        // real answer for a special or an extra and is NOT the same as episode zero - the catalog builder
        // therefore prefixes a number only when there is one.
        int     indexNumber = 0;
        int     parentIndexNumber = 0;
        // Where the server says this user got to. It is the number "Continue Watching" is built from and
        // the one an open defers to; see resumeSeconds() in section 5.
        qint64  positionTicks = 0;
        // The container this row hangs under, as the SERVER names it: a season's series, an episode's
        // season. Carried so a Continue Watching episode can be re-opened into its own show without a
        // second round trip.
        QString seriesId;
        QString seasonId;
    };

    // Reads the `Items` array of /Items. `ok` is false for a body that is not a Jellyfin item envelope at
    // all — a proxy's HTML error page, a truncated body, an empty reply — which is NOT the same as a server
    // with no items, and the union treats the two differently.
    QVector<RemoteItem> readItems(const QByteArray& body, bool* ok);

    // The one spelling of each path this increment uses, so the builder and any reader cannot drift.
    QString publicInfoPath();
    QString authenticatePath();
    QString itemsPath(const QString& userId);

    // A playable URL for one item on one server. CARRIES THE TOKEN — see the header's section 3. Minted at
    // the moment the player is handed it; never stored, never logged, never written into a queue.
    QString streamUrl(const QString& root, const QString& itemId, const QString& token);

    // ---- The union across servers ----------------------------------------------------------------------

    // How one server's fan-out leg ended. The socket half fills this in; everything below is pure.
    enum class Outcome
    {
        Ok,         // answered, and the body parsed
        TimedOut,   // did not answer inside the budget — contributes nothing, blocks nothing
        Failed,     // answered with something unusable, or the transport failed
        Disabled    // the user switched this server off: its rows are HIDDEN, not deleted
    };

    struct ServerReply
    {
        QString             serverId;
        QString             serverName;   // the display name, which is what a merged row is tagged with
        Outcome             outcome = Outcome::Ok;
        QVector<RemoteItem> items;
    };

    // One row of the merged library. `id` is QUALIFIED, so nothing downstream can lose track of which
    // server it belongs to, and `serverName` is what the row shows where it names its source — the way a
    // local-vs-addon item is already tagged.
    struct UnionItem
    {
        QString id;           // jf:<serverId>:<itemId>
        QString title;
        QString type;
        QString seriesName;
        int     year = 0;
        QString serverId;
        QString serverName;
        bool    played = false;
        // #83: carried through the merge unchanged, so a merged row is a COMPLETE row. They were added to
        // RemoteItem for the episode list and Continue Watching, and a union that dropped them would make
        // the merged shelf strictly worse than the per-server one it replaces.
        int     indexNumber = 0;
        int     parentIndexNumber = 0;
        qint64  runTimeTicks = 0;
        qint64  positionTicks = 0;
        // QUALIFIED, like `id` itself - the container this row hangs under, ready to be drilled or
        // re-opened. Empty when the server named no parent, and empty (never bare) when it named one this
        // reply could not qualify: an unqualified id is the corruption section 1 exists to prevent, and
        // that rule does not weaken for a field that is only used for navigation.
        QString seriesRef;
        QString seasonRef;
    };

    // THE MERGE. Servers in the order given, items in the order that server gave them — stable and total.
    //
    // A reply that is not Ok contributes NOTHING and is not an error: that is the whole failure-isolation
    // rule, expressed as the absence of a special case. An item whose id cannot be qualified (an empty id
    // from a malformed row, a reply carrying no server id) is DROPPED rather than emitted unqualified —
    // there is no safe half-measure, and an unqualified row is the corruption this file exists to prevent.
    QVector<UnionItem> unionOf(const QVector<ServerReply>& replies);

    // The ONE line a fan-out leg that did not contribute is allowed to log. It names the server by its
    // DISPLAY NAME and says what happened, and it contains no url, no token and no header — see the
    // header's section 3. Empty for a reply that contributed, so the caller logs nothing on the happy path.
    QString unavailableNote(const ServerReply& reply);

    // ====================================================================================================
    // 5. BROWSE, PLAY AND PROGRESS (issue #83) - still pure, still no socket
    // ====================================================================================================
    // #160 built the identity and the union; #83 is the surface over them. Everything below is the same
    // shape as everything above: a path builder, a body builder, or a reader that turns one of the server's
    // JSON answers into a struct. The socket half stays in JellyfinClient, the browse rows stay in
    // browse/JellyfinCatalogs, and probe_jellyfin drives every line here with no server, no account and no
    // clock.
    //
    // THE THREE RULES THIS SECTION IS WRITTEN AGAINST, each stated again at the function that carries it:
    //
    //   * THE SERVER DECIDES HOW ITS OWN FILE IS PLAYED. /Items/<id>/PlaybackInfo answers with a media
    //     source that either can be handed to a player as it stands or carries a transcode url the server
    //     has already built. We honour that answer rather than second-guessing it from a codec list of our
    //     own, which is the whole reason connecting to a media server is worth doing at all.
    //   * THE SERVER IS THE AUTHORITY FOR WHERE THE USER GOT TO. A resume position read back from
    //     `UserData.PlaybackPositionTicks` beats any local mark, INCLUDING when it is zero - a film
    //     finished on a phone reports zero, and a local mark that overrode it would restart the user two
    //     minutes from the end for ever.
    //   * A PLAYABLE URL IS A CREDENTIAL AND IS NEVER WRITTEN DOWN. recordedPath() below is the one
    //     decision that keeps it out of every store, and it is a function rather than a line at the write
    //     site because there is more than one write site.

    // ---- Ticks -----------------------------------------------------------------------------------------
    // Jellyfin measures time in 100-nanosecond ticks (the .NET TimeSpan unit) everywhere: run times, resume
    // positions, segment boundaries, progress reports. Spelled ONCE, because a factor-of-ten error here is
    // a bug that looks like a working feature - a resume ten seconds out reads as "close enough".
    constexpr qint64 kTicksPerSecond = 10000000LL;
    inline double secondsFromTicks(qint64 ticks) { return double(ticks) / double(kTicksPerSecond); }
    inline qint64  ticksFromSeconds(double s)    { return s <= 0.0 ? 0 : qint64(s * double(kTicksPerSecond)); }

    // ---- The user's libraries --------------------------------------------------------------------------
    // /Users/<uid>/Views is the list the Jellyfin web client's own sidebar is built from: "Movies", "Shows",
    // "Music", and whatever else this user is allowed to see. It is the natural top level of the browse
    // surface because it is the shape the user already recognises from their own server.
    QString viewsPath(const QString& userId);

    struct Library
    {
        QString id;
        QString name;
        QString collectionType;   // "movies" | "tvshows" | "music" | "boxsets" | "homevideos" | ...
    };

    // `ok` is false for a body that is not a Views envelope at all, exactly as readItems draws that line: a
    // proxy's error page and a user with no libraries are different answers and the caller shows different
    // things for them.
    QVector<Library> readViews(const QByteArray& body, bool* ok);

    // ONE LIBRARY IN THE MERGED SHELF: qualified, and tagged with the server it came from. The same shape
    // UnionItem has for an item, and for the same two reasons - a bare library id means nothing once two
    // servers exist, and a row that cannot say where it came from cannot be told apart from its twin.
    struct LibraryRef
    {
        QString ref;              // jf:<serverId>:<libraryId>
        QString name;
        QString collectionType;
        QString serverId;
        QString serverName;
    };

    struct LibraryReply
    {
        QString          serverId;
        QString          serverName;
        Outcome          outcome = Outcome::Ok;
        QVector<Library> libraries;
    };

    // THE MERGE, for views. Deliberately the same function twice rather than one templated one: the two
    // read different fields, and the properties that matter (failure isolation as the absence of a special
    // case; an unqualifiable row DROPPED rather than emitted bare) are asserted separately for each.
    QVector<LibraryRef> unionOfLibraries(const QVector<LibraryReply>& replies);

    // Which of THIS app's home categories a Jellyfin library belongs in - the token core::mediaCategory
    // reasons about, so a library lands where the rest of the app already puts that kind of thing. Empty
    // for a collection type this increment does not surface, and the caller SKIPS those rather than
    // guessing: a "boxsets" or "playlists" view listed under Video would open onto rows nothing can play.
    //
    // MUSIC MAPS TO "audio" AND IS DELIBERATELY NOT BROWSED HERE. #194 owns the music surface and consumes
    // the same client; this function names the category so that the two cannot disagree about what a music
    // library IS, while isVideoCollection() below is what the video browse actually walks.
    QString categoryForCollection(const QString& collectionType);
    bool    isVideoCollection(const QString& collectionType);

    // ---- Items, seasons and episodes -------------------------------------------------------------------
    // The query strings, spelled once each so the builder and any reader cannot drift, and so that a probe
    // can assert what was asked for. The FIELDS are requested explicitly: Jellyfin omits ProductionYear and
    // RunTimeTicks from a list unless they are asked for, and a run time that is silently absent looks
    // exactly like a run time of zero. (`UserData` is not an ItemFields value - it rides every user-scoped
    // request by default, which is why these are all addressed under /Users/<uid>/.)
    QString libraryItemsQuery(const QString& libraryId);
    QString seasonsPath(const QString& seriesId);         // /Shows/<seriesId>/Seasons
    QString episodesPath(const QString& seriesId);        // /Shows/<seriesId>/Episodes
    QString seasonsQuery(const QString& userId);
    QString episodesQuery(const QString& userId, const QString& seasonId);
    // "Continue Watching" as the server keeps it: the items this user is part-way through.
    QString resumeItemsPath(const QString& userId);
    QString resumeItemsQuery();

    // ---- What the server says about ONE item -----------------------------------------------------------
    // /Users/<uid>/Items/<itemId> - the single-item read that seeds a resume point and the watched tick.
    QString itemPath(const QString& userId, const QString& itemId);

    struct UserState
    {
        qint64 positionTicks = 0;
        bool   played        = false;
        bool   ok            = false;   // false: the server did not answer with a UserData block at all
    };
    UserState readUserState(const QByteArray& body);

    // WHERE AN OPEN STARTS. The server wins whenever it answered - including with zero, which is the case
    // this rule exists for: finishing a film elsewhere resets the position, and a local mark that beat it
    // would restart every re-watch two minutes from the end. When the server did NOT answer (it is down,
    // the item is gone, the request timed out) the local mark is all there is and it is used, because the
    // alternative is starting a half-watched film from the beginning to punish a network hiccup.
    double resumeSeconds(const UserState& server, double localSeconds);

    // ---- PlaybackInfo: the server decides ---------------------------------------------------------------
    QString    playbackInfoPath(const QString& itemId);              // /Items/<itemId>/PlaybackInfo
    QByteArray playbackInfoBody(const QString& userId, qint64 startTicks,
                                int audioStreamIndex, int subtitleStreamIndex);

    struct PlaybackChoice
    {
        enum class Mode
        {
            DirectPlay,   // the file as it stands - hand the static stream url to the player
            Transcode,    // the server has built us an HLS url; use IT, not one of our own
            Unavailable   // the server offered neither: there is nothing honest to open
        };
        Mode    mode = Mode::Unavailable;
        QString mediaSourceId;
        QString playSessionId;    // the id every progress report for this playback must carry
        QString transcodingUrl;   // SERVER-RELATIVE, exactly as the server gave it. Empty unless Transcode.
        QString container;        // display / diagnostic only
        bool    ok = false;
    };

    // THE FIRST MEDIA SOURCE DECIDES. Jellyfin returns them in its own preference order and the first is the
    // one its own clients play; picking a different one would be us overriding the server about its own
    // file, which is the opposite of the reason this integration exists. Within that source: direct play or
    // direct stream means "hand it over", and only when the server says it supports NEITHER do we fall to
    // the transcode url it built. A source that supports neither and carries no transcode url is
    // Unavailable - an honest refusal, not an empty player.
    PlaybackChoice readPlaybackInfo(const QByteArray& body);

    // THE ONE URL THE PLAYER IS HANDED, AND IT CARRIES THE TOKEN. Minted at the moment the player is handed
    // it; never stored, never logged, never written into a queue, a playlist or a recents row. Empty when
    // the choice is Unavailable, so a caller cannot open a player on nothing.
    //
    // The transcode url ALREADY carries `api_key` when the server built it with one, and appending a second
    // is not harmless - Jellyfin reads the first and the url stops matching the one its own session list
    // shows. So the token is appended only when the server's url does not already have one; both arms are
    // pinned by the probe.
    QString playbackUrl(const QString& root, const QString& itemId, const QString& token,
                        const PlaybackChoice& choice);

    // ---- Progress: the sessions API ---------------------------------------------------------------------
    enum class ProgressEvent { Start, Progress, Pause, Unpause, Stop };

    // /Sessions/Playing, /Sessions/Playing/Progress, /Sessions/Playing/Stopped. Start and Stop have their
    // own endpoints; a pause and an unpause are ordinary progress reports carrying `IsPaused`, which is
    // exactly how Jellyfin's own clients spell them.
    QString    progressPath(ProgressEvent ev);
    QByteArray progressBody(const QString& itemId, const QString& playSessionId,
                            const QString& mediaSourceId, double positionSeconds, ProgressEvent ev);

    // HOW OFTEN A PROGRESS REPORT GOES OUT. PlaybackSession's own throttle fires about every five seconds
    // of movement, which is more often than a server wants to hear from one client; this is the second
    // gate, on top of it. Ten seconds is what Jellyfin's own web client uses.
    //
    // A BACKWARD SEEK REPORTS IMMEDIATELY. The rule is on the ABSOLUTE difference, not on elapsed time:
    // someone who scrubs back thirty seconds and leaves has moved, and a rule that only noticed forward
    // movement would leave the server holding a position ahead of where they actually stopped.
    constexpr double kProgressIntervalS = 10.0;
    bool shouldReportProgress(double lastReportedS, double nowS);

    // ---- Media segments (Jellyfin 10.10+) ---------------------------------------------------------------
    // /MediaSegments/<itemId>. The server has already done the detection - an Intro Skipper-style scan
    // across the whole series - and this is one more provider tier beside .edl, chapters and what the user
    // taught us. No new UI: the existing skip chip and auto-skip act on it exactly as they act on a chapter.
    QString mediaSegmentsPath(const QString& itemId);
    QString mediaSegmentsQuery();

    struct RemoteSegment
    {
        double  start = 0.0;    // SECONDS, converted here so nothing downstream has to know about ticks
        double  end   = 0.0;
        QString type;           // the server's own token: "Intro" | "Outro" | "Recap" | "Commercial" | ...
    };

    // Rows with no usable range (an end at or before the start) are dropped rather than emitted: a
    // zero-length segment would arm a skip that jumps nowhere, which reads as the chip being broken.
    QVector<RemoteSegment> readMediaSegments(const QByteArray& body);

    // ---- What a Jellyfin row may be WRITTEN DOWN --------------------------------------------------------
    // THE #203 RULE, ONE SOURCE ALONG. A Jellyfin stream url carries the token in its query, and "recent/"
    // is a SYNCED store - so what a row records is its QUALIFIED ID, which is stable, credential-free, and
    // re-openable: openRecent mints a fresh link from it, which is also what makes a row survive the token
    // being rotated or the server being re-addressed.
    //
    // It is a FUNCTION and not a line at the write site because there is more than one write site, and
    // because "the recents row for a jf: item is its id" is a claim a probe can state. Anything that is not
    // a qualified id is returned byte for byte, so no other route changes.
    QString recordedPath(const QString& qualifiedId, const QString& playUrl);
}
