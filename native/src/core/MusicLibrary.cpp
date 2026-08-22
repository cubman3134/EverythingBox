#include "MusicLibrary.h"
#include "AppPaths.h"
#include "Settings.h"

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

QVector<TrackEntry> scanFolder(const QString& root, const QHash<QString, TrackEntry>& known, ScanStats* stats)
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

    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        if (!isAudioFile(path)) continue;      // extension-only, before anything is opened — the cheap filter
        const QFileInfo fi(path);
        const QString abs = fi.absoluteFilePath();
        const qint64 mtime = fi.lastModified().toSecsSinceEpoch();
        const qint64 size  = fi.size();
        ++s.files;

        // THE INCREMENTAL DECISION, and the only one. Same path, same mtime, same size => the bytes we
        // already parsed are still the bytes on disk, so the file is not opened at all. Size is checked as
        // well as mtime because a tag editor that rewrites a file can preserve the timestamp (and archives
        // restored from backup routinely do), while almost nothing preserves the length too.
        const auto cached = known.constFind(abs);
        if (cached != known.constEnd() && cached->mtime == mtime && cached->size == size)
        {
            ++s.reused;
            out.push_back(*cached);
            continue;
        }

        const AudioTags::Tags t = AudioTags::read(abs);
        ++s.retagged;

        TrackEntry e;
        e.path = abs; e.mtime = mtime; e.size = size;
        e.title = t.title; e.artist = t.artist; e.albumArtist = t.albumArtist;
        e.album = t.album; e.genre = t.genre;
        e.track = t.track; e.trackTotal = t.trackTotal;
        e.disc = t.disc;   e.discTotal = t.discTotal;
        e.year = t.year;   e.durationSec = t.durationSec;
        e.hasCover = !t.cover.isNull();
        e.trackGain = t.trackGain; e.albumGain = t.albumGain;
        e.trackPeak = t.trackPeak; e.albumPeak = t.albumPeak;
        e.untagged = t.isEmpty();
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

        IndexTrack t;
        t.path        = e.path;
        // Filename fallback for the title. completeBaseName() keeps "Track 2.part1" intact and drops only
        // the final extension, which is what a person reading the folder would call the file.
        t.title       = e.title.trimmed().isEmpty() ? fi.completeBaseName() : e.title.trimmed();
        t.artist      = e.artist.trimmed();
        t.disc        = e.disc;
        t.track       = e.track;
        t.durationSec = e.durationSec;
        t.hasCover    = e.hasCover;
        alb.tracks.push_back(t);

        idx.artists[ai].trackCount += 1;
        idx.trackCount += 1;
    }

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

QString displayArtist(const Artist& a)
{
    return a.name.isEmpty() ? QObject::tr("Unknown Artist") : a.name;
}

QString displayAlbum(const Album& a)
{
    return a.title.isEmpty() ? QObject::tr("Unknown Album") : a.title;
}

// ---------------------------------------------------------------------------------------------------------
// Persistence: { "version": 1, "tracks": [ { "p": …, "m": …, "s": …, … } ] }
//
// Short keys and omitted defaults, because this file has one entry per track and a big library has tens of
// thousands of them — spelling "albumArtist" out twenty thousand times costs more than the values do. The
// version field exists so a later increment can change the entry shape without mis-reading an old file as
// the new one; an unknown version loads as empty, which costs a full re-tag and nothing else.
// ---------------------------------------------------------------------------------------------------------
namespace { const int kIndexFileVersion = 1; }

QVector<TrackEntry> loadIndexFile(const QString& filePath)
{
    QVector<TrackEntry> out;
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.value(QStringLiteral("version")).toInt() != kIndexFileVersion) return out;

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
        e.trackGain = readGain(o, QStringLiteral("rgtg"));
        e.albumGain = readGain(o, QStringLiteral("rgag"));
        e.trackPeak = readGain(o, QStringLiteral("rgtp"));
        e.albumPeak = readGain(o, QStringLiteral("rgap"));
        out.push_back(e);
    }
    return out;
}

bool saveIndexFile(const QString& filePath, const QVector<TrackEntry>& entries)
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
        writeGain(o, QStringLiteral("rgtg"), e.trackGain);
        writeGain(o, QStringLiteral("rgag"), e.albumGain);
        writeGain(o, QStringLiteral("rgtp"), e.trackPeak);
        writeGain(o, QStringLiteral("rgap"), e.albumPeak);
        tracks.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kIndexFileVersion);
    root.insert(QStringLiteral("tracks"), tracks);

    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) >= 0;
}

// Cached process-wide index (main-thread only): the async scan installs it, browse reads it.
namespace { Index g_index; }

QString root() { return Settings::musicFolder(); }
QString indexFilePath() { return AppPaths::dataDir() + QStringLiteral("/musicindex.json"); }
void installIndex(Index idx) { g_index = std::move(idx); }
const Index& index() { return g_index; }

} // namespace MusicLibrary
