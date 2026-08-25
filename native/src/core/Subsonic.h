// THE SUBSONIC PROTOCOL, AS PURE FUNCTIONS (issue #193, increment 5) — ids, auth, the response envelope and
// the payload readers. No network, no settings, no clock, no UI: everything here takes its inputs as
// parameters, for the same reason Scrobble.h does, so probe_subsonic can drive every arm of it with no
// server, no socket and no account.
//
// Navidrome, Airsonic, Gonic, Ampache and Astiga all speak this API. What follows is the subset this
// increment needs — browse (artists -> albums -> tracks) and playback (stream + getCoverArt) — and the three
// places a naive client of it goes wrong.
//
// ==================================================================================================
// 1. SERVER-QUALIFIED IDS, FROM THE FIRST LINE OF CODE
// ==================================================================================================
// A Subsonic id is an opaque string chosen by the SERVER. Navidrome mints hex, Airsonic mints "al-123",
// Gonic mints small integers — so two servers hand out the same id constantly, and "1" means a different
// album on each of them. An app that stores a bare id has a CORRUPTION bug, not a display bug: the row you
// pressed on server B resolves against server A's cache and plays somebody else's record, and every
// downstream thing that keys on it (a favourite, a queue entry, a cached cover, a resume position) is filed
// against the wrong thing for ever. That is issue #160's lesson, and the reason this increment supports
// several servers from the start is precisely that it cannot be retrofitted: a store written against bare
// ids has no way to find out, later, which server each of its rows came from.
//
// So no id ever leaves this file unqualified. qualify() is the ONE minter and parse() the ONE reader:
//
//     "sub" <US> <server uuid> <US> <kind> <US> <the server's own id>          (US = 0x1F)
//
// Three properties, each load-bearing and each pinned by the probe:
//
//   * A ROUND TRIP IS EXACT, including a remote id that itself contains the separator or a colon — the
//     remote half is "everything after the third separator", never a section() split, which is the same
//     rule browse::musicKeyOf already applies to album keys for the same reason.
//   * AN ID FROM SERVER A NEVER RESOLVES AGAINST SERVER B. The server uuid is IN the key, so the lookup
//     that would have collided cannot even be spelled: two servers' album keys differ in their second
//     field whatever their remote halves are.
//   * A QUALIFIED ID CAN NEVER BE MISTAKEN FOR A LOCAL LIBRARY KEY, or the reverse. This is structural
//     rather than lucky. MusicLibrary's keys are: an artist key (a folded artist name, containing NO 0x1F
//     at all), an album key (artist <US> "t"|"d" <US> folded text) and a work key (composer <US> "w"|"a"
//     <US> ...). A qualified id needs four fields whose FIRST is exactly "sub" and whose SECOND parses as a
//     non-null uuid — and "t", "d", "w" and "a" are not uuids. So no local key can parse as a qualified id
//     however a user names their band, and MusicSupply can route on parse() alone.
//
// ==================================================================================================
// 2. AUTH: u / t / s, AND WHY NOTHING MAY LOG A REQUEST
// ==================================================================================================
// Subsonic authenticates every single request. The modern scheme sends the username `u`, a random salt `s`,
// and `t` = MD5(password + salt) — so the password is not on the wire, but the token is derived from it and
// a token+salt pair is enough to attack it offline. The legacy scheme sends the password itself as `p`
// (optionally hex-encoded behind "enc:"), and some old servers accept nothing else.
//
// THE CONSEQUENCE FOR DIAGNOSTICS IS SPECIFIC AND IT IS THE WHOLE REASON THIS NOTE EXISTS. The obvious
// thing to write when a request fails is "GET <url> failed" — and for this protocol that one string
// contains `t` and `s` together, which is the interesting half of the user's password. It then goes into a
// status line, a screenshot in a bug report, and a log file that gets pasted into an issue, and there is no
// later stage that can take it back out. So: SubsonicClient builds every user-visible message from the
// server's own error text and Qt's socket error, never from a url — exactly the rule ListenBrainzClient.cpp
// states for its Authorization header, arrived at from the other direction.
//
// The salt VARIES PER REQUEST (that is what a salt is for), so saltFrom takes its randomness as a
// parameter and the token is a pure function of password+salt. A probe can then pin both halves: that the
// token is exactly MD5(password+salt), and that two requests do not reuse a salt.
//
// ==================================================================================================
// 3. THE ENVELOPE — WHY A 200 IS NOT A SUCCESS, AND WHY THERE IS ONE NODE MODEL
// ==================================================================================================
// Every Subsonic error arrives as HTTP 200 with a failure envelope inside it:
//
//     <subsonic-response status="failed" version="1.16.1">
//       <error code="40" message="Wrong username or password."/>
//     </subsonic-response>
//
// A client that checks the HTTP status reports success, renders an empty shelf, and tells the user their
// library is empty when in fact their password is wrong. That is the single commonest bug in Subsonic
// clients and it is invisible from every layer above. So there is exactly one entry point — parseBody() —
// and NOTHING in this app reads a Subsonic payload without going through it.
//
// And the encoding is not knowable in advance. f=json is a REQUEST for JSON, not a guarantee: servers
// that predate it, servers with it disabled, and every error page produced by a reverse proxy in front of
// one, answer XML (or HTML). EverythingBoxServer's own Subsonic endpoint renders both envelopes from ONE
// node model, and this file is that trick run backwards: both encodings parse into the same Node, and the
// payload readers below are written ONCE, over Nodes. A reader that could see only one encoding would be a
// silent no-op against half the deployments in the wild.
#pragma once
#include "MusicLibrary.h"

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>
#include <QVector>

