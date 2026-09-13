#include "LeafRoute.h"
#include "AudiobookCatalogs.h"  // kAudiobookFilePrefix + audiobookKeyOf — likewise, for #139's books
#include "JellyfinCatalogs.h"   // kJellyfinItemPrefix + jellyfinKeyOf — and again, for #83's server items
#include "MusicCatalogs.h"   // kMusicTrackPrefix + musicKeyOf — a keyed kind's contract lives with its feature
#include "../core/LiveTvIdentity.h" // kLiveTvChannelPrefix + channelKeyOf — ditto, for #244's starred channel
                                    // (QtCore-only and header-only here: this adds no link dependency)

#include <QLatin1String>

namespace browse
{

const QVector<LocalLeafKind>& localLeafKinds()
{
    // THE list of local leaf kinds. Every entry is a kind that belongs to no addon, so HomeView::resolvePlay
    // cannot do anything with it — which is why both surfaces have to claim it here first.
    //
    // Order is match order, first hit wins. The kinds do not overlap today; if a future prefix could shadow a
    // whole-match kind, put the whole match above it.
    static const QVector<LocalLeafKind> kinds = {
        { kLocalVideoMime,   LocalLeafKind::Mime, false, LeafPlay::OpenFile   },
        { kPhotoMime,        LocalLeafKind::Mime, false, LeafPlay::OpenFile   },
        { kLocalBookMime,    LocalLeafKind::Mime, false, LeafPlay::OpenFile   },
        { kOpdsBookType,     LocalLeafKind::Type, false, LeafPlay::OpdsBook   },
        { kMusicTrackPrefix, LocalLeafKind::Mime, true,  LeafPlay::MusicAlbum },
        { kAudiobookFilePrefix, LocalLeafKind::Mime, true, LeafPlay::AudiobookBook },
        { kJellyfinItemPrefix,  LocalLeafKind::Mime, true, LeafPlay::JellyfinItem },
        { LiveTvIdentity::kLiveTvChannelPrefix, LocalLeafKind::Mime, true, LeafPlay::LiveTvChannel },
    };
    return kinds;
}

LeafRoute localLeafRoute(const MediaItem& it)
{
    for (const LocalLeafKind& k : localLeafKinds())
    {
        const QString& field = (k.field == LocalLeafKind::Mime) ? it.mime : it.type;
        const QLatin1String id(k.id);
        if (k.prefix ? !field.startsWith(id) : field != id) continue;

        LeafRoute r;
        r.play = k.play;
        if (k.play == LeafPlay::MusicAlbum)
        {
            // The key is "everything after the prefix" — musicKeyOf, never a section(':'), because an album
            // key is arbitrary tag text and an album titled "Vol. 1: Live" would be truncated by one.
            r.key = musicKeyOf(it.mime, k.id);
            if (r.key.isEmpty()) return {};   // a track row naming no album: let the caller resolve it instead
        }
        else if (k.play == LeafPlay::AudiobookBook)
        {
            // Same rule, and on Windows the same truncation avoided rather more often: a book key STARTS
            // with a folder path, so "C:/Books/…" would be cut at the drive letter by any section(':').
            r.key = audiobookKeyOf(it.mime, k.id);
            if (r.key.isEmpty()) return {};   // a part naming no book: let the caller resolve it instead
        }
        else if (k.play == LeafPlay::JellyfinItem)
        {
            // Same "everything after the prefix" rule, and here it is not optional: the key IS a qualified
            // id — "jf:<32 hex>:<item>" — so any section(':') split would hand back "jf" and route the row
            // at a server that does not exist. A row naming no item falls through to the resolve it would
            // have taken anyway rather than being claimed and dropped.
            //
            // AND NOTHING BELOW ASKS FOR A URL. A Jellyfin row carries none by design; the empty-url refusal
            // in the next arm would reject every one of them.
            r.key = jellyfinKeyOf(it.mime, k.id);
            if (r.key.isEmpty()) return {};
        }
        else if (k.play == LeafPlay::LiveTvChannel)
        {
            // Same "everything after the prefix" rule and the same reason: a channel identity is
            // "livetv:name:bbc one", so any section(':') split would hand back "livetv" and resolve nothing.
            // A row naming no channel falls through rather than being claimed and dropped — and NOTHING
            // below asks for a url, which a starred channel deliberately does not have.
            r.key = LiveTvIdentity::channelKeyOf(it.mime, k.id);
            if (r.key.isEmpty()) return {};
        }
        else if (it.url.isEmpty())
        {
            // A file route with no file. Answering OpenFile here would claim the row and then open nothing,
            // which reads as Enter doing absolutely nothing; falling through leaves it with the resolve it
            // had before this file existed.
            return {};
        }
        return r;
    }
    return {};   // NotLocal: an addon's row, or a container — not ours to play
}

QueueTarget queueTargetFor(const MediaItem& it)
{
    // A TRACK, asked through the very same table Enter reads. localLeafRoute already refuses a track row
    // whose mime carries no album key, so the only extra requirement here is the FILE: Enter on such a row
    // would queue the whole record from the top (playMusicAlbumRequested with an empty start path), which is
    // a sensible fallback for "play", and nonsense for "add this one track".
    const LeafRoute lr = localLeafRoute(it);
    if (lr.play == LeafPlay::MusicAlbum)
    {
        if (it.url.isEmpty()) return {};
        QueueTarget t;
        t.what      = QueueAdd::Track;
        t.albumKey  = lr.key;
        t.trackPath = it.url;   // for a cue track this is the EDL clip url, which IS what the queue holds
        return t;
    }

    // A RECORD. Two rows name one: the album row in an artist's (or the library's) album list, and the
    // "Play album" action row at the top of that album's own track list — where somebody standing inside an
    // album is most likely to reach for the verb. Both are '_'-prefixed synthetic rows, which is why neither
    // can be reached through the themed inline chooser (themedEnterFor sends '_' rows down the ordinary
    // browse path, deliberately, so they can DRILL) and why the browse context menu is what carries them.
    struct AlbumRow { const char* type; const char* prefix; };
    static const AlbumRow kAlbumRows[] = {
        { kMusicAlbumType,     kMusicAlbumPrefix     },
        { kMusicPlayAlbumType, kMusicPlayAlbumPrefix },
    };
    for (const AlbumRow& a : kAlbumRows)
    {
        if (it.type != QLatin1String(a.type)) continue;
        QueueTarget t;
        t.albumKey = musicKeyOf(it.mime, a.prefix);
        if (t.albumKey.isEmpty()) return {};   // an album row naming no album: nothing to add
        t.what = QueueAdd::Album;
        return t;
    }
    return {};
}

FavoriteItem trackFavoriteFor(const MediaItem& it)
{
    // Asked of queueTargetFor rather than of the mime a second time: a row that queues as a track is the row
    // that stars as one, and a rename of the track prefix cannot unroute one verb and leave the other.
    const QueueTarget target = queueTargetFor(it);
    if (target.what != QueueAdd::Track || it.id.isEmpty()) return {};
    // The themed chooser's generic arm (HomeView::favoriteThemedLeaf), field for field. A music level has no
    // addon, so addonId is empty there too; no path/kind/system, because a track re-opens through neither.
    FavoriteItem f;
    f.itemId       = it.id;          // a track row's id IS its path (trackRow) — the id the love hook maps
    f.title        = it.title;
    f.subtitle     = it.subtitle;
    f.type         = it.type;        // kMusicTrackType, "track": the one type the love hook acts on
    f.thumbnailUrl = it.thumbnailUrl;
    f.expandable   = it.expandable;
    // #368: the album the row is ON — the key its mime names, which queueTargetFor has just read. Recorded for
    // every track (this unit links no supplier's id reader, and a key is not a credential), read back only for
    // the supplier whose track id cannot name its album: FavoritesStore.h, at FavoriteItem::albumKey.
    f.albumKey     = target.albumKey;
    return f;
}

TrackFavVerb trackFavoriteVerb(const FavoriteItem& fav, bool alreadyFavorite)
{
    if (fav.itemId.isEmpty()) return TrackFavVerb::None;
    return alreadyFavorite ? TrackFavVerb::Remove : TrackFavVerb::Add;
}

TrackMenuVerbs trackMenuVerbsFor(const MediaItem& it, TrackAddon addon)
{
    TrackMenuVerbs v;
    // Not media, or not a row a playlist could ever re-open: a synthetic '_' row, a guidance line, a Recent
    // divider (the three addItemToPlaylistInteractive refuses itself), a container, or a row with no id to file.
    if (it.type.startsWith(QLatin1Char('_')) || it.type == QLatin1String("info")
        || it.type == QLatin1String("rechdr") || it.expandable || it.id.isEmpty())
        return v;
    // A LIBRARY track, asked of queueTargetFor exactly as trackFavoriteFor asks it. NEVER Download, whatever
    // add-on a caller believes is behind it: a local file is already local, and a Subsonic / Jellyfin / server
    // track has no add-on for the crawl to walk, so the press could only say "Nothing here could be downloaded."
    if (queueTargetFor(it).what == QueueAdd::Track) { v.playlist = true; return v; }
    // A row in the library track's own spelling that is not a usable Track names no file. It is a library row
    // that names nothing, not an add-on's — so nothing, for queueTargetFor's reason.
    if (it.mime.startsWith(QLatin1String(kMusicTrackPrefix))) return v;
    // An ADD-ON's track: a music leaf, under an add-on. The three spellings are the music-leaf types
    // core::mediaCategory files under "audio" (an album is a container, and caught above).
    if (addon == TrackAddon::None) return v;
    static const char* const kAddonTrackTypes[] = { "track", "song", "music" };
    bool track = false;
    for (const char* t : kAddonTrackTypes)
        if (it.type == QLatin1String(t)) { track = true; break; }
    if (!track) return v;
    v.playlist = true;
    // ...and Download only where the crawl downloads: a REMOTE add-on's leaf, through its /stream.
    v.download = (addon == TrackAddon::Remote);
    return v;
}

bool isReadableChapterType(const QString& type)
{
    static const QString kSuffix = QStringLiteral("_chapter");
    return type.size() > kSuffix.size() && type.endsWith(kSuffix);   // a bare "_chapter" names no family
}

DownloadLeafArm downloadLeafArmFor(const MediaItem& it, TrackAddon addon)
{
    // HomeView::dlResolveLeaf's arms, in its order. It dispatches on this, so this IS the crawl's table.
    // Can't pull as a single file: a store-launcher game, or a page-based chapter.
    if (it.mime == QLatin1String("steamgame") || it.mime == QLatin1String("epicgame")
        || it.mime == QLatin1String("goggame") || it.mime == QLatin1String("battlenetgame")
        || isReadableChapterType(it.type))
        return DownloadLeafArm::None;
    // A script add-on's document or game: searched for on the file provider by title.
    if (addon == TrackAddon::Script
        && (it.type == QLatin1String("comic_issue") || it.type == QLatin1String("book")
            || it.type == QLatin1String("audiobook") || it.type == QLatin1String("game")))
        return DownloadLeafArm::LocalBridge;
    // A remote add-on's leaf, of any type: its /stream (a file provider OR Stremio).
    if (addon == TrackAddon::Remote) return DownloadLeafArm::RemoteStream;
    // A movie / episode / series / tv from anywhere else: its /meta names the IMDB id, and that bridges.
    if (it.type == QLatin1String("movie") || it.type == QLatin1String("episode")
        || it.type == QLatin1String("series") || it.type == QLatin1String("tv"))
        return DownloadLeafArm::MetaBridge;
    return DownloadLeafArm::None;   // unknown / non-downloadable leaf
}

bool downloadOffered(const MediaItem& it, const DownloadOfferFacts& facts)
{
    // Already on this machine (a local game file, a Recent / Downloaded row): the press could only say so.
    if (facts.alreadyLocal) return false;
    // Everything the detail row offered before #372 is still offered — classicActionGates' answer, kept whole
    // (a remote / bridged catalog leaf, a crawlable container, a Jellyfin row).
    if (facts.classicGate) return true;
    // Beyond it, only a real LEAF can be claimed by a crawl arm: not a container (its download is the classic
    // gate's, above), not a synthetic '_' row, a guidance line or a Recent divider, and not a row with no id.
    if (it.expandable || it.id.isEmpty() || it.type.startsWith(QLatin1Char('_'))
        || it.type == QLatin1String("info") || it.type == QLatin1String("rechdr"))
        return false;
    // A LIBRARY row — a local / Subsonic / Jellyfin / server track, or a row in the library track's spelling
    // that names no file. #365's rule, asked of the same classification trackMenuVerbsFor uses: never Download,
    // whatever add-on context it is asked in. There is no add-on for the crawl to walk.
    if (queueTargetFor(it).what == QueueAdd::Track || it.mime.startsWith(QLatin1String(kMusicTrackPrefix)))
        return false;
    switch (downloadLeafArmFor(it, facts.addon))
    {
    case DownloadLeafArm::LocalBridge:  return facts.fileProvider;   // the title search needs a provider to ask
    case DownloadLeafArm::RemoteStream: return true;                 // the add-on's own /stream
    case DownloadLeafArm::MetaBridge:
        // requestMeta answers nothing without an add-on (the press would sit on "Preparing download…"), and
        // onMetaReady's resolve by IMDB id needs a stream provider for the row's kind.
        return facts.addon != TrackAddon::None && facts.streamProvider;
    case DownloadLeafArm::None:         break;
    }
    return false;
}

ThemedEnter themedEnterFor(const QString& type, bool expandable)
{
    if (expandable) return ThemedEnter::Drill;                        // a container: series / console / volume
    if (type.startsWith(QLatin1Char('_'))) return ThemedEnter::Drill;  // synthetic: Playlists, a playlist, New…
    if (type == QLatin1String("info")) return ThemedEnter::Drill;      // guidance prose: inert, never a chooser
    return ThemedEnter::Chooser;                                       // a real leaf: Play / Favorite / …
}

} // namespace browse
