# Trakt Calendar Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A home shelf and browse destination showing episodes airing soon for the shows a connected Trakt account follows.

**Architecture:** `TraktClient` today is write-only (scrobbling). This adds the read direction as three separable pieces: a pure tolerant parser plus identity mapper (`TraktRead`), an authorised fetch routed through the existing token gate, and a pure `MediaCatalog` builder following the established `SyntheticCatalogs` pattern. The surface is entirely absent when Trakt is not configured.

**Tech Stack:** Qt 6.8.3 (Core, Network), C++17, CMake. Headless console probe. No new dependencies.

## Global Constraints

- **Build config is always Release**, always a named target: `cmake --build build --config Release --target <name>`. A target-less build compiles ~43 probes and stalls.
- **Create a `.cpp` in the SAME step that adds it to CMake** — CMake resolves source lists at configure time. Adding a source needs ONE reconfigure.
- **The suite script only RUNS pre-built exes** — build first, then run it.
- **A new probe must be registered in THREE places** or it silently never runs: its `add_executable` in `native/CMakeLists.txt`, the loop list in `native/tools/run-headless-probes.sh` (~line 130), and the `--target` list in `.github/workflows/ci.yml` (~line 52).
- **Nav kit only** for any modal UI — never `QDialog`/`QMessageBox`/`QInputDialog`. `probe_nav` gates this.
- **The IMDB stream id format is exactly `ttShow:season:episode`** for an episode, keyed on the SHOW's IMDB id — the form the scrobbler already emits and the resolver already consumes.
- **An entry with no show IMDB id is shown, not skipped**, with no `imdbStreamId` and no `url`, and a subtitle saying no source was found.
- **Every new surface is completely absent when Trakt is not configured or not connected** — no row, no placeholder, no hint.
- **The disk cache keys MUST be excluded from `SettingsTxn::inScope`** (`native/src/core/SettingsTxn.cpp`). They are background-written; an in-scope background write produces a phantom dirty count in the settings Save/Discard prompt and gets clobbered by Discard. This is the same async trap that made the Trakt *token* keys a merge blocker.
- **Never print, log or echo a Trakt token, client id or client secret.** Report credentials only as "configured"/"not configured".
- **Mutation testing is the standard of proof.** Break the implementation, confirm the probe FAILS, revert, confirm green.

**Build command (Git Bash):**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --config Release --target <targets>
```

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `native/src/core/TraktRead.h` | `TraktIds`, `CalendarEntry`, `parseMyShowsCalendar`, `imdbStreamIdFor`. |
| `native/src/core/TraktRead.cpp` | Implementation. QtCore only (QJson*, QDateTime), so the probe links lean. |
| `native/tools/probe_trakt.cpp` | Headless probe. Sentinel `TRAKT-OK`. Fixtures inline as raw JSON strings. |

**Modified:**

| File | Change |
|---|---|
| `native/CMakeLists.txt` | `probe_trakt` target; `TraktRead.*` into the app target and `probe_browse`. |
| `native/tools/run-headless-probes.sh` | `probe_trakt TRAKT-OK` in the loop list. |
| `.github/workflows/ci.yml` | `probe_trakt` in the `--target` list. |
| `native/src/core/TraktClient.{h,cpp}` | `fetchMyShowsCalendar` + the disk cache. |
| `native/src/core/SettingsTxn.cpp` | Exclude the cache keys. |
| `native/src/browse/SyntheticCatalogs.{h,cpp}` | `traktCalendarCatalog`. |
| `native/src/ui/HomeView.{h,cpp}` / `MainWindow.cpp` | The shelf + browse destination. |

---

### Task 1: TraktRead — parser + identity mapper, with probe_trakt

**Files:**
- Create: `native/src/core/TraktRead.h`, `native/src/core/TraktRead.cpp`, `native/tools/probe_trakt.cpp`
- Modify: `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces, for Tasks 2–3:
  - `struct TraktIds { QString imdb, tmdb, tvdb, trakt; };`
  - `struct CalendarEntry { QDateTime airsAtUtc; QString showTitle; TraktIds showIds; int season; int episode; QString episodeTitle; TraktIds episodeIds; QString posterUrl; };`
  - `QVector<CalendarEntry> trakt::parseMyShowsCalendar(const QByteArray& json);`
  - `QString trakt::imdbStreamIdFor(const TraktIds& showIds, int season, int episode);`

