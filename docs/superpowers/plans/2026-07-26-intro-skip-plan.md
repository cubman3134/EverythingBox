# Intro / Credits Skip Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect an episode's intro and credits from three sources — a Kodi `.edl` sidecar, named mpv chapters, or a range the user marked once for that season — and offer a timed on-screen "Skip Intro" / "Next Episode" chip (or skip silently, by setting).

**Architecture:** A pure core (`MediaSegments`) owns the typed range model, all three parsers, the per-type precedence rule, and the enter/consume/re-arm tracker. A JSON store (`SegmentStore`) holds learned ranges keyed per season with a series-level fallback. `MpvWidget` gains read-only `chapters()`/`fps()`. `MainWindow` arms a segment context on open, gathers all three providers once at `onDuration`, and feeds the existing `time-pos` observer into the tracker.

**Tech Stack:** Qt 6.8.3 (Widgets/Core/Gui), C++17, libmpv, MSVC 2022. Headless probes as the test framework.

## Global Constraints

- **Build:** `export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"`; build dir `build`; **always `--config Release`** (the uitest harness runs the Release binary). App target `everythingbox`.
- **Build ONLY named targets.** Never run a target-less `cmake --build build` — it builds every probe and stalls. Adding a source or a probe requires exactly ONE reconfigure: `cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON` (no `-A`). Report BLOCKED if a build exceeds ~6 minutes with no progress.
- **Suite:** `BUILD_DIR=build bash native/tools/run-headless-probes.sh` must print `ALL HEADLESS PROBES PASSED`.
- **Sources are explicit, not globbed.** Every new `.cpp`/`.h` must be added to `qt_add_executable(everythingbox …)` in `native/CMakeLists.txt:122`.
- **Nav kit:** all *modal* UI goes through `src/ui/nav` (`NavMenu`/`NavConfirm`/`Osk`) — never `QDialog`/`QMessageBox`/`QInputDialog`/top-level windows. The skip chip is deliberately **not** a `NavOverlay` (every overlay grabs all input, wrong for a non-modal prompt over live video); it follows `streamIssueBtn_` (`MainWindow.cpp:760-774`), an existing non-modal child of `player_`.
- **Two settings builders.** `MainWindow::openGeneralSettings()` (`:8085`) contains a themed builder (`sep`/`info`/`toggle`/`action`/`textf`/`choice`, helpers at `:8165-8175`) **and** a classic QWidget builder. The themed one is the default-reachable surface; a setting added only to the QWidget builder is unconfigurable for default users. **Both must be touched.**
- **Exact constants** (from the spec, use verbatim): `kMinSegmentS = 5.0`, `kCreditsTailS = 60.0`, `kIntroWindowS = 900.0`, `kIntroMaxLenS = 300.0`, `kChipMs = 8000`.
- **Key bindings:** `S` = act on the visible chip. `I` = open the marks menu. Both only on the player page.
- **Pre-commit hook** auto-bumps the patch version in `native/CMakeLists.txt` + `native/src/main.cpp`. Do not fight it; `EB_NO_VERSION_BUMP=1` skips it for docs-only commits.
- **Commit messages:** end with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

## File Structure

| File | Responsibility |
|---|---|
| `native/src/core/MediaSegments.{h,cpp}` | **New.** Types (`Segment`/`SegmentType`/`Chapter`/`Key`), `parseEdl`, `fromChapters`, `resolve`, `keyFor`, `SegmentTracker`, and the type↔string mapping the store persists with. Pure: no Qt GUI, no mpv, no I/O. |
| `native/src/core/SegmentStore.{h,cpp}` | **New.** Learned ranges as JSON; season key with series fallback. |
| `native/src/video/MpvWidget.{h,cpp}` | `chapters()` and `fps()` — read-only property reads. |
| `native/src/core/Settings.{h,cpp}` | `skipSegments` / `skipSegmentsAuto`. |
| `native/src/ui/MainWindow.{h,cpp}` | `segCtx_` arming, gather-at-`onDuration`, tracker feed, auto-skip, the chip, the `I` marks menu, both settings surfaces. |
| `native/tools/probe_segments.cpp` | **New.** All pure coverage. Sentinel `SEGMENTS-OK`. |

---

### Task 1: `MediaSegments` core + `probe_segments`

**Files:**
- Create: `native/src/core/MediaSegments.h`, `native/src/core/MediaSegments.cpp`
- Create: `native/tools/probe_segments.cpp`
- Modify: `native/CMakeLists.txt` (add `probe_segments` target; add the two new sources to `everythingbox`)
- Modify: `native/tools/run-headless-probes.sh:119` (append `"probe_segments SEGMENTS-OK"` to the `for p in` list)

**Interfaces:**
- Consumes: `LocalLibrary::parseFile`, `LocalLibrary::showKeyFor`, `LocalLibrary::VideoEntry`, `LocalLibrary::Kind` (`native/src/core/LocalLibrary.h`).
- Produces: everything in the header below. Tasks 2–5 depend on these exact names.

- [ ] **Step 1: Write the header**

Create `native/src/core/MediaSegments.h`:

```cpp
// Typed, skippable ranges within a video — an episode's intro and its end credits — plus the three pure
// providers that produce them and the tracker that decides when one has been entered.
//
// Modeled on Jellyfin's Media Segments split: DETECTION (these providers) is separate from STORAGE
// (SegmentStore) and from ACTION (MainWindow's chip / auto-skip). A fourth provider — audio fingerprinting —
// would plug in here without touching either of the other two layers. Deliberately pure: no Qt GUI, no mpv,
// no file I/O, so probe_segments covers every rule in this file without a player or a fixture video.
#pragma once
#include <QString>
#include <QVector>
#include <optional>
#include <vector>

namespace MediaSegments
{
    enum class SegmentType { Intro, Credits, Recap, Commercial };

    struct Segment
    {
        double      start = 0.0;
        double      end   = 0.0;
        SegmentType type  = SegmentType::Intro;
    };

    // One mpv chapter. Declared HERE rather than in MpvWidget so core does not depend on the video layer —
    // and so probe_segments can test fromChapters() without linking libmpv.
    struct Chapter { double time = 0.0; QString title; };

    // The learned tier's identity. seriesKey is empty when nothing identifies a show (a movie, or a file
    // whose name carries no SxxExx), which is how callers know the learn tier is unavailable.
    struct Key { QString seriesKey; int season = 0; };

    // Ranges shorter than this are noise, not intros.
    constexpr double kMinSegmentS   = 5.0;
    // A range ending within this of the end of the file is the credits.
    constexpr double kCreditsTailS  = 60.0;
    // An intro starts before this, and runs no longer than kIntroMaxLenS.
    constexpr double kIntroWindowS  = 900.0;
    constexpr double kIntroMaxLenS  = 300.0;

    // Kodi .edl: "[start] [end] [action]", whitespace-separated, one range per line. Times are plain seconds
    // ("5.3"), "HH:MM:SS.sss", or "#<frame>". fps <= 0 means frames cannot be converted, so frame-form lines
    // are dropped and the rest of the file still applies. Actions: 0 cut, 1 mute, 2 scene marker, 3
    // commercial break — only 0 and 3 are skips. There is no comment syntax (and '#' cannot serve as one,
    // because it prefixes a frame number); any line that does not parse into three fields is dropped.
    QVector<Segment> parseEdl(const QString& text, double duration, double fps);

    // Chapters whose TITLE names a segment. A chapter runs to the next chapter's time, or to duration for
    // the last one (so a last chapter with duration <= 0 is dropped).
    QVector<Segment> fromChapters(const QVector<Chapter>& chapters, double duration);

    // Per-TYPE precedence: for each type independently, the first tier supplying that type wins. NOT
    // whole-list precedence, which would let an .edl carrying only a commercial break suppress a
    // chapter-derived Intro.
    QVector<Segment> resolve(const QVector<Segment>& edl,
                             const QVector<Segment>& chapters,
                             const QVector<Segment>& learned);

    // Series identity: the "tt…:S:E" stream id first, else the filename via LocalLibrary::parseFile.
    Key keyFor(const QString& imdbStreamId, const QString& localPath);

    // Stable tokens for SegmentStore's JSON. Unknown text maps back to Intro's absence (std::nullopt).
    QString                    typeToString(SegmentType t);
    std::optional<SegmentType> typeFromString(const QString& s);

    // Per-playback state: which segments have already been offered. Offers each at most once, until a
    // backward seek to before its start re-arms it, so scrubbing back and replaying re-offers the skip.
    class Tracker
    {
    public:
        void reset(QVector<Segment> segments);
        // The segment just entered, or nullopt. Call on every position tick.
        std::optional<Segment> onPosition(double t);
        bool empty() const { return segs_.isEmpty(); }

    private:
        QVector<Segment>  segs_;
        std::vector<bool> consumed_;
    };
}
```

