# Subtitle Accuracy + Picker Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make subtitle auto-matching accurate for local-library files (exact IMDB ids + OpenSubtitles `moviehash`), stop replays re-downloading (a persisted cache), and add a "Search subtitles…" picker so a wrong auto-pick can be corrected and the correction sticks.

**Architecture:** Two new pure/persisted cores (`SubtitleHash` = the OSDb hash; `SubtitleCache` = a device-local key→srt JSON, modeled on `LocalResolveCache`), a widened `SubtitleFetcher` (priority chain cache → moviehash → imdb → title, plus `searchList`/`downloadChoice` that expose the candidate array it already parses and discards), a one-line `imdbStreamId` plumbing fix on local-library items, and a themed picker in the existing subtitle overlay.

**Tech Stack:** Qt 6.8.3 (QFile, QDataStream/manual LE reads, QJson, QNetworkAccessManager), the shipped `SubtitleFetcher`/`MpvWidget::addSubtitle`/subtitle-overlay plumbing, headless probes.

## Global Constraints

- **Branch:** `local/subtitle-accuracy` off main. Standing autonomy through the merge gate. The pre-commit hook auto-bumps the patch version — expected; never hand-edit version lines.
- **DO NOT REBUILD what already ships** (spec §"Already shipped"): the OpenSubtitles REST v1 client's transport (`makeRequest`/`ensureLogin`/`download` HTTP), the credentials + language Settings and their two UI surfaces, the `armSubtitleFetch` → `fileLoaded` auto-fetch gate, `MpvWidget::addSubtitle` (`sub-add … select`), and mpv's `sub-auto=fuzzy` sidecar loading. This track only *extends* them.
- **Scope:** the four gaps only. **NO multi-language** (user explicitly skipped — `subs/language` stays a single code), no subtitle editing/resync beyond the shipped delay+scale, no non-OpenSubtitles providers, no burn-in/transcoding.
- **The auto-fetch gate is preserved:** a sidecar `.srt` or an embedded track still means `hasSub` is true ⇒ NO download. Never regress that.
- **ANCHOR ON FUNCTION NAMES.** Current code (main@fb3c2b4):

| Concern | Anchor |
|---|---|
| Fetch entry + chain to widen | `SubtitleFetcher::fetch(imdbStreamId, title, langCode, cb)` `SubtitleFetcher.cpp:70-118`; builds `primary` (imdb) + `fallback` (title) `QUrlQuery`s, adds `languages`, then `ensureLogin` → `searchQuery(primary)` → `download` → else `searchQuery(fallback)` |
| Candidate array (discarded today) | `SubtitleFetcher::searchQuery` `:147-179` — parses `data[].attributes` for `files[0].file_id`, `language`, `download_count`, keeps only the winner |
| Download + on-disk path | `SubtitleFetcher::download` `:181-237`; writes `QStandardPaths::CacheLocation + "/subs/<lang>-<fileId>.srt"` `:210-213` |
| Login/host/token | `ensureLogin` `:120-145`; `makeRequest` `:55-68` (`Api-Key` header, Bearer); `apiLang` `:24-40` (3→2 letter, default `"en"`) |
| Configured gate | `SubtitleFetcher::configured()` `:46-51` (key AND user AND pass) |
| Auto-fetch arm + trigger | `MainWindow::armSubtitleFetch` `MainWindow.cpp:6192-6205`; the `fileLoaded` lambda `:293-321` (fires only when `!hasSub`) |
| Overlay button to sit beside | `MainWindow.cpp:6592-6606` (`🔍  Download from OpenSubtitles`, `rowButton`/`rightCol`/`subRightCol_` idiom) |
| Local items missing the id | `browse::localLibraryCatalog` `SyntheticCatalogs.cpp:81-99` (`it.id = e.imdbId.isEmpty() ? "local:"+e.path : e.imdbId;` — never sets `imdbStreamId`) |
| Cache model to clone | `native/src/core/LocalResolveCache.{h,cpp}` (JSON load/save, `AppPaths::dataDir()`, probe fixture style) |

- **Env recipe:** PATH prepend `/c/Qt/6.8.3/msvc2022_64/bin` + `/c/mpv-dev`; build dir `build` (generated qt.conf, no `QT_PLUGIN_PATH`). **Harness runs RELEASE — build `--config Release`.** App target `everythingbox`. **Build hygiene:** build ONLY named targets (never target-less); adding a source/probe needs ONE `cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON` (no `-A`) regenerate; >6 min → report BLOCKED. Suite: `BUILD_DIR=build bash native/tools/run-headless-probes.sh`.

---

### Task 1: SubtitleHash + SubtitleCache + probe_subs (RED-first)

