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
#include <QSet>
#include <QPair>
#include <QString>
#include <QVector>

namespace Subsonic
{
    // ---- Ids -------------------------------------------------------------------------------------------

    // What a qualified id points AT. The kind is in the key so a stale route cannot resolve an album id as
    // a track id and hand mpv something that is not audio.
    //
    // `Playlist` (issue #193, increment 6) is a FIFTH kind rather than a re-used Album, and the reason is
    // the routing: a playlist is fetched with getPlaylist and an album with getAlbum, so an id that could
    // not say which it was would have to be guessed at by the one function that must never guess. It renders
    // as an album — MusicLibrary::Album is "a titled, ordered list of tracks with a cover", which is what a
    // playlist is — so no second row type, no second level and no second player exists for it.
    // `Virtual` is the one kind the SERVER has never heard of: a container this app invented (today, exactly
    // one - the record the starred loose tracks are queued behind). No endpoint takes it, SubsonicClient
    // refuses to put one in a request, and that refusal is what makes it impossible for an invented id to
    // collide with a server-minted one whatever ids that server uses. See starredTracksKey().
    enum class Kind { Artist, Album, Track, Cover, Playlist, Virtual };

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

    // The one spelling of the stream endpoint's path, so the url BUILDER (SubsonicClient::streamUrl) and the
    // url READER below cannot drift apart. Nothing else in the tree writes this string.
    QString streamPath();

    // ---- The reverse reader (issue #203) ---------------------------------------------------------------
    //
    // THE ONE PLACE A SIGNED STREAM URL IS TURNED BACK INTO THE TRACK IT NAMES. qualify() mints an id,
    // parse() reads one, and streamUrl() turns an id into a url that carries a credential — this closes the
    // loop, because an earlier build wrote that url into a playlist as the track's IDENTITY and those rows
    // have to be re-identified rather than thrown away.
    //
    // Pure, and the server list is a PARAMETER rather than a store lookup, for the same reason MusicRemap
    // takes its groups: the mapping is decided away from the records it will rewrite, so a probe can drive
    // every arm of it over a table of strings with no ini and no network.
    //
    // `serverRoots` is (serverId, normalizeRoot(server.url, server.allowPlainHttp)) for every configured
    // server. The match is an EXACT string compare against the root the builder itself concatenated, never a
    // host-only or a prefix match: two servers on one host under different paths are two servers, and
    // guessing between them would file a row under an id that resolves to the wrong library.
    //
    // Returns the qualified TRACK id, or an empty string when the url is not a stream url of any of these
    // servers — which the caller must treat as "leave the row alone", never as "drop it".
    QString trackIdFromStreamUrl(const QString& url, const QVector<QPair<QString, QString>>& serverRoots);

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

    // ONE PLAYLIST, as the three levels of #193 increment 6 need it. `songCount` is the server's own count
    // and is the only honest number between the playlists level and the moment the playlist is opened —
    // exactly the split MusicLibrary::Album::trackCount exists for.
    struct RemotePlaylist { QString id, name, comment, owner, coverArt; int songCount = 0, durationSec = 0; };

    QVector<RemoteArtist> readArtists(const Node& root);
    QVector<RemoteAlbum>  readAlbums(const Node& root);
    QVector<RemoteSong>   readSongs(const Node& root);

    // getPlaylists / getPlaylist. A record with no id is dropped for the same reason an artist without one
    // is: a row that cannot be pressed is worse than an absent row.
    QVector<RemotePlaylist> readPlaylists(const Node& root);

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