- [ ] **Step 2: Write the failing probe**

Create `native/tools/probe_segments.cpp`:

```cpp
// Headless coverage for the intro/credits segment core: the three providers, the per-type precedence rule,
// series-key derivation, and the tracker's enter/consume/re-arm behaviour. Pure — no player, no video file.
// Prints SEGMENTS-OK on success; any failure prints SEGMENTS-FAIL <what> and exits non-zero.
#include "MediaSegments.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, what)                                                        \
    do { if (!(cond)) { std::fprintf(stderr, "SEGMENTS-FAIL %s\n", (what)); ++failures; } } while (0)

using namespace MediaSegments;

static int countOf(const QVector<Segment>& v, SegmentType t)
{
    int n = 0;
    for (const Segment& s : v) if (s.type == t) ++n;
    return n;
}

static bool has(const QVector<Segment>& v, SegmentType t, double start, double end)
{
    for (const Segment& s : v)
        if (s.type == t && qAbs(s.start - start) < 0.01 && qAbs(s.end - end) < 0.01) return true;
    return false;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---------------------------------------------------------------- 1. parseEdl: the three time forms
    {
        // seconds, HH:MM:SS.sss, and #frames at 25fps (250/25 = 10s .. 750/25 = 30s).
        const QString edl = QStringLiteral(
            "10.0\t40.0\t3\n"
            "00:01:00.000\t00:01:30.000\t3\n"
            "#250\t#750\t3\n");
        const QVector<Segment> v = parseEdl(edl, 3600.0, 25.0);
        CHECK(v.size() == 3, "all three time forms parse");
        CHECK(has(v, SegmentType::Intro, 10.0, 40.0), "plain seconds");
        CHECK(has(v, SegmentType::Commercial, 60.0, 90.0), "HH:MM:SS");
        CHECK(has(v, SegmentType::Commercial, 10.0, 30.0), "frames at 25fps");

        // Without an fps the frame line is unusable — but it must not poison the rest of the file.
        const QVector<Segment> nofps = parseEdl(edl, 3600.0, 0.0);
        CHECK(nofps.size() == 2, "frame lines drop when fps is unknown, others survive");
    }

    // ---------------------------------------------------------------- 2. parseEdl: actions and junk
    {
        const QString edl = QStringLiteral(
            "10 40 0\n"      // cut -> a skip
            "50 80 1\n"      // mute -> not a skip
            "90 120 2\n"     // scene marker -> not a skip
            "130 160 3\n"    // commercial -> a skip
            "\n"
            "garbage\n"
            "200 abc 3\n"    // unparseable end time
            "300 250 3\n"    // end <= start
            "400 402 3\n");  // shorter than kMinSegmentS
        const QVector<Segment> v = parseEdl(edl, 3600.0, 25.0);
        CHECK(v.size() == 2, "only cut+commercial survive; junk, mute, marker, inverted and tiny all drop");
    }

    // ---------------------------------------------------------------- 3. parseEdl: position typing
    {
        // Credits: ends within kCreditsTailS of duration.
        CHECK(countOf(parseEdl(QStringLiteral("3500 3560 3\n"), 3600.0, 0.0), SegmentType::Credits) == 1,
              "a range ending 40s before the end is Credits");
        CHECK(countOf(parseEdl(QStringLiteral("3400 3530 3\n"), 3600.0, 0.0), SegmentType::Credits) == 0,
              "a range ending 70s before the end is NOT Credits");

        // Intro window boundary: starts before kIntroWindowS.
        CHECK(countOf(parseEdl(QStringLiteral("899 950 3\n"), 3600.0, 0.0), SegmentType::Intro) == 1,
              "starting at 899s is inside the intro window");
        CHECK(countOf(parseEdl(QStringLiteral("901 950 3\n"), 3600.0, 0.0), SegmentType::Intro) == 0,
              "starting at 901s is outside the intro window");

        // Intro length boundary: <= kIntroMaxLenS.
        CHECK(countOf(parseEdl(QStringLiteral("10 309 3\n"), 3600.0, 0.0), SegmentType::Intro) == 1,
              "a 299s range is short enough to be an intro");
        CHECK(countOf(parseEdl(QStringLiteral("10 311 3\n"), 3600.0, 0.0), SegmentType::Intro) == 0,
              "a 301s range is too long to be an intro");

        // Only the FIRST qualifying range is the intro.
        const QVector<Segment> two = parseEdl(QStringLiteral("10 40 3\n60 90 3\n"), 3600.0, 0.0);
        CHECK(countOf(two, SegmentType::Intro) == 1, "only the first qualifying range is the Intro");

        // THE OVERLAP CASE. In a short file one range satisfies both rules. Credits must win: typing it as
        // an Intro would make the chip offer to skip the entire rest of the episode.
        const QVector<Segment> overlap = parseEdl(QStringLiteral("30 100 3\n"), 120.0, 0.0);
        CHECK(countOf(overlap, SegmentType::Credits) == 1 && countOf(overlap, SegmentType::Intro) == 0,
              "a range satisfying both rules types as Credits, never Intro");
    }

    // ---------------------------------------------------------------- 4. fromChapters
    {
        const QVector<Chapter> ch = {
            { 0.0,   QStringLiteral("Recap") },
            { 45.0,  QStringLiteral("Opening Credits") },
            { 135.0, QStringLiteral("Part One") },
            { 900.0, QStringLiteral("End Credits") },
        };
        const QVector<Segment> v = fromChapters(ch, 1000.0);
        CHECK(has(v, SegmentType::Recap, 0.0, 45.0), "a Recap chapter runs to the next chapter");
        // "Opening Credits" CONTAINS "credits". Intro phrases are tested first for exactly this reason —
        // otherwise every anime and drama opening in the world types as end credits.
        CHECK(has(v, SegmentType::Intro, 45.0, 135.0), "\"Opening Credits\" is an Intro, not Credits");
        CHECK(has(v, SegmentType::Credits, 900.0, 1000.0), "the last chapter runs to duration");
        CHECK(countOf(v, SegmentType::Intro) == 1 && v.size() == 3, "\"Part One\" matches nothing");

        // Word-boundary matching, not substring. Without it every documentary gets a phantom intro.
        const QVector<Segment> introduction =
            fromChapters({ { 0.0, QStringLiteral("Introduction") }, { 300.0, QStringLiteral("Body") } }, 600.0);
        CHECK(introduction.isEmpty(), "\"Introduction\" does NOT match the intro phrase \"intro\"");

        // Punctuation and case are normalized away.
        const QVector<Segment> punct =
            fromChapters({ { 0.0, QStringLiteral("[OP]") }, { 90.0, QStringLiteral("A") } }, 600.0);
        CHECK(countOf(punct, SegmentType::Intro) == 1, "\"[OP]\" normalizes to the intro token \"op\"");

        // A last chapter cannot be sized without a duration.
        CHECK(fromChapters({ { 0.0, QStringLiteral("Intro") } }, 0.0).isEmpty(),
              "an unknown duration drops the last chapter");
    }

    // ---------------------------------------------------------------- 5. resolve: per-TYPE precedence
    {
        const QVector<Segment> edl      = { { 100, 200, SegmentType::Commercial } };
        const QVector<Segment> chapters = { { 10,  40,  SegmentType::Intro } };
        const QVector<Segment> learned  = { { 15,  45,  SegmentType::Intro },
                                            { 900, 1000, SegmentType::Credits } };
        const QVector<Segment> v = resolve(edl, chapters, learned);
        CHECK(has(v, SegmentType::Intro, 10.0, 40.0), "chapters beat learned for Intro");
        CHECK(countOf(v, SegmentType::Intro) == 1, "the losing tier's Intro is not also included");
        CHECK(has(v, SegmentType::Credits, 900.0, 1000.0), "learned supplies Credits when nothing else does");
        CHECK(has(v, SegmentType::Commercial, 100.0, 200.0), "the .edl Commercial survives");
        // The whole point of per-type precedence:
        CHECK(countOf(resolve(edl, chapters, {}), SegmentType::Intro) == 1,
              "an .edl with ONLY a Commercial does not suppress a chapter Intro");
    }

    // ---------------------------------------------------------------- 6. keyFor
    {
        const Key k = keyFor(QStringLiteral("tt0903747:2:7"), QString());
        CHECK(k.seriesKey == QStringLiteral("tt0903747") && k.season == 2, "the stream id supplies series+season");

        const Key f = keyFor(QString(), QStringLiteral("D:/TV/Breaking Bad/Breaking Bad S03E05.mkv"));
        CHECK(f.seriesKey == QStringLiteral("name:breaking bad") && f.season == 3,
              "a filename with no stream id still yields a series key");

        CHECK(keyFor(QString(), QStringLiteral("D:/Movies/Blade Runner (1982).mkv")).seriesKey.isEmpty(),
              "a movie has no series key");
        CHECK(keyFor(QString(), QString()).seriesKey.isEmpty(), "nothing in, nothing out");
    }

    // ---------------------------------------------------------------- 7. Tracker
    {
        Tracker t;
        t.reset({ { 10.0, 40.0, SegmentType::Intro } });
        CHECK(!t.onPosition(5.0).has_value(), "before the segment: no offer");
        CHECK(t.onPosition(10.0).has_value(), "entering the segment offers it");
        CHECK(!t.onPosition(20.0).has_value(), "still inside: not offered again");
        CHECK(!t.onPosition(50.0).has_value(), "past it: not offered again");
        CHECK(!t.onPosition(45.0).has_value(), "seeking back but still past the start does NOT re-arm");
        CHECK(!t.onPosition(9.0).has_value(), "seeking to before the start re-arms but does not itself offer");
        CHECK(t.onPosition(11.0).has_value(), "…and re-entering offers it again");

        Tracker empty;
        CHECK(empty.empty() && !empty.onPosition(10.0).has_value(), "an empty tracker never offers");
    }

    if (failures) { std::fprintf(stderr, "SEGMENTS-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("SEGMENTS-OK\n");
    return 0;
}
```