- [ ] **Step 1: Confirm the real response shape BEFORE writing the parser**

The Trakt v2 endpoint is `GET /calendars/my/shows/{start_date}/{days}`. Its documented per-entry
shape is approximately:

```json
{
  "first_aired": "2026-08-04T01:00:00.000Z",
  "episode": { "season": 1, "number": 4, "title": "…",
               "ids": { "trakt": 1, "tvdb": 2, "imdb": "tt…", "tmdb": 3 } },
  "show":    { "title": "…",
               "ids": { "trakt": 1, "slug": "…", "tvdb": 2, "imdb": "tt…", "tmdb": 3 } }
}
```

**Confirm this against Trakt's current API documentation before writing the parser**, and write the
probe fixtures from the confirmed shape. Do NOT invent field names. If the docs differ from the above
in any way, follow the docs and note the difference in your report — the rest of this task is
unchanged either way.

Note `ids.trakt`, `ids.tvdb` and `ids.tmdb` are **numbers**, while `ids.imdb` and `ids.slug` are
**strings**. Parse accordingly; a number read as a string yields empty and silently loses the id.

- [ ] **Step 2: Write the probe first (RED)**

Create `native/tools/probe_trakt.cpp`:

```cpp
// Headless check of the Trakt read layer (issue #23). TraktClient is a scrobbler — write-only — and
// this is the first read piece: a TOLERANT parser for the "my shows" calendar and the identity mapper
// the watchlist/collection and history-backfill follow-ups will both reuse.
//
// Two properties matter most and are pinned hardest:
//   * The parser is TOTAL. One malformed entry must never cost the user their whole calendar.
//   * imdbStreamIdFor returns "" — not an error, not a guess — when Trakt gave no show IMDB id.
//     That empty string is the documented signal for "show it, but it is not playable".
//
// Prints TRAKT-OK on success; any failure prints TRAKT-FAIL <cond> and exits non-zero.
#include "TraktRead.h"

#include <QByteArray>
#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "TRAKT-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// A normal two-entry batch. The SECOND show deliberately has NO imdb id — the unjoinable case.
static const char* kNormal = R"([
  { "first_aired": "2026-08-04T01:00:00.000Z",
    "episode": { "season": 1, "number": 4, "title": "Fourth",
                 "ids": { "trakt": 11, "tvdb": 12, "imdb": "tt2000004", "tmdb": 13 } },
    "show": { "title": "Alpha Show",
              "ids": { "trakt": 1, "slug": "alpha", "tvdb": 2, "imdb": "tt1000001", "tmdb": 3 } } },
  { "first_aired": "2026-08-05T02:30:00.000Z",
    "episode": { "season": 2, "number": 7, "title": "Seventh",
                 "ids": { "trakt": 21, "tvdb": 22, "tmdb": 23 } },
    "show": { "title": "Beta Show",
              "ids": { "trakt": 4, "slug": "beta", "tvdb": 5, "tmdb": 6 } } }
])";

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. the normal batch ----------------------------------------------------------------
    {
        const QVector<CalendarEntry> e = trakt::parseMyShowsCalendar(QByteArray(kNormal));
        CHECK(e.size() == 2);
        CHECK(e.value(0).showTitle == QStringLiteral("Alpha Show"));
        CHECK(e.value(0).season == 1);
        CHECK(e.value(0).episode == 4);
        CHECK(e.value(0).episodeTitle == QStringLiteral("Fourth"));
        CHECK(e.value(0).showIds.imdb == QStringLiteral("tt1000001"));
        CHECK(e.value(0).airsAtUtc.isValid());
        // Numeric ids must survive as strings — a number read with toString() yields empty and the
        // id is silently lost, which is the bug this pins.
        CHECK(e.value(0).showIds.tmdb == QStringLiteral("3"));
        CHECK(e.value(0).showIds.tvdb == QStringLiteral("2"));
        // The unjoinable second entry is PRESENT, with an empty imdb.
        CHECK(e.value(1).showTitle == QStringLiteral("Beta Show"));
        CHECK(e.value(1).showIds.imdb.isEmpty());
        CHECK(e.value(1).showIds.tmdb == QStringLiteral("6"));
    }

    // ---- 2. TOTALITY: a malformed entry must not cost the surrounding ones --------------------
    {
        const char* mixed = R"([
          { "first_aired": "2026-08-04T01:00:00.000Z",
            "episode": { "season": 1, "number": 1, "title": "Keep me",
                         "ids": { "imdb": "tt9" } },
            "show": { "title": "Good", "ids": { "imdb": "tt1" } } },
          { "first_aired": "not-a-date",
            "episode": { "season": 1, "number": 2 }, "show": { "title": "Bad date" } },
          "a bare string where an object belongs",
          { "episode": { "season": 1, "number": 3 }, "show": { "title": "No date at all" } },
          { "first_aired": "2026-08-06T01:00:00.000Z",
            "episode": { "season": 3, "number": 9, "title": "Keep me too",
                         "ids": { "imdb": "tt8" } },
            "show": { "title": "Also good", "ids": { "imdb": "tt2" } } }
        ])";
        const QVector<CalendarEntry> e = trakt::parseMyShowsCalendar(QByteArray(mixed));
        // Exactly the two well-formed entries survive; the three broken ones are dropped.
        CHECK(e.size() == 2);
        CHECK(e.value(0).showTitle == QStringLiteral("Good"));
        CHECK(e.value(1).showTitle == QStringLiteral("Also good"));
    }

    // ---- 3. degenerate inputs must return empty, never crash ---------------------------------
    CHECK(trakt::parseMyShowsCalendar(QByteArray()).isEmpty());
    CHECK(trakt::parseMyShowsCalendar(QByteArray("not json at all")).isEmpty());
    CHECK(trakt::parseMyShowsCalendar(QByteArray("[]")).isEmpty());
    CHECK(trakt::parseMyShowsCalendar(QByteArray("{}")).isEmpty());          // object, not array
    CHECK(trakt::parseMyShowsCalendar(QByteArray("null")).isEmpty());

    // ---- 4. a missing ids OBJECT is not a crash ----------------------------------------------
    {
        const char* noIds = R"([
          { "first_aired": "2026-08-04T01:00:00.000Z",
            "episode": { "season": 1, "number": 1 }, "show": { "title": "Idless" } }
        ])";
        const QVector<CalendarEntry> e = trakt::parseMyShowsCalendar(QByteArray(noIds));
        CHECK(e.size() == 1);
        CHECK(e.value(0).showIds.imdb.isEmpty());
        CHECK(e.value(0).showIds.tmdb.isEmpty());
        CHECK(e.value(0).showTitle == QStringLiteral("Idless"));
    }

    // ---- 5. imdbStreamIdFor: the identity mapper ---------------------------------------------
    {
        TraktIds full; full.imdb = QStringLiteral("tt1000001"); full.tmdb = QStringLiteral("3");
        CHECK(trakt::imdbStreamIdFor(full, 1, 4) == QStringLiteral("tt1000001:1:4"));
        CHECK(trakt::imdbStreamIdFor(full, 12, 205) == QStringLiteral("tt1000001:12:205"));
        // Season 0 is a REAL season on Trakt (specials) and must map, not be rejected.
        CHECK(trakt::imdbStreamIdFor(full, 0, 1) == QStringLiteral("tt1000001:0:1"));

        // No show imdb id -> empty. NOT an error, NOT a guess from tmdb/tvdb: the empty string is
        // the documented "not playable" signal the catalog builder keys on.
        TraktIds noImdb; noImdb.tmdb = QStringLiteral("6"); noImdb.tvdb = QStringLiteral("5");
        CHECK(trakt::imdbStreamIdFor(noImdb, 2, 7).isEmpty());
        CHECK(trakt::imdbStreamIdFor(TraktIds{}, 1, 1).isEmpty());

        // Nonsense episode numbers must not produce a malformed id that the resolver would choke on.
        CHECK(trakt::imdbStreamIdFor(full, -1, 4).isEmpty());
        CHECK(trakt::imdbStreamIdFor(full, 1, 0).isEmpty());
        CHECK(trakt::imdbStreamIdFor(full, 1, -3).isEmpty());
    }

    if (failures == 0) { std::puts("TRAKT-OK"); return 0; }
    std::fprintf(stderr, "TRAKT: %d check(s) failed\n", failures);
    return 1;
}
```

