#include "MusicLibrary.h"
#include "AppPaths.h"
#include "Settings.h"
#include "../media/CueSheet.h"   // the ONE cue parser; a single-file rip's track list comes from there

#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <algorithm>
#include <limits>

namespace MusicLibrary
{
namespace
{
    // The grouping keys join two or three fields into one string. UNIT SEPARATOR is the joiner because it is
    // the one byte a tag value or a path cannot contain — a '/' or a '|' would let "A|B" + "C" collide with
    // "A" + "B|C", which is a silent two-albums-become-one bug that only shows up on somebody's real library.
    const QChar kSep = QChar(0x1F);

    // A natural (numeric-aware, case-insensitive) collator, built once — the same one PhotoLibrary uses, for
    // the same reason: "Track 2" must sort before "Track 10", and an album whose files are numbered without
    // zero padding is the common case, not the exotic one.
    const QCollator& naturalCollator()
    {
        static QCollator coll = [] {
            QCollator c;
            c.setNumericMode(true);
            c.setCaseSensitivity(Qt::CaseInsensitive);
            return c;
        }();
        return coll;
    }

    // Case- and whitespace-insensitive grouping. "Various Artists", "various artists" and "Various Artists "
    // are ONE artist: taggers disagree about capitalisation across a library assembled over a decade, and a
    // browse that shows the same name three times is the compilation bug wearing a different hat. The first
    // spelling encountered is what gets displayed; only the key is folded.
    QString foldKey(const QString& s) { return s.trimmed().toCaseFolded(); }

    // Ordering rank for a disc number. 0 means "untagged", and an untagged disc is disc 1 — otherwise a
    // single-disc album whose files carry no TPOS would sort its tracks into a phantom disc 0 ahead of a
    // bonus track that happens to say "disc 1".
    int discRank(int disc) { return disc > 0 ? disc : 1; }

    // Ordering rank for a track number. 0 means "untagged", and an untagged track sorts AFTER every numbered
    // one: an album of twelve numbered tracks plus one untagged bonus reads correctly with the bonus at the
    // end, and reads as broken with it at the top. When every track in a group is untagged they all rank the
    // same and the filename tiebreak below decides, which is the "fall back to the filename" rule.
    int trackRank(int track) { return track > 0 ? track : std::numeric_limits<int>::max(); }

    // ReplayGain round-trips as "the key is there or it is not", because 0.00 dB is a real value and a
    // written-out zero must not be indistinguishable from an absent tag (AudioTags::GainValue says why).
    void writeGain(QJsonObject& o, const QString& key, const AudioTags::GainValue& g)
    {
        if (g.present) o.insert(key, g.value);
    }

    AudioTags::GainValue readGain(const QJsonObject& o, const QString& key)
    {
        AudioTags::GainValue g;
        const QJsonValue v = o.value(key);
        if (v.isDouble()) { g.present = true; g.value = v.toDouble(); }
        return g;
    }

    // A multi-value list round-trips ONLY when it holds more than one value (issue #196). One value is
    // already the display string beside it, and writing it twice would grow every entry in every ordinary
    // library to record something it already says. The read below reconstructs the single case from that
    // string, so the two halves cannot disagree about what "absent" meant.
    void writeList(QJsonObject& o, const QString& key, const QStringList& values)
    {
        if (values.size() < 2) return;
        QJsonArray a;
        for (const QString& v : values) a.append(v);
        o.insert(key, a);
    }

    QStringList readList(const QJsonObject& o, const QString& key, const QString& single)
    {
        const QJsonValue v = o.value(key);
        if (v.isArray())
        {
            QStringList out;
            for (const QJsonValue& e : v.toArray())
                if (!e.toString().isEmpty()) out << e.toString();
            if (!out.isEmpty()) return out;
        }
        return single.isEmpty() ? QStringList{} : QStringList{ single };
    }

    // One scanned entry -> the browse-facing track. ONE builder, because the same track is now emitted twice:
    // into its album, and into every co-credited artist's `credits` (issue #196). Two builders would drift,
    // and the drift would be a credit row whose title or album key disagreed with the album's own copy.
    IndexTrack indexTrackFor(const TrackEntry& e, const QString& albumKey)
    {
        IndexTrack t;
        t.path        = e.path;
        t.sourcePath  = e.path;   // the same string for an ordinary track; the cue expansion below moves `path`
        // Filename fallback for the title. completeBaseName() keeps "Track 2.part1" intact and drops only
        // the final extension, which is what a person reading the folder would call the file.
        t.title       = e.title.trimmed().isEmpty() ? QFileInfo(e.path).completeBaseName() : e.title.trimmed();
        t.artist      = e.artist.trimmed();
        t.albumKey    = albumKey;
        t.genres      = e.genres;
        t.composers   = e.composers;
        t.conductors  = e.conductors;
        t.performers  = e.performers;
        t.work        = e.work.trimmed();
        t.movement    = e.movement.trimmed();
        t.disc        = e.disc;
        t.track       = e.track;
        t.durationSec = e.durationSec;
        t.hasCover    = e.hasCover;
        return t;
    }