    // ====================================================================================================
    // WHAT THE SERVER ALREADY KNOWS (issue #193, increment 6) - playlists, starred, recently added
    // ====================================================================================================
    //
    // Three more levels, built exactly the way the three above were: one request each, a pure reader over the
    // envelope, and the result poured into the SAME MusicLibrary shapes #74's browse builders render. A
    // Subsonic playlist and a local album are the same UI with a different supplier, which is the whole
    // claim this feature rests on - so nothing below mints a row type, a level or a player of its own.
    //
    // WHY A PLAYLIST IS AN ALBUM. MusicLibrary::Album is "a titled, ordered list of tracks with a cover and a
    // count". That is a playlist. Giving playlists a parallel struct would have duplicated the album level,
    // the track row, the queue-behind-a-track rule and the cover cache, and the four copies would have
    // drifted the first time one of them was corrected. What a playlist does NOT share with an album is how
    // it is FETCHED (getPlaylist, not getAlbum), and that is why its qualified id carries Kind::Playlist:
    // the routing question is answered by the id itself rather than guessed at.

    // The rows a list of playlists becomes. Album::trackCount is the server's songCount and `tracks` is
    // empty - the same "counted before it is fetched" split the album level already has.
    QVector<MusicLibrary::Album> playlistRows(const QString& serverId,
                                              const QVector<RemotePlaylist>& playlists);

    // The rows a flat album list becomes (getAlbumList2?type=newest, and getStarred2's albums). Real album
    // keys, so opening one takes the ordinary album route and the ordinary getAlbum fetch.
    QVector<MusicLibrary::Album> albumRows(const QString& serverId, const QVector<RemoteAlbum>& albums);

    // The rows a flat artist list becomes (getStarred2's artists). Real artist keys, same reasoning.
    QVector<MusicLibrary::Artist> artistRows(const QString& serverId, const QVector<RemoteArtist>& artists);

    // ---- The one container this app invents ------------------------------------------------------------
    //
    // A level of LOOSE TRACKS - the songs a user starred, which belong to as many different records as they
    // like - still has to answer "what does pressing one play?". Every track row in this app carries the key
    // of the record it is queued behind (MusicCatalogs.h), and for these the honest answer is "the starred
    // list itself": press the fourth starred track and the starred tracks play from there.
    //
    // So there is one synthetic record, and its id carries Kind::Virtual for a structural reason rather than
    // a tidy one: a Virtual id is NEVER put in a request. The server has never heard of it, no endpoint takes
    // it, and SubsonicClient refuses to fetch one - which is what makes it impossible for this invented id to
    // collide with anything the server minted, whatever ids that server happens to use.
    QString starredTracksKey(const QString& serverId);

    // What one getStarred2 answer is, as the three row lists the level draws and nothing else.
    struct Starred
    {
        QVector<MusicLibrary::Artist>     artists;
        QVector<MusicLibrary::Album>      albums;
        QVector<MusicLibrary::IndexTrack> tracks;   // all carrying starredTracksKey(serverId)
        bool isEmpty() const { return artists.isEmpty() && albums.isEmpty() && tracks.isEmpty(); }
    };
    Starred readStarred(const QString& serverId, const Node& root);

    // ---- THE SECTIONS INDEX, WHICH IS A SEPARATE INDEX ON PURPOSE --------------------------------------
    //
    // The two functions below fill a DIFFERENT MusicLibrary::Index from the browse one: the containers this
    // app invented (a playlist rendered as an album, the starred loose tracks' record) live apart from the
    // server's own artists -> albums -> tracks. That separation is not tidiness; it is two bugs avoided.
    //
    //   * THE ARTISTS LEVEL WOULD GROW ROWS NOBODY MADE. Every invented record has to hang off an artist
    //     bucket, because that is the only place an Index holds an album - and musicArtistsCatalog draws one
    //     row per bucket. Put them in the browse index and the server's artist list gains an "Unknown Artist"
    //     holding the user's playlists.
    //   * A getArtists REPLACES THE BROWSE INDEX WHOLESALE (adoptAlbum's note says why that matters). Any
    //     invented record in it is destroyed by an ordinary refresh, and every track row already on screen
    //     that was queued behind one then plays nothing at all.
    //
    // MusicSupply::indexFor routes to whichever of the two a key belongs to, structurally, by its Kind - so
    // nothing above this file has to know there are two.
    void adoptStarred(MusicLibrary::Index& sections, const QString& serverId, const Starred& s);