**Files:**
- Create: `native/src/core/SubtitleHash.h`, `native/src/core/SubtitleHash.cpp`
- Create: `native/src/core/SubtitleCache.h`, `native/src/core/SubtitleCache.cpp`
- Create: `native/tools/probe_subs.cpp`
- Modify: `native/CMakeLists.txt` (app sources + a new `probe_subs` target), `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml`

**Interfaces (Produces):**
```cpp
// SubtitleHash.h
namespace SubtitleHash {
    // The OpenSubtitles OSDb hash: filesize + the sum of all little-endian uint64 words across the FIRST
    // 64 KiB and the LAST 64 KiB, as 16 lowercase hex digits. Empty when the file is unreadable or < 128 KiB.
    QString ofFile(const QString& path);
    // Pure core: head/tail must each be exactly 65536 bytes; size is the true file size.
    QString ofBytes(const QByteArray& head, const QByteArray& tail, qint64 size);
}
// SubtitleCache.h
class SubtitleCache {
public:
    explicit SubtitleCache(QString filePath);
    void    load();
    void    save() const;
    QString lookup(const QString& key) const;                 // "" if absent OR the recorded .srt is gone
    void    put(const QString& key, const QString& srtPath);  // overwrite-on-put (a picker choice wins)
    void    clear();
    static QString keyFor(const QString& identifier, const QString& lang);   // "<identifier>|<lang>"
};
```

