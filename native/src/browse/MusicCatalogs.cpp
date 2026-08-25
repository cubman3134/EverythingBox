#include "MusicCatalogs.h"
#include "../core/MusicArt.h"

#include <QObject>
#include <QString>

namespace browse
{
namespace {

// "m:ss" / "h:mm:ss". Nothing at all for a duration of 0, which is what a container that cannot give one
// cheaply reports (AudioTags::Tags::durationSec says so) — printing "0:00" beside a real track reads as a
// broken file rather than as a missing number.
QString fmtDuration(int secs)
{
    if (secs <= 0) return QString();
    const int h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString joinDot(const QStringList& parts)
{
    QStringList kept;
    for (const QString& p : parts) if (!p.isEmpty()) kept << p;
    return kept.join(QStringLiteral(" · "));
}

// The cover for one album, through the injected resolver or the default (extracted cache, then sibling file).
QString coverFor(const MusicLibrary::Album& b, const MusicCoverFn& fn)
{
    if (fn) return fn(b);
    static const QString dir = MusicArt::cacheDir();   // one AppPaths read per process, not one per tile
    return MusicArt::albumCover(b, dir);
}

MediaItem albumRow(const MusicLibrary::Album& b, const MusicCoverFn& cover)
{
    MediaItem it;
    it.id           = QString::fromLatin1(kMusicAlbumPrefix) + b.key;
    it.type         = QString::fromLatin1(kMusicAlbumType);
    it.mime         = QString::fromLatin1(kMusicAlbumPrefix) + b.key;   // -> musicAlbumCatalog
    it.expandable   = true;
    it.title        = MusicLibrary::displayAlbum(b);
    it.thumbnailUrl = coverFor(b, cover);
    // Album::trackCount, NOT tracks.size(): for a scanned album buildIndex makes them equal, and for a
    // remote album (#193) the songs have not been fetched yet at this level while the count has. See
    // MusicLibrary.h.
    it.subtitle     = joinDot({ b.year > 0 ? QString::number(b.year) : QString(),
                                QObject::tr("%n track(s)", "", b.trackCount),
                                b.discCount > 1 ? QObject::tr("%n disc(s)", "", b.discCount) : QString() });
    return it;
}

// A multi-album ACTION row (Play all / Shuffle all / Shuffle all music). No url, so the surface routes it by
// type; the id and the mime carry the same prefixed key, exactly as the "Play album" row does, so every
// music row in this file is read back through the one musicKeyOf reader.
// One TRACK row. Shared by an album's list and by an artist's credits (issue #196), because the two are the
// same thing seen from two sides and a second builder would drift: the mime is what tells the surface WHICH
// album to queue behind the file, and a credit row that got that wrong would play the wrong record.
// `numbered` is off for a credit — "7." is meaningful in a track list and meaningless in a list of one-off
// appearances — and `subtitle` says what the row's own context leaves unsaid.
MediaItem trackRow(const MusicLibrary::IndexTrack& t, const QString& albumKey, const QString& art,
                   const QString& title, const QString& subtitle)
{
    MediaItem it;
    // THE PLAYABLE HANDLE, which for a cue album's track is a clip of the shared file rather than the file
    // itself (#196 part 3 — MusicLibrary::IndexTrack::path says why). It is never opened as a url from here:
    // the surface intercepts kMusicTrackPrefix ahead of the generic "this item has a url" route (see the
    // note at the top of MusicCatalogs.h) and plays the ALBUM starting at this handle, so what this string
    // has to be is the thing the queue will hold and match on — which is exactly what it is.
    it.url          = t.path;
    it.id           = t.path;
    it.type         = QString::fromLatin1(kMusicTrackType);
    it.mime         = QString::fromLatin1(kMusicTrackPrefix) + albumKey; // WHICH album to queue behind it
    it.thumbnailUrl = art;
    it.title        = title;
    it.subtitle     = subtitle;
    // THE CLASSICAL CREDITS ON THE ROW (issue #196, part 2), and this is how composer/conductor become #63
    // FILTER FIELDS: the saved-filter evaluator reads a row's facts out of `art.meta`, exactly as it reads a
    // game's scraped genre and player count from there, so putting them here is what makes "all Bach
    // conducted by Gardiner" a saved filter rather than a bespoke query path. Two keys apiece for the same
    // reason the model carries two — the joined string is what a theme's meta panel displays, the list is
    // what the filter matches on, and re-splitting the display string would be a second copy of a split only
    // the scan was in a position to make.
    //
    // WRITTEN ONLY WHEN PRESENT. An ordinary track has empty lists, inserts nothing, and its MediaArt stays
    // isEmpty() — so a pop library's rows are byte-for-byte the rows they were.
    if (!t.composers.isEmpty())
    {
        it.art.meta.insert(QStringLiteral("composer"), t.composers.join(QStringLiteral("; ")));
        it.art.meta.insert(QStringLiteral("composers"), t.composers);
    }
    if (!t.conductors.isEmpty())
    {
        it.art.meta.insert(QStringLiteral("conductor"), t.conductors.join(QStringLiteral("; ")));
        it.art.meta.insert(QStringLiteral("conductors"), t.conductors);
    }
    return it;
}

MediaItem actionRow(const char* type, const char* prefix, const QString& key,
                    const QString& title, const QString& subtitle, const QString& art)
{
    MediaItem it;
    it.id           = QString::fromLatin1(prefix) + key;
    it.type         = QString::fromLatin1(type);
    it.mime         = QString::fromLatin1(prefix) + key;
    it.title        = title;
    it.subtitle     = subtitle;
    it.thumbnailUrl = art;
    return it;
}

} // namespace

// The "Music Servers" door (#193). Built here rather than inline so the two places that need it — the Music
// root and, one day, a search surface — cannot describe the same door differently.
static MediaItem musicServersDoor(int count)
{
    MediaItem it;
    it.id         = QString::fromLatin1(browse::kMusicServersPrefix);
    it.type       = QString::fromLatin1(browse::kMusicServersType);
    it.mime       = QString::fromLatin1(browse::kMusicServersPrefix);   // -> musicServersCatalog
    it.expandable = true;
    it.title      = QObject::tr("Music Servers");
    it.subtitle   = QObject::tr("%n server(s)", "", count);
    return it;
}

MediaCatalog musicServersCatalog(const QStringList& ids, const QStringList& names, const QStringList& urls)
{
    MediaCatalog cat; cat.title = QObject::tr("Music Servers");
    cat.hasMore = false;
    for (int i = 0; i < ids.size(); ++i)
    {
        MediaItem it;
        it.id         = QString::fromLatin1(kMusicServerPrefix) + ids.at(i);
        it.type       = QString::fromLatin1(kMusicServerType);
        it.mime       = QString::fromLatin1(kMusicServerPrefix) + ids.at(i);
        it.expandable = true;
        it.title      = i < names.size() && !names.at(i).trimmed().isEmpty() ? names.at(i)
                                                                             : QObject::tr("Music server");
        // The address, not the sign-in. A row that named the user would put an account name on a TV in a
        // living room to no purpose, and there is obviously no rendering of the password at all.
        it.subtitle   = i < urls.size() ? urls.at(i) : QString();
        cat.items.push_back(it);
    }
    // Always last, always present: with no servers saved this row is the whole level, which is what makes
    // the first one addable at all (the Playlists / Live TV / Book Servers rule).
    MediaItem add;
    add.id    = QString::fromLatin1(kMusicAddServerPrefix);
    add.type  = QString::fromLatin1(kMusicAddServerType);
    add.mime  = QString::fromLatin1(kMusicAddServerPrefix);
    add.title = QObject::tr("＋ Add a music server…");
    add.subtitle = QObject::tr("Navidrome, Airsonic, Gonic, Ampache…");
    cat.items.push_back(add);
    return cat;
}

MediaCatalog musicArtistsCatalog(const MusicLibrary::Index& idx, const MusicEmptyNote& note,
                                 const MusicCoverFn& cover, int musicServerCount)
{
    MediaCatalog cat; cat.title = QObject::tr("Music");
    cat.hasMore = false;

    if (idx.artists.isEmpty())
    {
        // A configured server is an answer to "there is nothing here", so the door comes FIRST and the
        // explanation — if the caller still has one — after it. Without this ordering a person whose whole
        // library is a Navidrome box would land on a sentence about choosing a folder with the thing they
        // actually want underneath it.
        if (musicServerCount > 0) cat.items.push_back(musicServersDoor(musicServerCount));
        // An empty shelf with no explanation is the failure this parameter exists to prevent: the user has
        // just pointed the app at a folder and wants to know what happened to it.
        if (!note.isEmpty())
        {
            MediaItem info;
            info.type     = QStringLiteral("info");   // the surface's own non-actionable guidance row
            info.id       = QStringLiteral("_musicempty");
            info.title    = note.text;
            info.subtitle = note.detail;
            cat.items.push_back(info);
        }
        return cat;
    }

    // "Shuffle all music", first: the plainest form of what a music library is for, and — until it existed —
    // the one thing this app could not do at all, because every queue it could build was one album. Offered
    // only when there is more than one track to shuffle. See the header for why there is no "play all" twin
    // at this level.
    if (idx.trackCount > 1)
    {
        // The first artist's first album cover, for the same reason an artist row borrows one: a row at the
        // top of the shelf should not be the only blank card on it.
        QString art;
        for (const MusicLibrary::Artist& a : idx.artists)
            if (!a.albums.isEmpty()) { art = coverFor(a.albums.first(), cover); break; }
        cat.items.push_back(actionRow(kMusicShuffleAllType, kMusicShuffleAllPrefix, QString(),
                                      QObject::tr("Shuffle all music"),
                                      joinDot({ QObject::tr("%n artist(s)", "", int(idx.artists.size())),
                                                QObject::tr("%n track(s)", "", idx.trackCount),
                                                QObject::tr("random order") }),
                                      art));
    }

    // The door to the classical view (#196, part 2), above the artists because it is a DIMENSION and they
    // are its contents. Offered only when the library holds a composer at all: for the very many people
    // whose tags carry no COMPOSER anywhere, this catalog is exactly what it was before this existed.
    if (!idx.composers.isEmpty())
    {
        int works = 0;
        for (const MusicLibrary::Composer& c : idx.composers) works += int(c.works.size());
        QString art;
        for (const MusicLibrary::Composer& c : idx.composers)
            if (!c.works.isEmpty())
                if (const MusicLibrary::Album* on = idx.album(c.works.first().albumKey))
                { art = coverFor(*on, cover); break; }
        MediaItem it;
        it.id         = QString::fromLatin1(kMusicComposersPrefix);
        it.type       = QString::fromLatin1(kMusicComposersType);
        it.mime       = QString::fromLatin1(kMusicComposersPrefix);   // -> musicComposersCatalog
        it.expandable = true;
        it.title      = QObject::tr("Composers");
        it.subtitle   = joinDot({ QObject::tr("%n composer(s)", "", int(idx.composers.size())),
                                  QObject::tr("%n work(s)", "", works) });
        it.thumbnailUrl = art;
        cat.items.push_back(it);
    }

    // The door to a user's own music SERVERS (#193), beside the Composers door and for the same reason:
    // both are DIMENSIONS over music and the artists below are contents. Offered only when at least one
    // server is configured, so an install with none gets this catalog exactly as it was.
    if (musicServerCount > 0) cat.items.push_back(musicServersDoor(musicServerCount));

    for (const MusicLibrary::Artist& a : idx.artists)
    {
        MediaItem it;
        it.id         = QString::fromLatin1(kMusicArtistPrefix) + a.key;
        it.type       = QString::fromLatin1(kMusicArtistType);
        it.mime       = QString::fromLatin1(kMusicArtistPrefix) + a.key;   // -> musicArtistCatalog
        it.expandable = true;
        it.title      = MusicLibrary::displayArtist(a);
        // Credits count towards the tracks (#196) — an artist who only ever appears as a co-credit has no
        // albums of their own, and "0 albums · 0 tracks" beside a row holding three songs is simply wrong.
        // Artist::albumCount rather than albums.size(), for the reason MusicLibrary.h gives: a remote
        // supplier knows the count at this level and fetches the albums themselves on drill.
        //
        // AND THE TRACK CLAUSE IS OMITTED WHEN THE COUNT IS ZERO. That cannot happen in a scanned library —
        // an artist bucket is minted BY a track, so an artist with no albums has credits and one with
        // albums has tracks, and either way the sum is at least one (probe_musicbrowse pins exactly that
        // over the real fixtures). It happens only for a remote artist list, where the server has told us
        // how many albums somebody has and nothing about their tracks, and "0 tracks" beside "12 albums"
        // would be a number this app invented.
        const int shownTracks = a.trackCount + int(a.credits.size());
        it.subtitle   = joinDot({ QObject::tr("%n album(s)", "", a.albumCount),
                                  shownTracks > 0 ? QObject::tr("%n track(s)", "", shownTracks) : QString() });
        // An artist has no artwork of their own here (that is a MusicBrainz job, which #74 defers): show the
        // first album's cover so the shelf is pictures rather than a grid of placeholders — falling back to
        // the album their first credit is on, for the same reason.
        if (!a.albums.isEmpty())
        {
            it.thumbnailUrl = coverFor(a.albums.first(), cover);
        }
        else if (!a.credits.isEmpty())
        {
            if (const MusicLibrary::Album* on = idx.album(a.credits.first().albumKey))
                it.thumbnailUrl = coverFor(*on, cover);
        }
        cat.items.push_back(it);
    }
    return cat;
}

MediaCatalog musicArtistCatalog(const MusicLibrary::Index& idx, const QString& artistKey,
                                const MusicCoverFn& cover)
{
    const MusicLibrary::Artist* a = idx.artist(artistKey);
    MediaCatalog cat;
    cat.title   = a ? MusicLibrary::displayArtist(*a) : QObject::tr("Music");
    cat.hasMore = false;
    if (!a) return cat;   // a stale route (the library was rescanned under us) is empty, never a crash

    // The two multi-album verbs, above the discography. An artist with a single track has nothing to order
    // and nothing to shuffle, so both rows are withheld rather than shown as no-ops. The count and the total
    // length are summed from the albums rather than read off Artist::trackCount, so the subtitle can never
    // disagree with the queue the row actually builds.
    if (a->trackCount > 1)
    {
        int tracks = 0, secs = 0;
        for (const MusicLibrary::Album& b : a->albums) { tracks += int(b.tracks.size()); secs += b.durationSec; }
        const QString art = a->albums.isEmpty() ? QString() : coverFor(a->albums.first(), cover);
        cat.items.push_back(actionRow(kMusicPlayArtistType, kMusicPlayArtistPrefix, artistKey,
                                      QObject::tr("Play all"),
                                      joinDot({ QObject::tr("%n album(s)", "", int(a->albums.size())),
                                                QObject::tr("%n track(s)", "", tracks),
                                                fmtDuration(secs) }),
                                      art));
        cat.items.push_back(actionRow(kMusicShuffleArtistType, kMusicShuffleArtistPrefix, artistKey,
                                      QObject::tr("Shuffle all"),
                                      joinDot({ QObject::tr("%n track(s)", "", tracks),
                                                QObject::tr("random order") }),
                                      art));
    }

    for (const MusicLibrary::Album& b : a->albums) cat.items.push_back(albumRow(b, cover));

    // Then the CREDITS (issue #196): tracks that name this artist but sit on a record filed under somebody
    // else — the multi-artist track that used to be reachable from neither of its artists. Rendered as plain
    // track rows below the discography, subtitled with the album they are on, because that is the one fact a
    // track pulled out of its album no longer says for itself.
    //
    // They are not folded into the Play all / Shuffle all rows above. Those queue this artist's DISCOGRAPHY,
    // and the subtitle is summed from the same albums the queue is built from precisely so the two can never
    // disagree; a credit belongs to another artist's album and pressing it plays THAT album, starting there.
    for (const MusicLibrary::IndexTrack& t : a->credits)
    {
        const MusicLibrary::Album* on = idx.album(t.albumKey);
        cat.items.push_back(trackRow(t, t.albumKey, on ? coverFor(*on, cover) : QString(), t.title,
                                     joinDot({ on ? MusicLibrary::displayAlbum(*on) : QString(),
                                               fmtDuration(t.durationSec) })));
    }
    return cat;
}

MediaCatalog musicAlbumCatalog(const MusicLibrary::Index& idx, const QString& albumKey,
                               const MusicCoverFn& cover)
{
    const MusicLibrary::Album* b = idx.album(albumKey);
    MediaCatalog cat;
    cat.title   = b ? MusicLibrary::displayAlbum(*b) : QObject::tr("Music");
    cat.hasMore = false;
    if (!b) return cat;

    const QString art = coverFor(*b, cover);

    // The action row, first: "play the whole thing" is what an album page is mostly for.
    {
        MediaItem it;
        it.id           = QString::fromLatin1(kMusicPlayAlbumPrefix) + b->key;
        it.type         = QString::fromLatin1(kMusicPlayAlbumType);
        it.mime         = QString::fromLatin1(kMusicPlayAlbumPrefix) + b->key;
        it.title        = QObject::tr("Play album");
        it.subtitle     = joinDot({ QObject::tr("%n track(s)", "", b->trackCount),
                                    fmtDuration(b->durationSec) });
        it.thumbnailUrl = art;
        cat.items.push_back(it);                 // no url: the surface routes it by mime, not as a file
    }

    for (const MusicLibrary::IndexTrack& t : b->tracks)
    {
        // The number, so a track list reads as one. On a multi-disc set the bare track number repeats across
        // discs, so it is qualified — "2-3." is the third track of the second disc.
        QString num;
        if (t.track > 0)
            num = (b->discCount > 1 && t.disc > 0) ? QStringLiteral("%1-%2. ").arg(t.disc).arg(t.track)
                                                   : QStringLiteral("%1. ").arg(t.track);
        // The track artist ONLY when it differs from the album's: on a compilation that is every row and is
        // the information the list is for; on an ordinary album it would be the same name eleven times.
        const bool differs = !t.artist.isEmpty()
                          && t.artist.trimmed().toCaseFolded() != b->albumArtist.trimmed().toCaseFolded();
        cat.items.push_back(trackRow(t, b->key, art, num + t.title,
                                     joinDot({ differs ? t.artist : QString(), fmtDuration(t.durationSec) })));
    }
    return cat;
}

// ---- The classical view (issue #196, part 2) -------------------------------------------------------------

MediaCatalog musicComposersCatalog(const MusicLibrary::Index& idx, const MusicCoverFn& cover)
{
    MediaCatalog cat; cat.title = QObject::tr("Composers");
    cat.hasMore = false;

    for (const MusicLibrary::Composer& c : idx.composers)
    {
        MediaItem it;
        it.id         = QString::fromLatin1(kMusicComposerPrefix) + c.key;
        it.type       = QString::fromLatin1(kMusicComposerType);
        it.mime       = QString::fromLatin1(kMusicComposerPrefix) + c.key;   // -> musicComposerCatalog
        it.expandable = true;
        it.title      = c.name;
        it.subtitle   = joinDot({ QObject::tr("%n work(s)", "", int(c.works.size())),
                                  QObject::tr("%n track(s)", "", c.trackCount) });
        // A composer has no portrait of their own here (that is the same MusicBrainz job an artist's picture
        // is, which #74 defers), so a row borrows the cover of the record its first work is on.
        if (!c.works.isEmpty())
            if (const MusicLibrary::Album* on = idx.album(c.works.first().albumKey))
                it.thumbnailUrl = coverFor(*on, cover);
        cat.items.push_back(it);
    }
    return cat;
}

MediaCatalog musicComposerCatalog(const MusicLibrary::Index& idx, const QString& composerKey,
                                  const MusicCoverFn& cover)
{
    const MusicLibrary::Composer* c = idx.composer(composerKey);
    MediaCatalog cat;
    cat.title   = c ? c->name : QObject::tr("Composers");
    cat.hasMore = false;
    if (!c) return cat;   // a stale route (the library was rescanned under us) is empty, never a crash

    for (const MusicLibrary::ComposerWork& w : c->works)
    {
        const MusicLibrary::Album* on = idx.album(w.albumKey);
        MediaItem it;
        it.id         = QString::fromLatin1(kMusicWorkPrefix) + w.key;
        it.type       = QString::fromLatin1(kMusicWorkType);
        it.mime       = QString::fromLatin1(kMusicWorkPrefix) + w.key;       // -> musicWorkCatalog
        it.expandable = true;
        // A work titled by a WORK tag says so for itself. One that borrowed its ALBUM's title needs the
        // fallback wording, because an untagged album has no title to borrow.
        it.title      = w.fromWork ? w.title
                                   : (on ? MusicLibrary::displayAlbum(*on) : QObject::tr("Unknown Album"));
        // WHO IS PLAYING, first: with four recordings of the same symphony on a shelf, the performers are
        // the only thing that distinguishes the rows, and the track count is not.
        it.subtitle   = joinDot({ w.performers.join(QStringLiteral("; ")),
                                  QObject::tr("%n track(s)", "", int(w.tracks.size())),
                                  fmtDuration(w.durationSec) });
        if (on) it.thumbnailUrl = coverFor(*on, cover);
        cat.items.push_back(it);
    }
    return cat;
}

MediaCatalog musicWorkCatalog(const MusicLibrary::Index& idx, const QString& workKey,
                              const MusicCoverFn& cover)
{
    const MusicLibrary::ComposerWork* w = idx.work(workKey);
    MediaCatalog cat;
    cat.title   = w ? w->title : QObject::tr("Composers");
    cat.hasMore = false;
    if (!w) return cat;

    const MusicLibrary::Album* on = idx.album(w->albumKey);
    const QString art = on ? coverFor(*on, cover) : QString();
    if (cat.title.isEmpty() && on) cat.title = MusicLibrary::displayAlbum(*on);

    // THERE IS NO "PLAY WORK" ACTION ROW, deliberately. A track row here already plays the album it is on
    // starting at that track (the routing contract at the top of the header), so a movement pressed halfway
    // down a work runs on into the rest of it. A "play work" verb would have to queue the movements ALONE,
    // which is a second kind of music queue — MusicQueue's whole subject — and #196 asks for a view over the
    // library, not another player. The album's own "Play album" row is one level away either way.
    for (const MusicLibrary::IndexTrack& t : w->tracks)
    {
        // The MOVEMENT is the title where a file names one, because inside a work that is the useful name —
        // "Variatio 1 a 1 Clav." rather than the track title repeating the piece for the twelfth time. The
        // number still leads, so the list reads as an ordered piece of music.
        const QString num  = t.track > 0 ? QStringLiteral("%1. ").arg(t.track) : QString();
        const QString name = t.movement.isEmpty() ? t.title : t.movement;
        // What the row's own context leaves unsaid: who plays it (or the conductor, or failing both the
        // track artist), and — when the work borrowed its title from one album — nothing about the album,
        // because it is the one directly above.
        QStringList who = t.performers + t.conductors;
        if (who.isEmpty() && !t.artist.isEmpty()) who << t.artist;
        cat.items.push_back(trackRow(t, t.albumKey, art, num + name,
                                     joinDot({ who.join(QStringLiteral("; ")), fmtDuration(t.durationSec) })));
    }
    return cat;
}

} // namespace browse
