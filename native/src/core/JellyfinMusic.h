// A JELLYFIN SERVER'S MUSIC, AS PURE FUNCTIONS (issue #194, increment 3) — the three payloads a music
// browse needs, and their conversion onto the MusicLibrary::Index that #74's own builders already render.
// No network, no settings, no clock, no UI: everything here takes its inputs as parameters, exactly as
// Subsonic.h does, so probe_musicsources drives every arm of it with no server and no account.
//
// ==================================================================================================
// WHY THIS IS A SEPARATE FILE FROM Jellyfin.h
// ==================================================================================================
// Jellyfin.h (#160) is the VIDEO library's protocol: item ids, the auth handshake, /Items, and the union
// across servers that renders one shelf out of N boxes. Its `RemoteItem` is deliberately the four fields a
// film row needs, and its union is deliberately a DEDUPE-FREE concatenation — "the same film on two servers
// is two rows" is the rule that file argues at length.
//
// Music is the opposite question, and #194 is the issue that says so: the same album on two servers is ONE
// record with two copies, because a person who owns it twice owns it once. So this file does not extend
// #160's union and does not touch it. It reads the music payloads into the same MusicLibrary::Index shape
// the local library and every Subsonic server already produce, and hands that to MusicMerge — which is
// where cross-source identity is decided, once, for every supplier.
//
// THE IDS ARE #160's, UNCHANGED. Every key minted here is Jellyfin::qualify(serverId, itemId) — the very
// same `jf:<serverId>:<itemId>` a film row carries — so a music row and a film row from the same box are
// filed under the same server identity, JellyfinClient::playUrlFor resolves either, and nothing in this
// increment invents a second namespace. Jellyfin's item ids are GUIDs unique across item types inside one
// server, so no per-kind field is needed to keep an album id from resolving as a track id: the two cannot
// collide in the first place. (Subsonic needs one because its ids are per-type small integers on some
// servers — Subsonic.h says so — which is a property of that protocol, not a rule about ids.)
//
// ==================================================================================================
// WHAT THE SERVER IS ASKED FOR, AND WHY EACH FIELD IS REQUESTED BY NAME
// ==================================================================================================
//   artists   /Artists?userId=…&Fields=ProviderIds
//   albums    /Users/<uid>/Items?IncludeItemTypes=MusicAlbum&AlbumArtistIds=<id>&Fields=ProviderIds
//   tracks    /Users/<uid>/Items?IncludeItemTypes=Audio&ParentId=<albumId>&Fields=MediaSources,ProviderIds
//
// One request per browse level, exactly as SubsonicClient does and for the reason stated there: walking the
// whole tree on entry is one request per artist plus one per album before a single row is drawn.
//
// `Fields=ProviderIds` is not decoration. Jellyfin does NOT return ProviderIds unless asked, and those are
// the MusicBrainz ids — the ground truth MusicId prefers over every string comparison. Omit the parameter
// and the merge silently falls back to the fuzzy matcher for a library that had the answer sitting in it,
// which is a whole class of missed merges nobody would ever see a cause for. Same for `MediaSources`: it is
// where the container and the bitrate live, and those are the picker's "the FLAC on the NAS is not the 128k
// copy on the phone" line.
//
// ==================================================================================================
// TICKS, AND THE ONE PLACE THEY ARE CONVERTED
// ==================================================================================================
// Jellyfin measures duration in RunTimeTicks: 100-nanosecond units. Everything above this file measures
// seconds. The conversion happens HERE, once, in the readers — never in a caller — so a duration cannot
// arrive somewhere as ticks and be shown as a runtime of 34,000 hours.
#pragma once
#include "Jellyfin.h"
#include "MusicLibrary.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace JellyfinMusic
{
    // ---- What the server says --------------------------------------------------------------------------
    // Flat structs of exactly what the browse levels need, so nothing above this file touches JSON. Every
    // `id` is the server's OWN id, unqualified: qualification happens in the index builders, where the
    // server it came from is known, so a reader cannot mint a half-formed key.

    struct RemoteArtist
    {
        QString id, name;
        QString musicBrainzArtistId;   // ProviderIds.MusicBrainzArtist; empty when the server has none
        int     albumCount = 0;
    };

    struct RemoteAlbum
    {
        QString id, name, artist, artistId;
        // TWO MusicBrainz ids, and they are never compared with each other — MusicId.h states that rule and
        // this is where both halves of it become available for the first time in this app. Jellyfin's
        // `MusicBrainzAlbum` is a RELEASE id and `MusicBrainzReleaseGroup` is a release-GROUP id; Subsonic
        // exposes only the former, which is why a Subsonic album's release group is always empty.
        QString musicBrainzAlbumId;        // the release
        QString musicBrainzReleaseGroupId; // the album across its reissues
        QString musicBrainzArtistId;       // the ALBUM ARTIST's id, when the row carries one
        int     songCount = 0, year = 0, durationSec = 0;
    };

    struct RemoteSong
    {
        QString id, title, artist, album, albumId;
        // WHAT THIS COPY IS. `format` is the container as the server spells it, upper-cased ("FLAC", "MP3");
        // `bitrateKbps` is MediaSources[0].Bitrate rounded to whole kbps. Both are 0/empty when the server
        // did not say, which is an ABSENCE and never a guess — see MusicLibrary::Album's fields.
        QString format;
        int     track = 0, disc = 0, year = 0, durationSec = 0, bitrateKbps = 0;
    };

    // ---- The readers -----------------------------------------------------------------------------------
    //
    // `ok` is false for a body that is not a Jellyfin item envelope at all — a proxy's HTML error page, a
    // truncated body, an empty reply. That is NOT the same as a server with no music, and the supplier
    // treats the two differently: an unparsable answer contributes nothing and leaves the level alone,
    // while an empty one is a real (and legal) empty library.
    QVector<RemoteArtist> readArtists(const QByteArray& body, bool* ok);
    QVector<RemoteAlbum>  readAlbums(const QByteArray& body, bool* ok);
    QVector<RemoteSong>   readSongs(const QByteArray& body, bool* ok);

    // ---- The one spelling of each request ---------------------------------------------------------------
    //
    // PATH **AND QUERY**, unlike Jellyfin::itemsPath, which returns a bare path and lets the client spell the
    // query inline. The difference is deliberate and it is about what can be tested: the `Fields=` list is
    // the whole reason the MusicBrainz ids and the bitrate reach this app at all (see the header), and a
    // parameter spelled inside a socket function is a parameter no probe can see. Here it is a pure string a
    // probe pins, so dropping `ProviderIds` in a future edit goes red instead of quietly costing every merge
    // its ground truth.
    QString artistsPath(const QString& userId);
    QString albumsPath(const QString& userId, const QString& artistId);
    QString songsPath(const QString& userId, const QString& albumId);

    // A playable URL for one AUDIO item. Jellyfin::streamUrl addresses /Videos/<id>/stream, which is the
    // video library's endpoint; music has its own, and a track asked for through the video one is a
    // transcode decision made by accident.
    //
    // CARRIES THE TOKEN, exactly as its video twin does — minted at the moment the player is handed it,
    // never stored, never logged, never written into a queue, a playlist or a recents row.
    QString audioStreamUrl(const QString& root, const QString& itemId, const QString& token);

    // ---- Onto the EXISTING music catalog shapes --------------------------------------------------------
    //
    // These build a MusicLibrary::Index — the very type #74's browse builders render and MusicMerge folds —
    // so a Jellyfin server's music and the local library are literally the same code with a different
    // supplier. Nothing here is a parallel browse tree.
    //
    // WHAT IS DELIBERATELY LEFT UNSET, and it is Subsonic's list for Subsonic's reasons: Index::trackCount
    // and Artist::trackCount stay 0, because they gate verbs ("Shuffle all music", an artist's "Play all")
    // that cannot work over tracks nobody has fetched. Album::trackCount carries the server's own count, so
    // #194 increment 2's reachable-count gate lights those verbs up the moment the albums land.

    MusicLibrary::Index indexOfArtists(const QString& serverId, const QVector<RemoteArtist>& artists);

    // Fill in ONE artist's albums. Tracks are not fetched here — the album level does that — so each Album
    // carries the server's own songCount and an empty `tracks`. A no-op when the artist key is not in the
    // index, which is what makes a stale route render nothing rather than crash.
    void fillArtistAlbums(MusicLibrary::Index& idx, const QString& serverId, const QString& artistKey,
                          const QVector<RemoteAlbum>& albums);

    // Fill in ONE album's tracks, in disc-then-track order. IndexTrack::path is the QUALIFIED id — never a
    // stream url, because a Jellyfin stream url carries `api_key=<token>` and this struct is copied into
    // queues, playlists and recents rows. JellyfinClient::playUrlFor mints the url at the one moment the
    // player is handed it.
    //
    // The album's `format` and `bitrateKbps` are taken from its tracks here: an album is one format when its
    // tracks agree and is left BLANK when they do not, because "this copy is FLAC" is a claim about the
    // whole record and a mixed folder cannot honestly make it.
    void fillAlbumTracks(MusicLibrary::Index& idx, const QString& serverId, const QString& albumKey,
                         const QVector<RemoteSong>& songs);
}