- [ ] **Step 3: Create the header in the same step it enters CMake**

Create `native/src/core/TraktRead.h`:

```cpp
// The Trakt READ layer (issue #23). TraktClient is a scrobbler — it pushes what you watch and never
// reads anything back. This is the first read piece: a tolerant parser for the "my shows" calendar,
// and the identity mapper that turns Trakt's id bag into the app's own key.
//
// Pure: QJson in, structs out. No network, no GUI, no ini — so probe_trakt links against Qt6::Core
// alone and pins the tables with no I/O.
#pragma once
#include <QDateTime>
#include <QString>
#include <QVector>

class QByteArray;

// Trakt returns an id bag per show and per episode. `trakt`, `tvdb` and `tmdb` arrive as JSON
// NUMBERS while `imdb` is a string — all are kept as QString here so callers have one type.
struct TraktIds
{
    QString imdb;   // "tt1000001" — the only one the app can key on
    QString tmdb;
    QString tvdb;
    QString trakt;
};

struct CalendarEntry
{
    QDateTime airsAtUtc;
    QString   showTitle;
    TraktIds  showIds;
    int       season = 0;
    int       episode = 0;
    QString   episodeTitle;
    TraktIds  episodeIds;
    QString   posterUrl;   // "" when Trakt gave none
};

namespace trakt
{
    // TOTAL and TOLERANT by contract: a malformed entry is skipped, a missing `ids` object yields an
    // empty TraktIds, an unparseable date drops that entry, and non-array or non-JSON input returns
    // empty. One bad row must never cost the user their whole calendar.
    QVector<CalendarEntry> parseMyShowsCalendar(const QByteArray& json);

    // "ttShow:season:episode" — keyed on the SHOW's imdb id, which is the form the scrobbler already
    // emits and the stream resolver already consumes.
    //
    // Returns "" when the show has no imdb id, or when season/episode are out of range. That empty
    // string is the DOCUMENTED SIGNAL for "show this entry but mark it not playable" — it is not an
    // error, and it must never be substituted with a tmdb/tvdb id, which nothing downstream can use.
    //
    // Season 0 is valid (Trakt uses it for specials). Episode numbers start at 1.
    QString imdbStreamIdFor(const TraktIds& showIds, int season, int episode);
}
```

