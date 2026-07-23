# Local Video Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Scan a configured local folder for movie/TV video files, match them (filename + Kodi `.nfo` sidecar → IMDB id), and make them first-class catalog content: a browsable "Local Library" folder, an "On disk" badge on matching addon tiles, and prefer-local playback for owned items.

**Architecture:** A new QtCore-only `LocalLibrary` namespace with a **pure, probe-tested core** (filename parse, NFO read, `scanFolder`, `buildIndex → OwnedIndex`) plus a thin main-thread cached-index convenience layer. Discovered videos ride the existing `MediaItem`/`MediaCatalog` spine: surfaced via a new `browse::localLibraryCatalog` synthetic folder (the Downloaded/Recent pattern), decorated at the single row-build choke point (`HomeView::browseItems`), and redirected to the local file at activation (`MainWindow::openLibraryItem`). No addon-transport or sync-transport changes.

**Tech Stack:** Qt 6.8.3 (QtCore: QDirIterator, QRegularExpression, QXmlStreamReader; QtConcurrent for the async scan), the existing CloudSync carve-out, SyntheticCatalogs, MetaCache, headless probes.

## Global Constraints

- **Branch:** `local/video-library` off main. Standing autonomy through the merge gate. The pre-commit hook auto-bumps the patch version on every commit — expected; never hand-edit the version lines.
- **Scope = video only (movies + TV).** No music/books, no network id-resolver, no subtitle sidecars, no duplicate quality-ranking, no on-disk index persistence. These are explicit non-goals (spec §Non-goals) — do not build them.
- **ANCHOR ON FUNCTION NAMES.** Scout anchors (main@bfbb6d6):

| Concern | Anchor |
|---|---|
| Settings folder-key clone | `Settings::romsFolder`/`setRomsFolder` decl `Settings.h:97-100`, def `Settings.cpp:205-213` |
| Device-local carve-out | `CloudSync::isDeviceLocalKey` `CloudSync.cpp:403-431` (the `kExact` QSet) |
| Synthetic builder shape | `browse::downloadsCatalog` `SyntheticCatalogs.h:30`, body `SyntheticCatalogs.cpp:51-77`; `iconTypeForKind` `:10-19` |
| MediaItem / MediaCatalog | `AddonModels.h:112-150` / `:152-160` |
| Synthetic folder surfacing | `pushFolders` lambda `HomeView.cpp:4137-4149`; root call site `:4185-4198`; dispatch `:2394-2408`; `showSyntheticCatalog`/`openDownloadsLevel`/`populateDownloads` `:1891-1928` |
| Seam A row-build choke | `HomeView::browseItems` header `:1361`, per-row map `:1397-1409` (the `it.art.writeInto(m)` at `:1408`) |
| Seam B activation | `MainWindow::openLibraryItem` `:6522`; empty-url guard `:6531`; goggame branch `:6539`; mpv else-branch `:6717-6743` (`player_->play(url)` `:6739`); re-enter idiom `fetchRemoteDocumentThenOpen` `:6759-6760` |
| Probe copy-target | `probe_browse` CMake block `CMakeLists.txt:449-453`; runner loop `run-headless-probes.sh:119-122`; CI build list `ci.yml:52`; probe main+sentinel shape `probe_marks.cpp:1-14,21-34,53,212-214` |
| Scanner shape to mirror | `RomLibrary.h:1-49` (free-function namespace, `root()` reads `Settings::romsFolder`) |

- **Env recipe (probes/build), copy verbatim** — PATH prepend `/c/Qt/6.8.3/msvc2022_64/bin` + `/c/mpv-dev`; the build dir carries a generated `qt.conf` (no `QT_PLUGIN_PATH` needed); configure `cmake -S native -B build -G "Visual Studio 18 2026" -A x64 -DMYMEDIAVAULT_BUILD_APP=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 -DMPV_INCLUDE_DIR=C:/mpv-dev/include -DMPV_LIBRARY=C:/mpv-dev/libmpv.lib`; build with `--parallel` (never `-- /m`). Probe runner: `BUILD_DIR=build bash native/tools/run-headless-probes.sh`.
- **Probe discipline:** RED-first (write the failing probe, see it fail, then implement). `probe_locallib` is QtCore-only (links `LocalLibrary.cpp` + `AddonModels.cpp` + `SyntheticCatalogs.cpp` for the browse builder; NOT CloudSync, NOT Settings) and builds a hermetic `QTemporaryDir` fixture tree in-process. Sentinel `LOCALLIB-OK` / `LOCALLIB-FAIL <cond> (line)`.
- **Feature dormant when `library/folder` unset:** empty index, no synthetic folder, no decoration, zero render cost (the ROMs precedent). Every task must preserve this.
- **Additive only:** un-owned addon tiles and the create-a-profile/fresh paths stay byte-identical. Seam A adds map keys before `writeInto`; Seam B adds an early redirect before existing branches.

---

### Task 1: Settings::libraryFolder + device-local carve-out

**Files:**
- Modify: `native/src/core/Settings.h` (declare the pair, after `:100`)
- Modify: `native/src/core/Settings.cpp` (define the pair, after `:213`)
- Modify: `native/src/core/CloudSync.cpp:403-431` (add `library/folder` to `kExact`)
- Modify: `native/tools/probe_cloudmerge.cpp` (assert the new key is device-local; the sibling `library/showHidden` is not)

**Interfaces:**
- Consumes: nothing.
- Produces: `QString Settings::libraryFolder()` (resolved, never empty) and `void Settings::setLibraryFolder(const QString& path)`; `CloudSync::isDeviceLocalKey("library/folder") == true`.

- [ ] **Step 1: Write the failing probe assertion.** In `native/tools/probe_cloudmerge.cpp`, find the carve-out / `isDeviceLocalKey` section (where existing device-local keys like `roms/folder` are checked) and add:

```cpp
    // Local video library folder is device-local (each machine points at its own disk); the
    // library/showHidden sibling is a user preference and DOES sync (leaf-exact match, not group).
    CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("library/folder")) == true);
    CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("library/showHidden")) == false);
```

(Use whatever CHECK/sentinel macro that probe already defines. If the probe does not currently include `CloudSync.h`, add `#include "CloudSync.h"`.)

- [ ] **Step 2: Build the probe, verify it FAILS.**

Run (bash, env recipe applied):
```bash
cmake --build build --target probe_cloudmerge --parallel && BUILD_DIR=build bash native/tools/run-headless-probes.sh 2>&1 | grep -i cloudmerge
```
Expected: `probe_cloudmerge` reports `CLOUDMERGE-FAIL ... (line NNN)` on the new `library/folder` assertion (the key is not yet device-local, so `isDeviceLocalKey` returns false).

- [ ] **Step 3: Add the Settings pair.** In `native/src/core/Settings.h` after line 100 (right after `setRomsFolder`):