- [ ] **Step 1: Write the failing probe.** Create `native/tools/probe_subs.cpp` (mirrors `probe_locallib.cpp`'s shape: header doc, CHECK macro, `QCoreApplication`, sentinel `SUBS-OK` / `SUBS-FAIL <cond> (line)`):
```cpp
// Headless probe for the subtitle accuracy cores: the OpenSubtitles OSDb hash and the download cache.
// Prints SUBS-OK on success; any failure prints SUBS-FAIL <cond> (line) and exits non-zero.
#include "SubtitleHash.h"
#include "SubtitleCache.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QByteArray>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "SUBS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// An INDEPENDENT reference implementation of the OSDb hash, so the probe proves the real one rather than
// merely agreeing with itself: sum the file size with every little-endian quint64 in the two 64 KiB windows.
static QString refHash(const QByteArray& head, const QByteArray& tail, qint64 size)
{
    quint64 h = quint64(size);
    const auto addAll = [&h](const QByteArray& b) {
        for (int i = 0; i + 8 <= b.size(); i += 8) {
            quint64 w = 0;
            for (int k = 7; k >= 0; --k) w = (w << 8) | quint8(b.at(i + k));   // little-endian
            h += w;
        }
    };
    addAll(head); addAll(tail);
    return QStringLiteral("%1").arg(h, 16, 16, QLatin1Char('0'));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const int W = 65536;

    // --- OSDb hash: pure core matches an independent implementation -------------------------------------
    {
        QByteArray head(W, '\0'), tail(W, '\0');
        for (int i = 0; i < W; ++i) { head[i] = char(i % 251); tail[i] = char((i * 7) % 241); }
        const qint64 size = 734003200;                       // a plausible 700 MiB rip
        CHECK(SubtitleHash::ofBytes(head, tail, size) == refHash(head, tail, size));
        CHECK(SubtitleHash::ofBytes(head, tail, size).size() == 16);           // 16 hex digits, zero-padded
        CHECK(SubtitleHash::ofBytes(head, tail, size) ==
              SubtitleHash::ofBytes(head, tail, size).toLower());              // lowercase
        // Endianness is load-bearing: a byte-swapped window must NOT produce the same hash.
        QByteArray swapped = head;
        for (int i = 0; i + 8 <= swapped.size(); i += 8)
            for (int k = 0; k < 4; ++k) std::swap(swapped[i + k], swapped[i + 7 - k]);
        CHECK(SubtitleHash::ofBytes(swapped, tail, size) != SubtitleHash::ofBytes(head, tail, size));
        // Size participates.
        CHECK(SubtitleHash::ofBytes(head, tail, size + 1) != SubtitleHash::ofBytes(head, tail, size));
    }

    // --- OSDb hash: file path wrapper ------------------------------------------------------------------
    {
        QTemporaryDir tmp; CHECK(tmp.isValid());
        const QString small = tmp.path() + QStringLiteral("/small.mkv");
        { QFile f(small); f.open(QIODevice::WriteOnly); f.write(QByteArray(1000, 'x')); f.close(); }
        CHECK(SubtitleHash::ofFile(small).isEmpty());                  // < 128 KiB ⇒ no valid hash
        CHECK(SubtitleHash::ofFile(tmp.path() + QStringLiteral("/missing.mkv")).isEmpty());

        const QString big = tmp.path() + QStringLiteral("/big.mkv");
        QByteArray head(W, '\0'), mid(4096, 'm'), tail(W, '\0');
        for (int i = 0; i < W; ++i) { head[i] = char(i % 251); tail[i] = char((i * 7) % 241); }
        { QFile f(big); f.open(QIODevice::WriteOnly); f.write(head); f.write(mid); f.write(tail); f.close(); }
        const qint64 sz = qint64(W) * 2 + mid.size();
        CHECK(SubtitleHash::ofFile(big) == refHash(head, tail, sz));   // reads only the two windows
    }

    // --- SubtitleCache --------------------------------------------------------------------------------
    {
        QTemporaryDir tmp; CHECK(tmp.isValid());
        const QString cachePath = tmp.path() + QStringLiteral("/subtitles.json");
        const QString srt = tmp.path() + QStringLiteral("/a.srt");
        { QFile f(srt); f.open(QIODevice::WriteOnly); f.write("1\n"); f.close(); }

        CHECK(SubtitleCache::keyFor(QStringLiteral("tt1375666"), QStringLiteral("en"))
              == QStringLiteral("tt1375666|en"));
        {
            SubtitleCache c(cachePath); c.load();
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))).isEmpty());
            c.put(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en")), srt);
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))) == srt);
            c.save();
        }
        {
            SubtitleCache c(cachePath); c.load();                       // round-trip
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))) == srt);
            // A picker choice OVERWRITES, so the correction sticks on replay.
            const QString srt2 = tmp.path() + QStringLiteral("/b.srt");
            { QFile f(srt2); f.open(QIODevice::WriteOnly); f.write("2\n"); f.close(); }
            c.put(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en")), srt2);
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))) == srt2);
            // A recorded file deleted behind our back reads as a MISS (self-healing ⇒ re-fetch).
            QFile::remove(srt2);
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))).isEmpty());
            c.clear();
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))).isEmpty());
        }
    }

    if (failures == 0) { std::puts("SUBS-OK"); return 0; }
    std::fprintf(stderr, "SUBS: %d check(s) failed\n", failures);
    return 1;
}
```

- [ ] **Step 2: Wire CMake + verify RED.** In `native/CMakeLists.txt`, add `src/core/SubtitleHash.cpp/.h` and `src/core/SubtitleCache.cpp/.h` to the app source list (beside `SubtitleFetcher.cpp`), and add a probe target after the `probe_locallib` block:
```cmake
    # Headless test for the subtitle accuracy cores (OSDb hash + download cache).
    add_executable(probe_subs tools/probe_subs.cpp
        src/core/SubtitleHash.cpp src/core/SubtitleHash.h
        src/core/SubtitleCache.cpp src/core/SubtitleCache.h)
    target_include_directories(probe_subs PRIVATE src src/core)
    target_link_libraries(probe_subs PRIVATE Qt6::Core)
```
Then:
```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON
cmake --build build --target probe_subs --config Release --parallel
```
Expected: compile/link failure (the two headers don't exist). If CMake needs the files present to configure, create them as empty stubs first so the failure is a genuine missing-declaration/unresolved-symbol RED.

- [ ] **Step 3: Implement `SubtitleHash.h`:**
```cpp
// The OpenSubtitles "OSDb" movie hash — the highest-accuracy way to match a subtitle to an EXACT release
// (so the timings actually line up with this rip). It is NOT a cryptographic digest: it is filesize plus the
// sum of every little-endian 64-bit word in the first and last 64 KiB, truncated to 64 bits, printed as 16
// lowercase hex digits. Files under 128 KiB (two windows) have no valid hash.
#pragma once
#include <QByteArray>
#include <QString>

namespace SubtitleHash
{
    QString ofFile(const QString& path);
    // Pure core (probe-tested): head and tail are each exactly 65536 bytes; size is the true file size.
    QString ofBytes(const QByteArray& head, const QByteArray& tail, qint64 size);
}
```

- [ ] **Step 4: Implement `SubtitleHash.cpp`:**
```cpp
#include "SubtitleHash.h"

#include <QFile>

namespace {
constexpr int kWindow = 65536;              // 64 KiB, per the OSDb spec
constexpr qint64 kMinSize = 2 * kWindow;    // a file smaller than both windows has no valid hash

void addWords(quint64& h, const QByteArray& b)
{
    for (int i = 0; i + 8 <= b.size(); i += 8)
    {
        quint64 w = 0;
        for (int k = 7; k >= 0; --k) w = (w << 8) | quint8(b.at(i + k));   // little-endian
        h += w;                                                            // wraps at 64 bits, by design
    }
}
}

namespace SubtitleHash
{
QString ofBytes(const QByteArray& head, const QByteArray& tail, qint64 size)
{
    quint64 h = quint64(size);
    addWords(h, head);
    addWords(h, tail);
    return QStringLiteral("%1").arg(h, 16, 16, QLatin1Char('0'));
}

QString ofFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const qint64 size = f.size();
    if (size < kMinSize) return QString();          // too small to hash
    const QByteArray head = f.read(kWindow);
    if (!f.seek(size - kWindow)) return QString();
    const QByteArray tail = f.read(kWindow);
    if (head.size() != kWindow || tail.size() != kWindow) return QString();
    return ofBytes(head, tail, size);
}
}
```

- [ ] **Step 5: Implement `SubtitleCache.h`:**
```cpp
// Remembers which .srt we already downloaded for a given (identifier, language), so replaying a film does not
// re-hit OpenSubtitles — their free tier has a hard daily download quota. The identifier is whichever thing
// matched: the OSDb hash, the IMDB stream id, or the title. Device-local JSON; never synced (it points at
// local cache paths).
#pragma once
#include <QHash>
#include <QString>

class SubtitleCache
{
public:
    explicit SubtitleCache(QString filePath) : file_(std::move(filePath)) {}
    void load();
    void save() const;
    // The recorded path, or empty when absent OR when the file has since been deleted (self-healing miss).
    QString lookup(const QString& key) const;
    void put(const QString& key, const QString& srtPath);   // overwrite: a picker choice replaces an auto-pick
    void clear();
    static QString keyFor(const QString& identifier, const QString& lang)
    { return identifier + QLatin1Char('|') + lang; }

private:
    QString file_;
    QHash<QString, QString> byKey_;
};
```

- [ ] **Step 6: Implement `SubtitleCache.cpp`:**
```cpp
#include "SubtitleCache.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

void SubtitleCache::load()
{
    byKey_.clear();
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it)
        byKey_.insert(it.key(), it.value().toString());
}

void SubtitleCache::save() const
{
    QJsonObject root;
    for (auto it = byKey_.constBegin(); it != byKey_.constEnd(); ++it) root.insert(it.key(), it.value());
    QFile f(file_);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString SubtitleCache::lookup(const QString& key) const
{
    const QString p = byKey_.value(key);
    if (p.isEmpty() || !QFileInfo::exists(p)) return QString();   // deleted behind our back ⇒ re-fetch
    return p;
}

void SubtitleCache::put(const QString& key, const QString& srtPath)
{
    if (key.isEmpty() || srtPath.isEmpty()) return;
    byKey_.insert(key, srtPath);
    save();
}

void SubtitleCache::clear()
{
    byKey_.clear();
    save();
}
```

- [ ] **Step 7: Build GREEN + wire runner/CI.** `cmake --build build --target probe_subs --config Release --parallel` then `./build/Release/probe_subs.exe` → `SUBS-OK`. Append `"probe_subs SUBS-OK"` to the `for p in …` loop in `native/tools/run-headless-probes.sh`, and `probe_subs` to the build-target list in `.github/workflows/ci.yml`. Full suite: `BUILD_DIR=build bash native/tools/run-headless-probes.sh` → `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 8: Commit.**
```bash
git add native/src/core/SubtitleHash.h native/src/core/SubtitleHash.cpp native/src/core/SubtitleCache.h native/src/core/SubtitleCache.cpp native/tools/probe_subs.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: OSDb subtitle hash + download cache + probe_subs (subs T1)"
```

---

### Task 2: SubtitleFetcher priority chain + searchList/downloadChoice + local imdbStreamId

**Files:** Modify `native/src/core/SubtitleFetcher.h`/`.cpp`; `native/src/browse/SyntheticCatalogs.cpp`; `native/src/ui/MainWindow.cpp` (pass the local path + own the cache); `native/src/ui/MainWindow.h`.

**Interfaces:**
- Consumes T1's `SubtitleHash::ofFile`, `SubtitleCache`.
- Produces:
```cpp
struct SubtitleCandidate { qint64 fileId = 0; QString language; QString release; int downloads = 0; };
// SubtitleFetcher
void fetch(const QString& imdbStreamId, const QString& title, const QString& langCode,
           const QString& localPath, std::function<void(const QString& srtPath)> cb);   // localPath may be empty
void searchList(const QString& imdbStreamId, const QString& title, const QString& langCode,
                const QString& localPath, std::function<void(const QVector<SubtitleCandidate>&)> cb);
void downloadChoice(qint64 fileId, const QString& langCode,
                    std::function<void(const QString& srtPath)> cb);
// The identifier the cache should key on for this request (hash if hashable, else imdb, else title).
static QString cacheIdentifier(const QString& imdbStreamId, const QString& title, const QString& localPath);
```

- [ ] **Step 1: Split the query builder out of `fetch`.** In `SubtitleFetcher.cpp`, extract the `QUrlQuery` construction (`:78-100`) into a private helper so both `fetch` and `searchList` use one chain builder:
```cpp
// Ordered search queries for this request, most precise first: OSDb moviehash (exact release — only when we
// have local bytes), then IMDB id (+season/episode), then a title query. Each already carries languages=.
QStringList SubtitleFetcher::buildQueries(const QString& imdbStreamId, const QString& title,
                                          const QString& lang, const QString& localPath)
{
    QStringList out;
    if (!localPath.isEmpty())
    {
        const QString h = SubtitleHash::ofFile(localPath);
        if (!h.isEmpty())
        {
            QUrlQuery q; q.addQueryItem(QStringLiteral("moviehash"), h);
            q.addQueryItem(QStringLiteral("languages"), lang);
            out << q.toString(QUrl::FullyEncoded);
        }
    }
    if (!imdbStreamId.isEmpty())
    {
        const QStringList parts = imdbStreamId.split(QLatin1Char(':'));
        const QString num = QString(parts.value(0)).remove(QStringLiteral("tt"));
        QUrlQuery q;
        if (parts.size() >= 3)
        {
            q.addQueryItem(QStringLiteral("parent_imdb_id"), num);
            q.addQueryItem(QStringLiteral("season_number"), parts.value(1));
            q.addQueryItem(QStringLiteral("episode_number"), parts.value(2));
        }
        else if (!num.isEmpty()) q.addQueryItem(QStringLiteral("imdb_id"), num);
        if (!q.isEmpty()) { q.addQueryItem(QStringLiteral("languages"), lang); out << q.toString(QUrl::FullyEncoded); }
    }
    if (!title.trimmed().isEmpty())
    {
        QUrlQuery q; q.addQueryItem(QStringLiteral("query"), title.trimmed());
        q.addQueryItem(QStringLiteral("languages"), lang);
        out << q.toString(QUrl::FullyEncoded);
    }
    return out;
}
```
Declare it in the header's private section, add `#include "SubtitleHash.h"`.

- [ ] **Step 2: Rewrite `fetch` to walk the chain.** Replace the primary/fallback body with a recursive walk over `buildQueries` (keeping `ensureLogin`, `searchQuery`, `download` untouched):
```cpp
void SubtitleFetcher::fetch(const QString& imdbStreamId, const QString& title, const QString& langCode,
                            const QString& localPath, std::function<void(const QString&)> cb)
{
    if (!configured()) { cb(QString()); return; }
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    const QString lang = apiLang(langCode);
    const QStringList queries = buildQueries(imdbStreamId, title, lang, localPath);
    if (queries.isEmpty()) { cb(QString()); return; }

    ensureLogin([this, queries, lang, cb](bool ok) {
        if (!ok) { cb(QString()); return; }
        // Walk the queries in precision order; the first one that yields a file id wins.
        auto step = std::make_shared<std::function<void(int)>>();
        *step = [this, queries, lang, cb, step](int i) {
            if (i >= queries.size()) { emit log(QStringLiteral("subs: no match")); cb(QString()); return; }
            searchQuery(queries.at(i), lang, [this, i, lang, cb, step](qint64 fileId) {
                if (fileId > 0) { download(fileId, lang, cb); return; }
                (*step)(i + 1);
            });
        };
        (*step)(0);
    });
}
```
(Add `#include <memory>`.) Keep the OLD 4-arg overload as a thin forwarder with an empty `localPath` so no caller breaks:
```cpp
void SubtitleFetcher::fetch(const QString& imdbStreamId, const QString& title, const QString& langCode,
                            std::function<void(const QString&)> cb)
{ fetch(imdbStreamId, title, langCode, QString(), std::move(cb)); }
```

- [ ] **Step 3: Add `searchList` + `downloadChoice` + `cacheIdentifier`.** In `SubtitleFetcher.cpp`:
```cpp
QString SubtitleFetcher::cacheIdentifier(const QString& imdbStreamId, const QString& title,
                                         const QString& localPath)
{
    if (!localPath.isEmpty())
    {
        const QString h = SubtitleHash::ofFile(localPath);
        if (!h.isEmpty()) return QStringLiteral("hash:") + h;
    }
    if (!imdbStreamId.isEmpty()) return imdbStreamId;
    return QStringLiteral("title:") + title.trimmed();
}

void SubtitleFetcher::searchList(const QString& imdbStreamId, const QString& title, const QString& langCode,
                                 const QString& localPath,
                                 std::function<void(const QVector<SubtitleCandidate>&)> cb)
{
    if (!configured()) { cb({}); return; }
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    const QString lang = apiLang(langCode);
    const QStringList queries = buildQueries(imdbStreamId, title, lang, localPath);
    if (queries.isEmpty()) { cb({}); return; }
    ensureLogin([this, queries, lang, cb](bool ok) {
        if (!ok) { cb({}); return; }
        auto step = std::make_shared<std::function<void(int)>>();
        *step = [this, queries, lang, cb, step](int i) {
            if (i >= queries.size()) { cb({}); return; }
            searchCandidates(queries.at(i), [this, i, cb, step](const QVector<SubtitleCandidate>& list) {
                if (!list.isEmpty()) { cb(list); return; }
                (*step)(i + 1);
            });
        };
        (*step)(0);
    });
}

void SubtitleFetcher::downloadChoice(qint64 fileId, const QString& langCode,
                                     std::function<void(const QString&)> cb)
{
    if (!configured() || fileId <= 0) { cb(QString()); return; }
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    const QString lang = apiLang(langCode);
    ensureLogin([this, fileId, lang, cb](bool ok) {
        if (!ok) { cb(QString()); return; }
        download(fileId, lang, cb);
    });
}
```
And add `searchCandidates` — the same GET as `searchQuery` but returning every parsed row instead of the winner (sorted language-match first, then download count):
```cpp
void SubtitleFetcher::searchCandidates(const QString& query,
                                       std::function<void(const QVector<SubtitleCandidate>&)> done)
{
    QNetworkRequest rq = makeRequest(apiHost_, QStringLiteral("/subtitles?") + query, true, token_);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            emit log(QStringLiteral("subs: search failed (%1)").arg(reply->errorString()));
            done({});
            return;
        }
        const QJsonArray data = QJsonDocument::fromJson(reply->readAll()).object()
                                    .value(QStringLiteral("data")).toArray();
        QVector<SubtitleCandidate> out;
        for (const QJsonValue& v : data)
        {
            const QJsonObject a = v.toObject().value(QStringLiteral("attributes")).toObject();
            const QJsonArray files = a.value(QStringLiteral("files")).toArray();
            if (files.isEmpty()) continue;
            SubtitleCandidate c;
            c.fileId = files.first().toObject().value(QStringLiteral("file_id")).toVariant().toLongLong();
            if (c.fileId <= 0) continue;
            c.language = a.value(QStringLiteral("language")).toString();
            c.release = a.value(QStringLiteral("release")).toString();
            if (c.release.isEmpty()) c.release = files.first().toObject().value(QStringLiteral("file_name")).toString();
            c.downloads = a.value(QStringLiteral("download_count")).toInt();
            out.push_back(c);
        }
        std::sort(out.begin(), out.end(), [](const SubtitleCandidate& a, const SubtitleCandidate& b) {
            return a.downloads > b.downloads;                       // most-downloaded first
        });
        done(out);
    });
}
```
Declare `SubtitleCandidate`, the three public methods, `buildQueries`, and `searchCandidates` in `SubtitleFetcher.h`; add `#include <QVector>` and `#include <algorithm>`.

- [ ] **Step 4: Plumb `imdbStreamId` onto local items.** In `native/src/browse/SyntheticCatalogs.cpp`, in `localLibraryCatalog`'s per-entry mapping (beside `it.id = …`), add:
```cpp
        // Give subtitle matching an exact IMDB key (armSubtitleFetch reads imdbStreamId, not id): a movie's
        // own tt id, or "<seriesTt>:<season>:<episode>" for an episode — the format SubtitleFetcher parses
        // into parent_imdb_id/season_number/episode_number. Empty when unknown ⇒ the title-query fallback.
        if (e.kind == LocalLibrary::Kind::Movie) it.imdbStreamId = e.imdbId;
        else if (!e.seriesImdbId.isEmpty())
            it.imdbStreamId = e.seriesImdbId + QStringLiteral(":")
                            + QString::number(e.season) + QStringLiteral(":") + QString::number(e.episode);
```
(`it.id` is unchanged — it stays the OwnedIndex/merge key.)

- [ ] **Step 5: MainWindow — own the cache, pass the local path, consult the cache first.** In `native/src/ui/MainWindow.h` add `std::unique_ptr<SubtitleCache> subCache_;` and extend the `SubContext` struct with `QString localPath;`. In `MainWindow.cpp`:
  (a) construct the cache in the ctor beside the other stores: `subCache_ = std::make_unique<SubtitleCache>(AppPaths::dataDir() + QStringLiteral("/subtitles.json")); subCache_->load();`
  (b) in `armSubtitleFetch`, capture the local path when the item is a local file:
```cpp
    subCtx_.localPath = (item.mime == QStringLiteral("local:video")) ? item.url : QString();
```
  (c) in the `fileLoaded` lambda (`:293-321`), consult the cache before any network, and record on success:
```cpp
        const QString lang = Settings::subtitleLanguage();
        const QString ident = SubtitleFetcher::cacheIdentifier(subCtx_.imdbStreamId, subCtx_.title,
                                                               subCtx_.localPath);
        const QString key = SubtitleCache::keyFor(ident, lang);
        if (subCache_) {
            const QString hit = subCache_->lookup(key);
            if (!hit.isEmpty()) { player_->addSubtitle(hit); return; }   // cached ⇒ zero network, zero quota
        }
        subFetcher_->fetch(subCtx_.imdbStreamId, subCtx_.title, lang, subCtx_.localPath,
                           [this, key](const QString& srt) {
            if (srt.isEmpty()) return;
            player_->addSubtitle(srt);
            if (subCache_) subCache_->put(key, srt);
        });
```
  (Adapt to the lambda's real captured names; keep the existing `hasSub`/`isVideo`/one-shot guards exactly as they are.)

- [ ] **Step 6: Build + suite.** `cmake --build build --target everythingbox probe_subs --config Release --parallel` (app compiles clean) then `BUILD_DIR=build bash native/tools/run-headless-probes.sh` → `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 7: Commit.**
```bash
git add native/src/core/SubtitleFetcher.h native/src/core/SubtitleFetcher.cpp native/src/browse/SyntheticCatalogs.cpp native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
git commit -m "feat: hash/imdb/title priority chain + cache short-circuit + local imdbStreamId (subs T2)"
```

---

### Task 3: the "Search subtitles…" picker

**Files:** Modify `native/src/ui/MainWindow.cpp` (+ `.h` if a helper is added).

**Interfaces:** Consumes T2's `searchList`/`downloadChoice`/`cacheIdentifier` and T1's `SubtitleCache`.

- [ ] **Step 1: Add the button beside the existing one.** In `MainWindow::showSubtitleMenu`, immediately after the `🔍  Download from OpenSubtitles` block (`MainWindow.cpp:6592-6606`), add a second row button using the same `rowButton`/`rightCol`/`subRightCol_` idiom:
```cpp
        auto* pickBtn = rowButton(tr("🔎  Search subtitles…"), false);
        connect(pickBtn, &QPushButton::clicked, this, [this] {
            hideSubtitleMenu();
            notify(tr("Searching OpenSubtitles…"), 0);
            const QString lang = Settings::subtitleLanguage();
            subFetcher_->searchList(subCtx_.imdbStreamId, subCtx_.title, lang, subCtx_.localPath,
                                    [this, lang](const QVector<SubtitleCandidate>& list) {
                if (list.isEmpty()) { notify(tr("No subtitles found on OpenSubtitles."), kFeedbackLong); return; }
                presentSubtitleCandidates(list, lang);
            });
        });
        rightCol->addWidget(pickBtn);
        subRightCol_ << pickBtn;
```

- [ ] **Step 2: Add the candidate list presenter.** Add a private method `void presentSubtitleCandidates(const QVector<SubtitleCandidate>& list, const QString& lang);` to `MainWindow.h` and implement it in `MainWindow.cpp` using the app's existing themed chooser idiom — **read how another list-of-choices is presented in this file (e.g. the subtitle TRACK list at `MainWindow.cpp:6434-6460`, or a NavMenu-based chooser) and mirror it exactly**; do not introduce a QDialog (the nav-kit rule forbids top-level windows). Each row's label:
```cpp
    // "en · Blade.Runner.1982.Final.Cut.1080p · 12,431 downloads"
    const QString label = QStringLiteral("%1 · %2 · %3")
        .arg(c.language.isEmpty() ? tr("?") : c.language,
             c.release.isEmpty() ? tr("(unnamed)") : c.release,
             tr("%n download(s)", "", c.downloads));
```
On activation:
```cpp
    notify(tr("Downloading subtitle…"), 0);
    subFetcher_->downloadChoice(c.fileId, lang, [this, lang](const QString& srt) {
        if (srt.isEmpty()) { notify(tr("Couldn't download that subtitle."), kFeedbackLong); return; }
        player_->addSubtitle(srt);
        // The user's correction WINS over the auto-pick, and sticks on replay.
        const QString ident = SubtitleFetcher::cacheIdentifier(subCtx_.imdbStreamId, subCtx_.title,
                                                               subCtx_.localPath);
        if (subCache_) subCache_->put(SubtitleCache::keyFor(ident, lang), srt);
        notify(tr("Subtitle added."), 3000);
    });
```

- [ ] **Step 3: Unconfigured affordance.** The existing block is wrapped in `if (SubtitleFetcher::configured() && …)`. When `!SubtitleFetcher::configured()`, add a single non-interactive info row in the same column so the feature is discoverable rather than invisible:
```cpp
    else
    {
        auto* hint = rowButton(tr("🔎  Search subtitles… (add OpenSubtitles credentials in Settings)"), false);
        hint->setEnabled(false);
        rightCol->addWidget(hint);
        subRightCol_ << hint;
    }
```
(Match `rowButton`'s real signature; if a disabled row isn't supported by the nav kit, use whatever non-focusable info-row primitive the overlay already has and say which you used.)

- [ ] **Step 4: Build + suite + commit.**
```bash
cmake --build build --target everythingbox --config Release --parallel
BUILD_DIR=build bash native/tools/run-headless-probes.sh
git add native/src/ui/MainWindow.cpp native/src/ui/MainWindow.h
git commit -m "feat: Search subtitles… picker with cache-overwrite on choice (subs T3)"
```

---

### Task 4: close-out — live verify, review, merge

- [ ] **Step 1: Gates.** Full suite green (`SUBS-OK` + `ALL HEADLESS PROBES PASSED`); app compiles Release. No perf run (fetching is off the render path, on the file-loaded event) — note that inline.
- [ ] **Step 2: Live verify (portable throwaway; NEVER touch `C:\EverythingBox-app`).** Copy the deployed data dir, cloud-stripped, `EB_UITEST=1` + `native/tools/uitest.py`. **Check first whether `subs/osApiKey`/`osUser`/`osPass` are configured in the copied ini.**
  - **If configured:** point `library/folder` at a local video with NO sidecar `.srt` → play → a subtitle is fetched and appears; note whether the hash tier hit (`subs:` log lines); **replay** → the cache short-circuits (no second network call — confirm via the log and that `subtitles.json` gained the key); open the subtitle overlay → **Search subtitles…** → a candidate list appears → choose a different one → it loads → **replay again** → the *chosen* one comes back (cache overwrite). Screenshots `subs-picker.png`, `subs-cached.png`.
  - **If NOT configured:** verify the DORMANT path instead — the overlay shows the disabled "add OpenSubtitles credentials" row, no network is attempted, a sidecar `.srt` still auto-loads via `sub-auto=fuzzy` (place one beside a fixture video and confirm it appears as a track), and nothing regresses. Record the credentialed pass as **USER-GATED** and say so plainly.
- [ ] **Step 3: Review.** `scripts/review-package $(git merge-base main HEAD) HEAD`, most-capable model. Dimensions: the OSDb hash matches the spec (LE words, both windows, size, 16 hex, <128 KiB ⇒ empty) and reads ONLY two windows (never the whole file — a multi-GB rip must not be slurped); the priority chain order and that a cache hit truly short-circuits before `ensureLogin`; `cacheIdentifier` agrees with what each tier matched; cache overwrite-on-put and self-healing miss; the shipped auto-fetch gate (`hasSub`) is untouched so sidecars/embedded tracks still suppress downloads; the old 4-arg `fetch` forwarder keeps every existing caller working; local `imdbStreamId` composition (movie vs episode vs unknown) and that `it.id` is unchanged; the picker uses the nav-kit list idiom (no QDialog/top-level window) and the unconfigured affordance; no credential ever reaches a log line. Fix rounds → merge.
- [ ] **Step 4: Merge + push + redeploy.** Spec Status → complete (record which live steps ran vs were user-gated). Merge `local/subtitle-accuracy` → main (version-line conflict → take the higher patch), rebuild the combined tree, full suite green (**build all probe targets** to catch a latent link break), push, delete the branch, redeploy Release to `C:\EverythingBox-app` (md5-verify), update `.superpowers/sdd/progress.md`, mark the chapter.

## Self-Review (done at write time)

- **Spec coverage:** `SubtitleHash` OSDb ✅T1; `SubtitleCache` (lookup/put-overwrite/self-heal/keyFor) ✅T1; priority chain cache→hash→imdb→title ✅T2 (cache in MainWindow, the rest in `buildQueries`); `searchList`/`downloadChoice` ✅T2; local `imdbStreamId` plumbing ✅T2; picker + cache-overwrite ✅T3; unconfigured affordance ✅T3; edge table (small file, unconfigured, network/401, zero results ⇒ nothing cached, deleted cache file, streams, sidecar-suppresses) ✅T1/T2 code + ✅T4 verify. Non-goals (multi-language, editing, other providers, burn-in) not built ✅.
- **Placeholder scan:** every code step carries real code. Two steps deliberately say "mirror the existing idiom" (the themed candidate list, `rowButton`'s disabled variant) and name the exact anchor to read — precise instructions, not TBDs, because the nav-kit list primitive must be matched rather than invented.
- **Type consistency:** `SubtitleHash::{ofFile,ofBytes}`, `SubtitleCache::{load,save,lookup,put,clear,keyFor}`, `SubtitleCandidate{fileId,language,release,downloads}`, `SubtitleFetcher::{fetch(5-arg + 4-arg forwarder),searchList,downloadChoice,cacheIdentifier,buildQueries,searchCandidates}`, `SubContext::localPath`, `subCache_` — consistent across T1–T3 and with the probe assertions.
- **Ambiguity resolved:** the cache is consulted in MainWindow (not inside the fetcher) so the fetcher stays a pure transport and the cache stays app-owned, matching how the other stores are wired; the 4-arg `fetch` is kept as a forwarder so the existing overlay button and any other caller compile unchanged; `cacheIdentifier` prefixes (`hash:`/`title:`) prevent a title colliding with an imdb id in the key space.