namespace Subsonic
{
    // ---- Ids -------------------------------------------------------------------------------------------

    // What a qualified id points AT. The kind is in the key so a stale route cannot resolve an album id as
    // a track id and hand mpv something that is not audio.
    enum class Kind { Artist, Album, Track, Cover };

    // The separator. 0x1F (UNIT SEPARATOR) — the same character MusicLibrary joins its own key fields with,
    // which is what makes the two key families comparable at all (see the header's third property).
    inline QChar idSep() { return QChar(0x1F); }

    // The ONE minter. An empty serverId or remoteId yields an empty string: an unqualifiable id must be
    // absent rather than half-formed, so a caller cannot accidentally mint "sub<US><US>album<US>7".
    QString qualify(const QString& serverId, Kind kind, const QString& remoteId);

    // The ONE reader. `ok` is false for anything that is not a qualified id — which includes every
    // MusicLibrary key, every file path and every url — so callers route on it rather than on a prefix test.
    struct Ref
    {
        QString serverId;
        Kind    kind = Kind::Artist;
        QString remoteId;
        bool    ok = false;
    };
    Ref parse(const QString& qualified);

    inline bool isQualified(const QString& s) { return parse(s).ok; }

    // The server a qualified id belongs to, or an empty string. The routing question MusicSupply asks.
    inline QString serverOf(const QString& s) { const Ref r = parse(s); return r.ok ? r.serverId : QString(); }

    // ---- Auth ------------------------------------------------------------------------------------------

    // t = MD5(password + salt), lower-case hex. Exactly the scheme's definition, and nothing else in this
    // app computes it. An empty password yields an empty token rather than MD5("" + salt): a token derived
    // from no password would authenticate as nobody while looking perfectly well-formed.
    QString tokenFor(const QString& password, const QString& salt);

    // A salt, from randomness the CALLER supplies. Pure, so a probe can pin the token; the client passes
    // QRandomGenerator. 16 hex characters — comfortably above the 6 the spec asks for.
    QString saltFrom(quint64 seed);

    // A salt that is the SAME every time for the same subject. Used for exactly one thing — the `stream`
    // url of one track — and the reason is that a stream url is not only a request, it is an IDENTITY.
    //
    // PlaybackSession keys a track's resume position, its consumption-stats row and its queue-to-album map
    // on the string it was handed to play. Mint a fresh random salt per call and that string changes on
    // every play of the same track, so: the position is never found again (a Subsonic track can never
    // resume), a new resume row accumulates in the ini on every play for ever, and the now-playing sleeve
    // cannot be looked up. A per-track salt makes the url a pure function of (server, track) and all three
    // work exactly as they do for a file.
    //
    // It costs nothing cryptographically. A salt is PUBLIC — it travels in the url beside the token it
    // salted — so the only thing varying it defends against is a precomputed table covering many salts, and
    // that is defeated by the salt being 64 unpredictable bits, not by it changing between two requests for
    // the same track. The API calls (getArtists, getArtist, getAlbum, getCoverArt) still salt randomly per
    // request; this is the one deliberate exception, and it is derived from the subject, never the password.
    QString stableSalt(const QString& subject);