```cpp
    // Root of the local VIDEO library (movies + TV), scanned by LocalLibrary. Empty stored value =>
    // the default (<data>/library). Device-local (never synced): each machine points at its own disk.
    QString libraryFolder();       // resolved path (never empty)
    void setLibraryFolder(const QString& path);
```

In `native/src/core/Settings.cpp` after line 213 (right after `setRomsFolder`'s closing brace):

```cpp
QString Settings::libraryFolder()
{
    const QString p = store().value(QStringLiteral("library/folder")).toString();
    return p.isEmpty() ? (AppPaths::dataDir() + QStringLiteral("/library")) : p;
}
void Settings::setLibraryFolder(const QString& path)
{
    store().setValue(QStringLiteral("library/folder"), path); store().sync();
}
```

- [ ] **Step 4: Add the carve-out entry.** In `native/src/core/CloudSync.cpp`, inside the `kExact` QSet (between `roms/folder` at `:407` and the closing brace at `:415`), add:

```cpp
        QStringLiteral("library/folder"),        // where THIS machine keeps its local video library
```

- [ ] **Step 5: Rebuild the probe, verify it PASSES.**

Run:
```bash
cmake --build build --target probe_cloudmerge --parallel && BUILD_DIR=build bash native/tools/run-headless-probes.sh 2>&1 | grep -i cloudmerge
```
Expected: `probe_cloudmerge ... CLOUDMERGE-OK`.

- [ ] **Step 6: Full suite green + commit.**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh   # expect ALL HEADLESS PROBES PASSED
git add native/src/core/Settings.h native/src/core/Settings.cpp native/src/core/CloudSync.cpp native/tools/probe_cloudmerge.cpp
git commit -m "feat: Settings::libraryFolder + device-local carve-out (locallib T1)"
```

---

### Task 2: LocalLibrary pure core + probe_locallib (RED-first)

**Files:**
- Create: `native/src/core/LocalLibrary.h`
- Create: `native/src/core/LocalLibrary.cpp`
- Create: `native/tools/probe_locallib.cpp`
- Modify: `native/CMakeLists.txt` (new `add_executable(probe_locallib ...)` block after `:453`; add `LocalLibrary.cpp` to the app target's source list)
- Modify: `native/tools/run-headless-probes.sh:119` (append `"probe_locallib LOCALLIB-OK"` to the loop)
- Modify: `.github/workflows/ci.yml:52` (append `probe_locallib` to the build-target list)

**Interfaces:**
- Consumes: nothing (pure; explicit root param — does NOT read Settings in this task's tested functions).
- Produces (the full public API later tasks depend on — names/types are binding):

```cpp
namespace LocalLibrary {
    enum class Kind { Movie, Episode };
    struct VideoEntry {
        QString path;          // absolute path to the video file
        Kind    kind = Kind::Movie;
        QString title;         // parsed movie title (Movie) / raw base (Episode)
        int     year = 0;      // Movie release year, 0 if unknown
        QString show;          // Episode: cleaned show name
        int     season = 0;    // Episode
        int     episode = 0;   // Episode
        QString imdbId;        // Movie's own imdb id from .nfo ("tt..."), else empty
        QString seriesImdbId;  // Episode's series imdb id from tvshow.nfo ("tt..."), else empty
        QString plot;          // from .nfo, else empty
        QString thumbPath;     // from .nfo <thumb> resolved to absolute/http, else empty
    };
    struct OwnedIndex {
        QHash<QString, QString> pathById;    // merge key -> local path (movie id, or "seriesId:S:E")
        QHash<QString, int>     seriesCount; // series imdb id -> owned episode count
        QVector<VideoEntry>     entries;     // everything scanned (for the browse surface)
        bool    ownsId(const QString& id) const { return pathById.contains(id) || seriesCount.contains(id); }
        QString localPathFor(const QString& id) const { return pathById.value(id); }
        int     ownedEpisodes(const QString& seriesId) const { return seriesCount.value(seriesId); }
        const QVector<VideoEntry>& all() const { return entries; }
    };
    VideoEntry          parseFile(const QString& path);          // filename parse only (no disk read)
    QVector<VideoEntry> scanFolder(const QString& root);         // walk + parse + read .nfo sidecars
    OwnedIndex          buildIndex(const QVector<VideoEntry>& entries);
    QString             displayTitle(const VideoEntry& e);       // "Title (Year)" / "Show S01E02"
}
```

- [ ] **Step 1: Write the failing probe.** Create `native/tools/probe_locallib.cpp` (mirrors `probe_marks.cpp`'s shape: header doc, CHECK macro, `QCoreApplication`, sentinel). It builds a hermetic fixture tree in a `QTemporaryDir`, scans it, and asserts the parse/index matrix:

```cpp
// Headless probe for the local video library core (filename parse + NFO read + scan + OwnedIndex).
// Builds a hermetic QTemporaryDir fixture and asserts the parse/index matrix.
// Prints LOCALLIB-OK on success; any failure prints LOCALLIB-FAIL <cond> (line) and exits non-zero.
#include "LocalLibrary.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "LOCALLIB-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static void writeFile(const QString& path, const QByteArray& bytes = QByteArray())
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path); f.open(QIODevice::WriteOnly); f.write(bytes); f.close();
}