    // One scanned entry -> the browse-facing tracks, PLURAL (issue #196, part 3). Exactly one for every
    // ordinary file — the vector is the same row indexTrackFor built and nothing downstream can tell — and
    // one per cue track for a single-file rip. Every caller that used to build a row builds rows through
    // here instead, because a cue album has to expand in all three of them (its album, a co-credited
    // artist's list, and a composer's work) or the same record would have five tracks in one place and one
    // in another.
    //
    // THE ONE FIELD THAT MAKES A CUE TRACK PLAYABLE IS `path`. It becomes the mpv EDL clip url for this
    // track's span of the shared file, which is what lets the ordinary queue hold five distinct entries for
    // one file and start at the third; `sourcePath` keeps the real file for whoever needs bytes. See
    // MusicLibrary.h and CueSheet::mpvClipUrl.
    QVector<IndexTrack> indexTracksFor(const TrackEntry& e, const QString& albumKey)
    {
        const IndexTrack base = indexTrackFor(e, albumKey);
        if (e.cueTracks.isEmpty())
            return { base };

        QVector<IndexTrack> out;
        out.reserve(e.cueTracks.size());
        for (const CueTrack& c : e.cueTracks)
        {
            IndexTrack t = base;
            t.path  = CueSheet::mpvClipUrl(e.path, c.startMs, c.endMs);
            // A sheet is allowed to leave a track untitled. The FILE's title is the wrong fallback here —
            // it would name every track after the album — so a numbered one is used instead.
            t.title = c.title.trimmed().isEmpty() ? QObject::tr("Track %1").arg(c.number) : c.title.trimmed();
            if (!c.artist.trimmed().isEmpty()) t.artist = c.artist.trimmed();
            t.track = c.number;
            // The track's own length: the span when the sheet closed it, and what is left of the file when
            // it did not (the last track). Rounded to the nearest second, like every other duration here.
            t.durationSec = (c.endMs > c.startMs)
                              ? (c.endMs - c.startMs + 500) / 1000
                              : std::max(0, e.durationSec - c.startMs / 1000);
            out.push_back(t);
        }
        return out;
    }
}

bool isAudioFile(const QString& path) { return AudioTags::isSupportedFile(path); }

QString artistKeyFor(const TrackEntry& e) { return foldKey(e.effectiveAlbumArtist()); }

QString albumKeyFor(const TrackEntry& e)
{
    // (album artist, album title) — the artist half is what keeps two different bands' self-titled albums
    // apart, and the album half is what keeps a two-disc set together (the disc number is not in the key).
    //
    // With no album tag the second half becomes the containing FOLDER PATH, not the folder NAME: two "CD1"
    // folders under two different rips must not merge. It is tagged with a 'd'/'t' discriminator so a folder
    // path can never be mistaken for an album literally named after it.
    const QString artist = artistKeyFor(e);
    if (!e.album.trimmed().isEmpty())
        return artist + kSep + QLatin1String("t") + kSep + foldKey(e.album);
    return artist + kSep + QLatin1String("d") + kSep + foldKey(QFileInfo(e.path).absolutePath());
}

QVector<TrackEntry> scanFolder(const QString& root, const QHash<QString, TrackEntry>& known, ScanStats* stats,
                               const QStringList& separators)
{
    QVector<TrackEntry> out;
    ScanStats s;
    if (root.isEmpty() || !QFileInfo::exists(root))
    {
        // Nothing configured, or the folder went away with the drive it was on. Dormant, instant, and NOT a
        // reason to forget what we knew: `known` is left alone, so plugging the drive back in re-uses it.
        if (stats) *stats = s;
        return out;
    }

    // ONE walk, two kinds of file. The audio files are collected rather than processed in place because a
    // cue sidecar can be enumerated AFTER the album it describes, and a file's cue is half of its cache key
    // (see below) — so every folder's sheets have to be known before the first reuse decision is made. The
    // cost of that is one small struct per audio file; the alternative is a second directory walk, which is
    // strictly more work on the very libraries this must not slow down.
    struct Walked { QString abs, folder; qint64 mtime = 0, size = 0; };
    QVector<Walked> found;
    QHash<QString, QStringList> cuesByFolder;              // folder -> its .cue files, sorted
    QHash<QString, QPair<qint64, qint64>> cueStat;         // cue path -> (mtime, size), free from the walk

    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        const QFileInfo fi = it.fileInfo();
        if (fi.suffix().compare(QLatin1String("cue"), Qt::CaseInsensitive) == 0)
        {
            // Noted, not parsed. A folder with no cue in it never reaches CueSheet at all (#196 part 3).
            const QString abs = fi.absoluteFilePath();
            cuesByFolder[fi.absolutePath()] << abs;
            cueStat.insert(abs, qMakePair(fi.lastModified().toSecsSinceEpoch(), fi.size()));
            continue;
        }
        if (!isAudioFile(fi.filePath())) continue;   // extension-only, before anything is opened
        found.push_back({ fi.absoluteFilePath(), fi.absolutePath(),
                          fi.lastModified().toSecsSinceEpoch(), fi.size() });
    }
    // Deterministic sidecar choice when a folder holds several: the walk's order is the filesystem's.
    for (QStringList& l : cuesByFolder) l.sort();

