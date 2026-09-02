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
}