- [ ] **Step 4: Create the implementation**

Create `native/src/core/TraktRead.cpp`:

```cpp
#include "TraktRead.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace {

// Trakt sends `trakt`/`tvdb`/`tmdb` as numbers and `imdb`/`slug` as strings. Reading a number with
// toString() silently yields empty and loses the id, so normalise both shapes here.
QString idString(const QJsonValue& v)
{
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(qint64(v.toDouble()));
    return QString();
}

TraktIds idsFrom(const QJsonObject& owner)
{
    const QJsonObject o = owner.value(QStringLiteral("ids")).toObject();
    TraktIds ids;
    ids.imdb  = idString(o.value(QStringLiteral("imdb")));
    ids.tmdb  = idString(o.value(QStringLiteral("tmdb")));
    ids.tvdb  = idString(o.value(QStringLiteral("tvdb")));
    ids.trakt = idString(o.value(QStringLiteral("trakt")));
    return ids;
}

} // namespace

QVector<CalendarEntry> trakt::parseMyShowsCalendar(const QByteArray& json)
{
    QVector<CalendarEntry> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) return out;                 // non-JSON, an object, or null -> empty

    for (const QJsonValue& v : doc.array())
    {
        if (!v.isObject()) continue;                // a bare string where an object belongs
        const QJsonObject o = v.toObject();

        // An entry with no usable air time is dropped: the calendar is ordered by it, and an entry
        // that cannot be placed on a date has nothing to say.
        const QDateTime airs =
            QDateTime::fromString(o.value(QStringLiteral("first_aired")).toString(), Qt::ISODateWithMs)
                .toUTC();
        if (!airs.isValid()) continue;

        const QJsonObject show = o.value(QStringLiteral("show")).toObject();
        const QJsonObject ep   = o.value(QStringLiteral("episode")).toObject();

        CalendarEntry e;
        e.airsAtUtc    = airs;
        e.showTitle    = show.value(QStringLiteral("title")).toString();
        e.showIds      = idsFrom(show);
        e.season       = ep.value(QStringLiteral("season")).toInt(-1);
        e.episode      = ep.value(QStringLiteral("number")).toInt(-1);
        e.episodeTitle = ep.value(QStringLiteral("title")).toString();
        e.episodeIds   = idsFrom(ep);
        out.push_back(e);
    }
    return out;
}

QString trakt::imdbStreamIdFor(const TraktIds& showIds, int season, int episode)
{
    // No imdb id -> "", the "not playable" signal. Never fall back to tmdb/tvdb: nothing downstream
    // can resolve those, so a substituted id would produce an item that LOOKS playable and is not.
    if (showIds.imdb.isEmpty()) return QString();
    if (season < 0 || episode < 1) return QString();   // season 0 is valid (specials); episodes are 1-based
    return showIds.imdb + QStringLiteral(":") + QString::number(season)
                        + QStringLiteral(":") + QString::number(episode);
}
```