    // Parsed at most once per sheet, however many audio files sit beside it.
    QHash<QString, CueSheet::Sheet> sheets;
    auto sheetFor = [&sheets](const QString& cuePath) -> const CueSheet::Sheet& {
        auto at = sheets.find(cuePath);
        if (at == sheets.end()) at = sheets.insert(cuePath, CueSheet::load(cuePath));
        return *at;
    };

    for (const Walked& w : found)
    {
        const QString& abs = w.abs;
        const qint64 mtime = w.mtime;
        const qint64 size  = w.size;
        ++s.files;

        // WHICH SHEET, IF ANY, DESCRIBES THIS FILE. The base name wins outright — `Album.cue` beside
        // `Album.flac` is what a single-file rip looks like, and it wins even when the sheet's own FILE line
        // still names the .wav it was transcoded from, which is the commonest inconsistency there is.
        // Otherwise a sheet is only claimed if it NAMES this file, so a cue pointing at something that is
        // not on the disk quietly describes nothing rather than being attached to a neighbour.
        QString cuePath;
        qint64  cueMtime = 0, cueSize = 0;
        const auto folderCues = cuesByFolder.constFind(w.folder);
        if (folderCues != cuesByFolder.constEnd())
        {
            const QFileInfo afi(abs);
            for (const QString& c : *folderCues)
                if (QFileInfo(c).completeBaseName().compare(afi.completeBaseName(), Qt::CaseInsensitive) == 0)
                    { cuePath = c; break; }
            if (cuePath.isEmpty())
                for (const QString& c : *folderCues)
                    if (CueSheet::namesAudioFile(sheetFor(c), afi.fileName())) { cuePath = c; break; }
            if (!cuePath.isEmpty())
            {
                const QPair<qint64, qint64> st = cueStat.value(cuePath);
                cueMtime = st.first; cueSize = st.second;
            }
        }

        // THE INCREMENTAL DECISION, and the only one. Same path, same mtime, same size => the bytes we
        // already parsed are still the bytes on disk, so the file is not opened at all. Size is checked as
        // well as mtime because a tag editor that rewrites a file can preserve the timestamp (and archives
        // restored from backup routinely do), while almost nothing preserves the length too.
        //
        // THE SIDECAR IS PART OF THE SAME QUESTION (#196 part 3): editing, adding or removing an `Album.cue`
        // changes what a read of this file should produce and touches neither its mtime nor its size, so its
        // identity is compared too. All three cue fields are zero/empty for a file with no sheet, which is
        // every file in an ordinary library — so this clause is exactly the old one for them.
        const auto cached = known.constFind(abs);
        if (cached != known.constEnd() && cached->mtime == mtime && cached->size == size
            && cached->cuePath == cuePath && cached->cueMtime == cueMtime && cached->cueSize == cueSize)
        {
            ++s.reused;
            out.push_back(*cached);
            continue;
        }

        const AudioTags::Tags t = AudioTags::read(abs, separators);
        ++s.retagged;

        TrackEntry e;
        e.path = abs; e.mtime = mtime; e.size = size;
        e.title = t.title; e.artist = t.artist; e.albumArtist = t.albumArtist;
        e.album = t.album; e.genre = t.genre;
        e.artists = t.artists; e.genres = t.genres;
        // The classical fields (#196, part 2) — out of the same read, into the same entry.
        e.composer = t.composer; e.conductor = t.conductor; e.performer = t.performer;
        e.composers = t.composers; e.conductors = t.conductors; e.performers = t.performers;
        e.work = t.work; e.movement = t.movement;
        e.track = t.track; e.trackTotal = t.trackTotal;
        e.disc = t.disc;   e.discTotal = t.discTotal;
        e.year = t.year;   e.durationSec = t.durationSec;
        e.hasCover = !t.cover.isNull();
        e.trackGain = t.trackGain; e.albumGain = t.albumGain;
        e.trackPeak = t.trackPeak; e.albumPeak = t.albumPeak;
        e.untagged = t.isEmpty();

        // THE CUE EXPANSION (#196 part 3). The SIDECAR wins over an embedded CUESHEET tag when both exist:
        // the tag is baked into the file and the sidecar is the thing a person can fix, so the one they
        // edited has to be the one that counts. Everything about whether a sheet is even relevant — invalid,
        // multi-FILE, single-track — is CueSheet::singleFileSegments' answer, not a second rule here.
        e.cuePath = cuePath; e.cueMtime = cueMtime; e.cueSize = cueSize;
        const CueSheet::Sheet sheet = cuePath.isEmpty() ? (t.cuesheet.isEmpty() ? CueSheet::Sheet{}
                                                                                : CueSheet::parse(t.cuesheet))
                                                        : sheetFor(cuePath);
        for (const CueSheet::Segment& g : CueSheet::singleFileSegments(sheet))
        {
            CueTrack c;
            c.number  = g.number;
            c.title   = g.title;
            c.artist  = g.performer;
            c.startMs = g.startMs;
            c.endMs   = g.endMs;
            e.cueTracks.push_back(c);
        }

        // THE SHEET FILLS IN WHAT THE FILE DID NOT SAY, and only that. A great many single-file rips are one
        // enormous UNTAGGED wav whose entire metadata is the .cue beside it — for those the sheet's TITLE and
        // PERFORMER are the album and the album artist, and without this the record would be filed under
        // "Unknown Artist" and named after its folder while a file two inches away spells both out. TAGS WIN
        // wherever the file carries one: a tag is what the person who tagged the file meant, and a sheet
        // written by the ripper is what the ripper guessed. Nothing here runs for a file with no cue.
        if (!e.cueTracks.isEmpty())
        {
            if (e.album.isEmpty())       e.album       = sheet.title.trimmed();
            if (e.albumArtist.isEmpty()) e.albumArtist = sheet.performer.trimmed();
            if (e.artist.isEmpty())
            {
                e.artist = sheet.performer.trimmed();
                // Kept in step with the display string, exactly as AudioTags does: `artists` is what the
                // grouping and the credit index read, and a pair that disagreed would file the album under
                // one spelling and index it under another.
                e.artists = e.artist.isEmpty() ? QStringList{} : QStringList{ e.artist };
            }
            if (e.genre.isEmpty() && !sheet.genre.trimmed().isEmpty())
            {
                e.genre  = sheet.genre.trimmed();
                e.genres = QStringList{ e.genre };
            }
            if (e.year == 0) e.year = sheet.year;
        }
        out.push_back(e);
    }

