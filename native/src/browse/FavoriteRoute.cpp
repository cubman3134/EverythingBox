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

namespace
{
// A server-shelf TRACK id, and nothing else a shelf mints: an album or an artist names nothing a player opens.
bool isShelfTrack(const QString& s)
{
    const ServerMusic::Ref r = ServerMusic::parse(s);
    return r.ok && r.kind == ServerMusic::Kind::Track;
}

// `albumKey` when it is an album OF THE SAME SHELF as `trackId`, else empty. A key that names another server's
// album, or something that is not an album, would send the fetch to the wrong place or to nothing — worse than
// having no album, which at least says so.
QString sameShelfAlbum(const QString& trackId, const QString& albumKey)
{
    const ServerMusic::Ref t = ServerMusic::parse(trackId);
    const ServerMusic::Ref a = ServerMusic::parse(albumKey);
    return (t.ok && a.ok && a.kind == ServerMusic::Kind::Album && a.sourceId == t.sourceId) ? albumKey : QString();
}
} // namespace

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
        else if (Jellyfin::isQualified(id))
        {
            // #368. JellyfinMusicClient::streamUrl mints from the id and the server's STORED sign-in, with nothing
            // fetched first — so it opens straight after a restart, exactly as a Subsonic track does. It mints
            // nothing for a server that is gone or switched off, and each of those says which it is.
            const QString srv = Jellyfin::serverOf(id);
            if (!(world.jellyfinKnown && world.jellyfinKnown(srv)))
            {
                r.how = FavoriteOpen::TrackServerGone;
                return r;
            }
            if (!(world.jellyfinOn && world.jellyfinOn(srv)))
            {
                r.how = FavoriteOpen::TrackServerOff;
                return r;
            }
            r.how = FavoriteOpen::ServerTrack;
        }
        else if (ServerMusic::isQualified(id))
        {
            // #368. An EverythingBox server's track url may be signed, so it lives for one session — filled when
            // the track's ALBUM is fetched — and the id cannot name that album. The record can: every track
            // favourite carries the album its row was on (FavoriteItem::albumKey). openRecent is handed that
            // album as the path and the track as the key, and fetches the album first when it holds no url.
            if (!(world.shelfKnown && world.shelfKnown(ServerMusic::sourceOf(id))))
            {
                r.how = FavoriteOpen::TrackServerGone;
                return r;
            }
            QString album;
            for (const FavoriteItem& f : stored)
                if (f.itemId == id) { album = sameShelfAlbum(id, f.albumKey); break; }
            // A star from before #368 has no album. It still opens when this session already holds its url
            // (its album was browsed); otherwise it says what to do, rather than open a player on nothing.
            if (album.isEmpty() && !(world.shelfUrlReady && world.shelfUrlReady(id)))
            {
                r.how = FavoriteOpen::TrackAlbumUnknown;
                return r;
            }
            r.how = FavoriteOpen::ServerTrack;
            r.path = album.isEmpty() ? id : album;   // where it plays from: its album, when one is known
            r.albumKey = album;
            r.kind = QStringLiteral("audio"); r.resumeKey = id;
            r.title = favItem.title; r.thumb = favItem.thumbnailUrl;
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
        // Subsonic or Jellyfin id reaches its qualified-track arm), and reads the path for a local file.
        r.path = id; r.kind = QStringLiteral("audio"); r.resumeKey = id;
        r.title = favItem.title; r.thumb = favItem.thumbnailUrl;
        return r;
    }
    // 5. An add-on's item.
    r.addonId = addonId;
    r.how = (world.sourceKnown && world.sourceKnown(addonId)) ? FavoriteOpen::Addon : FavoriteOpen::AddonMissing;
    return r;
}

RemoteTrackOpen remoteTrackOpenFor(const QString& path, const QString& kind, const QString& resumeKey)
{
    RemoteTrackOpen o;
    // THE KEY FIRST, then the path — openRecent's rule for every identity: the key is what the row IS, the path
    // only where it played from. For a shelf track the path may be that album, and it is kept only when it is.
    if (isShelfTrack(resumeKey))
    {
        o.trackId = resumeKey;
        o.albumKey = sameShelfAlbum(resumeKey, path);
        return o;
    }
    if (isShelfTrack(path))
    {
        o.trackId = path;
        return o;
    }
    // A Jellyfin id carries no kind, and openJellyfinItem opens it as a VIDEO. Only the caller's "audio" says
    // this one is a music track (the favourite's route and openAudioStream's Recents row both file that kind).
    if (kind == QLatin1String("audio"))
    {
        if (Jellyfin::isQualified(resumeKey))  o.trackId = resumeKey;
        else if (Jellyfin::isQualified(path)) o.trackId = path;
    }
    return o;
}

void openRemoteTrack(const RemoteTrackOpen& o, const QString& title, const RemoteTrackDoors& doors)
{
    if (o.trackId.isEmpty() || !doors.mint || !doors.play || !doors.say) return;
    // Minted now, handed straight to the player, kept nowhere.
    const QString url = doors.mint(o.trackId);
    if (!url.isEmpty())
    {
        doors.play(url, o.trackId);
        return;
    }
    if (o.albumKey.isEmpty() || !doors.fetchAlbum)
    {
        doors.say(Jellyfin::isQualified(o.trackId)
                      ? QCoreApplication::translate("MainWindow", "“%1” can't be played — its music server is not "
                                                                  "set up, or is switched off.").arg(title)
                      : QCoreApplication::translate("MainWindow", "“%1” can't be played until its album has been "
                                                                  "opened in Music.").arg(title));
        return;
    }
    // COLD: this session holds no url for it. Fetch the album it is on — its urls come with it — then play. The
    // doors are copied into the callback: the album lands later, after this call and its caller have returned.
    const RemoteTrackDoors d = doors;
    const QString trackId = o.trackId;
    doors.fetchAlbum(o.albumKey, [d, trackId, title](bool ok, const QString& sentence) {
        if (!ok)
        {
            // The client's own sentence ("That server is not connected any more."), which has never seen a url.
            d.say(!sentence.isEmpty() ? sentence
                                      : QCoreApplication::translate("MainWindow", "“%1” could not be fetched from its "
                                                                                  "music server.").arg(title));
            return;
        }
        const QString u = d.mint(trackId);
        if (u.isEmpty())
        {
            d.say(QCoreApplication::translate("MainWindow", "“%1” is no longer on its album on the music server.")
                      .arg(title));
            return;
        }
        d.play(u, trackId);
    });
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
        case FavoriteOpen::TrackServerOff:
            return QCoreApplication::translate("HomeView", "“%1” can't be played — its music server is switched "
                                                           "off.").arg(title);
        case FavoriteOpen::TrackAlbumUnknown:
            return QCoreApplication::translate("HomeView", "“%1” was starred before Favorites kept its album — play "
                                                           "it from Music, and star it again there to open it from "
                                                           "here.").arg(title);
        case FavoriteOpen::ReopenByPath: case FavoriteOpen::NativeStore: case FavoriteOpen::LocalTrack:
        case FavoriteOpen::LocalAlbum:
        case FavoriteOpen::ServerTrack:  case FavoriteOpen::Addon:       case FavoriteOpen::AddonMissing:
            break;
    }
    return QString();
}

} // namespace browse