Note the fixture in probe §2 relies on a bad-date entry being dropped; if the confirmed API uses a
different date field name, update BOTH the implementation and the fixtures together.

- [ ] **Step 5: Register the probe target in CMake**

In `native/CMakeLists.txt`, after the `probe_settingstxn` block, add:

```cmake
    # Headless test for the Trakt read layer (issue #23): the tolerant calendar parser and the
    # identity mapper. TraktRead.cpp is QtCore-only (QJson + QDateTime), so this links leanest of all.
    add_executable(probe_trakt tools/probe_trakt.cpp
        src/core/TraktRead.cpp src/core/TraktRead.h)
    target_include_directories(probe_trakt PRIVATE src src/core)
    target_link_libraries(probe_trakt PRIVATE Qt6::Core)
```

Also add `src/core/TraktRead.cpp` / `.h` to the main app target's source list.

- [ ] **Step 6: Register in the suite script and CI**

In `native/tools/run-headless-probes.sh` (~line 130) add `"probe_trakt TRAKT-OK"` to the `for p in ...`
list. In `.github/workflows/ci.yml` (~line 52) append `probe_trakt` to the `--target` list.

- [ ] **Step 7: Configure, build, run**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON \
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" \
  -DMPV_INCLUDE_DIR="C:/mpv-dev/include" -DMPV_LIBRARY="C:/mpv-dev/libmpv.lib" \
  -DSDL2_INCLUDE_DIR="C:/SDL2/include" -DSDL2_LIBRARY="C:/SDL2/lib/x64/SDL2.lib"
cmake --build build --config Release --target probe_trakt
./build/Release/probe_trakt.exe
```

Expected: `TRAKT-OK`, exit 0.

- [ ] **Step 8: Mutation-test every assertion group**

Apply each, rebuild, run, confirm FAILURE at the named check, revert, confirm `TRAKT-OK`. **Record the
observed failing line for each.**

| # | Mutation in `TraktRead.cpp` | Must fail |
|---|---|---|
| 1 | `idString`: drop the `isDouble` branch (`return v.isString() ? v.toString() : QString();`) | the `showIds.tmdb == "3"` / `tvdb == "2"` checks in §1 |
| 2 | `parseMyShowsCalendar`: `return out;` immediately on the first invalid entry instead of `continue` | the `e.size() == 2` check in §2 (totality) |
| 3 | `parseMyShowsCalendar`: drop the `if (!v.isObject()) continue;` | §2 — a bare string would be parsed as an empty object |
| 4 | `parseMyShowsCalendar`: drop the `if (!doc.isArray()) return out;` | the `"{}"` and `"null"` checks in §3 |
| 5 | `imdbStreamIdFor`: fall back to `showIds.tmdb` when `imdb` is empty | the `noImdb` / `TraktIds{}` empty checks in §5 |
| 6 | `imdbStreamIdFor`: `if (season < 1 …)` (reject season 0) | the specials check `imdbStreamIdFor(full, 0, 1)` in §5 |
| 7 | `imdbStreamIdFor`: drop the range guard entirely | the `-1` / `0` / `-3` checks in §5 |

If any mutation does NOT fail, that assertion is not real coverage — strengthen it before proceeding.

- [ ] **Step 9: Full suite, then commit**

```bash
bash native/tools/run-headless-probes.sh
git add native/src/core/TraktRead.h native/src/core/TraktRead.cpp native/tools/probe_trakt.cpp \
        native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: TraktRead — tolerant calendar parser + IMDB identity mapper"
