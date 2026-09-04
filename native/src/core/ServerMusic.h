// THE EVERYTHINGBOX SERVER'S MUSIC SHELF, AS PURE FUNCTIONS (issue #194, increment 3) — the ids, the three
// payloads a music browse needs, and their conversion onto the MusicLibrary::Index #74's own builders
// render. No network, no settings, no clock, no UI, and — deliberately — no AddonManager: everything here
// takes bytes and strings as parameters, exactly as Subsonic.h and JellyfinMusic.h do, so a probe drives
// every arm of it with no server, no addon runtime and no socket.
//
// ==================================================================================================
// WHAT THIS SUPPLIER IS, AND WHAT DECIDES THAT A SHELF IS ONE
// ==================================================================================================
// EverythingBoxServer's music library (EverythingBoxServer#23) reaches this app the way every other shelf
// it serves does: as a connected media-source SERVER, over the addon protocol's own HTTP shape —
//
//     GET <base>/catalog/<catalogId>.json          the artists
//     GET <base>/detail/<type>/<id>.json           that row's children
//
// — which is the shape AddonManager already speaks, spelled here so this file can be driven without it.
//
// A shelf qualifies as a music SUPPLIER when it is served by a REMOTE server the user connected and its
// catalogue's media type is `music`. The remote half of that test is the load-bearing half, and it is a
// structural gate rather than a name check: a bundled metadata add-on (the AIO catalog's MusicBrainz
// shelf) also has a catalogue of type `music`, and merging THAT into somebody's library would fold a
// database of every record ever pressed into the twelve albums they own. A metadata shelf answers "what
// exists"; a server shelf answers "what you have". Only the second is a library, and only a server the
// user deliberately connected serves one.
//
// ==================================================================================================
// IDS: A FOURTH KEY FAMILY, MINTED THE WAY THE OTHER THREE ARE
// ==================================================================================================
//     "ebs" <US> <the source's addon id> <US> <kind> <US> <the server's own item id>       (US = 0x1F)
//
// The second field is the ADDON ID — the identity a connected server already has in this app, stable across
// its url changing — so two connected servers are two suppliers and neither can resolve the other's rows.
// That is Subsonic.h's first section applied unchanged; the reasoning is identical and is not repeated here.
//
// The kind IS in the key, unlike Jellyfin's. A Jellyfin id is a GUID unique across item types inside one
// server, so an album id cannot collide with a track id; nothing says a shelf's own ids are, and the cost of
// being wrong is handing the player something that is not audio.
//
// Structural distinctness from every other key family in this app, which is what lets MusicSupply route on
// parse() alone: four 0x1F-separated fields whose first is exactly "ebs". A MusicLibrary artist key holds no
// 0x1F at all; its album and work keys hold exactly two; a Subsonic id has four with "sub" first; a Jellyfin
// id has no 0x1F.
//
// ==================================================================================================
// WHAT A ROW MAY SAY ABOUT ITSELF, AND WHY IT GOES IN `meta`
// ==================================================================================================
// The addon protocol's item already carries an open-ended `meta` object (AddonModels' MediaArt::fromJson
// copies it verbatim), which is exactly the extensible slot a music row needs — so nothing here extends the
// protocol, and a server that says nothing extra still produces a browsable, mergeable shelf out of
// `id`/`title` alone. The keys this reads:
//
//   an artist   albumCount, musicBrainzArtistId
//   an album    albumArtist, year, trackCount, durationSec, format, bitrateKbps,
//               musicBrainzAlbumId, musicBrainzReleaseGroupId, musicBrainzArtistId
//   a track     artist, track, disc, durationSec, format, bitrateKbps
//
// EVERY ONE OF THEM IS OPTIONAL AND EVERY ABSENCE IS AN ABSENCE. A missing year is 0, which MusicId reads as
// "compatible with everything" rather than as a disagreement; a missing MusicBrainz id is silent, not
// different; a missing format shows no badge rather than a guessed one. A shelf that reports nothing merges
// on normalised artist + album, which is the same deal the local library gets.
//
// ==================================================================================================
// A TRACK'S URL IS NOT ITS IDENTITY
// ==================================================================================================
// The shelf hands each track a playable url, and that url may carry a credential (EverythingBoxServer's file
// server signs the ones it serves). IndexTrack::path therefore holds the QUALIFIED ID, never the url — the
// rule SubsonicClient.h and Jellyfin.h both arrive at, for the reason both state: the index is copied into
// queues, a queue is persisted, and no later change takes a credential back out of the files already
// written. readSongs returns the url beside the id so the CLIENT can hold it for this session only.
#pragma once
#include "MusicLibrary.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace ServerMusic
{
    // ---- Ids -------------------------------------------------------------------------------------------

    enum class Kind { Artist, Album, Track };

    // The separator. 0x1F, the same character MusicLibrary joins its own key fields with and Subsonic joins
    // its ids with — which is what makes the families comparable at all.
    inline QChar idSep() { return QChar(0x1F); }

    // The ONE minter. An empty sourceId or itemId yields an empty string: an unqualifiable reference must be
    // absent rather than half-formed, so no caller can mint "ebs<US><US>album<US>7" and file a row under it.
    QString qualify(const QString& sourceId, Kind kind, const QString& remoteId);

    // The ONE reader. `ok` is false for anything that is not one of these — every MusicLibrary key, every
    // file path, every Subsonic id, every Jellyfin id — so callers route on it rather than on a prefix test.
    struct Ref
    {
        QString sourceId;
        Kind    kind = Kind::Artist;
        QString remoteId;
        bool    ok = false;
    };
    Ref parse(const QString& qualified);

    inline bool isQualified(const QString& s) { return parse(s).ok; }

    // The source a qualified id belongs to, or an empty string. The routing question MusicSupply asks.
    inline QString sourceOf(const QString& s) { const Ref r = parse(s); return r.ok ? r.sourceId : QString(); }

    // ---- The requests ----------------------------------------------------------------------------------
    // Path AND query, relative to the source's base url, spelled once so the builder and the reader cannot
    // drift. The addon protocol's own shape; see the header.
    QString catalogPath(const QString& catalogId);
    QString detailPath(const QString& type, const QString& remoteId);

    // The `type` segment each level's children are asked for. Constants rather than literals at three call
    // sites, because a typo in one of them is a level that silently returns nothing.
    inline const char* kArtistType = "artist";
    inline const char* kAlbumType  = "album";
    inline const char* kTrackType  = "track";

    // ---- What the shelf says ---------------------------------------------------------------------------

    struct RemoteArtist
    {
        QString id, name, musicBrainzArtistId;
        int     albumCount = 0;
    };

    struct RemoteAlbum
    {
        QString id, name, artist;
        QString musicBrainzAlbumId, musicBrainzReleaseGroupId, musicBrainzArtistId;
        QString format;
        // THE SLEEVE'S URL, HELD BY THE CLIENT, NEVER BY THE INDEX — the same rule as a track's url below,
        // for the same reason: a MediaItem's thumbnail url is copied into caches and item records, and a
        // shelf's url can be signed. The client fetches the bytes into MetaCache under the album's key.
        QString coverUrl;
        int     trackCount = 0, year = 0, durationSec = 0, bitrateKbps = 0;
    };

    struct RemoteSong
    {
        QString id, title, artist;
        // THE PLAYABLE URL, HELD BY THE CLIENT FOR THIS SESSION ONLY — never by the index. See the header.
        QString url;
        QString format;
        int     track = 0, disc = 0, durationSec = 0, bitrateKbps = 0;
    };

    // ---- The readers -----------------------------------------------------------------------------------
    //
    // `ok` is false for a body that is not the protocol's catalogue envelope at all — a proxy's HTML error
    // page, a truncated body, an empty reply. That is NOT the same as a shelf with no music: the first
    // contributes nothing and leaves the level alone, the second is a real and legal empty library.
    //
    // Rows whose `type` is not the level's own are SKIPPED rather than coerced. A shelf is allowed to mix a
    // heading row or a verb row into a level — the local library's own browse levels do — and reading one as
    // an album would put a row in the merge that names nothing.
    QVector<RemoteArtist> readArtists(const QByteArray& body, bool* ok);
    QVector<RemoteAlbum>  readAlbums(const QByteArray& body, bool* ok);
    QVector<RemoteSong>   readSongs(const QByteArray& body, bool* ok);

    // ---- Onto the EXISTING music catalog shapes --------------------------------------------------------
    // The same three builders every other supplier has, with the same deliberate absences (Index::trackCount
    // and Artist::trackCount stay 0 — they gate verbs that cannot work over unfetched tracks).
    MusicLibrary::Index indexOfArtists(const QString& sourceId, const QVector<RemoteArtist>& artists);
    void fillArtistAlbums(MusicLibrary::Index& idx, const QString& sourceId, const QString& artistKey,
                          const QVector<RemoteAlbum>& albums);
    void fillAlbumTracks(MusicLibrary::Index& idx, const QString& sourceId, const QString& albumKey,
                         const QVector<RemoteSong>& songs);
}