static const LocalLibrary::VideoEntry* findByPathSuffix(const QVector<LocalLibrary::VideoEntry>& v, const QString& suffix)
{
    for (const auto& e : v) if (e.path.endsWith(suffix)) return &e;
    return nullptr;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString root = tmp.path();

    // Fixture tree.
    writeFile(root + "/Inception (2010)/Inception (2010).mkv");
    writeFile(root + "/Inception (2010)/Inception (2010).nfo",
              "<movie><title>Inception</title><year>2010</year>"
              "<uniqueid type=\"imdb\">tt1375666</uniqueid>"
              "<plot>A thief.</plot></movie>");
    writeFile(root + "/Movies/The Matrix.1999.mp4");                 // no nfo
    writeFile(root + "/Show/Season 01/Show - S01E02.mkv");
    writeFile(root + "/Show/tvshow.nfo",
              "<tvshow><title>Show</title><uniqueid type=\"imdb\">tt2000000</uniqueid></tvshow>");
    writeFile(root + "/junk/random.mkv");                            // no year, no SxxEyy
    writeFile(root + "/Show/notes.txt", "ignore me");               // non-video

    const QVector<LocalLibrary::VideoEntry> scanned = LocalLibrary::scanFolder(root);
    CHECK(scanned.size() == 4);   // 3 mkv + 1 mp4; txt ignored

    const auto* inc = findByPathSuffix(scanned, "Inception (2010).mkv");
    CHECK(inc != nullptr);
    if (inc) {
        CHECK(inc->kind == LocalLibrary::Kind::Movie);
        CHECK(inc->title == QStringLiteral("Inception"));
        CHECK(inc->year == 2010);
        CHECK(inc->imdbId == QStringLiteral("tt1375666"));
    }

    const auto* mtx = findByPathSuffix(scanned, "The Matrix.1999.mp4");
    CHECK(mtx != nullptr);
    if (mtx) {
        CHECK(mtx->kind == LocalLibrary::Kind::Movie);
        CHECK(mtx->title == QStringLiteral("The Matrix"));
        CHECK(mtx->year == 1999);
        CHECK(mtx->imdbId.isEmpty());
    }

    const auto* ep = findByPathSuffix(scanned, "Show - S01E02.mkv");
    CHECK(ep != nullptr);
    if (ep) {
        CHECK(ep->kind == LocalLibrary::Kind::Episode);
        CHECK(ep->season == 1);
        CHECK(ep->episode == 2);
        CHECK(ep->show == QStringLiteral("Show"));
        CHECK(ep->seriesImdbId == QStringLiteral("tt2000000"));
    }

    const auto* rnd = findByPathSuffix(scanned, "random.mkv");
    CHECK(rnd != nullptr);
    if (rnd) {
        CHECK(rnd->kind == LocalLibrary::Kind::Movie);
        CHECK(rnd->title == QStringLiteral("random"));
        CHECK(rnd->year == 0);
    }

    const LocalLibrary::OwnedIndex idx = LocalLibrary::buildIndex(scanned);
    CHECK(idx.all().size() == 4);
    CHECK(idx.ownsId(QStringLiteral("tt1375666")));                       // movie
    if (inc) CHECK(idx.localPathFor(QStringLiteral("tt1375666")) == inc->path);
    CHECK(idx.ownsId(QStringLiteral("tt2000000")));                       // series (by count)
    CHECK(idx.ownedEpisodes(QStringLiteral("tt2000000")) == 1);
    if (ep) CHECK(idx.localPathFor(QStringLiteral("tt2000000:1:2")) == ep->path);
    CHECK(!idx.ownsId(QStringLiteral("tt9999999")));                      // not owned

    if (inc) CHECK(LocalLibrary::displayTitle(*inc) == QStringLiteral("Inception (2010)"));
    if (ep)  CHECK(LocalLibrary::displayTitle(*ep)  == QStringLiteral("Show S01E02"));

    // Empty / missing root => empty scan (feature-dormant contract).
    CHECK(LocalLibrary::scanFolder(QString()).isEmpty());
    CHECK(LocalLibrary::scanFolder(root + "/does-not-exist").isEmpty());

    if (failures == 0) { std::puts("LOCALLIB-OK"); return 0; }
    std::fprintf(stderr, "LOCALLIB: %d check(s) failed\n", failures);
    return 1;
}
```

- [ ] **Step 2: Wire the probe into CMake.** In `native/CMakeLists.txt`, after the `probe_browse` block (`:449-453`), add:

```cmake
    # Headless test for the local video library core (parse + NFO + scan + OwnedIndex).
    add_executable(probe_locallib tools/probe_locallib.cpp
        src/core/LocalLibrary.cpp src/core/LocalLibrary.h
        src/browse/SyntheticCatalogs.cpp src/browse/SyntheticCatalogs.h
        src/core/MetaCache.cpp src/core/SteamLibrary.cpp src/addons/AddonModels.cpp)
    target_include_directories(probe_locallib PRIVATE src src/core src/addons src/browse)
    target_link_libraries(probe_locallib PRIVATE Qt6::Core Qt6::Network Qt6::Gui)
```

Also add `src/core/LocalLibrary.cpp` and `src/core/LocalLibrary.h` to the **app target** source list (find where `src/core/RomLibrary.cpp` is listed in the `MYMEDIAVAULT_BUILD_APP` target and add the LocalLibrary lines beside it).

- [ ] **Step 3: Verify RED.**

```bash
cmake -S native -B build -G "Visual Studio 18 2026" -A x64 -DMYMEDIAVAULT_BUILD_APP=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 -DMPV_INCLUDE_DIR=C:/mpv-dev/include -DMPV_LIBRARY=C:/mpv-dev/libmpv.lib
cmake --build build --target probe_locallib --parallel
```
Expected: **link error** (`LocalLibrary::scanFolder` etc. undefined) — the header/impl don't exist yet. That is the RED state for a fresh module.

- [ ] **Step 4: Create the header.** `native/src/core/LocalLibrary.h`:

```cpp
// A local VIDEO library (movies + TV) laid out Kodi/Plex-style. The root is Settings::libraryFolder
// (default <data>/library). We walk it, classify each video by filename convention, read Kodi .nfo
// sidecars for the imdb id + plot + thumb, and surface each file as a first-class MediaItem. The pure
// functions (parseFile/scanFolder/buildIndex) take an explicit root and are probe-tested; the cached
// index convenience layer (Task 3) reads Settings and is main-thread only.
#pragma once
#include <QString>
#include <QVector>
#include <QHash>

namespace LocalLibrary
{
    enum class Kind { Movie, Episode };

    struct VideoEntry
    {
        QString path;
        Kind    kind = Kind::Movie;
        QString title;
        int     year = 0;
        QString show;
        int     season = 0;
        int     episode = 0;
        QString imdbId;
        QString seriesImdbId;
        QString plot;
        QString thumbPath;
    };

    struct OwnedIndex
    {
        QHash<QString, QString> pathById;
        QHash<QString, int>     seriesCount;
        QVector<VideoEntry>     entries;

        bool    ownsId(const QString& id) const { return pathById.contains(id) || seriesCount.contains(id); }
        QString localPathFor(const QString& id) const { return pathById.value(id); }
        int     ownedEpisodes(const QString& seriesId) const { return seriesCount.value(seriesId); }
        const QVector<VideoEntry>& all() const { return entries; }
    };

    // Pure (probe-tested), root explicit.
    VideoEntry          parseFile(const QString& path);
    QVector<VideoEntry> scanFolder(const QString& root);
    OwnedIndex          buildIndex(const QVector<VideoEntry>& entries);
    QString             displayTitle(const VideoEntry& e);
}
```

- [ ] **Step 5: Create the implementation.** `native/src/core/LocalLibrary.cpp`:

```cpp
#include "LocalLibrary.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>
#include <QXmlStreamReader>

namespace LocalLibrary
{
namespace
{
    const QStringList kVideoExt = {
        QStringLiteral("mkv"), QStringLiteral("mp4"), QStringLiteral("avi"), QStringLiteral("m4v"),
        QStringLiteral("mov"), QStringLiteral("webm"), QStringLiteral("mpg"), QStringLiteral("mpeg"),
        QStringLiteral("ts"),  QStringLiteral("wmv"), QStringLiteral("flv")
    };