```

Expected: `ALL HEADLESS PROBES PASSED`.

---

### Task 2: The authorised fetch + the disk cache

**Files:**
- Modify: `native/src/core/TraktClient.{h,cpp}`, `native/src/core/SettingsTxn.cpp`, `native/tools/probe_settingstxn.cpp`

**Interfaces:**
- Consumes: `trakt::parseMyShowsCalendar`, `CalendarEntry` (Task 1).
- Produces, for Task 3:
  - `void TraktClient::fetchMyShowsCalendar(int daysBack, int daysForward, std::function<void(bool ok, QVector<CalendarEntry>)> cb);`
  - `QVector<CalendarEntry> TraktClient::cachedCalendar();` — the last good result, for an offline launch.
  - `static bool TraktClient::calendarAvailable();` — configured AND connected.

- [ ] **Step 1: Add the fetch**

In `native/src/core/TraktClient.h`, add to the public section:

```cpp
    // Episodes airing in [today - daysBack, today + daysForward] for the shows this account follows.
    // Routed through the SAME ensureValidToken gate scrobbling uses, so refresh/expiry live in one
    // place — no read path may issue a raw request. Calls back with ok=false and an empty list when
    // Trakt is not configured or not connected: Trakt being off is not a failure.
    void fetchMyShowsCalendar(int daysBack, int daysForward,
                              std::function<void(bool ok, QVector<CalendarEntry> entries)> cb);

    // The last successfully fetched calendar, persisted so an offline launch still shows something.
    // A stale calendar is far more useful than an empty one.
    static QVector<CalendarEntry> cachedCalendar();

    // configured() && connected() — the one predicate every surface gates on.
    static bool calendarAvailable();
```

Add `#include "TraktRead.h"` and `#include <QVector>`.

In `native/src/core/TraktClient.cpp`, implement using the file's existing `req(path, auth)` helper
(which sets the Trakt headers and the bearer token) and `ensureValidToken`:

```cpp
bool TraktClient::calendarAvailable() { return configured() && connected(); }

void TraktClient::fetchMyShowsCalendar(int daysBack, int daysForward,
                                       std::function<void(bool, QVector<CalendarEntry>)> cb)
{
    if (!calendarAvailable()) { if (cb) cb(false, {}); return; }

    ensureValidToken([this, daysBack, daysForward, cb](bool ok) {
        if (!ok) { if (cb) cb(false, {}); return; }
        const QString start = QDate::currentDate().addDays(-qMax(0, daysBack)).toString(Qt::ISODate);
        const int days = qMax(1, daysBack + daysForward);
        QNetworkReply* r = nam_->get(req(QStringLiteral("/calendars/my/shows/") + start
                                         + QStringLiteral("/") + QString::number(days), true));
        connect(r, &QNetworkReply::finished, this, [this, r, cb] {
            r->deleteLater();
            if (r->error() != QNetworkReply::NoError) {
                emit log(QStringLiteral("trakt: calendar fetch failed"));   // never log the token
                if (cb) cb(false, {});
                return;
            }
            const QVector<CalendarEntry> e = trakt::parseMyShowsCalendar(r->readAll());
            writeCalendarCache(e);
            if (cb) cb(true, e);
        });
    });
}
```

- [ ] **Step 2: Add the cache**

Persist the parsed entries as JSON under two keys in the shared ini, written by
`writeCalendarCache` and read by `cachedCalendar`:

- `trakt/calendarCache` — the serialised entries.
- `trakt/calendarCachedAt` — a unix timestamp, so a caller can tell how stale it is.

Serialise the `CalendarEntry` fields explicitly; do not rely on any implicit conversion. Read
defensively — a corrupt or absent cache must yield an empty vector, never a crash.

- [ ] **Step 3: Exclude the cache keys from the settings transaction**

**This is the step most likely to be skipped and most costly to miss.** The settings Save/Discard
transaction (`native/src/core/SettingsTxn.cpp`) snapshots settings-scope keys on entry to Settings and
restores them on Discard. The calendar cache is written by a background fetch. If it is in scope:

- a fetch completing while the user is in Settings adds a **phantom dirty count** to the prompt
  ("2 settings changed" that the user never touched), and
- **Discard reverts the calendar cache**, throwing away a good fetch.

This is the identical trap that made the Trakt *token* keys a merge blocker on issue #26.

Add `trakt/calendarCache` and `trakt/calendarCachedAt` to the exact-key exclusion list in
`SettingsTxn::inScope`, beside the existing `trakt/access` / `trakt/refresh` / `trakt/expiry` entries.
Keep the exclusion **surgical** — `trakt/clientId` and `trakt/clientSecret` are user-entered settings
rows and must stay IN scope.

