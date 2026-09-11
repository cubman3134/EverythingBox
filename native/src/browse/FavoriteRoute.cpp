#include "FavoriteRoute.h"
#include "MusicCatalogs.h"          // kMusicTrackType — the one spelling of a track row's type
#include "../core/Jellyfin.h"       // Jellyfin::isQualified — each supplier's own reader, never a prefix test
#include "../core/MusicLibrary.h"   // #369: Index::track — which album a local track is on
#include "../core/ServerMusic.h"    // ServerMusic::isQualified
#include "../core/Subsonic.h"       // Subsonic::isQualified / serverOf
#include "../media/CueSheet.h"      // #369: clipFile — the file a cue track's clip url names

#include <QCoreApplication>
#include <QLatin1String>

namespace browse
{

MediaItem favoriteShelfRow(const FavoriteItem& f)
{
    MediaItem it;
    it.id           = f.itemId;
    it.type         = f.type;
    it.title        = f.title;
    it.subtitle     = f.subtitle;
    it.thumbnailUrl = f.thumbnailUrl;   // the shelf swaps in MetaCache's offline-first copy of this
    it.expandable   = f.expandable;
    it.mime         = QStringLiteral("fav:") + f.addonId;   // marks a favourite + carries its source add-on
    return it;
}

FavoriteRoute favoriteRouteFor(const MediaItem& favItem, const QVector<FavoriteItem>& stored,
                               const FavoriteWorld& world)
{
    FavoriteRoute r;
    // 2. A stored record carrying a path re-opens by it, with the STORED record's own title and cover —
    // exactly the emit openFavorite has always made.
    for (const FavoriteItem& f : stored)
        if (f.itemId == favItem.id && !f.path.isEmpty())
        {
            r.how = FavoriteOpen::ReopenByPath;
            r.path = f.path; r.kind = f.kind; r.resumeKey = f.itemId; r.title = f.title; r.thumb = f.thumbnailUrl;
            return r;
        }
    // 3. A native-store game with no local file.
    if (favItem.id.startsWith(QLatin1String("steam:")) || favItem.id.startsWith(QLatin1String("epic:")))
    {
        r.how = FavoriteOpen::NativeStore;
        return r;
    }
    // mid(4) strips "fav:" — the marker favoriteShelfRow puts in front of the add-on id.
    const QString addonId = favItem.mime.mid(4);
    // 4. A MUSIC TRACK (#364): type "track" AND naming no add-on — FavoriteRoute.h says why it takes both.
    if (favItem.type == QLatin1String(kMusicTrackType) && addonId.isEmpty() && !favItem.id.isEmpty())
    {
        const QString& id = favItem.id;
        if (Subsonic::isQualified(id))
        {
            // The server it names must still be set up. MusicSupply::playUrl mints nothing for a removed one,
            // and openRecent's line saying so goes to a status bar the app keeps hidden.
            if (!(world.serverKnown && world.serverKnown(Subsonic::serverOf(id))))
            {
                r.how = FavoriteOpen::TrackServerGone;
                return r;
            }
            r.how = FavoriteOpen::ServerTrack;
        }
        else if (Jellyfin::isQualified(id) || ServerMusic::isQualified(id))
        {
            r.how = FavoriteOpen::TrackNoDoor;
            return r;
        }
        else
        {
            // LOCAL. The local music index is asked first (#369): a track it holds opens its ALBUM below, and
            // its sourcePath is the real file on disk — the shared file, for a cue track.
            const MusicLibrary::IndexTrack* t = world.localMusic ? world.localMusic->track(id) : nullptr;
            // THE FILE TO ASK ABOUT. A clip url ("edl://…", a cue track) names a span of a file rather than a
            // file, so the url itself is never asked about — the file it names is: the index's sourcePath, else
            // the one CueSheet reads back out of the url. ("://" is openRecent's own test for a link.)
            const bool isUrl = id.contains(QLatin1String("://"));
            const QString file = (t && !t->sourcePath.isEmpty()) ? t->sourcePath
                                                                 : (isUrl ? CueSheet::clipFile(id) : id);
            // Empty only for a clip url nothing could read a file out of. That is "cannot tell", never "gone":
            // it opens exactly as it did before #369, unchecked.
            if (!file.isEmpty() && !(world.fileExists && world.fileExists(file)))
            {
                r.how = FavoriteOpen::TrackFileGone;
                return r;
            }
            // THE ALBUM, in its disc-then-track order, starting at this track: openMusicAlbum(albumKey, path).
            if (t && !t->albumKey.isEmpty())
            {
                r.how = FavoriteOpen::LocalAlbum;
                r.albumKey = t->albumKey;
                r.path = id; r.title = favItem.title; r.thumb = favItem.thumbnailUrl;
                return r;
            }
            // Not in the index: #364's route, unchanged — the track's own Recents-row door.
            r.how = FavoriteOpen::LocalTrack;
        }
        // The id in BOTH path and resume key: openRecent consults the key first for a music identity (so a
        // Subsonic id reaches its qualified-track arm), and reads the path for a local file.
        r.path = id; r.kind = QStringLiteral("audio"); r.resumeKey = id;
        r.title = favItem.title; r.thumb = favItem.thumbnailUrl;
        return r;
    }
    // 5. An add-on's item.
    r.addonId = addonId;
    r.how = (world.sourceKnown && world.sourceKnown(addonId)) ? FavoriteOpen::Addon : FavoriteOpen::AddonMissing;
    return r;
}

QString favoriteOpenSentence(FavoriteOpen how, const QString& title)
{
    // HomeView's translation context: these are shown by HomeView's toast, beside the add-on sentence.
    switch (how)
    {
        case FavoriteOpen::TrackFileGone:
            return QCoreApplication::translate("HomeView", "“%1” can't be played — its file has been moved or "
                                                           "deleted.").arg(title);
        case FavoriteOpen::TrackServerGone:
            return QCoreApplication::translate("HomeView", "“%1” can't be played — its music server is no "
                                                           "longer set up.").arg(title);
        case FavoriteOpen::TrackNoDoor:
            return QCoreApplication::translate("HomeView", "“%1” is on a music source Favorites can't open "
                                                           "yet — play it from Music.").arg(title);
        case FavoriteOpen::ReopenByPath: case FavoriteOpen::NativeStore: case FavoriteOpen::LocalTrack:
        case FavoriteOpen::LocalAlbum:
        case FavoriteOpen::ServerTrack:  case FavoriteOpen::Addon:       case FavoriteOpen::AddonMissing:
            break;
    }
    return QString();
}

} // namespace browse