- [ ] **Step 3: Wire the build**

In `native/CMakeLists.txt`, add the sources to the app target — insert after `src/core/SubtitleCache.cpp src/core/SubtitleCache.h` inside `qt_add_executable(everythingbox …)` (starts at `:122`):

```cmake
        src/core/MediaSegments.cpp src/core/MediaSegments.h
        src/core/SegmentStore.cpp  src/core/SegmentStore.h
```

> `SegmentStore` is created in Task 2. Create both `.cpp`/`.h` pairs now as part of this step — `SegmentStore.h`/`.cpp` as an empty-bodied stub with only the `#pragma once` and includes — so the app target keeps linking between tasks. Task 2 fills them in.

Then add the probe next to `probe_subs` (`:478`):

```cmake
    # Headless test for the intro/credits segment core: the three providers (.edl / chapters / learned),
    # per-type precedence, series-key derivation, and the tracker. Pure — no mpv, no video file.
    add_executable(probe_segments tools/probe_segments.cpp
        src/core/MediaSegments.cpp src/core/MediaSegments.h
        src/core/LocalLibrary.cpp  src/core/LocalLibrary.h
        src/core/Settings.cpp      src/core/Settings.h
        src/theme2/FormFactor.cpp  src/theme2/FormFactor.h)
    target_include_directories(probe_segments PRIVATE src src/core src/theme2)
    target_link_libraries(probe_segments PRIVATE Qt6::Core Qt6::Gui)
```

> `LocalLibrary.cpp` is required because `keyFor` calls `parseFile`/`showKeyFor`; `Settings`/`FormFactor` come with it (`LocalLibrary::root()` reads Settings). This mirrors `probe_resolver` (`:488`).

And append to the runner list at `native/tools/run-headless-probes.sh:119`, after `"probe_subs SUBS-OK"`:

```
"probe_segments SEGMENTS-OK"
```

- [ ] **Step 4: Reconfigure and run the probe to watch it FAIL**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON
```
Then:
```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target probe_segments
```
Expected: **compile errors** — `MediaSegments.cpp` does not exist yet. That is the RED state.

- [ ] **Step 5: Implement `MediaSegments.cpp`**

```cpp
#include "MediaSegments.h"
#include "LocalLibrary.h"

#include <QLatin1Char>
#include <QRegularExpression>
#include <QStringList>
#include <initializer_list>

namespace {

// One EDL time token: plain seconds, HH:MM:SS.sss, or #frames. Returns false when unusable — including a
// frame token with no frame rate to convert it, which is why fps is threaded all the way down here.
bool parseTime(const QString& tok, double fps, double* out)
{
    if (tok.isEmpty()) return false;
    if (tok.startsWith(QLatin1Char('#')))
    {
        if (fps <= 0.0) return false;
        bool ok = false;
        const double frames = tok.mid(1).toDouble(&ok);
        if (!ok || frames < 0.0) return false;
        *out = frames / fps;
        return true;
    }
    if (tok.contains(QLatin1Char(':')))
    {
        const QStringList p = tok.split(QLatin1Char(':'));
        if (p.size() != 3) return false;
        bool h = false, m = false, s = false;
        const double hh = p[0].toDouble(&h), mm = p[1].toDouble(&m), ss = p[2].toDouble(&s);
        if (!h || !m || !s) return false;
        *out = hh * 3600.0 + mm * 60.0 + ss;
        return *out >= 0.0;
    }
    bool ok = false;
    *out = tok.toDouble(&ok);
    return ok && *out >= 0.0;
}

// The chapter title, lowercased with every non-alphanumeric run collapsed to a single space. "[OP]" -> "op",
// "Opening Credits!" -> "opening credits".
QString normalizeTitle(const QString& t)
{
    QString s;
    s.reserve(t.size());
    for (const QChar c : t) s += c.isLetterOrNumber() ? c.toLower() : QLatin1Char(' ');
    return s.simplified();
}

// WORD-BOUNDARY containment on a space-normalized string. Substring matching would make "Introduction" an
// intro and would match "op"/"ed" inside ordinary words.
bool hasPhrase(const QString& norm, const char* phrase)
{
    const QString padded = QLatin1Char(' ') + norm + QLatin1Char(' ');
    return padded.contains(QLatin1Char(' ') + QLatin1String(phrase) + QLatin1Char(' '));
}

bool matchAny(const QString& norm, std::initializer_list<const char*> phrases)
{
    for (const char* p : phrases) if (hasPhrase(norm, p)) return true;
    return false;
}

// Intro phrases are tested BEFORE credits phrases, and the order is load-bearing: "opening credits" contains
// "credits", so a credits-first test would type every opening as end credits.
std::optional<MediaSegments::SegmentType> typeForTitle(const QString& title)
{
    using T = MediaSegments::SegmentType;
    const QString n = normalizeTitle(title);
    if (n.isEmpty()) return std::nullopt;
    if (matchAny(n, { "intro", "opening", "opening credits", "opening titles", "titles", "theme", "op" }))
        return T::Intro;
    if (matchAny(n, { "recap", "previously", "previously on" }))
        return T::Recap;
    if (matchAny(n, { "credits", "end credits", "ending", "outro", "closing credits", "ed" }))
        return T::Credits;
    return std::nullopt;
}

} // namespace