    // ...and a playlist's tracks, once getPlaylist has answered. Same cold-cache story as adoptAlbum: the
    // playlist may be opened from a Recents row in a session where the playlists level was never visited.
    // Idempotent - re-running it replaces the record's tracks rather than doubling them.
    void adoptPlaylist(MusicLibrary::Index& sections, const QString& serverId,
                       const RemotePlaylist& playlist, const QVector<RemoteSong>& songs);

    // ...and the ARTIST cold-cache case, which starred made ordinary. fillArtistAlbums is a no-op for an
    // artist the index has never heard of, and until now that could only happen on a stale route; a starred
    // artist row opened before the server's artist list was ever fetched reaches it legitimately. Creates
    // the bucket from the server's own answer and then fills it, so the row leads somewhere.
    void adoptArtist(MusicLibrary::Index& idx, const QString& serverId, const RemoteArtist& artist,
                     const QVector<RemoteAlbum>& albums);

    // ---- Starred, and the local favourites (issue #193, increment 6) -----------------------------------
    //
    // THE UNION RULE, ENFORCED BY THIS FUNCTION'S SHAPE RATHER THAN BY A COMMENT. Reading a server's starred
    // list may ADD favourites; it may never remove one. A user's local favourites include things this server
    // has never held - a film, a game, a track on another server - and "make the local list match the remote
    // one" would delete every one of them the first time a Starred level was opened. That is not a bug you
    // find in testing; it is a bug you find when somebody's shelf is empty.
    //
    // So there is no removal to get wrong: this returns ADDITIONS and there is no other return value. Pure,
    // with the existing favourites passed IN as a set of ids, so the probe drives the rule with no store.
    struct StarredFavorite { QString itemId, title, subtitle; };
    QVector<StarredFavorite> starredAdditions(const QString& serverId, const QVector<RemoteSong>& songs,
                                              const QSet<QString>& alreadyFavourite);

    // ---- Telling the server what happened --------------------------------------------------------------

    // `scrobble.view`'s parameters for one batch. Repeated `id`/`time` pairs, which the spec allows and every
    // server implements - one request for a queue's worth rather than one per listen. `submission=false` is
    // the ephemeral "now playing" hint and `true` is the durable play.
    //
    // The times are UNIX SECONDS here and MILLISECONDS on the wire, which is the spec's unit and the single
    // easiest thing to get wrong in a Subsonic client: seconds read as milliseconds land the play in January
    // 1970, where nothing displays it and nothing complains. A non-positive time is omitted rather than sent
    // as zero, because "no time" means "now" to every server and 1970 means nothing to any of them.
    QList<QPair<QString, QString>> scrobbleParams(const QVector<QString>& remoteIds,
                                                  const QVector<qint64>& atUnixSeconds, bool submission);

    // `star.view` / `unstar.view`'s parameter for one thing. WHICH parameter depends on the kind - `id` for a
    // song, `albumId` for an ID3 album, `artistId` for an ID3 artist - and sending the wrong one stars
    // whatever record happens to share that id in the other namespace. Empty for a kind that cannot be
    // starred (a cover, a virtual container), so a caller can hand it anything.
    QList<QPair<QString, QString>> starParams(Kind kind, const QString& remoteId);

    // How one answer ends, in the four fates the scrobble orchestrator acts on. Pure over the envelope, so
    // every arm is drivable from a recorded body with no socket:
    //   Ok         the server took it
    //   Auth       a credential code (see isAuthCode) - keep the listens, stop pumping
    //   Rejected   the server understood and refused permanently (a missing or unknown id): DROP, or the
    //              queue jams for ever behind one row and everything after it is lost too
    //   Retryable  anything else, including an unparsable body - a proxy's error page is a transport problem
    enum class Fate { Ok, Retryable, Auth, Rejected };
    Fate fateOf(const Envelope& env);
}