    QString cleanName(QString s)
    {
        s.replace(QLatin1Char('.'), QLatin1Char(' '));
        s.replace(QLatin1Char('_'), QLatin1Char(' '));
        return s.simplified();
    }

    // Reads a Kodi .nfo, filling imdbId/plot/thumbPath on `e`. Malformed/empty => returns false, e untouched.
    bool readNfo(const QString& nfoPath, VideoEntry& e)
    {
        QFile f(nfoPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        QXmlStreamReader xml(&f);
        QString imdb, uniqueImdb, plot, thumb;
        while (!xml.atEnd())
        {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            const QStringView n = xml.name();
            if (n == u"imdbid" && imdb.isEmpty()) imdb = xml.readElementText();
            else if (n == u"uniqueid")
            {
                const QString type = xml.attributes().value(QStringLiteral("type")).toString();
                const QString v = xml.readElementText();
                if (type.compare(QStringLiteral("imdb"), Qt::CaseInsensitive) == 0 && uniqueImdb.isEmpty())
                    uniqueImdb = v;
            }
            else if (n == u"plot" && plot.isEmpty()) plot = xml.readElementText();
            else if (n == u"thumb" && thumb.isEmpty()) thumb = xml.readElementText();
        }
        QString id = !uniqueImdb.isEmpty() ? uniqueImdb : imdb;
        if (id.isEmpty() && plot.isEmpty() && thumb.isEmpty()) return false;  // nothing useful (or malformed)
        if (!id.isEmpty())
        {
            if (!id.startsWith(QStringLiteral("tt"))) id = QStringLiteral("tt") + id;
            e.imdbId = id;
        }
        if (!plot.isEmpty()) e.plot = plot;
        if (!thumb.isEmpty())
        {
            if (!thumb.startsWith(QStringLiteral("http")) && !QFileInfo(thumb).isAbsolute())
                thumb = QFileInfo(nfoPath).dir().absoluteFilePath(thumb);
            e.thumbPath = thumb;
        }
        return true;
    }
}

VideoEntry parseFile(const QString& path)
{
    const QFileInfo fi(path);
    VideoEntry e; e.path = path;
    const QString base = fi.completeBaseName();

    static const QRegularExpression reSE(QStringLiteral("[Ss](\\d{1,2})[Ee](\\d{1,2})"));
    static const QRegularExpression reX(QStringLiteral("(?:^|[^\\d])(\\d{1,2})[xX](\\d{1,2})(?:[^\\d]|$)"));
    QRegularExpressionMatch m = reSE.match(base);
    if (!m.hasMatch()) m = reX.match(base);
    if (m.hasMatch())
    {
        e.kind = Kind::Episode;
        e.season = m.captured(1).toInt();
        e.episode = m.captured(2).toInt();
        QString show = cleanName(base.left(m.capturedStart(0)));
        const QString dirName = fi.dir().dirName();
        if (show.isEmpty() || dirName.startsWith(QStringLiteral("Season"), Qt::CaseInsensitive))
        {
            QDir d = fi.dir();
            if (dirName.startsWith(QStringLiteral("Season"), Qt::CaseInsensitive)) d.cdUp();
            const QString fromDir = cleanName(d.dirName());
            if (!fromDir.isEmpty()) show = fromDir;
        }
        e.show = show;
        e.title = base;
        return e;
    }

    static const QRegularExpression reYear(QStringLiteral("^(.*?)[ ._]*\\(?(19\\d{2}|20\\d{2})\\)?"));
    const QRegularExpressionMatch my = reYear.match(base);
    e.kind = Kind::Movie;
    if (my.hasMatch()) { e.title = cleanName(my.captured(1)); e.year = my.captured(2).toInt(); }
    else               { e.title = cleanName(base); e.year = 0; }
    return e;
}

QVector<VideoEntry> scanFolder(const QString& root)
{
    QVector<VideoEntry> out;
    if (root.isEmpty() || !QFileInfo::exists(root)) return out;

    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        const QString ext = QFileInfo(path).suffix().toLower();
        if (!kVideoExt.contains(ext)) continue;

        VideoEntry e = parseFile(path);

        // Sidecar NFO: "<file>.nfo" beside it, else "movie.nfo" in the folder (movies only).
        const QString sidecar = path.left(path.size() - ext.size() - 1) + QStringLiteral(".nfo");
        if (QFileInfo::exists(sidecar)) readNfo(sidecar, e);
        else if (e.kind == Kind::Movie)
        {
            const QString movieNfo = QFileInfo(path).dir().absoluteFilePath(QStringLiteral("movie.nfo"));
            if (QFileInfo::exists(movieNfo)) readNfo(movieNfo, e);
        }

        // Episode: series imdb id from tvshow.nfo (this dir, then up one level).
        if (e.kind == Kind::Episode)
        {
            QDir d = QFileInfo(path).dir();
            for (int up = 0; up < 2; ++up)
            {
                const QString tv = d.absoluteFilePath(QStringLiteral("tvshow.nfo"));
                VideoEntry s;
                if (QFileInfo::exists(tv) && readNfo(tv, s) && !s.imdbId.isEmpty())
                {
                    e.seriesImdbId = s.imdbId;
                    break;
                }
                if (!d.cdUp()) break;
            }
        }

        out.push_back(e);
    }
    return out;
}

OwnedIndex buildIndex(const QVector<VideoEntry>& entries)
{
    OwnedIndex idx;
    idx.entries = entries;
    for (const VideoEntry& e : entries)
    {
        if (e.kind == Kind::Movie && !e.imdbId.isEmpty())
            idx.pathById.insert(e.imdbId, e.path);
        else if (e.kind == Kind::Episode && !e.seriesImdbId.isEmpty())
        {
            const QString epKey = e.seriesImdbId + QStringLiteral(":")
                                + QString::number(e.season) + QStringLiteral(":")
                                + QString::number(e.episode);
            idx.pathById.insert(epKey, e.path);
            idx.seriesCount[e.seriesImdbId] += 1;
        }
    }
    return idx;
}

QString displayTitle(const VideoEntry& e)
{
    if (e.kind == Kind::Episode)
        return QStringLiteral("%1 S%2E%3")
            .arg(e.show.isEmpty() ? QObject::tr("Episode") : e.show)
            .arg(e.season, 2, 10, QLatin1Char('0'))
            .arg(e.episode, 2, 10, QLatin1Char('0'));
    return e.year > 0 ? QStringLiteral("%1 (%2)").arg(e.title).arg(e.year) : e.title;
}

} // namespace LocalLibrary
```

- [ ] **Step 6: Build + verify GREEN.**

```bash
cmake --build build --target probe_locallib --parallel
BUILD_DIR=build bash native/tools/run-headless-probes.sh 2>&1 | grep -i locallib
```
Expected: `probe_locallib ... LOCALLIB-OK`.

- [ ] **Step 7: Wire the runner + CI, run full suite.** In `native/tools/run-headless-probes.sh:119`, append `"probe_locallib LOCALLIB-OK"` to the `for p in ...` list. In `.github/workflows/ci.yml:52`, append `probe_locallib` to the `cmake --build build --target ...` list. Then:

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh   # expect ALL HEADLESS PROBES PASSED (incl. probe_locallib)
```