Add the paired probe assertions in `native/tools/probe_settingstxn.cpp` §1 — the two new keys out of
scope, AND `trakt/clientId` still in scope. Verify by mutation that removing the exclusion fails.

- [ ] **Step 4: Build, run, commit**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --config Release --target everythingbox probe_settingstxn probe_trakt
./build/Release/probe_settingstxn.exe
bash native/tools/run-headless-probes.sh
git add native/src/core/TraktClient.h native/src/core/TraktClient.cpp \
        native/src/core/SettingsTxn.cpp native/tools/probe_settingstxn.cpp
git commit -m "feat: Trakt calendar fetch through the token gate, cached and out of settings scope"
```

Expected: `SETTINGSTXN-OK` and `ALL HEADLESS PROBES PASSED`.

---

### Task 3: The catalog builder and the surface

**Files:**
- Modify: `native/src/browse/SyntheticCatalogs.{h,cpp}`, `native/tools/probe_browse.cpp`, `native/CMakeLists.txt`, `native/src/ui/HomeView.{h,cpp}`, `native/src/ui/MainWindow.cpp`

**Interfaces:**
- Consumes: `CalendarEntry`, `trakt::imdbStreamIdFor` (Task 1); `TraktClient::fetchMyShowsCalendar`, `cachedCalendar`, `calendarAvailable` (Task 2).
- Produces: `MediaCatalog browse::traktCalendarCatalog(const QVector<CalendarEntry>& entries, const QDateTime& nowUtc);`

- [ ] **Step 1: Add the builder**

In `native/src/browse/SyntheticCatalogs.h`, declare inside `namespace browse`:

```cpp
    // Episodes airing soon, from a connected Trakt account. Sorted by air time, soonest first.
    // PAST entries are excluded: recently-aired episodes are issue #25's job ("You missed"), and two
    // surfaces both claiming the same episode is worse than either alone.
    //
    // An entry whose show has no IMDB id is INCLUDED but left with no imdbStreamId and no url — the
    // app's existing representation of "nothing to play" — and says so in its subtitle. Omitting it
    // would be worse: the user would silently lose a third of their calendar with no way to tell.
    MediaCatalog traktCalendarCatalog(const QVector<CalendarEntry>& entries, const QDateTime& nowUtc);
```

In `SyntheticCatalogs.cpp`, implement it following `favoritesCatalog`'s shape exactly — build
`MediaItem`s, push into `cat.items`, set `cat.hasMore = false`. Per item:

- `it.type = QStringLiteral("episode")`
- `it.title` = the show title
- `it.subtitle` = `S%1E%2` zero-padded to two digits, plus the air day; when unplayable, append the
  no-source note
- `it.thumbnailUrl` = `e.posterUrl`
- `it.imdbStreamId = trakt::imdbStreamIdFor(e.showIds, e.season, e.episode)`
- `it.id` = the same stream id when non-empty, else a stable synthetic id from the show title and
  season/episode so the item still has an identity for marks and focus
- leave `it.url` empty always — the resolver fills it at play time from `imdbStreamId`

Sort by `airsAtUtc` ascending before building. Exclude any entry with `airsAtUtc <= nowUtc`.

- [ ] **Step 2: Add probe coverage to probe_browse**

`probe_browse` already links `SyntheticCatalogs.cpp`. Add `src/core/TraktRead.cpp` to its target in
`native/CMakeLists.txt` (the builder now calls `imdbStreamIdFor`), then add cases asserting:

- ordering is by air time ascending regardless of input order;
- an entry in the past is excluded, one in the future is kept, and one exactly equal to `nowUtc` is
  excluded (the boundary — pick it deliberately and pin it);
- a show WITH an imdb id yields a non-empty `imdbStreamId`; one WITHOUT yields empty **and is still
  present in `cat.items`**;
- every item has an empty `url`;
- an empty input yields an empty catalog with a valid title, not a malformed one.

Mutation-test at least: reversing the sort, dropping the past-exclusion, and skipping unplayable
entries instead of including them. Each must fail a named assertion.

- [ ] **Step 3: Wire the surface**

Add the shelf and browse destination, rendered through the existing synthetic-catalog path.

**Gate every part on `TraktClient::calendarAvailable()`.** When it is false there must be no shelf, no
browse entry, and no placeholder — verify by running with Trakt unconfigured and confirming the home
screen is byte-identical to before this branch.

Populate from `TraktClient::cachedCalendar()` at startup so an offline launch shows the last known
calendar, then refresh via `fetchMyShowsCalendar` behind the existing debounce pattern — **not** per
navigation. Use `daysBack = 0`, `daysForward = 7`.

- [ ] **Step 4: Build, suite, commit**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --config Release --target everythingbox probe_browse probe_trakt probe_nav probe_navqml
bash native/tools/run-headless-probes.sh
git add -A native/src/browse native/src/ui native/tools/probe_browse.cpp native/CMakeLists.txt
git commit -m "feat: Trakt calendar shelf and browse destination"
```