    // Everything `known` held that the walk did not find is gone from the disk, and therefore gone from the
    // library — the scan is authoritative about what exists. Counted rather than acted on: the caller's next
    // save writes `out`, which already omits them.
    int kept = 0;
    for (const TrackEntry& e : out)
        if (known.contains(e.path)) ++kept;
    s.dropped = int(known.size()) - kept;
    if (s.dropped < 0) s.dropped = 0;   // a `known` with paths outside this root is the caller's business

    if (stats) *stats = s;
    return out;
}

QHash<QString, TrackEntry> byPath(const QVector<TrackEntry>& entries)
{
    QHash<QString, TrackEntry> out;
    out.reserve(entries.size());
    for (const TrackEntry& e : entries) out.insert(e.path, e);
    return out;
}

Index buildIndex(const QVector<TrackEntry>& entries)
{
    // Sort the input by natural path order FIRST, so everything decided by "first one seen" — an artist's
    // display capitalisation, an album's folder, its year — is a property of the library rather than of
    // whatever order QDirIterator happened to hand back on this filesystem. Two runs must build the same
    // index from the same disk.
    QVector<TrackEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const TrackEntry& a, const TrackEntry& b) {
        return naturalCollator().compare(a.path, b.path) < 0;
    });

    Index idx;
    QHash<QString, int> artistAt;                 // artist key   -> position in idx.artists
    QHash<QString, QPair<int, int>> albumAt;      // album  key   -> (artist position, album position)
    QHash<QString, int> composerAt;               // composer key -> position in idx.composers (#196 part 2)
    QHash<QString, int> workAt;                   // work key     -> position in THAT composer's works; the
                                                  // key contains the composer key, so one hash is enough

    for (const TrackEntry& e : sorted)
    {
        const QString aKey = artistKeyFor(e);
        const QString bKey = albumKeyFor(e);

        int ai = artistAt.value(aKey, -1);
        if (ai < 0)
        {
            Artist a;
            a.key  = aKey;
            a.name = e.effectiveAlbumArtist().trimmed();   // display spelling: the first one seen
            ai = idx.artists.size();
            idx.artists.push_back(a);
            artistAt.insert(aKey, ai);
        }

        const QFileInfo fi(e.path);
        // The album key already contains the artist key, so a hit here always belongs to `ai` — but the
        // position is checked anyway, because if that ever stopped being true the alternative is indexing
        // one artist's album vector with another artist's position, which is not a wrong answer, it is
        // undefined behaviour.
        const auto bIt = albumAt.constFind(bKey);
        int bi = (bIt != albumAt.constEnd() && bIt->first == ai) ? bIt->second : -1;
        if (bi < 0)
        {
            Album b;
            b.key    = bKey;
            b.albumArtist = idx.artists[ai].name;
            b.folder = fi.absolutePath();
            if (!e.album.trimmed().isEmpty())
            {
                b.title = e.album.trimmed();
            }
            else
            {
                // Untagged album: named after its directory (see the header's "where untagged files go").
                // QDir::dirName() of a drive root is empty, and the title stays empty in that case rather
                // than being invented — displayAlbum() then says "Unknown Album", which is honest.
                b.title = QDir(b.folder).dirName();
                b.titleFromFolder = true;
            }
            bi = idx.artists[ai].albums.size();
            idx.artists[ai].albums.push_back(b);
            albumAt.insert(bKey, qMakePair(ai, bi));
        }

        Album& alb = idx.artists[ai].albums[bi];
        if (alb.year == 0 && e.year > 0) alb.year = e.year;      // earliest non-zero, by path order
        alb.discCount   = std::max(alb.discCount, discRank(e.disc));
        alb.durationSec += e.durationSec;

        // One row for an ordinary file; one per cue track for a single-file rip (#196 part 3). The COUNTS
        // follow the rows rather than the files, because a person browsing a cue album is looking at twelve
        // tracks and a subtitle that said "1 track" would be describing the disk instead of the record.
        // The album's DURATION does not: it is the file's, which is already the sum of its cue tracks.
        const QVector<IndexTrack> rows = indexTracksFor(e, bKey);
        alb.tracks += rows;

        idx.artists[ai].trackCount += int(rows.size());
        idx.trackCount += int(rows.size());
    }

    // ---- The credits (issue #196) ------------------------------------------------------------------------
    // A SECOND pass, after every album exists, because a credit is defined against the album's own artist key
    // and rebuilding the track here is cheaper than carrying indices through a vector that is about to be
    // sorted. Path order is preserved, so an artist's co-credits read in library order and two runs agree.
    //
    // Only a track with MORE THAN ONE artist mints one — the header says why in full, and it is the line that
    // keeps a library with no multi-value tags byte-identical to what it built before this existed.
    for (const TrackEntry& e : sorted)
    {
        if (e.artists.size() < 2)
            continue;
        const QString aKey = artistKeyFor(e);   // the artist the ALBUM is filed under; not a credit
        const QString bKey = albumKeyFor(e);
        for (const QString& credited : e.artists)
        {
            const QString cKey = foldKey(credited);
            if (cKey.isEmpty() || cKey == aKey)
                continue;
            int ci = artistAt.value(cKey, -1);
            if (ci < 0)
            {
                Artist c;
                c.key  = cKey;
                c.name = credited.trimmed();    // display spelling: the first one seen, as everywhere else
                ci = idx.artists.size();
                idx.artists.push_back(c);
                artistAt.insert(cKey, ci);
            }
            idx.artists[ci].credits += indexTracksFor(e, bKey);
        }
    }

    // ---- The classical view (issue #196, part 2) ---------------------------------------------------------
    // A THIRD pass, for the same reason the credits are a second one: every album exists by now, so a
    // composer's tracks can carry the album key that routes them home. Path order again, so a work's
    // movements arrive in library order before the disc/track sort below refines them.
    //
    // THE GATE IS THE FIRST LINE: a track with no COMPOSER tag mints nothing at all. That is what makes this
    // increment invisible to the libraries that have no classical music in them — no composer, no bucket, no
    // extra row anywhere, and an index that compares equal to the one built before this code existed.
    for (const TrackEntry& e : sorted)
    {
        if (e.composers.isEmpty())
            continue;
        const QString bKey = albumKeyFor(e);
        for (const QString& composed : e.composers)
        {
            const QString cKey = foldKey(composed);
            if (cKey.isEmpty())
                continue;
            int ci = composerAt.value(cKey, -1);
            if (ci < 0)
            {
                Composer c;
                c.key  = cKey;
                c.name = composed.trimmed();   // display spelling: the first one seen, as everywhere else
                ci = idx.composers.size();
                idx.composers.push_back(c);
                composerAt.insert(cKey, ci);
            }

            // WHICH WORK. The WORK tag when the file carries one, and the ALBUM when it does not — the issue
            // asks a composer to list "the works/albums they wrote", and those are the two things a file can
            // actually tell us.
            //
            // THE KEY CARRIES THE ALBUM as well as the composer and the title, so a row is ONE RECORDING of
            // a piece rather than every recording of it at once. Two Goldberg Variations on one shelf are
            // two rows, told apart by who is playing — which is how a classical listener chooses between
            // them, and the only reading under which a work row can have one album's artwork, one album's
            // queue and one coherent movement order. Merging them would interleave two performances by
            // track number and leave the row pointing at whichever record was scanned first. The composer
            // key is in there too, so two composers sharing a disc get a row each rather than colliding.
            const QString wTitle = e.work.trimmed();
            const bool fromWork  = !wTitle.isEmpty();
            const QString wKey   = cKey + kSep + QLatin1String(fromWork ? "w" : "a") + kSep + bKey
                                 + (fromWork ? kSep + foldKey(wTitle) : QString());
            int wi = workAt.value(wKey, -1);
            if (wi < 0)
            {
                ComposerWork w;
                w.key      = wKey;
                w.albumKey = bKey;
                w.fromWork = fromWork;
                if (fromWork)
                {
                    w.title = wTitle;
                }
                else if (const Album* on = idx.album(bKey))
                {
                    w.title = on->title;      // may be empty: displayed as "Unknown Album", never invented here
                }
                wi = idx.composers[ci].works.size();
                idx.composers[ci].works.push_back(w);
                workAt.insert(wKey, wi);
            }

            ComposerWork& work = idx.composers[ci].works[wi];
            work.durationSec += e.durationSec;
            // A SYMPHONY RIPPED AS ONE FILE reaches this list as its movements rather than as one hour-long
            // row (#196 part 3) — the same expansion the album level does, from the same builder.
            const QVector<IndexTrack> rows = indexTracksFor(e, bKey);
            work.tracks += rows;
            // WHO IS PLAYING IT. Performers first, then conductors, then the track artist as the last
            // resort: a recording tagged with none of the classical credits still has to be told apart from
            // the other three recordings of the same piece, and the performing artist is what does it.
            QStringList heard = e.performers + e.conductors;
            if (heard.isEmpty() && !e.artist.trimmed().isEmpty()) heard << e.artist.trimmed();
            for (const QString& who : heard)
            {
                const QString t = who.trimmed();
                if (t.isEmpty()) continue;
                bool seen = false;
                for (const QString& have : work.performers)
                    if (have.compare(t, Qt::CaseInsensitive) == 0) { seen = true; break; }
                if (!seen) work.performers << t;
            }
            idx.composers[ci].trackCount += int(rows.size());
        }
    }

    for (Composer& c : idx.composers)
    {
        for (ComposerWork& w : c.works)
            std::sort(w.tracks.begin(), w.tracks.end(), [](const IndexTrack& x, const IndexTrack& y) {
                if (discRank(x.disc) != discRank(y.disc)) return discRank(x.disc) < discRank(y.disc);
                if (trackRank(x.track) != trackRank(y.track)) return trackRank(x.track) < trackRank(y.track);
                return naturalCollator().compare(x.path, y.path) < 0;
            });
        // Works alphabetically. NOT by year the way an artist's albums are: a work row is a piece of music,
        // and the date a library happens to hold is the date of the RECORDING, so ordering by it would file
        // Bach after Bartok on the strength of when somebody pressed the CD.
        std::sort(c.works.begin(), c.works.end(), [](const ComposerWork& x, const ComposerWork& y) {
            return naturalCollator().compare(x.title, y.title) < 0;
        });
    }
    std::sort(idx.composers.begin(), idx.composers.end(), [](const Composer& x, const Composer& y) {
        return naturalCollator().compare(x.name, y.name) < 0;
    });

    for (Artist& a : idx.artists)
    {
        for (Album& b : a.albums)
        {
            // disc, then track number, then the filename — the rule #74 asks for, with the two ranks above
            // deciding what an untagged 0 means at each level.
            std::sort(b.tracks.begin(), b.tracks.end(), [](const IndexTrack& x, const IndexTrack& y) {
                if (discRank(x.disc) != discRank(y.disc)) return discRank(x.disc) < discRank(y.disc);
                if (trackRank(x.track) != trackRank(y.track)) return trackRank(x.track) < trackRank(y.track);
                return naturalCollator().compare(x.path, y.path) < 0;
            });
        }
        // Albums oldest first, with an unknown year LAST — a release date we do not have should not claim
        // to be the artist's earliest record.
        std::sort(a.albums.begin(), a.albums.end(), [](const Album& x, const Album& y) {
            const int xy = x.year > 0 ? x.year : std::numeric_limits<int>::max();
            const int yy = y.year > 0 ? y.year : std::numeric_limits<int>::max();
            if (xy != yy) return xy < yy;
            return naturalCollator().compare(x.title, y.title) < 0;
        });
        idx.albumCount += int(a.albums.size());
    }

    // Artists alphabetically, with the UNKNOWN bucket last: a folder of untagged rips should not be the
    // first thing the browse shows, and an empty name would otherwise sort to the very top.
    std::sort(idx.artists.begin(), idx.artists.end(), [](const Artist& x, const Artist& y) {
        if (x.name.isEmpty() != y.name.isEmpty()) return y.name.isEmpty();
        return naturalCollator().compare(x.name, y.name) < 0;
    });
    return idx;
}

