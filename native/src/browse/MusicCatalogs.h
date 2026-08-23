// The BROWSE half of the local music library (issue #74, increment 3): the three levels a person walks —
// Artists -> that artist's Albums -> that album's Tracks — as pure builders over MusicLibrary::Index.
//
// This is the increment the user feels. Increments 1 and 2 read tags and grouped them; until these functions
// existed, pointing the app at a music folder still produced a file browser, which is the complaint #74 opens
// with. Nothing here decides anything about grouping — that is settled in MusicLibrary.h, including the two
// rules this file must not quietly re-implement: an album is keyed by (album artist, album title) so a
// compilation is ONE album under "Various Artists", and a multi-disc set is ONE album whose tracks are
// already ordered disc-then-track. These builders RENDER that index; they never regroup it.
//
// KEPT IN ITS OWN TRANSLATION UNIT, not in SyntheticCatalogs.cpp, for exactly the reason LiveTvGuide.cpp
// gives: only the app and this feature's probe want MusicLibrary/AudioTags/TagLib linked, and folding these
// three functions into SyntheticCatalogs would hand probe_browse, probe_iptv and probe_locallib a TagLib
// dependency they have no use for. The shape is otherwise localLibraryCatalog's and photosCatalog's, down to
// the namespace: data in, a MediaCatalog out, no Settings read, no UI, no scan.
//
// THE ROUTING CONTRACT is the `type` + `mime` pair on each row, spelled out as constants below because the
// surface dispatches on them and on nothing else. An album key is arbitrary tag text joined by 0x1F, so it
// can contain ':' — every reader takes the key as "everything after the prefix" (musicKeyOf), never as a
// colon-separated field.
//
// WHY A TRACK ROW CARRIES A URL AND STILL NEEDS INTERCEPTING. A track's `url` is its file path, so the tile
// looks and behaves like the playable thing it is. But the generic "this item has a url" route in the browse
// surface would open it as a lone file and queue its CONTAINING FOLDER, which is wrong for a multi-disc album
// (one disc per folder) and wrong for a compilation folder that also holds a bonus disc. So the surface
// intercepts kMusicTrackPrefix ahead of that route — the way an OPDS book is intercepted — and plays the
// ALBUM through PlaybackSession, starting at that track.
#pragma once
#include "../addons/AddonModels.h"   // MediaCatalog / MediaItem
#include "../core/MusicLibrary.h"    // the Index these three render

#include <QString>
#include <functional>

namespace browse
{
    // How a row gets its picture. Injected for the reason pcGamesCatalog injects `poster`: the default
    // touches the filesystem (an extracted-cover cache and a sibling-image lookup), and a probe wants to pin
    // the ROWS without either. Default {} = MusicArt::albumCover over MusicArt::cacheDir().
    using MusicCoverFn = std::function<QString(const MusicLibrary::Album&)>;

    // What the Music category says when there is nothing in it. TWO fields rather than one sentence because
    // the useful thing to name is a FOLDER PATH, and a path spliced into a sentence inside a tile wraps into
    // an unreadable ribbon — the same split every other row in this app uses, title over subtitle. `detail`
    // is optional; `text` empty means "say nothing at all" (see musicArtistsCatalog).
    struct MusicEmptyNote
    {
        QString text;      // the sentence a person reads
        QString detail;    // the folder it is about, in native separators, or empty
        bool isEmpty() const { return text.isEmpty(); }
    };

    // ---- The routing contract ----------------------------------------------------------------------------
    inline const char* kMusicArtistType      = "_musicartist";
    inline const char* kMusicAlbumType       = "_musicalbum";
    inline const char* kMusicPlayAlbumType   = "_musicplayalbum";
    inline const char* kMusicTrackType       = "track";   // NOT a "_" type: it is a real, playable leaf, and
                                                          // "track" is what gives it the music tile + colour
    inline const char* kMusicArtistPrefix    = "musicartist:";
    inline const char* kMusicAlbumPrefix     = "musicalbum:";
    inline const char* kMusicPlayAlbumPrefix = "musicplayalbum:";
    inline const char* kMusicTrackPrefix     = "musictrack:";

    // The MULTI-ALBUM verbs. Same shape as "Play album" above and for the same reason (see the action-row
    // note on musicAlbumCatalog): a "_"-prefixed type carries no url, so the surface routes it by type
    // instead of opening it as a file — and, on the themed/XMB layout, a "_" row is dispatched through the
    // ordinary browse activation rather than through the per-leaf action chooser, which is what makes these
    // verbs reachable there at all. That is the whole reason they are ROWS: the themed surface has no
    // container menu to hang "act on this artist" off, and adding one would be a new UI idiom.
    inline const char* kMusicPlayArtistType     = "_musicplayartist";
    inline const char* kMusicShuffleArtistType  = "_musicshuffleartist";
    inline const char* kMusicShuffleAllType     = "_musicshuffleall";
    inline const char* kMusicPlayArtistPrefix    = "musicplayartist:";
    inline const char* kMusicShuffleArtistPrefix = "musicshuffleartist:";
    inline const char* kMusicShuffleAllPrefix    = "musicshuffleall:";   // keyless: the whole library