- [ ] **Step 8: Commit.**

```bash
git add native/src/core/LocalLibrary.h native/src/core/LocalLibrary.cpp native/tools/probe_locallib.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: LocalLibrary pure core (parse+nfo+scan+index) + probe_locallib (locallib T2)"
```

---

### Task 3: Cached index + async startup scan + the "Local Library" browse folder

**Files:**
- Modify: `native/src/core/LocalLibrary.h` / `.cpp` (add the cached-index convenience layer)
- Modify: `native/src/browse/SyntheticCatalogs.h` / `.cpp` (add `localLibraryCatalog`)
- Modify: `native/src/ui/HomeView.h` / `.cpp` (the `_locallib` synthetic folder, `openLocalLibraryLevel`/`populateLocalLibrary`, dispatch, `onLocalLibraryChanged`)
- Modify: `native/src/ui/MainWindow.cpp` (kick the async scan at startup)
- Modify: `native/tools/probe_locallib.cpp` (assert the browse builder mapping)

**Interfaces:**
- Consumes: Task 2's `LocalLibrary::{scanFolder,buildIndex,displayTitle,OwnedIndex,VideoEntry}`; `Settings::libraryFolder()` (Task 1).
- Produces:
  - `QString LocalLibrary::root()` (= `Settings::libraryFolder()`); `void LocalLibrary::installIndex(OwnedIndex)`; `const OwnedIndex& LocalLibrary::index()` (main-thread cache).
  - `MediaCatalog browse::localLibraryCatalog(const QVector<LocalLibrary::VideoEntry>& entries)`.
  - `void HomeView::onLocalLibraryChanged()` — refreshes the current level if the video category root is showing.

- [ ] **Step 1: Add the failing browse assertion.** In `native/tools/probe_locallib.cpp`, before the final sentinel block, add (and `#include "SyntheticCatalogs.h"`):

```cpp
    // Browse builder: each VideoEntry -> a playable MediaItem (url=path, mime=local:video), stable id.
    const MediaCatalog cat = browse::localLibraryCatalog(scanned);
    CHECK(cat.items.size() == 4);
    {
        const MediaItem* mi = nullptr;
        for (const auto& x : cat.items) if (x.url.endsWith("Inception (2010).mkv")) mi = &x;
        CHECK(mi != nullptr);
        if (mi) {
            CHECK(mi->mime == QStringLiteral("local:video"));
            CHECK(mi->id == QStringLiteral("tt1375666"));          // known imdb id
            CHECK(mi->title == QStringLiteral("Inception (2010)"));
        }
        const MediaItem* rnd = nullptr;
        for (const auto& x : cat.items) if (x.url.endsWith("random.mkv")) rnd = &x;
        CHECK(rnd != nullptr);
        if (rnd) CHECK(rnd->id.startsWith(QStringLiteral("local:")));   // no imdb id -> local:<path> key
    }
```

- [ ] **Step 2: Build the probe, verify FAIL** (`browse::localLibraryCatalog` undefined):

```bash
cmake --build build --target probe_locallib --parallel
```
Expected: link/compile error — `localLibraryCatalog` not declared.

- [ ] **Step 3: Add the browse builder.** In `native/src/browse/SyntheticCatalogs.h`, after the `downloadsCatalog` declaration (`:30-31`), add:

```cpp
    // Build a "Local Library" catalog from scanned local video entries. Each becomes a playable
    // MediaItem (url = local path, mime = "local:video"); id = imdb id when known, else "local:<path>".
    MediaCatalog localLibraryCatalog(const QVector<LocalLibrary::VideoEntry>& entries);
```

Add `#include "LocalLibrary.h"` to `SyntheticCatalogs.h`'s includes. In `native/src/browse/SyntheticCatalogs.cpp`, after `downloadsCatalog`'s body (`:77`), add:

```cpp
MediaCatalog localLibraryCatalog(const QVector<LocalLibrary::VideoEntry>& entries)
{
    MediaCatalog cat; cat.title = QObject::tr("Local Library");
    for (const LocalLibrary::VideoEntry& e : entries)
    {
        MediaItem it;
        it.url = e.path;
        it.mime = QStringLiteral("local:video");
        it.type = QStringLiteral("movie");                 // both movies and episodes render as video tiles
        it.id = e.imdbId.isEmpty() ? (QStringLiteral("local:") + e.path) : e.imdbId;
        it.title = LocalLibrary::displayTitle(e);
        it.subtitle = e.plot;
        // Offline-first: a local NFO <thumb> is a file path; MetaCache::displayImage serves it if present.
        it.thumbnailUrl = MetaCache::displayImage(it.id, e.thumbPath);
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}
```

- [ ] **Step 4: Rebuild the probe, verify PASS.**

```bash
cmake --build build --target probe_locallib --parallel && BUILD_DIR=build bash native/tools/run-headless-probes.sh 2>&1 | grep -i locallib
```
Expected: `LOCALLIB-OK`.

- [ ] **Step 5: Add the cached-index convenience layer.** In `native/src/core/LocalLibrary.h`, add to the namespace (after `displayTitle`):

```cpp
    // Cached process-wide index (main-thread only). root() reads Settings::libraryFolder().
    QString            root();
    void               installIndex(OwnedIndex idx);
    const OwnedIndex&  index();
```

In `native/src/core/LocalLibrary.cpp`, add `#include "Settings.h"` and, inside `namespace LocalLibrary`, a file-static cache + accessors:

```cpp
namespace { OwnedIndex g_index; }

QString root() { return Settings::libraryFolder(); }
void installIndex(OwnedIndex idx) { g_index = std::move(idx); }
const OwnedIndex& index() { return g_index; }
```