const Artist* Index::artist(const QString& artistKey) const
{
    for (const Artist& a : artists)
        if (a.key == artistKey) return &a;
    return nullptr;
}

const Album* Index::album(const QString& albumKey) const
{
    for (const Artist& a : artists)
        for (const Album& b : a.albums)
            if (b.key == albumKey) return &b;
    return nullptr;
}

const Composer* Index::composer(const QString& composerKey) const
{
    for (const Composer& c : composers)
        if (c.key == composerKey) return &c;
    return nullptr;
}

const ComposerWork* Index::work(const QString& workKey) const
{
    for (const Composer& c : composers)
        for (const ComposerWork& w : c.works)
            if (w.key == workKey) return &w;
    return nullptr;
}

QString displayArtist(const Artist& a)
{
    return a.name.isEmpty() ? QObject::tr("Unknown Artist") : a.name;
}

QString displayAlbum(const Album& a)
{
    return a.title.isEmpty() ? QObject::tr("Unknown Album") : a.title;
}

// ---------------------------------------------------------------------------------------------------------
// Persistence: { "version": 1, "rules": "2 ;", "tracks": [ { "p": …, "m": …, "s": …, … } ] }
//
// Short keys and omitted defaults, because this file has one entry per track and a big library has tens of
// thousands of them — spelling "albumArtist" out twenty thousand times costs more than the values do. The
// version field exists so a later increment can change the entry shape without mis-reading an old file as
// the new one; an unknown version loads as empty, which costs a full re-tag and nothing else.
//
// "rules" IS THE PARSE STAMP (issue #196), and it is here because the scan's whole speed trick is that an
// unchanged file is never re-opened. Anything that changes what a READ of an unchanged file would produce
// therefore has to invalidate the cache by hand, or it appears to do nothing until every file is edited.
// The caller compares this stamp with parseStamp(the list it is about to scan with) and drops the cache when
// they differ; the version number cannot express it, because the FILE shape did not change, the parsing
// rules did.
//
// TWO things live in the one stamp, and that is the point of it (see MusicLibrary.h):
//   * kTagRules — bumped whenever the READER learns a field. Part 2 of #196 taught it composer, conductor,
//     performer, work and movement, and every cached entry from before that is missing all five on files
//     whose mtime and size have not moved.
//   * the separator list, from part 1.
// Folding them together means one rescan for one settings change. A second condition beside this one would
// mean a library that walks itself twice, which is a worse outcome than either change alone.
//
// An index written before the stamp existed carries no "rules" key at all, reads as "", differs from every
// stamp, and therefore re-tags exactly once — which is exactly right.
// ---------------------------------------------------------------------------------------------------------
namespace
{
    const int kIndexFileVersion = 1;
    // Bump when AudioTags starts reading something new, or when the SCAN starts making something new of
    // what it reads. 1 == #196 part 1 (multi-value artist/genre), 2 == #196 part 2 (composer/conductor/
    // performer/work/movement), 3 == #196 part 3 (cue sheets: the embedded CUESHEET tag, and the sidecar
    // expansion — a cached entry from before this is a single-file rip still stored as one track).
    const int kTagRules = 3;
}