    // The query parameters every request carries. `legacy` selects the old plaintext form (p=enc:<hex>)
    // for the servers that accept nothing else; it is a per-server opt-in, never a silent fallback, because
    // a client that retries a rejected token as a plaintext password has just sent the password to a server
    // that may have rejected the token precisely because it is not the server the user thinks it is.
    //
    // `client` is the `c` parameter — servers log it and show it in their own "now playing" surfaces.
    QList<QPair<QString, QString>> authParams(const QString& user, const QString& password,
                                              const QString& salt, bool legacy, const QString& client);

    // ---- Transport safety ------------------------------------------------------------------------------

    // HTTPS unless the user has explicitly said otherwise, per server. The verdict is an ENUM rather than a
    // bool so the surface can say WHICH it was: "that is not a URL" and "that URL is plain HTTP and you have
    // not allowed plain HTTP for this server" are different problems with different fixes, and collapsing
    // them is how a downgrade becomes silent.
    enum class UrlVerdict { Ok, Malformed, NotHttp, InsecureRefused };
    UrlVerdict checkUrl(const QString& url, bool allowPlainHttp);

    // The root, with any trailing slashes removed, so every caller can concatenate without thinking. Empty
    // for a url checkUrl refuses — there is no fallback, because there is no other server this could mean.
    QString normalizeRoot(const QString& url, bool allowPlainHttp);

    // ---- The response ----------------------------------------------------------------------------------

    // ONE tree for BOTH encodings. XML attributes and JSON scalar members both become `attrs`; XML child
    // elements and JSON objects/arrays both become `kids` named by their key. See the header for why the
    // payload readers must not know which encoding they came from.
    struct Node
    {
        QString                 name;
        QMap<QString, QString>  attrs;
        QVector<Node>           kids;

        QString attr(const QString& k) const { return attrs.value(k); }
        int     attrInt(const QString& k, int def = 0) const;
        // The first DESCENDANT with this name, at any depth. Depth-first, so it finds the outermost one
        // first. Null when there is none.
        const Node* find(const QString& n) const;
        // Every descendant with this name, in document order. Used for the repeated payload elements
        // (artist, album, song) — which sit at different depths in different endpoints' answers
        // (artists > index > artist vs artist > album), which is exactly why this is recursive.
        QVector<const Node*> findAll(const QString& n) const;
    };

    Node parseXml(const QByteArray& body, bool* ok);
    Node parseJson(const QByteArray& body, bool* ok);
    // Sniffs. The one entry point: everything that reads a Subsonic reply reads it through here.
    Node parseBody(const QByteArray& body, bool* ok);

    // What the envelope SAID, which is not what HTTP said. See the header: a failure arrives as 200.
    enum class Status
    {
        Ok,          // status="ok"
        Failed,      // status="failed" — the server refused, and code/message say why
        Unparsable   // not a subsonic-response at all: a proxy's HTML error page, a truncated body, garbage
    };

    struct Envelope
    {
        Status  status = Status::Unparsable;
        int     code = 0;              // the server's own error code; 0 when it gave none
        QString message;               // the server's own words — the ONLY thing a user is ever shown
        QString version;
        bool ok() const { return status == Status::Ok; }
    };

    // Read the envelope off a parsed root. Deliberately separate from parseBody so a caller can read the
    // envelope and the payload from ONE parse rather than parsing twice.
    Envelope envelopeOf(const Node& root);

    // The credential-refused codes: 40 wrong username or password, 41 token auth not supported for that
    // user, 42 provided authentication mechanism not supported, 43 multiple conflicting mechanisms, 44
    // invalid API key. All mean "no amount of retrying helps" — the surface must say so rather than
    // retrying a refused credential in a loop, which is how an account gets rate-limited.
    bool isAuthCode(int code);

    // ---- The payloads this increment reads -------------------------------------------------------------
    // Flat structs of exactly what the browse levels need, so nothing above this file touches a Node.