(Because `root()` now reads `Settings`, add `src/core/Settings.cpp` to the `probe_locallib` CMake sources — the probe never calls `root()`/`index()`, but the translation unit references `Settings::libraryFolder`. Add `src/core/Settings.cpp src/core/AppPaths.cpp` to the `probe_locallib add_executable(...)` list; if that pulls further symbols, add them until it links — mirror `probe_browse`'s set.)

- [ ] **Step 6: Kick the async scan at startup.** In `native/src/ui/MainWindow.cpp`, in the constructor after the main window is constructed and `home_`/`homeView_` exists (near where the startup cloud-pull is armed), add an off-thread scan that installs on the main thread:

```cpp
    // Local video library: scan off-thread at startup, install the index + refresh the home on the main
    // thread. Dormant (instant, empty) when no library/folder is configured.
    {
        auto* w = new QFutureWatcher<LocalLibrary::OwnedIndex>(this);
        connect(w, &QFutureWatcher<LocalLibrary::OwnedIndex>::finished, this, [this, w] {
            LocalLibrary::installIndex(w->result());
            if (homeView_) homeView_->onLocalLibraryChanged();
            w->deleteLater();
        });
        w->setFuture(QtConcurrent::run([] {
            return LocalLibrary::buildIndex(LocalLibrary::scanFolder(LocalLibrary::root()));
        }));
    }
```

Add includes to `MainWindow.cpp`: `#include "LocalLibrary.h"`, `#include <QFutureWatcher>`, `#include <QtConcurrent>`. Ensure `Qt6::Concurrent` is linked by the app target (add to `target_link_libraries` for the app in `native/CMakeLists.txt` if not already present). Use the actual HomeView member name found at the injection site (the scout cited `homeView_`/`home_`; grep the ctor to confirm which pointer the class uses and match it).

- [ ] **Step 7: Add the synthetic "Local Library" folder.** In `native/src/ui/HomeView.cpp`, in the catalogue-root `pushFolders({...})` list (`:4193-4197`), add a fourth entry gated on the video category:

```cpp
            const bool isVideo = (rkind == QStringLiteral("video"));
            pushFolders({
                { QLatin1String("_recents"),   tr("Recent"),        QStringLiteral("recents:") + rkind,                  hasRecents },
                { QLatin1String("_downloads"), tr("Downloaded"),    QStringLiteral("downloads:") + rkind + QLatin1Char('|'), hasDownloads },
                { QLatin1String("_locallib"),  tr("Local Library"), QStringLiteral("locallib:") + rkind,                  isVideo && !LocalLibrary::index().all().isEmpty() },
                { QLatin1String("_playlists"), tr("Playlists"),     QStringLiteral("playlists:") + currentCategoryKey(),  true },
            });
```

Add `#include "LocalLibrary.h"` to `HomeView.cpp`.

- [ ] **Step 8: Add the drill dispatch + level.** In `native/src/ui/HomeView.cpp` dispatch block (`:2394-2408`), after the `_downloads` branch, add:

```cpp
    // The synthetic Local Library folder drills into this machine's scanned local videos.
    if (it.type == QStringLiteral("_locallib"))
        { openLocalLibraryLevel(it.mime.mid(QStringLiteral("locallib:").size())); return; }
```

After `populateDownloads` (`:1928`), add the level + populate pair (mirroring `openDownloadsLevel`):

```cpp
void HomeView::openLocalLibraryLevel(const QString& marker)
{
    if (xmbMode_) { atXmbRoot_ = false; if (xmb_) xmb_->setAtRoot(false); }
    Level lvl;
    lvl.addon = nullptr; lvl.detail = true; lvl.title = tr("Local Library");
    lvl.item.id = QStringLiteral("_locallib");
    lvl.item.type = QStringLiteral("_locallib");
    lvl.item.expandable = true;
    lvl.item.mime = QStringLiteral("locallib:") + marker; // so loadTop() repopulates on Back
    stack_.push_back(lvl);
    populateLocalLibrary(marker);
}

void HomeView::populateLocalLibrary(const QString& /*marker*/)
{ showSyntheticCatalog(browse::localLibraryCatalog(LocalLibrary::index().all())); }

void HomeView::onLocalLibraryChanged()
{
    // If the video category root (or the Local Library level) is currently showing, rebuild it so the
    // folder appears / refreshes once the async scan lands. Cheap no-op otherwise.
    if (!stack_.isEmpty() && stack_.last().item.type == QStringLiteral("_locallib"))
        populateLocalLibrary(stack_.last().item.mime.mid(QStringLiteral("locallib:").size()));
    else
        loadTop();  // re-evaluate the root's synthetic folders (the pushFolders present-checks)
}
```

Declare `openLocalLibraryLevel(const QString&)`, `populateLocalLibrary(const QString&)`, and `onLocalLibraryChanged()` in `native/src/ui/HomeView.h` (beside `openDownloadsLevel`/`populateDownloads`, ~`:269-270`). Confirm `loadTop()` is the existing root-repopulation method (the scout cited it as the Back path); if the class uses a differently-named refresh, call that instead.

- [ ] **Step 9: Build the app, full suite, live smoke.**

```bash
cmake --build build --target MyMediaVault --parallel
BUILD_DIR=build bash native/tools/run-headless-probes.sh   # ALL HEADLESS PROBES PASSED
```
Live smoke (uitest, per the verify-app-gui-capture memory — launch with `MMV_UITEST=1`, drive via `native/tools/uitest.py`; seed a throwaway `library/folder` pointing at a 2-3 file fixture tree in the scratchpad; do NOT touch the real deployed ini): open the video category → the **Local Library** folder appears → drill in → the fixture videos list → selecting a plain (no-nfo) file plays it from disk through mpv. Screenshot `locallib-browse.png`, `locallib-play.png`.

- [ ] **Step 10: Commit.**

```bash
git add native/src/core/LocalLibrary.h native/src/core/LocalLibrary.cpp native/src/browse/SyntheticCatalogs.h native/src/browse/SyntheticCatalogs.cpp native/src/ui/HomeView.h native/src/ui/HomeView.cpp native/src/ui/MainWindow.cpp native/CMakeLists.txt native/tools/probe_locallib.cpp
git commit -m "feat: cached index + async startup scan + Local Library browse folder (locallib T3)"
```

---

### Task 4: Seam A (on-disk badge) + Seam B (prefer-local playback)

**Files:**
- Modify: `native/src/ui/HomeView.cpp:1397-1409` (Seam A: decorate the row map)
- Modify: the grid delegate QML under `native/src/theme2/` (render the badge when `onDisk`)
- Modify: `native/src/ui/MainWindow.cpp` (Seam B: prefer-local redirect at the top of `openLibraryItem`)
- Modify: `native/tools/probe_locallib.cpp` (assert the pure decoration decision)

**Interfaces:**
- Consumes: `LocalLibrary::index()`, `OwnedIndex::{ownsId,localPathFor,ownedEpisodes}` (Tasks 2-3).
- Produces: row-map keys `onDisk` (bool) + `onDiskCount` (int, series only); the prefer-local activation redirect. No new types.

- [ ] **Step 1: Add the failing decoration-decision assertion.** The decoration is pure — it's `OwnedIndex` lookups on an id. In `native/tools/probe_locallib.cpp`, after the browse assertions, pin the exact decisions Seam A/B rely on:

```cpp
    // Seam A/B decision surface (pure): movie id owned -> local path; series id owned -> episode count;
    // episode key owned -> its file; unknown -> not owned.
    CHECK(idx.ownsId(QStringLiteral("tt1375666")) == true);
    CHECK(!idx.localPathFor(QStringLiteral("tt1375666")).isEmpty());
    CHECK(idx.ownedEpisodes(QStringLiteral("tt2000000")) == 1);
    CHECK(!idx.localPathFor(QStringLiteral("tt2000000:1:2")).isEmpty());
    CHECK(idx.ownsId(QStringLiteral("tt0000001")) == false);
    CHECK(idx.localPathFor(QStringLiteral("tt0000001")).isEmpty());
```

- [ ] **Step 2: Verify FAIL-or-PASS honestly.** These call already-implemented methods, so the probe should PASS after rebuild — this step pins the contract Seam A/B consume (a regression in `OwnedIndex` would now break `probe_locallib`, not just the UI). Run:

```bash
cmake --build build --target probe_locallib --parallel && BUILD_DIR=build bash native/tools/run-headless-probes.sh 2>&1 | grep -i locallib
```
Expected: `LOCALLIB-OK`. (If any assertion fails, the index contract regressed — fix before proceeding.)

- [ ] **Step 3: Seam A — decorate the row map.** In `native/src/ui/HomeView.cpp`, in `browseItems()` immediately BEFORE `it.art.writeInto(m);` (`:1408`), add:

```cpp
        // Local library: if we own this catalog item on disk, flag it so the delegate shows an "On disk"
        // badge (and the count for a series). Purely additive — un-owned tiles are untouched.
        if (!it.id.isEmpty() && LocalLibrary::index().ownsId(it.id))
        {
            m[QStringLiteral("onDisk")] = true;
            const int eps = LocalLibrary::index().ownedEpisodes(it.id);
            if (eps > 0) m[QStringLiteral("onDiskCount")] = eps;
        }
```

- [ ] **Step 4: Render the badge in the delegate.** Find the grid delegate QML that binds the row map (grep `native/src/theme2` for a delegate reading `model.expandable` or `model.image` — it's the tile used by the browse grid). Add a small corner badge shown when `onDisk` is true. Example (adapt property/anchor names to the delegate's existing style tokens):

```qml
    // "On disk" badge for locally-owned items (LocalLibrary Seam A).
    Rectangle {
        visible: !!model.onDisk
        anchors { top: parent.top; right: parent.right; margins: 4 }
        radius: 3
        color: Qt.rgba(0, 0, 0, 0.65)
        implicitWidth: badgeText.implicitWidth + 10
        implicitHeight: badgeText.implicitHeight + 4
        Text {
            id: badgeText
            anchors.centerIn: parent
            text: model.onDiskCount ? ("● " + model.onDiskCount) : "● ON DISK"
            color: "white"
            font.pixelSize: 10
            font.bold: true
        }
    }
```

If the browse grid is a non-QML (QWidget) delegate on this path, add the equivalent painted badge in that delegate's paint method instead; keep the visual minimal. Note in the report which delegate file was edited.

- [ ] **Step 5: Seam B — prefer-local redirect.** In `native/src/ui/MainWindow.cpp`, at the TOP of `openLibraryItem` (`:6522`), immediately after the empty-url guard (`:6531-6536`), add the redirect:

```cpp
    // Local library prefer-local: if we own this item on disk, play the local file instead of resolving
    // a stream. Guard on mime so the re-entry (with a filesystem url) doesn't recurse. Movies key on id;
    // episodes key on imdbStreamId ("ttShow:season:episode", matches the OwnedIndex episode key).
    if (item.mime != QStringLiteral("local:video"))
    {
        QString localPath = LocalLibrary::index().localPathFor(item.id);
        if (localPath.isEmpty() && !item.imdbStreamId.isEmpty())
            localPath = LocalLibrary::index().localPathFor(item.imdbStreamId);
        if (!localPath.isEmpty() && QFileInfo::exists(localPath))
        {
            MediaItem local = item;
            local.url = localPath;
            local.mime = QStringLiteral("local:video");
            openLibraryItem(local);   // re-enter: filesystem url + local:video mime -> mpv branch
            return;
        }
    }
```

Add `#include "LocalLibrary.h"` and `#include <QFileInfo>` to `MainWindow.cpp` if not already present. Verify the re-entry reaches the mpv else-branch (`:6717`): `local:video` mime matches none of goggame/epic/steam/pcgame/m3u/http, `type` stays `movie`/`series`/`episode` (not a ROM extension), so it falls through to `player_->play(url)`. A `series` container item is `expandable` and drills into detail rather than reaching `openLibraryItem` for direct play, so the series-tile badge (Seam A) is informational; episode-level prefer-local fires when an episode leaf is activated with its `imdbStreamId` set.

- [ ] **Step 6: Build + full suite + live smoke.**

```bash
cmake --build build --target MyMediaVault probe_locallib --parallel
BUILD_DIR=build bash native/tools/run-headless-probes.sh   # ALL HEADLESS PROBES PASSED
```
Live (best-effort, per spec's honest posture): with the fixture `library/folder` set AND a fixture whose imdb id matches a tile in an installed addon catalog present, open that catalog → the owned tile shows the **On disk** badge → activating it plays the local file (not a stream). If no matching addon tile is available on this machine, record that Seam A/B are pinned by `probe_locallib`'s decision assertions + a code walk, and screenshot the badge on whatever tile can be induced (e.g. via a fixture nfo whose id matches a Cinemeta entry if installed). Screenshot `locallib-badge.png`.

- [ ] **Step 7: Commit.**

```bash
git add native/src/ui/HomeView.cpp native/src/ui/MainWindow.cpp native/src/theme2 native/tools/probe_locallib.cpp
git commit -m "feat: Seam A on-disk badge + Seam B prefer-local playback (locallib T4)"
```

---

### Task 5: Settings folder-picker UI + rescan action

**Files:**
- Modify: `native/src/ui/MainWindow.cpp:8213-8246` (add a "Local Library" section beside the "Game ROMs" block in the settings panel)

**Interfaces:**
- Consumes: `Settings::libraryFolder/setLibraryFolder` (T1); the async-scan idiom (T3); `HomeView::onLocalLibraryChanged` (T3).
- Produces: user-facing folder picker + "Rescan library" that rebuilds the index and refreshes the home.

- [ ] **Step 1: Add the settings section.** In `native/src/ui/MainWindow.cpp`, in the settings-panel builder right after the "Game ROMs" block (`:8213-8246`), add a parallel "Local Library" block (clone the ROMs block's widgets/layout; substitute the library key + a rescan). Extract the async-scan into a reusable lambda/member so the picker and rescan share it:

```cpp
    // --- Local Library (movies + TV) ------------------------------------------------------------------
    {
        auto* row = new QHBoxLayout();
        auto* edit = new QLineEdit(Settings::libraryFolder());
        edit->setReadOnly(true);
        auto* change = new QPushButton(tr("Change…"));
        auto* rescan = new QPushButton(tr("Rescan"));
        row->addWidget(new QLabel(tr("Local Library folder")));
        row->addWidget(edit, 1);
        row->addWidget(change);
        row->addWidget(rescan);
        // (add `row` to the settings panel layout the same way the ROMs row is added)

        auto kickScan = [this] {
            auto* w = new QFutureWatcher<LocalLibrary::OwnedIndex>(this);
            connect(w, &QFutureWatcher<LocalLibrary::OwnedIndex>::finished, this, [this, w] {
                LocalLibrary::installIndex(w->result());
                if (homeView_) homeView_->onLocalLibraryChanged();
                w->deleteLater();
            });
            w->setFuture(QtConcurrent::run([] {
                return LocalLibrary::buildIndex(LocalLibrary::scanFolder(LocalLibrary::root()));
            }));
        };

        connect(change, &QPushButton::clicked, this, [this, edit, kickScan] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your local video library folder"),
                                                                  Settings::libraryFolder());
            if (dir.isEmpty()) return;
            Settings::setLibraryFolder(dir);
            edit->setText(dir);
            kickScan();
        });
        connect(rescan, &QPushButton::clicked, this, [kickScan] { kickScan(); });
    }
```

(If the settings panel is themed/QML rather than QWidget on this path, mirror the ROMs block's actual construction — the point is: read-only path field + "Change…" (`QFileDialog::getExistingDirectory` → `setLibraryFolder` → rescan) + "Rescan". Reuse the ROMs block's exact widget/theme idiom found at `:8213-8246`.) Consider promoting `kickScan` to a private `MainWindow::rescanLocalLibrary()` method and calling it from both here and the T3 startup site to keep the async-scan idiom in one place (DRY).

- [ ] **Step 2: Build + suite.**

```bash
cmake --build build --target MyMediaVault --parallel
BUILD_DIR=build bash native/tools/run-headless-probes.sh   # ALL HEADLESS PROBES PASSED
```

- [ ] **Step 3: Live verify (uitest, throwaway ini).** Open Settings → the **Local Library folder** row shows → "Change…" picks the fixture tree → the Local Library folder now appears in the video category → "Rescan" after adding a file to the fixture surfaces it. Screenshot `locallib-settings.png`. Snapshot/restore any real ini touched; use a throwaway data-dir so the real deployed app is untouched.

- [ ] **Step 4: Commit.**

```bash
git add native/src/ui/MainWindow.cpp native/src/ui/MainWindow.h
git commit -m "feat: Local Library settings folder-picker + rescan (locallib T5)"
```

---

### Task 6: Close-out — spec status, gates, fable, merge

- [ ] **Step 1: Spec status → complete.** Update `docs/superpowers/specs/2026-07-23-local-media-library-design.md` Status to complete, recording: desktop-verified (browse + play + settings); Seam A/B pinned by `probe_locallib` + code walk with a live badge demo where a matching addon tile was inducible (else honestly recorded, the game-importers posture); the network id-resolver remains the flagged follow-up. Note any residual follow-ups discovered.
- [ ] **Step 2: Full gates.** `BUILD_DIR=build bash native/tools/run-headless-probes.sh` (ALL PASSED, incl. `probe_locallib`). Perf baseline 3 runs → `docs/superpowers/perf/2026-07-23-locallib-baseline.md`; confirm `nav.select` stays flat (the only steady-state add is Seam A's per-row `OwnedIndex` hash lookup; startup adds one off-thread scan off the hot path). Record numbers honestly.
- [ ] **Step 3: Fable whole-branch review.** Run `scripts/review-package $(git merge-base main HEAD) HEAD` and dispatch the final reviewer (most capable model) with the printed path. Review dimensions: the dormant-when-unset contract (no folder → zero cost, no folder shown, no decoration); Seam A additivity (un-owned tiles byte-identical; keys added before `writeInto` never clobber reserved keys); Seam B non-recursion + not intercepting ROM/game/http items; the async-scan thread-safety (scan off-thread, `installIndex`/`index` main-thread only — no data race on `g_index`); the NFO parser's malformed-input safety (never aborts a scan); the id-key formats matching between `buildIndex` (`seriesId:S:E`) and Seam B (`imdbStreamId`); the fixture-only live posture recorded honestly.
- [ ] **Step 4: Fix rounds** (one fix subagent per review wave with the full findings list), re-review until clean.
- [ ] **Step 5: Merge + push + redeploy.** Merge `local/video-library` → main (resolve any version-line conflict by taking the higher patch), rebuild the combined tree, full suite green, push, delete the branch, redeploy Release to `C:\MyMediaVault-app` (md5-verify the exe), update `.superpowers/sdd/progress.md`, mark the chapter.

## Self-Review (done at write time)

- **Spec coverage:** `Settings::libraryFolder` + carve-out ✅T1; `LocalLibrary` scanner + `NfoReader` + `OwnedIndex` (QtCore-only, injectable root, `probe_locallib` RED-first) ✅T2; cached index + async startup + `localLibraryCatalog` synthetic folder ✅T3; Seam A render-time badge + Seam B prefer-local ✅T4; settings folder-picker UI + rescan ✅T5; live verify + fable + merge ✅T6. NFO-first matching ✅T2 (readNfo ahead of filename); local-only degradation (no id → `local:<path>` key, browse tile, playable) ✅T2/T3; dormant-when-unset ✅ every task. Non-goals (music/books/network-resolver/subtitles/persistence/quality-rank) not built ✅.
- **Placeholder scan:** no TBD/TODO; every code step carries full code; the two UI-delegate specifics (grid badge QML, settings block) name the exact anchor to clone and give example code, with the implementer pinning the delegate file by grep (the plan cannot embed a file it must locate, but names the search + the binding keys).
- **Type consistency:** `VideoEntry`/`OwnedIndex`/`Kind` fields, `scanFolder`/`buildIndex`/`parseFile`/`displayTitle`/`root`/`installIndex`/`index`, `localLibraryCatalog`, `onDisk`/`onDiskCount` map keys, `openLocalLibraryLevel`/`populateLocalLibrary`/`onLocalLibraryChanged`, the `local:video` mime, and the `seriesId:S:E` episode key are used identically across T2-T5. Seam B's episode key (`imdbStreamId`) matches `buildIndex`'s `seriesImdbId:season:episode` format.
- **Ambiguity resolved:** the index lives as a main-thread-only file-static cache (scan off-thread returns a value, `installIndex` swaps on the main thread — no lock); Seam B is a redirect (a filesystem `url` already reaches the mpv branch), not new playback plumbing; series tiles badge-only (containers drill), episode leaves prefer-local via `imdbStreamId`; the badge is a delegate addition the implementer pins by grep.