QString parseStamp(const QStringList& separators)
{
    return QString::number(kTagRules) + QChar(' ') + separators.join(QChar(' '));
}

QVector<TrackEntry> loadIndexFile(const QString& filePath, QString* rulesUsed)
{
    if (rulesUsed) rulesUsed->clear();
    QVector<TrackEntry> out;
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.value(QStringLiteral("version")).toInt() != kIndexFileVersion) return out;
    if (rulesUsed) *rulesUsed = root.value(QStringLiteral("rules")).toString();

    const QJsonArray tracks = root.value(QStringLiteral("tracks")).toArray();
    out.reserve(tracks.size());
    for (const QJsonValue& v : tracks)
    {
        const QJsonObject o = v.toObject();
        TrackEntry e;
        e.path = o.value(QStringLiteral("p")).toString();
        if (e.path.isEmpty()) continue;                  // an entry with no path can key nothing
        e.mtime = qint64(o.value(QStringLiteral("m")).toDouble());
        e.size  = qint64(o.value(QStringLiteral("s")).toDouble());
        e.title       = o.value(QStringLiteral("ti")).toString();
        e.artist      = o.value(QStringLiteral("ar")).toString();
        e.albumArtist = o.value(QStringLiteral("aa")).toString();
        e.album       = o.value(QStringLiteral("al")).toString();
        e.genre       = o.value(QStringLiteral("ge")).toString();
        e.track      = o.value(QStringLiteral("tn")).toInt();
        e.trackTotal = o.value(QStringLiteral("tt")).toInt();
        e.disc       = o.value(QStringLiteral("dn")).toInt();
        e.discTotal  = o.value(QStringLiteral("dt")).toInt();
        e.year       = o.value(QStringLiteral("yr")).toInt();
        e.durationSec = o.value(QStringLiteral("du")).toInt();
        e.hasCover = o.value(QStringLiteral("cv")).toBool();
        e.untagged = o.value(QStringLiteral("nt")).toBool();
        // The multi-value lists (#196), written only when they say more than the display string does. Absent
        // means single-valued, which is the whole library for most people — so the common entry is unchanged
        // in size, and an index written before this existed loads with the same meaning it always had.
        e.artists = readList(o, QStringLiteral("ars"), e.artist);
        e.genres  = readList(o, QStringLiteral("ges"), e.genre);
        // The classical fields (#196 part 2). Absent on every entry from an ordinary library, which is why
        // they are written only when present — an index of pop music is the same size it was.
        e.composer   = o.value(QStringLiteral("cm")).toString();
        e.conductor  = o.value(QStringLiteral("cd")).toString();
        e.performer  = o.value(QStringLiteral("pf")).toString();
        e.work       = o.value(QStringLiteral("wk")).toString();
        e.movement   = o.value(QStringLiteral("mv")).toString();
        e.composers  = readList(o, QStringLiteral("cms"), e.composer);
        e.conductors = readList(o, QStringLiteral("cds"), e.conductor);
        e.performers = readList(o, QStringLiteral("pfs"), e.performer);
        // The cue album (#196 part 3). Absent from every ordinary entry, so an index of files that are just
        // files is byte-for-byte the file it always was.
        e.cuePath  = o.value(QStringLiteral("cp")).toString();
        e.cueMtime = qint64(o.value(QStringLiteral("cmt")).toDouble());
        e.cueSize  = qint64(o.value(QStringLiteral("csz")).toDouble());
        for (const QJsonValue& cv : o.value(QStringLiteral("cue")).toArray())
        {
            const QJsonObject co = cv.toObject();
            CueTrack c;
            c.number  = co.value(QStringLiteral("n")).toInt();
            c.title   = co.value(QStringLiteral("t")).toString();
            c.artist  = co.value(QStringLiteral("a")).toString();
            c.startMs = co.value(QStringLiteral("s")).toInt();
            // "e" absent means the open-ended last track, which is what -1 means everywhere else here.
            c.endMs   = co.contains(QStringLiteral("e")) ? co.value(QStringLiteral("e")).toInt() : -1;
            e.cueTracks.push_back(c);
        }
        e.trackGain = readGain(o, QStringLiteral("rgtg"));
        e.albumGain = readGain(o, QStringLiteral("rgag"));
        e.trackPeak = readGain(o, QStringLiteral("rgtp"));
        e.albumPeak = readGain(o, QStringLiteral("rgap"));
        out.push_back(e);
    }
    return out;
}