    // `musicBrainzId` is an OpenSubsonic extension rather than a guarantee: Navidrome serves it, an older
    // Subsonic does not, and an absent one is simply empty. It is the ground truth the cross-source merge
    // (#194) prefers over any string comparison — and for an ALBUM it is the RELEASE id, which is not the
    // same thing as a release GROUP id and is never compared against one. MusicId.h has the rule.
    struct RemoteArtist { QString id, name, coverArt, musicBrainzId; int albumCount = 0; };
    struct RemoteAlbum  { QString id, name, artist, artistId, coverArt, musicBrainzId;
                          int songCount = 0, year = 0, durationSec = 0; };
    struct RemoteSong
    {
        QString id, title, artist, album, albumId, coverArt, contentType, suffix;
        int track = 0, disc = 0, year = 0, durationSec = 0;
    };

    QVector<RemoteArtist> readArtists(const Node& root);
    QVector<RemoteAlbum>  readAlbums(const Node& root);
    QVector<RemoteSong>   readSongs(const Node& root);

    // ---- Onto the EXISTING music catalog shapes --------------------------------------------------------
    //
    // These build a MusicLibrary::Index — the very type #74's browse builders already render — so a
    // Subsonic server's views and the local library's views are literally the same code with a different
    // supplier, which is what the issue asks for. Nothing here is a parallel browse tree: there is no
    // second artist list, no second album row, no second track row and no second player.
    //
    // WHAT IS DELIBERATELY LEFT UNSET, AND WHY IT IS NOT AN OVERSIGHT:
    //
    //   * Index::trackCount stays 0. It gates ONE thing — the "Shuffle all music" row — and a shuffle of a
    //     library whose tracks have not been fetched would produce an empty queue. Offering a verb that can
    //     only no-op is worse than not offering it (the rule browse::queueTargetFor already states). A
    //     server-side shuffle is getRandomSongs, and is named in the report as follow-up work.
    //   * Artist::trackCount stays 0, which suppresses that artist's "Play all" / "Shuffle all" rows for
    //     exactly the same reason: at the artists level we know an artist's album count and nothing else.
    //     Artist::albumCount carries what we DO know, so the row still reads "12 albums".
    //
    // Both are honest absences rather than wrong numbers, and both become available the moment a follow-up
    // fetches the tracks behind them.

    // The artists level: one bucket per artist, no albums yet. `serverId` qualifies every key it mints.
    MusicLibrary::Index indexOfArtists(const QString& serverId, const QVector<RemoteArtist>& artists);

    // Fill in ONE artist's albums (getArtist). Tracks are not fetched here — the album level does that —
    // so each Album carries the server's own songCount in Album::trackCount and an empty `tracks`.
    // A no-op when the artist key is not in the index.
    void fillArtistAlbums(MusicLibrary::Index& idx, const QString& serverId, const QString& artistKey,
                          const QVector<RemoteAlbum>& albums);

    // Fill in ONE album's tracks (getAlbum), in disc-then-track order. A no-op when the album key is not in
    // the index. IndexTrack::path is the qualified TRACK id — NOT a stream url; see MusicSupply.h for why a
    // credential must never be what the index stores.
    void fillAlbumTracks(MusicLibrary::Index& idx, const QString& serverId, const QString& albumKey,
                         const QVector<RemoteSong>& songs);

    // Put an album into an index that has never heard of it, creating the artist bucket it hangs off, then
    // fill its tracks. This is the COLD CACHE case and it is not an edge case: the per-server cache is
    // per session, so a Recents row remembered from yesterday names an album whose artist has not been
    // browsed today. Without this the row would be silently dead — activate it and nothing happens at all,
    // which is the failure mode this codebase treats as worse than an error message.
    //
    // The artist bucket is minted from the album's own artistId when the server gave one and from a synthetic
    // key derived from the album otherwise; either way the album's key is unchanged, so the record the user
    // remembered is the record they get. A later getArtists REPLACES the cache wholesale, which is what keeps
    // a bucket invented here from lingering beside the real one.
    void adoptAlbum(MusicLibrary::Index& idx, const QString& serverId, const RemoteAlbum& album,
                    const QVector<RemoteSong>& songs);
}