QVector<MediaSegments::Segment> MediaSegments::parseEdl(const QString& text, double duration, double fps)
{
    QVector<Segment> out;
    bool introTaken = false;
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]")), Qt::SkipEmptyParts);
    for (const QString& raw : lines)
    {
        const QStringList f = raw.trimmed().split(ws, Qt::SkipEmptyParts);
        if (f.size() != 3) continue;
        double start = 0.0, end = 0.0;
        if (!parseTime(f[0], fps, &start) || !parseTime(f[1], fps, &end)) continue;
        bool ok = false;
        const int action = f[2].toInt(&ok);
        if (!ok) continue;
        if (action != 0 && action != 3) continue;          // 1 mute / 2 scene marker are not skips
        if (end <= start) continue;
        if (duration > 0.0)
        {
            if (start >= duration) continue;
            end = qMin(end, duration);
        }
        if (end - start < kMinSegmentS) continue;

        Segment s{ start, end, SegmentType::Commercial };
        // Credits FIRST: in a short file one range can satisfy both rules, and typing it as an Intro would
        // offer to skip the rest of the episode.
        if (duration > 0.0 && end >= duration - kCreditsTailS)
            s.type = SegmentType::Credits;
        else if (!introTaken && start < kIntroWindowS && (end - start) <= kIntroMaxLenS)
        {
            s.type = SegmentType::Intro;
            introTaken = true;
        }
        out.push_back(s);
    }
    return out;
}

QVector<MediaSegments::Segment> MediaSegments::fromChapters(const QVector<Chapter>& chapters, double duration)
{
    QVector<Segment> out;
    for (int i = 0; i < chapters.size(); ++i)
    {
        const std::optional<SegmentType> ty = typeForTitle(chapters[i].title);
        if (!ty) continue;
        const double start = chapters[i].time;
        const double end   = (i + 1 < chapters.size()) ? chapters[i + 1].time : duration;
        if (end <= start || end - start < kMinSegmentS) continue;
        out.push_back(Segment{ start, end, *ty });
    }
    return out;
}

QVector<MediaSegments::Segment> MediaSegments::resolve(const QVector<Segment>& edl,
                                                       const QVector<Segment>& chapters,
                                                       const QVector<Segment>& learned)
{
    QVector<Segment> out;
    for (const SegmentType t : { SegmentType::Intro, SegmentType::Credits,
                                 SegmentType::Recap, SegmentType::Commercial })
    {
        for (const QVector<Segment>* tier : { &edl, &chapters, &learned })
        {
            bool found = false;
            for (const Segment& s : *tier) if (s.type == t) { out.push_back(s); found = true; }
            if (found) break;                              // this type is settled; lower tiers do not add
        }
    }
    return out;
}

MediaSegments::Key MediaSegments::keyFor(const QString& imdbStreamId, const QString& localPath)
{
    Key k;
    const QStringList p = imdbStreamId.split(QLatin1Char(':'));
    if (p.size() == 3 && !p[0].isEmpty())
    {
        k.seriesKey = p[0];
        k.season    = p[1].toInt();
        return k;
    }
    if (!localPath.isEmpty())
    {
        const LocalLibrary::VideoEntry e = LocalLibrary::parseFile(localPath);
        if (e.kind == LocalLibrary::Kind::Episode && !e.show.isEmpty())
        {
            k.seriesKey = LocalLibrary::showKeyFor(e);
            k.season    = e.season;
        }
    }
    return k;
}

QString MediaSegments::typeToString(SegmentType t)
{
    switch (t)
    {
    case SegmentType::Intro:      return QStringLiteral("intro");
    case SegmentType::Credits:    return QStringLiteral("credits");
    case SegmentType::Recap:      return QStringLiteral("recap");
    case SegmentType::Commercial: return QStringLiteral("commercial");
    }
    return QStringLiteral("intro");
}

std::optional<MediaSegments::SegmentType> MediaSegments::typeFromString(const QString& s)
{
    if (s == QLatin1String("intro"))      return SegmentType::Intro;
    if (s == QLatin1String("credits"))    return SegmentType::Credits;
    if (s == QLatin1String("recap"))      return SegmentType::Recap;
    if (s == QLatin1String("commercial")) return SegmentType::Commercial;
    return std::nullopt;
}

void MediaSegments::Tracker::reset(QVector<Segment> segments)
{
    segs_ = std::move(segments);
    consumed_.assign(static_cast<size_t>(segs_.size()), false);
}

std::optional<MediaSegments::Segment> MediaSegments::Tracker::onPosition(double t)
{
    std::optional<Segment> hit;
    for (int i = 0; i < segs_.size(); ++i)
    {
        const Segment& s = segs_[i];
        const size_t ix = static_cast<size_t>(i);
        // Positional re-arm: being before a segment's start means it lies ahead again, however we got here.
        // No need to track the previous position — a backward seek is implied by t < start.
        if (consumed_[ix] && t < s.start) consumed_[ix] = false;
        if (!consumed_[ix] && t >= s.start && t < s.end && !hit)
        {
            consumed_[ix] = true;
            hit = s;
        }
    }
    return hit;
}
```

- [ ] **Step 6: Build and run the probe — expect PASS**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target probe_segments && ./build/Release/probe_segments.exe
```
Expected: `SEGMENTS-OK`, exit 0.

- [ ] **Step 7: Confirm the app still links**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target everythingbox
```
Expected: builds clean (the `SegmentStore` stub compiles to nothing).

- [ ] **Step 8: Commit**

```bash
git add native/src/core/MediaSegments.h native/src/core/MediaSegments.cpp native/src/core/SegmentStore.h native/src/core/SegmentStore.cpp native/tools/probe_segments.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh
git commit -m "feat: MediaSegments core — .edl/chapter parsers, per-type precedence, tracker"
```

---

### Task 2: `SegmentStore` (learned ranges, season → series fallback)

**Files:**
- Modify: `native/src/core/SegmentStore.h`, `native/src/core/SegmentStore.cpp` (stubs from Task 1)
- Modify: `native/tools/probe_segments.cpp` (add section 8)
- Modify: `native/CMakeLists.txt` (add `SegmentStore.cpp` to the `probe_segments` target)

**Interfaces:**
- Consumes: `MediaSegments::Segment`, `MediaSegments::SegmentType`, `MediaSegments::typeToString`, `MediaSegments::typeFromString`.
- Produces: `SegmentStore` with `load`/`save`/`lookup`/`put`/`forget`/`keyFor` exactly as below. Tasks 4–5 call `lookup` and `put`.

- [ ] **Step 1: Write the failing probe section**

Append to `native/tools/probe_segments.cpp`, immediately before the final `if (failures)` block. Also add `#include "SegmentStore.h"`, `#include <QDir>`, and `#include <QTemporaryDir>` at the top:

```cpp
    // ---------------------------------------------------------------- 8. SegmentStore
    {
        QTemporaryDir tmp;
        CHECK(tmp.isValid(), "temp dir for the store");
        const QString path = QDir(tmp.path()).filePath(QStringLiteral("segments.json"));

        SegmentStore st(path);
        st.load();
        CHECK(st.lookup(QStringLiteral("tt1"), 1).isEmpty(), "an empty store has nothing");

        st.put(QStringLiteral("tt1"), 2, Segment{ 10.0, 40.0, SegmentType::Intro });

        // Round-trip through disk: a fresh store on the same file sees it.
        SegmentStore re(path);
        re.load();
        const QVector<Segment> s2 = re.lookup(QStringLiteral("tt1"), 2);
        CHECK(s2.size() == 1 && has(s2, SegmentType::Intro, 10.0, 40.0), "the season mark round-trips");

        // The series-level fallback: season 3 was never marked, so it inherits the most recent mark.
        const QVector<Segment> s3 = re.lookup(QStringLiteral("tt1"), 3);
        CHECK(s3.size() == 1 && has(s3, SegmentType::Intro, 10.0, 40.0), "an unmarked season falls back");

        // A different show gets nothing.
        CHECK(re.lookup(QStringLiteral("tt2"), 2).isEmpty(), "the fallback does not leak across series");

        // Same type overwrites; a different type coexists.
        re.put(QStringLiteral("tt1"), 2, Segment{ 12.0, 42.0, SegmentType::Intro });
        re.put(QStringLiteral("tt1"), 2, Segment{ 900.0, 1000.0, SegmentType::Credits });
        const QVector<Segment> both = re.lookup(QStringLiteral("tt1"), 2);
        CHECK(both.size() == 2, "marking credits does not clobber the learned intro");
        CHECK(has(both, SegmentType::Intro, 12.0, 42.0), "the same type overwrites in place");
        CHECK(has(both, SegmentType::Credits, 900.0, 1000.0), "the new type is added");

        // forget clears the season but leaves the series fallback intact.
        re.forget(QStringLiteral("tt1"), 2);
        CHECK(re.lookup(QStringLiteral("tt1"), 2).size() > 0, "forget falls back to the series entry");

        // A season of 0 (unknown) keys the bare series entry, not "|s0".
        CHECK(SegmentStore::keyFor(QStringLiteral("tt1"), 0) == QStringLiteral("tt1"),
              "season 0 keys the bare series");
        CHECK(SegmentStore::keyFor(QStringLiteral("tt1"), 2) == QStringLiteral("tt1|s2"), "season key shape");

        // A missing file is a clean empty store, not a crash.
        SegmentStore missing(QDir(tmp.path()).filePath(QStringLiteral("nope.json")));
        missing.load();
        CHECK(missing.lookup(QStringLiteral("tt1"), 1).isEmpty(), "a missing file loads as empty");
    }
```

- [ ] **Step 2: Add `SegmentStore.cpp` to the probe target**

In `native/CMakeLists.txt`, inside `add_executable(probe_segments …)`, after the `MediaSegments` line:

```cmake
        src/core/SegmentStore.cpp  src/core/SegmentStore.h
```

- [ ] **Step 3: Run the probe to watch it FAIL**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON && cmake --build build --config Release --target probe_segments
```
Expected: **compile errors** — `SegmentStore` has no `lookup`/`put`/`forget`/`keyFor` yet.

- [ ] **Step 4: Write `SegmentStore.h`**

```cpp
// The LEARNED tier of intro/credits detection: ranges the user marked once, remembered against a season.
//
// Keyed "<seriesKey>|s<N>" with a bare "<seriesKey>" fallback, because openings genuinely change between
// seasons (this is why Jellyfin fingerprints per season) — but most shows never change theirs, so put()
// writes BOTH keys and an unmarked season inherits the most recent mark instead of demanding a fresh one.
// Device-local JSON, never synced: it is a small personal preference, not library metadata.
#pragma once
#include "MediaSegments.h"

#include <utility>   // std::move (do not rely on a transitive Qt include)
#include <QHash>
#include <QString>
#include <QVector>

class SegmentStore
{
public:
    explicit SegmentStore(QString filePath) : file_(std::move(filePath)) {}
    void load();
    void save() const;

    // The season's segments, else the series-level fallback, else empty.
    QVector<MediaSegments::Segment> lookup(const QString& seriesKey, int season) const;
    // Replaces any segment of the SAME type at that scope and leaves other types alone, so marking credits
    // never clobbers a learned intro. Writes the season key and the bare series key together.
    void put(const QString& seriesKey, int season, const MediaSegments::Segment& seg);
    // Drops this season's entry. The series-level entry survives, so lookup falls back rather than going dark.
    void forget(const QString& seriesKey, int season);

    // season <= 0 means "unknown", which keys the bare series entry rather than a bogus "|s0".
    static QString keyFor(const QString& seriesKey, int season)
    {
        return season > 0 ? seriesKey + QStringLiteral("|s") + QString::number(season) : seriesKey;
    }

private:
    QString file_;
    QHash<QString, QVector<MediaSegments::Segment>> byKey_;
};
```

- [ ] **Step 5: Write `SegmentStore.cpp`**

```cpp
#include "SegmentStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// Replace the same-type entry in place, or append. Shared by put()'s two writes (season key + series key).
void upsert(QVector<MediaSegments::Segment>& list, const MediaSegments::Segment& seg)
{
    for (MediaSegments::Segment& s : list)
        if (s.type == seg.type) { s = seg; return; }
    list.push_back(seg);
}

} // namespace

void SegmentStore::load()
{
    byKey_.clear();
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) return;              // no file yet is a normal empty store
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it)
    {
        QVector<MediaSegments::Segment> list;
        for (const QJsonValue v : it.value().toArray())
        {
            const QJsonObject o = v.toObject();
            const auto ty = MediaSegments::typeFromString(o.value(QStringLiteral("t")).toString());
            if (!ty) continue;                             // an unknown type from a newer build is skipped
            const double s = o.value(QStringLiteral("s")).toDouble();
            const double e = o.value(QStringLiteral("e")).toDouble();
            if (e <= s) continue;
            list.push_back(MediaSegments::Segment{ s, e, *ty });
        }
        if (!list.isEmpty()) byKey_.insert(it.key(), list);
    }
}