bool saveIndexFile(const QString& filePath, const QVector<TrackEntry>& entries, const QStringList& separators)
{
    QJsonArray tracks;
    for (const TrackEntry& e : entries)
    {
        QJsonObject o;
        o.insert(QStringLiteral("p"), e.path);
        o.insert(QStringLiteral("m"), double(e.mtime));
        o.insert(QStringLiteral("s"), double(e.size));
        if (!e.title.isEmpty())       o.insert(QStringLiteral("ti"), e.title);
        if (!e.artist.isEmpty())      o.insert(QStringLiteral("ar"), e.artist);
        if (!e.albumArtist.isEmpty()) o.insert(QStringLiteral("aa"), e.albumArtist);
        if (!e.album.isEmpty())       o.insert(QStringLiteral("al"), e.album);
        if (!e.genre.isEmpty())       o.insert(QStringLiteral("ge"), e.genre);
        if (e.track)       o.insert(QStringLiteral("tn"), e.track);
        if (e.trackTotal)  o.insert(QStringLiteral("tt"), e.trackTotal);
        if (e.disc)        o.insert(QStringLiteral("dn"), e.disc);
        if (e.discTotal)   o.insert(QStringLiteral("dt"), e.discTotal);
        if (e.year)        o.insert(QStringLiteral("yr"), e.year);
        if (e.durationSec) o.insert(QStringLiteral("du"), e.durationSec);
        if (e.hasCover)    o.insert(QStringLiteral("cv"), true);
        if (e.untagged)    o.insert(QStringLiteral("nt"), true);
        if (!e.composer.isEmpty())  o.insert(QStringLiteral("cm"), e.composer);
        if (!e.conductor.isEmpty()) o.insert(QStringLiteral("cd"), e.conductor);
        if (!e.performer.isEmpty()) o.insert(QStringLiteral("pf"), e.performer);
        if (!e.work.isEmpty())      o.insert(QStringLiteral("wk"), e.work);
        if (!e.movement.isEmpty())  o.insert(QStringLiteral("mv"), e.movement);
        writeList(o, QStringLiteral("ars"), e.artists);
        writeList(o, QStringLiteral("ges"), e.genres);
        writeList(o, QStringLiteral("cms"), e.composers);
        writeList(o, QStringLiteral("cds"), e.conductors);
        writeList(o, QStringLiteral("pfs"), e.performers);
        if (!e.cuePath.isEmpty())
        {
            o.insert(QStringLiteral("cp"), e.cuePath);
            o.insert(QStringLiteral("cmt"), double(e.cueMtime));
            o.insert(QStringLiteral("csz"), double(e.cueSize));
        }
        if (!e.cueTracks.isEmpty())
        {
            QJsonArray cue;
            for (const CueTrack& c : e.cueTracks)
            {
                QJsonObject co;
                co.insert(QStringLiteral("n"), c.number);
                if (!c.title.isEmpty())  co.insert(QStringLiteral("t"), c.title);
                if (!c.artist.isEmpty()) co.insert(QStringLiteral("a"), c.artist);
                co.insert(QStringLiteral("s"), c.startMs);
                if (c.endMs >= 0) co.insert(QStringLiteral("e"), c.endMs);
                cue.append(co);
            }
            o.insert(QStringLiteral("cue"), cue);
        }
        writeGain(o, QStringLiteral("rgtg"), e.trackGain);
        writeGain(o, QStringLiteral("rgag"), e.albumGain);
        writeGain(o, QStringLiteral("rgtp"), e.trackPeak);
        writeGain(o, QStringLiteral("rgap"), e.albumPeak);
        tracks.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kIndexFileVersion);
    root.insert(QStringLiteral("rules"), parseStamp(separators));
    root.insert(QStringLiteral("tracks"), tracks);

    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) >= 0;
}

// Cached process-wide index (main-thread only): the async scan installs it, browse reads it.
namespace { Index g_index; bool g_indexReady = false; }

QString root() { return Settings::musicFolder(); }
QString indexFilePath() { return AppPaths::dataDir() + QStringLiteral("/musicindex.json"); }
void installIndex(Index idx) { g_index = std::move(idx); g_indexReady = true; }
const Index& index() { return g_index; }
bool indexReady() { return g_indexReady; }

bool hasLibrary()
{
    if (!g_index.isEmpty()) return true;
    const QString r = root();
    return !r.isEmpty() && QFileInfo::exists(r);
}

} // namespace MusicLibrary