    // "everything after `prefix`", or an empty string when `mime` does not start with it. The ONE reader, so
    // a key holding a ':' (an album titled "Vol. 1: Live") can never be truncated by a section() somewhere.
    //
    // INLINE, in the header, so that LeafRoute.cpp — which reads a track row's album key on the way to
    // routing it — can call the one reader without linking this TU. That link edge would drag MusicLibrary
    // and TagLib into probe_browse / probe_iptv / probe_locallib, which is precisely what keeping these
    // builders in their own translation unit exists to prevent (see the note at the top of this file).
    inline QString musicKeyOf(const QString& mime, const char* prefix)
    {
        const QString p = QString::fromLatin1(prefix);
        return mime.startsWith(p) ? mime.mid(p.size()) : QString();
    }

    // ---- Level 1: the Music category root — every artist -------------------------------------------------
    // Leads with a "Shuffle all music" ACTION row (kMusicShuffleAllType) whenever the library holds more than
    // one track, then one expandable row per artist, in the index's order (display name, unknown bucket
    // last). The subtitle counts their albums and tracks; the tile art is their FIRST album's cover, so an
    // artist row is not a blank card when there is a perfectly good picture one level down.
    //
    // ONLY shuffle at this level, not "play all": a library played start to finish in alphabetical-artist
    // order is not a thing anyone asks for, while "put on all my music" is the plainest form of the request
    // this whole feature exists to answer. The per-artist level offers both, because there an ordered play IS
    // a discography.
    //
    // `note` is what to say when the index has nothing: rather than an empty shelf the catalog then carries
    // ONE non-actionable "info" row saying it. It is a PARAMETER because only the caller can tell "no folder
    // chosen" from "still scanning" from "that folder has no music in it" — those need different sentences,
    // and each of them reads Settings or scan state, which is what this file has none of. An empty note over
    // an empty index yields an empty catalog (the pre-existing shape), so an explanation is never fabricated
    // here.
    MediaCatalog musicArtistsCatalog(const MusicLibrary::Index& idx, const MusicEmptyNote& note,
                                     const MusicCoverFn& cover = {});

    // ---- Level 2: one artist's albums --------------------------------------------------------------------
    // Titled with the artist's display name. Leads with "Play all" and "Shuffle all" ACTION rows
    // (kMusicPlayArtistType / kMusicShuffleArtistType) whenever the artist has more than one track — one
    // track has nothing to order and nothing to shuffle — and then one expandable row per album in the
    // index's order (year, then title), subtitled with the year, the track count, and the disc count when
    // there is more than one — the multi-disc album appears ONCE, which is the whole point of the key not
    // carrying the disc number.
    // Below the discography come this artist's CREDITS (issue #196) — tracks that name them but belong to an
    // album filed under somebody else, subtitled with that album. An artist who is only ever a co-credit has
    // no albums and nothing but these, which is what they are; pressing one plays the album it is on. They
    // are outside the Play all / Shuffle all rows on purpose (MusicCatalogs.cpp says why).
    // An unknown artistKey yields an empty, titled catalog with NO action rows: a stale route must not be
    // able to crash a navigation, and it must not offer to play an artist that is not there either. The
    // surface re-reads the index on Back.
    MediaCatalog musicArtistCatalog(const MusicLibrary::Index& idx, const QString& artistKey,
                                    const MusicCoverFn& cover = {});

    // ---- Level 3: one album's tracks ---------------------------------------------------------------------
    // Titled with the album. Leads with a "Play album" ACTION row (kMusicPlayAlbumType) and then every track
    // in the index's disc-then-track order.
    //
    // The action row exists because an album row one level up has to do exactly one thing when activated, and
    // that thing is "show me what is on it"; without a row here, "play the whole album" would be reachable
    // only by pressing its first track, which is not a thing anyone would guess. It is a row rather than a
    // button for the reason pcLauncherFilterRow gives: this app has four layouts and only one of them has
    // chrome a button could live in, while a row is D-pad reachable in all four by construction.
    //
    // A track's title carries its number ("7." — or "2-3." on a multi-disc set, where the bare number would
    // repeat), and its subtitle carries the TRACK artist whenever it differs from the album artist, which on
    // a compilation is every row and is the only thing that makes such a list readable. An unknown albumKey
    // yields an empty, titled catalog.
    MediaCatalog musicAlbumCatalog(const MusicLibrary::Index& idx, const QString& albumKey,
                                   const MusicCoverFn& cover = {});
}
