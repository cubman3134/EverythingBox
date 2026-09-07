// Trickplay — the arithmetic behind seek previews (issue #85): where frame N lives in a sprite sheet, what
// keys a file's sheets, which sheet a half-finished run resumes at, and which cached items an over-budget
// cache gives up first. Plus the one predicate that decides whether a thing may be previewed at all.
//
// Everything here is PURE and Qt-Core-only (QString/QJsonObject and nothing heavier), for two reasons:
//
//   * it is the half of the feature that can be wrong in a way nobody sees. A tile lookup that is one column
//     out shows the WRONG SECOND of the film with a confident timestamp under it, which is worse than no
//     preview at all — the whole point of the feature is that the picture and the number agree. So the
//     lookup, the key, the resume point and the eviction order are separated from anything that needs a
//     decoder, a window or a disk, and pinned headlessly by native/tools/probe_trickplay.cpp;
//   * the STREAM GUARD has to be a value, not an `if` at a call site. Generating previews means seeking a
//     file three hundred times: harmless on a file the user owns, and on a debrid or IPTV url it is three
//     hundred range requests against someone's quota for a courtesy nobody asked for. `classify()` is the
//     only place that decision is made, it answers WHY it refused, and the probe drives it over the real
//     shapes this app hands the player.
//
// THE SHEET. Standard trickplay/BIF layout: one frame every `intervalMs` at thumbnail size, packed
// left-to-right, top-to-bottom into a `cols` x `rows` JPEG grid, one grid per `cols*rows` frames. At the
// shipped 10 s / 5x5 that is one grid per 250 s of film — a two-hour film is 29 grids and about 720 tiles.
// Finding a frame is then arithmetic and never a scan: divide, take the remainder, blit one tile out of one
// already-decoded grid. That is what makes scrubbing free at play time, which is the feature.
//
// THE LAST GRID IS SHORT, and it is the case everything gets wrong. A film is not a whole number of grids,
// so the final sheet holds between 1 and cols*rows frames and is only as tall as the rows it uses. Every
// function here that could assume a full grid takes `frameCount` into account instead.
#pragma once
#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <algorithm>

namespace Trickplay
{

// ---- The stream guard ---------------------------------------------------------------------------------
//
// Why a thing may not be previewed. `None` is the only value that permits generation; every other value
// names the refusal so a log line (and the probe) can say which rule fired rather than "no".
enum class Refusal
{
    None,      // a plain absolute path on this machine: generate
    Nothing,   // empty / whitespace: nothing to generate from
    Url,       // has a scheme — http(s), a debrid link, an IPTV/udp/rtp channel, magnet:, av://, edl://…
    Unc,       // \\server\share or //server/share: a file, but not a file on THIS machine
    Relative   // not rooted: we cannot key it, and it means different files from different directories
};

// A `file:` URL is a spelling of a local path, so it is unwrapped once and re-judged as a path — which is
// what makes `file://server/share/x.mkv` come out as Unc rather than sneaking through as "a local file".
// Everything else with a scheme is refused: an http(s) stream, a signed debrid url, an IPTV channel
// (http/udp/rtp/rtsp), a magnet, and mpv's own pseudo-protocols (av://, edl://, bd://, dvd://).
//
// The scheme test requires TWO or more characters before the colon, which is the one subtlety: a Windows
// path begins "C:", and a one-letter "scheme" is always a drive letter and never a protocol. (No RFC 3986
// scheme is a single character either, so nothing real is lost.)
//
// Deliberately platform-INDEPENDENT: "/srv/media/film.mkv" and "D:/media/film.mkv" both answer None on
// Windows and on Linux alike. Whether the path exists is a separate question the caller asks of the disk;
// mixing the two would make this untestable on the CI runner, which has neither shape on it.
inline Refusal classify(const QString& urlOrPath)
{
    const QString s = urlOrPath.trimmed();
    if (s.isEmpty()) return Refusal::Nothing;

    // file: → unwrap to the path it names, then judge that. QUrl::toLocalFile keeps the leading "//" of a
    // file://server/share url, so the UNC test below still sees it.
    if (s.startsWith(QLatin1String("file:"), Qt::CaseInsensitive))
    {
        const QString local = QUrl(s).toLocalFile();
        if (local.isEmpty()) return Refusal::Url;
        // One unwrap only — a local path can never itself be a file: url, so this cannot recurse.
        return classify(local);
    }

    // A scheme of two or more characters: not a path at all. (See the header note on "C:".)
    const int colon = s.indexOf(QLatin1Char(':'));
    if (colon >= 2)
    {
        bool schemeish = true;
        for (int i = 0; i < colon; ++i)
        {
            const QChar c = s.at(i);
            if (!(c.isLetterOrNumber() || c == QLatin1Char('+') || c == QLatin1Char('-') || c == QLatin1Char('.')))
            { schemeish = false; break; }
        }
        if (schemeish && s.at(0).isLetter()) return Refusal::Url;
    }

    // A network share is a file we could read and must not: pulling a whole film across SMB to make
    // thumbnails is the same bargain the http refusal above declines, paid in someone else's bandwidth.
    if (s.startsWith(QLatin1String("\\\\")) || s.startsWith(QLatin1String("//"))) return Refusal::Unc;

    // Rooted, either way a rooted path is spelled. A relative path is refused rather than resolved: the key
    // below is the path, and the same relative string means different files from different directories.
    if (s.startsWith(QLatin1Char('/'))) return Refusal::None;
    if (s.size() >= 3 && s.at(0).isLetter() && s.at(1) == QLatin1Char(':')
        && (s.at(2) == QLatin1Char('/') || s.at(2) == QLatin1Char('\\')))
        return Refusal::None;

    return Refusal::Relative;
}

inline bool eligible(const QString& urlOrPath) { return classify(urlOrPath) == Refusal::None; }

// ---- The sheet ----------------------------------------------------------------------------------------

struct Layout
{
    int intervalMs = 10000;  // one frame per this much film
    int tileW      = 320;    // thumbnail size, in pixels
    int tileH      = 180;
    int cols       = 5;      // tiles across one grid…
    int rows       = 5;      // …and down it
    int frameCount = 0;      // how many frames were captured in total (the last grid is usually short)