void SegmentStore::save() const
{
    QJsonObject root;
    for (auto it = byKey_.constBegin(); it != byKey_.constEnd(); ++it)
    {
        QJsonArray arr;
        for (const MediaSegments::Segment& s : it.value())
        {
            QJsonObject o;
            o.insert(QStringLiteral("s"), s.start);
            o.insert(QStringLiteral("e"), s.end);
            o.insert(QStringLiteral("t"), MediaSegments::typeToString(s.type));
            arr.append(o);
        }
        root.insert(it.key(), arr);
    }
    QFile f(file_);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QVector<MediaSegments::Segment> SegmentStore::lookup(const QString& seriesKey, int season) const
{
    if (seriesKey.isEmpty()) return {};
    const QVector<MediaSegments::Segment> exact = byKey_.value(keyFor(seriesKey, season));
    if (!exact.isEmpty()) return exact;
    return byKey_.value(seriesKey);                        // the series-level fallback
}

void SegmentStore::put(const QString& seriesKey, int season, const MediaSegments::Segment& seg)
{
    if (seriesKey.isEmpty() || seg.end <= seg.start) return;
    upsert(byKey_[keyFor(seriesKey, season)], seg);
    if (season > 0) upsert(byKey_[seriesKey], seg);        // the next unmarked season inherits this
    save();
}

void SegmentStore::forget(const QString& seriesKey, int season)
{
    if (seriesKey.isEmpty()) return;
    byKey_.remove(keyFor(seriesKey, season));
    save();
}
```

- [ ] **Step 6: Build and run — expect PASS**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target probe_segments && ./build/Release/probe_segments.exe
```
Expected: `SEGMENTS-OK`, exit 0.

- [ ] **Step 7: Commit**

```bash
git add native/src/core/SegmentStore.h native/src/core/SegmentStore.cpp native/tools/probe_segments.cpp native/CMakeLists.txt
git commit -m "feat: SegmentStore — learned intro/credits ranges with a season->series fallback"
```

---

### Task 3: `MpvWidget::chapters()` / `fps()` + Settings on both surfaces

**Files:**
- Modify: `native/src/video/MpvWidget.h:44-56` (declarations), `native/src/video/MpvWidget.cpp:487+` (beside `tracksOfType`)
- Modify: `native/src/core/Settings.h:22-24`, `native/src/core/Settings.cpp`
- Modify: `native/src/ui/MainWindow.cpp` themed Playback section (`:8208-8221`), themed row handlers (near `:8359`), classic QWidget Playback block (`:8633-8640`)

**Interfaces:**
- Consumes: `MediaSegments::Chapter` (Task 1).
- Produces: `QVector<MediaSegments::Chapter> MpvWidget::chapters() const`, `double MpvWidget::fps() const`, `Settings::skipSegments()/setSkipSegments(bool)`, `Settings::skipSegmentsAuto()/setSkipSegmentsAuto(bool)`. Task 4 calls all four.

- [ ] **Step 1: Declare the mpv accessors**

In `native/src/video/MpvWidget.h`, add `#include "MediaSegments.h"` near the top, and after the `Track` declarations (`:44-45`):

```cpp
    // The current file's chapters, with their titles — the raw material for chapter-derived skip segments.
    // (nextChapter()/prevChapter() below are relative jumps and cannot answer "is chapter 2 called Intro?".)
    QVector<MediaSegments::Chapter> chapters() const;
    // Container frame rate, 0 when unknown. Only needed to convert a Kodi .edl's "#<frame>" time form.
    double fps() const;
```

- [ ] **Step 2: Implement them**

In `native/src/video/MpvWidget.cpp`, immediately after the `tracksOfType` static function (ends ~`:518`):

```cpp
QVector<MediaSegments::Chapter> MpvWidget::chapters() const
{
    QVector<MediaSegments::Chapter> out;
    if (!mpv) return out;
    int64_t count = 0;
    mpv_get_property(mpv, "chapter-list/count", MPV_FORMAT_INT64, &count);
    for (int64_t i = 0; i < count; ++i)
    {
        char key[80];
        auto field = [&](const char* name) {
            std::snprintf(key, sizeof key, "chapter-list/%lld/%s", static_cast<long long>(i), name);
            return key;
        };
        MediaSegments::Chapter c;
        mpv_get_property(mpv, field("time"), MPV_FORMAT_DOUBLE, &c.time);
        char* ti = mpv_get_property_string(mpv, field("title"));
        if (ti) { c.title = QString::fromUtf8(ti); mpv_free(ti); }
        out.push_back(c);
    }
    return out;
}

double MpvWidget::fps() const
{
    if (!mpv) return 0.0;
    double f = 0.0;
    mpv_get_property(mpv, "container-fps", MPV_FORMAT_DOUBLE, &f);
    return f > 0.0 ? f : 0.0;
}
```

- [ ] **Step 3: Add the settings**

In `native/src/core/Settings.h`, after the `autoplayNextEpisode` block (`:22-24`):

```cpp
    // Skip an episode's intro / end credits when one is known (default on). skipSegmentsAuto seeks silently
    // instead of offering the on-screen chip (default off — a wrong learned range is recoverable when it is
    // a button you ignored, and invisible when it is a seek that already happened).
    bool skipSegments();
    void setSkipSegments(bool on);
    bool skipSegmentsAuto();
    void setSkipSegmentsAuto(bool on);
```

In `native/src/core/Settings.cpp`, after the `autoplayNextEpisode` pair (`:51`). Note the accessor is
`store()` and the key namespace is `playback/`, matching its neighbours:

```cpp
bool Settings::skipSegments() { return store().value(QStringLiteral("playback/skipSegments"), true).toBool(); }
void Settings::setSkipSegments(bool on) { store().setValue(QStringLiteral("playback/skipSegments"), on); }
bool Settings::skipSegmentsAuto() { return store().value(QStringLiteral("playback/skipSegmentsAuto"), false).toBool(); }
void Settings::setSkipSegmentsAuto(bool on) { store().setValue(QStringLiteral("playback/skipSegmentsAuto"), on); }
```

> `setAutoplayNextEpisode` is written as a multi-line body (it does extra work); the four above are
> single-line because they only read and write. Follow the file's existing brace style for each.

- [ ] **Step 4: Add the themed rows**

In `MainWindow::openGeneralSettings()`, in the Playback section (`:8208-8221`), after the `pb.autonext` toggle:

```cpp
    toggle(QStringLiteral("pb.skipseg"), tr("Skip intros and credits"),
           tr("Offer to skip an episode's opening and end credits when one is known."),
           Settings::skipSegments());
    toggle(QStringLiteral("pb.skipsegauto"), tr("Skip automatically"),
           tr("Seek past them without asking, instead of showing a button."),
           Settings::skipSegmentsAuto());
    info(tr("While a video is playing: S skips the offered segment, I marks where one starts and ends."));
```

And in the themed row handler dispatch (beside the `pb.autonext` case near `:8359`):

```cpp
    if (id == QStringLiteral("pb.skipseg"))     { Settings::setSkipSegments(!Settings::skipSegments()); return; }
    if (id == QStringLiteral("pb.skipsegauto")) { Settings::setSkipSegmentsAuto(!Settings::skipSegmentsAuto()); return; }
```

> Match the surrounding handlers' exact shape (they may re-render the panel after toggling — do whatever `pb.autonext` does).

- [ ] **Step 5: Add the classic QWidget rows**

In the classic Playback block, immediately after `v->addWidget(autoNext);` (`:8640`). The neighbouring
checkbox takes **no parent argument** and carries a `font-size:15px` stylesheet — match that exactly:

```cpp
        auto* skipSeg = new QCheckBox(tr("Skip intros and credits"));
        skipSeg->setStyleSheet(QStringLiteral("font-size:15px;"));
        skipSeg->setChecked(Settings::skipSegments());
        connect(skipSeg, &QCheckBox::toggled, this, [](bool c) { Settings::setSkipSegments(c); });
        v->addWidget(skipSeg);

        auto* skipAuto = new QCheckBox(tr("Skip them automatically (no button)"));
        skipAuto->setStyleSheet(QStringLiteral("font-size:15px;"));
        skipAuto->setChecked(Settings::skipSegmentsAuto());
        connect(skipAuto, &QCheckBox::toggled, this, [](bool c) { Settings::setSkipSegmentsAuto(c); });
        v->addWidget(skipAuto);

        auto* skipHint = new QLabel(tr("While a video is playing: S skips the offered segment, "
                                       "I marks where one starts and ends."));
        skipHint->setStyleSheet(QStringLiteral("font-size:13px;color:#999;"));
        skipHint->setWordWrap(true);
        v->addWidget(skipHint);
```

- [ ] **Step 6: Build the app**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target everythingbox
```
Expected: builds clean.

- [ ] **Step 7: Commit**

```bash
git add native/src/video/MpvWidget.h native/src/video/MpvWidget.cpp native/src/core/Settings.h native/src/core/Settings.cpp native/src/ui/MainWindow.cpp
git commit -m "feat: expose mpv chapter-list + container-fps; skip-segment settings on both surfaces"
```

---

### Task 4: Wire it up — context, gather, tracker, auto-skip

Delivers a **working feature with no new UI**: with `skipSegmentsAuto` on, intros and credits are skipped silently with a notice. The chip comes in Task 5.

**Files:**
- Modify: `native/src/ui/MainWindow.h` (members)
- Modify: `native/src/ui/MainWindow.cpp` — `armSubtitleFetch` neighbourhood (`:6227-6243`), `openVideoPath` (`:2406`), `onDuration` (`:10164`), `onPosition` (`:10178`)

**Interfaces:**
- Consumes: `MediaSegments::{Segment,SegmentType,Key,Tracker,parseEdl,fromChapters,resolve,keyFor}`, `SegmentStore`, `MpvWidget::chapters()/fps()/setPosition()`, `Settings::skipSegments()/skipSegmentsAuto()`, `Notifier::playerNotice`, `AppPaths::dataDir()`.
- Produces: `segCtx_`, `segTracker_`, `segStore_`, `MainWindow::gatherSegments()`, `MainWindow::onSegmentEntered(const MediaSegments::Segment&)`. Task 5 calls `onSegmentEntered` and reads `segCtx_`.

- [ ] **Step 1: Add the members**

In `native/src/ui/MainWindow.h`, near the `subCtx_` declaration (`:518`), add `#include "core/MediaSegments.h"` and `#include "core/SegmentStore.h"` at the top and:

```cpp
    // Intro/credits skipping. A SEPARATE context from subCtx_ on purpose: subCtx_ is the subtitle system's
    // and is deliberately cleared on the openVideoPath route, whereas segments can still be derived there
    // from the filename alone.
    struct SegmentCtx { QString seriesKey; int season = 0; QString localPath; };
    SegmentCtx               segCtx_;
    MediaSegments::Tracker   segTracker_;
    SegmentStore*            segStore_ = nullptr;
    void gatherSegments();
    void onSegmentEntered(const MediaSegments::Segment& seg);
```

- [ ] **Step 2: Create the store**

In the `MainWindow` constructor, beside where the subtitle cache is constructed:

```cpp
    segStore_ = new SegmentStore(QDir(AppPaths::dataDir()).filePath(QStringLiteral("segments.json")));
    segStore_->load();
```

- [ ] **Step 3: Arm the context on every open**

`segCtx_` must be set on **both** open routes. In `armSubtitleFetch` (`:6227`), after `subCtx_` is populated:

```cpp
    // Segment context rides along on the tile route, where the stream id gives series + season directly.
    const MediaSegments::Key k = MediaSegments::keyFor(imdbStreamId, localPath);
    segCtx_ = { k.seriesKey, k.season, localPath };
```

And in `openVideoPath` (`:2406`), where `subCtx_` is **cleared**, set the segment context from the filename instead:

```cpp
    // subCtx_ is cleared here (no catalog metadata), but a filename alone can still name the show and
    // season — so segments keep working on the Recents / "Open Video…" route.
    const MediaSegments::Key k = MediaSegments::keyFor(QString(), path);
    segCtx_ = { k.seriesKey, k.season, path };
```

> Use the actual local variable holding the path at each site; do not invent names.

- [ ] **Step 4: Gather once, when the duration is known**

Add to `native/src/ui/MainWindow.cpp` near `onDuration`:

```cpp
// Collect every provider's segments for the file that just loaded, resolve them, and arm the tracker.
// Runs ONCE per file (from onDuration, because credits classification needs the length), never per tick.
void MainWindow::gatherSegments()
{
    segTracker_.reset({});
    if (!Settings::skipSegments() || !player_) return;

    // 1. Kodi .edl sidecar, local files only — a stream has no sidecar.
    QVector<MediaSegments::Segment> edl;
    if (!segCtx_.localPath.isEmpty())
    {
        const QFileInfo fi(segCtx_.localPath);
        const QString edlPath = fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".edl"));
        QFile f(edlPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text))
            edl = MediaSegments::parseEdl(QString::fromUtf8(f.readAll()), duration_, player_->fps());
    }

    // 2. Named chapters. 3. What the user taught us for this season.
    const QVector<MediaSegments::Segment> chapters =
        MediaSegments::fromChapters(player_->chapters(), duration_);
    const QVector<MediaSegments::Segment> learned =
        segStore_ ? segStore_->lookup(segCtx_.seriesKey, segCtx_.season) : QVector<MediaSegments::Segment>{};

    segTracker_.reset(MediaSegments::resolve(edl, chapters, learned));
}
```

Call it at the end of `onDuration` (`:10176`), after the resume seek:

```cpp
    gatherSegments();
```

- [ ] **Step 5: Feed the tracker from the position tick**

In `onPosition` (`:10178`), after the existing `session_->setPosition(seconds);`:

```cpp
    lastPos_ = seconds;   // the marks menu needs "where am I now"; nothing else in MainWindow tracks it
    if (const auto seg = segTracker_.onPosition(seconds)) onSegmentEntered(*seg);
```

`MpvWidget` has **no** position accessor and `MainWindow` stores only `duration_` (`MainWindow.h:628`), so
add the member beside it:

```cpp
    double lastPos_ = 0.0;   // last reported playback position, for the segment marks menu
```

- [ ] **Step 6: Act on it (auto path only, for now)**

```cpp
// A segment was just entered. Task 5 replaces the else branch with the on-screen chip.
void MainWindow::onSegmentEntered(const MediaSegments::Segment& seg)
{
    const bool isCredits = seg.type == MediaSegments::SegmentType::Credits;
    if (seg.type != MediaSegments::SegmentType::Intro && !isCredits) return; // Recap/Commercial are stored,
                                                                            // not acted on
    if (!Settings::skipSegmentsAuto()) return;                              // Task 5: show the chip instead

    if (isCredits && Settings::autoplayNextEpisode()) { tryPlayNextEpisode(); return; }
    player_->setPosition(seg.end);
    // Name what was skipped: an auto-skip from a slightly wrong learned range is otherwise an unexplained
    // jump, and the user needs a thread to pull to find the setting.
    notifier_->playerNotice(isCredits ? tr("Skipped the credits") : tr("Skipped the intro"), 2500);
}
```

- [ ] **Step 7: Build**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target everythingbox
```
Expected: builds clean.

- [ ] **Step 8: Run the full suite**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 9: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
git commit -m "feat: gather intro/credits segments per file and auto-skip them"
```

---

### Task 5: The chip, the `S` key, and the `I` marks menu

**Files:**
- Modify: `native/src/ui/MainWindow.h` (members)
- Modify: `native/src/ui/MainWindow.cpp` — chip construction beside `streamIssueBtn_` (`:760-774`), `positionMediaControls` (`:2240-2253`), `hideMediaControls` (`:2263`), player `keyPressEvent` (`:2037-2055`), `onSegmentEntered`

**Interfaces:**
- Consumes: everything from Task 4 plus `NavMenu` (`native/src/ui/nav/NavOverlay.h`), `tryPlayNextEpisode()`.
- Produces: no new cross-task interface — this is the last implementation task.

- [ ] **Step 1: Add the members**

In `native/src/ui/MainWindow.h`, beside `streamIssueBtn_`:

```cpp
    QPushButton*             skipChip_ = nullptr;   // non-modal "Skip Intro" / "Next Episode" over the video
    QTimer*                  skipChipTimer_ = nullptr;
    MediaSegments::Segment   skipChipSeg_;          // what the visible chip would skip
    void showSkipChip(const MediaSegments::Segment& seg);
    void hideSkipChip();
    void activateSkipChip();
    void showSegmentMarksMenu();
```

- [ ] **Step 2: Build the chip**

In the constructor, immediately after the `streamIssueBtn_` block (`:774`):

```cpp
    // The skip affordance. Deliberately NOT a NavOverlay: every overlay grabs all input (keyboard grab +
    // NavContext routing, topmost owns everything), which is exactly wrong for a non-modal prompt over live
    // video. This follows streamIssueBtn_ above — a plain child of player_, composited over the GL surface.
    skipChip_ = new QPushButton(tr("Skip Intro"), player_);
    skipChip_->setObjectName(QStringLiteral("skipChip"));
    skipChip_->setStyleSheet(QStringLiteral(
        "#skipChip { background: rgba(20,20,24,0.85); color:#e8e8e8; border:2px solid transparent; border-radius:8px;"
        " padding:10px 20px; font-weight:bold; }"
        "#skipChip:hover { background: rgba(45,45,52,0.95); }"
        "#skipChip:focus { background: rgba(90,140,255,0.80); border:2px solid #fff; }"));
    skipChip_->setCursor(Qt::PointingHandCursor);
    skipChip_->hide();
    skipChip_->installEventFilter(this);
    connect(skipChip_, &QPushButton::clicked, this, &MainWindow::activateSkipChip);

    // Its OWN timer, not the shared 4s controlsHideTimer_: the chip must outlive a chrome hide.
    skipChipTimer_ = new QTimer(this);
    skipChipTimer_->setSingleShot(true);
    connect(skipChipTimer_, &QTimer::timeout, this, &MainWindow::hideSkipChip);
```

- [ ] **Step 3: Show / hide / activate**

```cpp
void MainWindow::showSkipChip(const MediaSegments::Segment& seg)
{
    if (!skipChip_) return;
    skipChipSeg_ = seg;
    const bool isCredits = seg.type == MediaSegments::SegmentType::Credits;
    skipChip_->setText(isCredits ? tr("Next Episode") : tr("Skip Intro"));
    skipChip_->adjustSize();
    positionMediaControls();          // lays the chip out too (Step 4)
    skipChip_->show();
    skipChip_->raise();
    skipChipTimer_->start(8000);      // kChipMs
}

void MainWindow::hideSkipChip()
{
    if (!skipChip_) return;
    skipChipTimer_->stop();
    // Do not strand focus on a widget that is about to vanish.
    if (skipChip_->hasFocus() && videoBack_) videoBack_->setFocus(Qt::OtherFocusReason);
    skipChip_->hide();
}

void MainWindow::activateSkipChip()
{
    if (!skipChip_ || !skipChip_->isVisible()) return;
    const MediaSegments::Segment seg = skipChipSeg_;
    hideSkipChip();                   // before acting: tryPlayNextEpisode tears down this playback
    if (seg.type == MediaSegments::SegmentType::Credits) { tryPlayNextEpisode(); return; }
    player_->setPosition(seg.end);
}
```

- [ ] **Step 4: Position it**

In `positionMediaControls()` (`:2240-2253`), at the end — bottom-right, clear of the transport bar:

```cpp
    if (skipChip_ && skipChip_->isVisible())
    {
        const int m = 24;
        const int above = mediaControls_ && mediaControls_->isVisible() ? mediaControls_->height() + m : m;
        skipChip_->move(player_->width() - skipChip_->width() - m,
                        player_->height() - skipChip_->height() - above);
    }
```

- [ ] **Step 5: Replace the auto-only branch in `onSegmentEntered`**

Replace the `if (!Settings::skipSegmentsAuto()) return;` line from Task 4 with:

```cpp
    if (!Settings::skipSegmentsAuto()) { showSkipChip(seg); return; }
```

- [ ] **Step 6: Bind `S` and `I`**

In the player branch of `keyPressEvent` (`:2037-2055`), before `default:`:

```cpp
        case Qt::Key_S:
            // Only meaningful while a chip is up; otherwise it is a no-op, not a surprise.
            if (skipChip_ && skipChip_->isVisible()) activateSkipChip();
            return;
        case Qt::Key_I: showSegmentMarksMenu(); return;
```

- [ ] **Step 7: The marks menu**

```cpp
// The learn tier's entry point. A NavMenu is right here — the user asked for a menu, so modal is correct;
// this is NOT the app-level Esc menu (showEscMenu, :1163), which during playback is unreachable because
// Escape exits the player.
void MainWindow::showSegmentMarksMenu()
{
    if (!player_ || stack_->currentWidget() != playerPage_) return;

    const bool canLearn = !segCtx_.seriesKey.isEmpty();
    const double at = lastPos_;              // set in onPosition (Task 4)

    QStringList rows;
    rows << (canLearn ? tr("Mark intro start here") : tr("Mark intro — unavailable, no series information"))
         << (canLearn ? tr("Mark intro end here")   : tr("Mark intro end — unavailable"))
         << (canLearn ? tr("Mark credits start here") : tr("Mark credits — unavailable"))
         << tr("Forget marks for this season");

    new NavMenu(tr("Skip segments"), rows, [this, canLearn, at](int row) {
        if (row < 0 || !segStore_) return;
        if (!canLearn)
        {
            notifier_->playerNotice(tr("No series information for this file, so there is nothing to mark against."), 4000);
            return;
        }
        if (row == 0) { segIntroStart_ = at; notifier_->playerNotice(tr("Intro starts here. Press I again at the end."), 4000); return; }
        if (row == 1)
        {
            if (segIntroStart_ < 0.0 || at <= segIntroStart_)
            { notifier_->playerNotice(tr("Mark the intro's START first."), 3000); return; }
            segStore_->put(segCtx_.seriesKey, segCtx_.season,
                           { segIntroStart_, at, MediaSegments::SegmentType::Intro });
            segIntroStart_ = -1.0;
            notifier_->playerNotice(tr("Intro remembered for season %1.").arg(segCtx_.season), 3000);
            gatherSegments();      // the new mark applies to the rest of THIS episode too
            return;
        }
        if (row == 2)
        {
            segStore_->put(segCtx_.seriesKey, segCtx_.season,
                           { at, duration_, MediaSegments::SegmentType::Credits });
            notifier_->playerNotice(tr("Credits remembered for season %1.").arg(segCtx_.season), 3000);
            gatherSegments();
            return;
        }
        segStore_->forget(segCtx_.seriesKey, segCtx_.season);
        notifier_->playerNotice(tr("Marks for this season forgotten."), 3000);
        gatherSegments();
    }, this);
}
```

Add the pending-start member to `MainWindow.h` beside the others:

```cpp
    double segIntroStart_ = -1.0;   // a marked intro start awaiting its end (-1 = none pending)
```

- [ ] **Step 8: Hide the chip when playback ends or the file changes**

In `hideMediaControls()` (`:2263`) leave the chip alone (it has its own timer), but in `gatherSegments()` (Task 4) add as the first line after `segTracker_.reset({})`:

```cpp
    hideSkipChip();          // a new file must not inherit the previous one's chip
    segIntroStart_ = -1.0;
```

- [ ] **Step 9: Build and run the suite**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target everythingbox probe_nav && BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: builds clean, `ALL HEADLESS PROBES PASSED`. `probe_nav` is built explicitly because the marks menu adds a new `NavMenu` caller.

- [ ] **Step 10: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
git commit -m "feat: skip chip (S), the intro/credits marks menu (I), and credits->next episode"
```

---

### Task 6: Close-out — live verification and merge

**Files:**
- Modify: `docs/superpowers/specs/2026-07-26-intro-skip-design.md` (Status + an outcome section)
- Modify: `.superpowers/sdd/progress.md`

- [ ] **Step 1: Live-verify against a throwaway copy**

**Never launch or modify the deployed app at `C:\EverythingBox-app` or its ini.** Copy it to a scratch dir, strip `cloud/*` and `sync/*` from the throwaway ini, and drive it with `EB_UITEST=1` + a unique `EB_UITEST_PIPE` via `python native/tools/uitest.py`. Read `verify-app-gui-capture.md` in the memory dir first.

Verify and screenshot each:
1. **`.edl` tier** — put `10 40 3` in a `<name>.edl` beside a test video; play it; the chip appears at 0:10 and lands at 0:40.
2. **Chapter tier** — a file with a chapter named "Opening Credits"; the chip appears at that chapter.
3. **Learn tier** — a plain episode file: press `I`, mark start, `I`, mark end; replay; the chip appears at the marked place.
4. **Season fallback** — the same show's next season offers the mark without re-marking.
5. **Auto-skip setting** — turn it on; the skip happens silently with the notice, no chip.
6. **Credits → next episode** — a credits segment shows "Next Episode" and the hand-off works.
7. **No-series file** — open a movie via `Open Video…`; `I` shows the disabled rows with the reason.

- [ ] **Step 2: Record the outcome in the spec**

Set `**Status:** Complete` and add a "Live verification outcome" section stating exactly which of the seven steps ran and which were deferred, plus any defects found and how they were fixed. **Do not claim a step passed that was not run.**

- [ ] **Step 3: Merge**

```bash
git checkout main && git pull --ff-only && git merge local/intro-skip --no-edit
```
On a version-line conflict in `native/CMakeLists.txt` / `native/src/main.cpp`, take the **higher** patch number.

- [ ] **Step 4: Build EVERY probe target on the merged tree**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && T=$(grep -o 'add_executable([[:space:]]*probe_[a-z0-9_]*' native/CMakeLists.txt | sed 's/.*(\s*//' | tr '\n' ' ') && cmake --build build --config Release --target $T everythingbox
```
Expected: exit 0. This catches a latent link break in a probe that compiles a source now depending on `MediaSegments` — that class of break has been caught at a merge gate on this project more than once, and the suite alone does not catch it because an unbuilt probe is silently skipped.

- [ ] **Step 5: Suite, push, delete the branch**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && BUILD_DIR=build bash native/tools/run-headless-probes.sh && git push origin main && git branch -d local/intro-skip
```
Expected: `ALL HEADLESS PROBES PASSED`, push succeeds.

- [ ] **Step 6: Redeploy and verify the copy**

```bash
cp build/Release/EverythingBox.exe /c/EverythingBox-app/EverythingBox.exe && md5sum build/Release/EverythingBox.exe /c/EverythingBox-app/EverythingBox.exe
```
Expected: the two hashes match.

- [ ] **Step 7: Update the ledger**

Append a section to `.superpowers/sdd/progress.md` recording the merge commit, what live verification covered, and any follow-ups.