Expected: `ALL HEADLESS PROBES PASSED`.

---

### Task 4: Close-out — review, merge, redeploy

- [ ] **Step 1: Verify the Trakt-off path**

Build a throwaway copy (never touch `C:\EverythingBox-app` or its ini; strip `cloud/*` and `sync/*`
from the throwaway ini; never print a credential value). With Trakt **unconfigured**, launch under
`EB_UITEST=1` with a unique `EB_UITEST_PIPE` and confirm via the state snapshot that the home screen
has no calendar shelf and no new browse entry. Capture the evidence.

Clean up with zero residue, including screenshots. Scope any process kill to PIDs you launched.

- [ ] **Step 2: State the verification limit honestly**

The live authorised fetch **cannot be verified here.** It needs a real Trakt account and a registered
developer app, and credentials will not be entered on the user's behalf. Report exactly which paths
are proven (parse, mapping, building, the off-path, the cache round-trip) and which are not (the live
`GET /calendars/my/shows/...` and its real response shape). Do not claim otherwise.

- [ ] **Step 3: Full suite on the final tree**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --config Release --target everythingbox probe_trakt probe_browse probe_settingstxn probe_nav probe_navqml
bash native/tools/run-headless-probes.sh
```

Expected: `ALL HEADLESS PROBES PASSED`, including `TRAKT-OK`.

- [ ] **Step 4: Whole-branch review**

Generate the review package for `$(git merge-base main HEAD)..HEAD` and dispatch the final reviewer on
the most capable model with the spec and this plan's Global Constraints. Point it at the cross-task
seams: the cache/`SettingsTxn` interaction, the Trakt-off gating, and whether any surface can render
before `calendarAvailable()` is consulted.

- [ ] **Step 5: Merge, push, redeploy**

```bash
git checkout main && git merge --no-ff <branch> && git push origin main
cp build/Release/EverythingBox.exe /c/EverythingBox-app/EverythingBox.exe
md5sum build/Release/EverythingBox.exe /c/EverythingBox-app/EverythingBox.exe
```

Expected: identical md5. Absorb upstream first and re-run the suite on the combined tree before
pushing. **The deploy copy fails while the app is running** — confirm it is closed first rather than
killing the user's session.

- [ ] **Step 6: Update the ledger**

Append one line per task to `.superpowers/sdd/progress.md`, plus a close-out line with the merge
commit, the deploy md5, and the unverified-live-fetch residual.

---

## Self-Review

**Spec coverage:** §1 `TraktRead` → Task 1. §2 identity mapper → Task 1 (`imdbStreamIdFor` + §5
assertions). §3 authorised fetch → Task 2 Step 1. §4 catalog builder → Task 3 Steps 1–2. §5 surfacing
and the Trakt-off rule → Task 3 Step 3, verified in Task 4 Step 1. §6 caching, device-level, and the
`SettingsTxn` exclusion → Task 2 Steps 2–3. §7 testing → Task 1 Steps 2/8, Task 3 Step 2, and the
stated limit in Task 4 Step 2. No gaps.

**Type consistency:** `TraktIds`, `CalendarEntry`, `trakt::parseMyShowsCalendar`,
`trakt::imdbStreamIdFor`, `TraktClient::fetchMyShowsCalendar`, `cachedCalendar`, `calendarAvailable`
and `browse::traktCalendarCatalog` are spelled identically in every task that declares or consumes
them.

**Known judgement calls for the reviewer:**
- Task 1 Step 1 requires confirming the real Trakt response shape before writing the parser. The JSON
  shown is the documented form but is **not** treated as verified; fixtures come from the confirmed
  response. A reviewer should check that this was actually done rather than assumed.
- The past-entry boundary (`airsAtUtc <= nowUtc` excluded) is a deliberate choice to avoid colliding
  with issue #25. It is pinned by a probe assertion so a later change is visible.