    int perGrid() const { return cols * rows; }
    // A layout nothing can be looked up in is not "empty", it is broken — a zero interval divides by zero
    // and a zero column count makes every tile column 0. Callers check this before trusting a lookup.
    bool valid() const
    {
        return intervalMs > 0 && tileW > 0 && tileH > 0 && cols > 0 && rows > 0 && frameCount >= 0;
    }
};

// How many frames a file of this length earns. The frame at 0 counts, so a 25 s file at 10 s gets three
// (0 s, 10 s, 20 s) — ceil, not floor, or the tail of every file has no preview.
inline int frameCountFor(qint64 durationMs, int intervalMs)
{
    if (durationMs <= 0 || intervalMs <= 0) return 0;
    return int((durationMs + intervalMs - 1) / intervalMs);
}

inline int gridCount(const Layout& l)
{
    if (!l.valid() || l.frameCount <= 0) return 0;
    const int per = l.perGrid();
    return (l.frameCount + per - 1) / per;
}

// The number of frames grid `g` actually holds — the whole point of this function is the LAST one, which
// holds the remainder and not a full sheet.
inline int framesInGrid(const Layout& l, int g)
{
    const int n = gridCount(l);
    if (g < 0 || g >= n) return 0;
    const int per = l.perGrid();
    const int rest = l.frameCount - g * per;
    return rest < per ? rest : per;
}

// How many rows of the grid image are used. A short final grid is written only as tall as it needs to be,
// so a 3-frame tail of a 5x5 sheet is one row and not five (25x the bytes for nothing).
inline int rowsInGrid(const Layout& l, int g)
{
    const int f = framesInGrid(l, g);
    if (f <= 0 || l.cols <= 0) return 0;
    return (f + l.cols - 1) / l.cols;
}

inline int gridPixelWidth(const Layout& l, int g)
{
    const int f = framesInGrid(l, g);
    if (f <= 0) return 0;
    return (f < l.cols ? f : l.cols) * l.tileW;
}
inline int gridPixelHeight(const Layout& l, int g) { return rowsInGrid(l, g) * l.tileH; }

// The frame index nearest to `atMs`, CLAMPED at both ends. Clamping rather than refusing is a deliberate
// UI decision and the reason a time past the end is not an error: a drag can put the handle a hair past the
// duration mpv reported (and does, routinely, on a VBR file), and the honest answer there is the last frame
// we have — never a blank. Answers -1 only when there are no frames at all, which is the "no strip" case
// the player renders as today's behaviour, silently.
inline int frameAt(const Layout& l, qint64 atMs)
{
    if (!l.valid() || l.frameCount <= 0) return -1;
    if (atMs <= 0) return 0;
    const qint64 idx = atMs / l.intervalMs;
    return int(idx >= l.frameCount ? l.frameCount - 1 : idx);
}

struct Tile
{
    int frame = -1;
    int grid  = -1;
    int row   = -1;
    int col   = -1;
    bool ok() const { return frame >= 0; }
};

inline Tile tileOfFrame(const Layout& l, int frame)
{
    Tile t;
    if (!l.valid() || frame < 0 || frame >= l.frameCount) return t;
    const int per = l.perGrid();
    t.frame = frame;
    t.grid  = frame / per;
    const int within = frame % per;
    t.row = within / l.cols;
    t.col = within % l.cols;
    return t;
}

inline Tile tileAt(const Layout& l, qint64 atMs) { return tileOfFrame(l, frameAt(l, atMs)); }

// The film time a frame stands for — the number shown under the thumbnail. It is the CAPTURE time and not
// the time the user is pointing at, so the label can never claim a second the picture is not of.
inline qint64 frameTimeMs(const Layout& l, int frame)
{
    if (!l.valid() || frame < 0) return 0;
    return qint64(frame) * l.intervalMs;
}

inline int tilePixelX(const Layout& l, const Tile& t) { return t.ok() ? t.col * l.tileW : 0; }
inline int tilePixelY(const Layout& l, const Tile& t) { return t.ok() ? t.row * l.tileH : 0; }

// The file name of grid `g` inside an item's cache directory. Zero-padded so a directory listing sorts the
// way the grids run, and fixed-width so a half-written "g0007.jpg.part" is obvious next to it.
inline QString gridFileName(int g)
{
    return QStringLiteral("g%1.jpg").arg(g, 4, 10, QLatin1Char('0'));
}

// ---- The cache key ------------------------------------------------------------------------------------
//
// Path AND mtime AND size, and none of the three is optional. Jellyfin 10.11 learned this the expensive way:
// key by path alone and a re-encoded, re-downloaded or simply replaced file keeps the previous film's
// thumbnails, so the strip shows one movie while the player shows another — with the timestamps still
// looking authoritative. mtime catches the ordinary replacement; size joins it because a tool that preserves
// timestamps (rsync -t, a restore from backup, an archiver) is exactly the tool most likely to have changed
// the bytes underneath us.
//
// Separators are normalised so "D:\Films\a.mkv" and "D:/Films/a.mkv" are one item. Case is deliberately NOT
// folded: it would have to be folded on Windows and not on Linux, and a key whose value depends on which
// machine computed it is not a cache key at all.
inline QString cacheKey(const QString& path, qint64 mtimeSecs, qint64 sizeBytes)
{
    QString norm = path;
    norm.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QByteArray material = norm.toUtf8() + '\0' + QByteArray::number(mtimeSecs)
                                + '\0' + QByteArray::number(sizeBytes);
    return QString::fromLatin1(QCryptographicHash::hash(material, QCryptographicHash::Sha1).toHex());
}

// ---- The sidecar --------------------------------------------------------------------------------------
//
// One small JSON file per item, beside its grids. It exists so the player can find frame N by arithmetic
// with nothing but this file open — no grid is read, no directory is scanned, and a cache of a thousand
// items costs one read of a few hundred bytes at play time.
struct Index
{
    int     version      = 1;
    Layout  layout;
    qint64  durationMs   = 0;
    qint64  sourceMtime  = 0;   // the two halves of the key, stored so a stale directory can say so itself
    qint64  sourceSize   = 0;
    QString sourceName;         // the file's own name, for diagnosis only — NEVER part of the key
    int     completeGrids = 0;  // grids fully written and renamed into place; the resume point
};

inline QByteArray writeIndex(const Index& ix)
{
    QJsonObject o;
    o[QStringLiteral("version")]       = ix.version;
    o[QStringLiteral("intervalMs")]    = ix.layout.intervalMs;
    o[QStringLiteral("tileW")]         = ix.layout.tileW;
    o[QStringLiteral("tileH")]         = ix.layout.tileH;
    o[QStringLiteral("cols")]          = ix.layout.cols;
    o[QStringLiteral("rows")]          = ix.layout.rows;
    o[QStringLiteral("frameCount")]    = ix.layout.frameCount;
    o[QStringLiteral("durationMs")]    = double(ix.durationMs);
    o[QStringLiteral("sourceMtime")]   = double(ix.sourceMtime);
    o[QStringLiteral("sourceSize")]    = double(ix.sourceSize);
    o[QStringLiteral("sourceName")]    = ix.sourceName;
    o[QStringLiteral("completeGrids")] = ix.completeGrids;
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// Strict on purpose. A sidecar that does not parse, or that describes a layout nothing can be looked up in,
// must read as "no previews" and never as "previews with a broken layout" — the second one is how a tile
// lookup starts dividing by zero on a user's machine. A truncated write (power loss mid-flush) lands here.
inline bool readIndex(const QByteArray& json, Index* out)
{
    if (!out) return false;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject o = doc.object();
    Index ix;
    ix.version            = o.value(QStringLiteral("version")).toInt(0);
    ix.layout.intervalMs  = o.value(QStringLiteral("intervalMs")).toInt(0);
    ix.layout.tileW       = o.value(QStringLiteral("tileW")).toInt(0);
    ix.layout.tileH       = o.value(QStringLiteral("tileH")).toInt(0);
    ix.layout.cols        = o.value(QStringLiteral("cols")).toInt(0);
    ix.layout.rows        = o.value(QStringLiteral("rows")).toInt(0);
    ix.layout.frameCount  = o.value(QStringLiteral("frameCount")).toInt(-1);
    ix.durationMs         = qint64(o.value(QStringLiteral("durationMs")).toDouble(0.0));
    ix.sourceMtime        = qint64(o.value(QStringLiteral("sourceMtime")).toDouble(0.0));
    ix.sourceSize         = qint64(o.value(QStringLiteral("sourceSize")).toDouble(0.0));
    ix.sourceName         = o.value(QStringLiteral("sourceName")).toString();
    ix.completeGrids      = o.value(QStringLiteral("completeGrids")).toInt(0);
    if (ix.version != 1) return false;
    if (!ix.layout.valid()) return false;
    if (ix.completeGrids < 0 || ix.completeGrids > gridCount(ix.layout)) return false;
    *out = ix;
    return true;
}

// ---- Resuming -----------------------------------------------------------------------------------------
//
// A run that was interrupted keeps every grid it finished and picks up at the first one it did not. `present`
// is what the cache directory holds, grid by grid; the answer is a grid INDEX, so "interrupted after grid 2
// of 5" (grids 0 and 1 on disk) answers 2 — the third grid, the first missing one.
//
// The first HOLE, not the count: a directory holding grids 0, 1 and 3 (grid 2 lost to a failed rename) must
// resume at 2 and not at 4, or the gap is permanent and the film has a hole in its strip forever. Answers
// gridCount() when nothing is missing, which is the caller's "already done" and its loop bound at once.
inline int resumeGrid(const Layout& l, const QVector<bool>& present)
{
    const int n = gridCount(l);
    for (int g = 0; g < n; ++g)
        if (g >= present.size() || !present.at(g)) return g;
    return n;
}

// ---- Eviction -----------------------------------------------------------------------------------------

struct CacheEntry
{
    QString key;
    qint64  bytes        = 0;
    qint64  lastUsedSecs = 0;   // when this item's previews were last SHOWN (not when they were made)
};

// Which items an over-budget cache gives up, least recently used first, until it fits. Returns the keys to
// delete, in the order they should go.
//
// `keepKey` is never evicted whatever its age: it is the item being watched or generated right now, and
// deleting the strip out from under a live scrub — or the half-finished grids of the file the job is walking
// — is how an LRU sweep turns into a loop that generates and destroys the same work forever.
//
// Ties break on the key so the answer is deterministic: two items last used in the same second must not
// evict in whatever order the directory happened to be listed in, or the same cache state gives different
// answers on two runs and nothing about it is testable.
inline QStringList planEviction(QVector<CacheEntry> entries, qint64 boundBytes, const QString& keepKey)
{
    qint64 total = 0;
    for (const CacheEntry& e : entries) total += e.bytes;
    if (total <= boundBytes) return QStringList();

    std::sort(entries.begin(), entries.end(), [](const CacheEntry& a, const CacheEntry& b) {
        if (a.lastUsedSecs != b.lastUsedSecs) return a.lastUsedSecs < b.lastUsedSecs;
        return a.key < b.key;
    });

    QStringList victims;
    for (const CacheEntry& e : entries)
    {
        if (total <= boundBytes) break;
        if (!keepKey.isEmpty() && e.key == keepKey) continue;
        victims << e.key;
        total -= e.bytes;
    }
    return victims;
}

inline QStringList planEviction(const QVector<CacheEntry>& entries, qint64 boundBytes)
{
    return planEviction(entries, boundBytes, QString());
}

} // namespace Trickplay
