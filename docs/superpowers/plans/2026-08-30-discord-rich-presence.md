# Discord Rich Presence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** EverythingBox tells Discord what the user is watching, playing, listening to or reading, with real artwork, a countdown, and an off switch.

**Architecture:** Four new files in `native/src/core/`, in the seam shape `Scrobble.h` / `ScrobbleProvider.h` / `Scrobbler.cpp` already established: pure rules that touch nothing (`Presence.h`), a non-`QObject` transport seam a probe can fake (`PresenceTransport.h`), the single I/O unit (`DiscordPresence.cpp`, a `QLocalSocket` speaking Discord's IPC framing), and an orchestrator (`PresenceController.cpp`) that gates on settings and sends only when the card actually changes. Six existing seams feed it.

**Tech Stack:** Qt 6 / C++17, `QtCore` + `QtNetwork` (`QLocalSocket`). No new third-party dependency, no Discord SDK.

Design spec: [`docs/superpowers/specs/2026-08-30-discord-rich-presence-design.md`](../specs/2026-08-30-discord-rich-presence-design.md)

## Global Constraints

- **Build named targets only.** `cmake --build build --config Release --target everythingbox` / `--target probe_presence`. A target-less build compiles 52+ probe harnesses.
- **The gate must end with `ALL HEADLESS PROBES PASSED`:** `BUILD_DIR=build bash native/tools/run-headless-probes.sh`.
- **A new probe is registered in three places** or it silently does not run: the `add_executable` block in `native/CMakeLists.txt`, the `for p in ...` list in `native/tools/run-headless-probes.sh`, and the `--target` list in the "Build probes" step of `.github/workflows/ci.yml`. A **fourth** site applies whenever the app itself calls the new code: `qt_add_executable(everythingbox ...)` in `native/CMakeLists.txt` (CONTRIBUTING.md "The app's own source list is the fourth place").
- **Byte-exact file edits.** `native/tools/run-headless-probes.sh` is CRLF and `native/CMakeLists.txt` contains a lone CR. Never normalise line endings in either; edit in place with a byte-preserving edit.
- **Run `bash -n native/tools/run-headless-probes.sh` after touching it.** A merged-in gate section has twice eaten a neighbouring gate's `fi`.
- **Both settings builders or it does not exist.** Every user-facing setting goes in the themed builder (`sep`/`toggle`/`info` rows in `MainWindow::openGeneralSettings`) *and* the classic `QWidget` builder below it in the same function.
- **No old-brand literals.** Use `AppBrand::kName` / `AppBrand::kSiteUrl` — the old-brand gate scans the tree.
- **No AI attribution in commits.** No `Co-Authored-By`, no generated-by footer. Conventional prefixes (`feat:`, `fix:`, `docs:`, `test:`).
- **Discord field limits, copied from the spec:** `details` and `state` are **128 bytes** each (not characters); at most **2** buttons; activity types are Playing `0`, Listening `2`, Watching `3`; rate limit **5 updates per 20 seconds**.
- **Running a probe needs Qt on PATH**, or it dies in the loader before `main` and looks like a failing
  probe: `export PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH"`. The build additionally needs `/c/mpv-dev`.
- **A fresh worktree has no submodule.** `git submodule update --init --recursive external/RetroPark` before
  the first configure, or CMake fails in `ExternalProject_Add` with "is not an existing non-empty directory".
- **A fresh worktree has no probes built**, so the gate reports every one of them missing. Build CI's list
  once first: `cmake --build build --config Release --parallel --target $(grep -o 'cmake --build build --target probe_.*' .github/workflows/ci.yml | head -1 | sed 's/cmake --build build --target //')`
- **Work happens in the worktree** `C:\Users\cubma\goliath-wt-discord` on branch `feat/discord-rich-presence`. The main tree is shared with other sessions — never commit there.

---

### Task 1: `Presence.h` / `Presence.cpp` — the pure rules, and the probe that pins them

**Files:**
- Create: `native/src/core/Presence.h`
- Create: `native/src/core/Presence.cpp`
- Create: `native/tools/probe_presence.cpp`
- Modify: `native/CMakeLists.txt` (add `probe_presence`; add the two sources to the app target)
- Modify: `native/tools/run-headless-probes.sh` (add `"probe_presence PRESENCE-OK"`)
- Modify: `.github/workflows/ci.yml` (add `probe_presence` to the "Build probes" `--target` list)

**Interfaces:**
- Consumes: nothing.
- Produces: `Presence::Kind`, `Presence::Item`, `Presence::Activity`, `Presence::build(const Item&, double positionSec, double durationSec, bool paused, qint64 nowUnix)`, `Presence::idle(qint64 sessionStartUnix)`, `Presence::clampUtf8(const QString&, int maxBytes)`, `Presence::typeFor(Kind)`, `Presence::fallbackAsset(Kind)`, `Presence::kindLabel(Kind)`, `Presence::hasCountdown(Kind)`, and the constants `kPlaying`/`kListening`/`kWatching`/`kMaxFieldBytes`/`kMaxButtons`.

- [ ] **Step 1: Write the header**

Create `native/src/core/Presence.h`:

```cpp
// DISCORD RICH PRESENCE — the rules that decide what a presence card SAYS, and nothing that touches the
// world.
//
// WHY THIS IS A SEPARATE FILE FROM THE TRANSPORT. Everything below is arithmetic and string handling over
// plain structs: which Discord activity type a kind maps to, whether a countdown is meaningful, what happens
// to the timestamp when playback pauses, how a title is cut to fit a byte budget. None of it is a property
// of Discord's socket, and every part of it is wrong in a way nothing in the app would notice — a card that
// counts a paused film down to zero, a truncation that splits a codepoint and makes Discord discard the
// update whole. So it lives here, as pure functions, and probe_presence drives every arm of it with no
// Discord running, no socket and no clock.
//
// NOTHING HERE READS THE CLOCK. `nowUnix` is a PARAMETER, for the same reason trakt::planMissed takes its
// own: a rule that reads the clock itself can only be tested by waiting.
#pragma once
#include <QPair>
#include <QString>
#include <QVector>

namespace Presence
{
    // Discord's own activity type numbers. SET_ACTIVITY accepts only these three (plus Competing, which
    // nothing here is).
    inline constexpr int kPlaying   = 0;
    inline constexpr int kListening = 2;
    inline constexpr int kWatching  = 3;

    // Discord's limit on `details` and `state` is 128 BYTES, not 128 characters, and an update that exceeds
    // it is discarded whole rather than truncated — so an over-long CJK title makes presence silently stop
    // updating rather than showing a shortened name. See clampUtf8.
    inline constexpr int kMaxFieldBytes = 128;
    inline constexpr int kMaxButtons    = 2;

    enum class Kind { None, Movie, Episode, LiveTv, Music, Audiobook, Game, PcGame, Reading };

    // What the app knows about the thing that is open. Filled in at the six hook points from the same locals
    // that already build the RecentItem there — this struct deliberately holds nothing that would need a new
    // lookup, a scrape or a network call.
    struct Item
    {
        Kind    kind = Kind::None;
        QString title;      // "The Bear"
        QString subtitle;   // "S3E4 · Violet" / "Sigur Rós — Ágætis byrjun" / "SNES" / "Reading · Ch. 3"
        QString artUrl;     // used ONLY when it starts with https:// (Discord's CDN cannot fetch a local
                            // path); anything else falls back to the uploaded key for the kind
        QString imdbId;     // "tt0083658" or "ttShow:3:4" -> the IMDb button. Empty for everything else.
        QString system;     // console id / storefront, for the caller's own use when building `subtitle`
    };

    // Exactly what goes on the wire. Compared field-for-field by the orchestrator: two builds that are equal
    // produce no frame at all, which is what makes Discord's 5-per-20-seconds limit a non-issue rather than
    // something to engineer around.
    struct Activity
    {
        bool    valid = false;   // false = there is nothing to show; the orchestrator clears instead
        int     type  = kPlaying;
        QString details;
        QString state;
        QString largeImage, largeText;
        QString smallImage, smallText;
        qint64  startUnix = 0;   // set -> Discord counts UP from here
        qint64  endUnix   = 0;   // set -> Discord counts DOWN to here. Never both.
        QVector<QPair<QString, QString>> buttons;   // label, url — at most kMaxButtons

        bool operator==(const Activity& o) const;
        bool operator!=(const Activity& o) const { return !(*this == o); }
    };

    int     typeFor(Kind k);        // Watching / Listening / Playing
    QString fallbackAsset(Kind k);  // the uploaded asset key: "movie", "tv", "livetv", …
    QString kindLabel(Kind k);      // the small badge's hover text: "Movie", "Live TV", …

    // Whether a countdown is meaningful for this kind. A film and a track end; a live channel, a game and a
    // book do not, and a countdown on one of those would be a fabricated number.
    bool hasCountdown(Kind k);

    // Cut `s` to at most `maxBytes` UTF-8 bytes WITHOUT splitting a codepoint. A naive left(128) does both
    // halves of this wrong: it counts the wrong unit, and it can leave a truncated multi-byte sequence that
    // makes the whole payload invalid.
    QString clampUtf8(const QString& s, int maxBytes);

    // The card for whatever is open. An empty title or Kind::None yields an invalid Activity (send nothing).
    Activity build(const Item& item, double positionSec, double durationSec, bool paused, qint64 nowUnix);

    // The card for "the app is open but nothing is playing".
    Activity idle(qint64 sessionStartUnix);
}
```

- [ ] **Step 2: Write the failing probe**

Create `native/tools/probe_presence.cpp`:

```cpp
// Headless test for DISCORD RICH PRESENCE. Prints PRESENCE-OK.
//
// §1 THE MAPPING. Each Kind's activity type, asset key and badge. Asserted per kind rather than in a loop so
//    a wrong arm names itself.
// §2 THE BYTE CLAMP. The arm that catches a naive left(128): Discord's limit is BYTES, and an over-long
//    update is discarded whole, so getting this wrong makes presence stop updating rather than show a
//    shortened title. Driven with multi-byte text, and asserted to land on a codepoint boundary.
// §3 TIMESTAMPS. A countdown only where one is meaningful; pause drops it; resume restores it from the
//    position resumed at; a live channel and a game count UP.
// §4 ART AND BUTTONS. An https poster is used verbatim, anything else falls back to the kind's key; the IMDb
//    button appears only for an IMDB-keyed item and never pushes the card past two buttons.
// §5 EQUALITY, which is the whole basis of the send-only-on-change rule in PresenceController.
#include "Presence.h"

#include <QString>
#include <cstdio>

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

using Presence::Activity;
using Presence::Item;
using Presence::Kind;

static Item movie()
{
    Item i; i.kind = Kind::Movie; i.title = QStringLiteral("Blade Runner");
    i.subtitle = QStringLiteral("1982"); i.imdbId = QStringLiteral("tt0083658");
    i.artUrl = QStringLiteral("https://img.example/poster.jpg");
    return i;
}

int main()
{
    const qint64 kNow = 1'700'000'000;

    // ---- §1 THE MAPPING ----------------------------------------------------------------------------
    CHECK(Presence::typeFor(Kind::Movie)     == Presence::kWatching,  "map/movie is Watching");
    CHECK(Presence::typeFor(Kind::Episode)   == Presence::kWatching,  "map/episode is Watching");
    CHECK(Presence::typeFor(Kind::LiveTv)    == Presence::kWatching,  "map/live TV is Watching");
    CHECK(Presence::typeFor(Kind::Music)     == Presence::kListening, "map/music is Listening");
    CHECK(Presence::typeFor(Kind::Audiobook) == Presence::kListening, "map/audiobook is Listening");
    CHECK(Presence::typeFor(Kind::Game)      == Presence::kPlaying,   "map/game is Playing");
    CHECK(Presence::typeFor(Kind::PcGame)    == Presence::kPlaying,   "map/pc game is Playing");
    // Reading has no honest Discord type; it takes Playing and puts the verb in the body. Pinned so the
    // choice is deliberate rather than a default nobody revisited.
    CHECK(Presence::typeFor(Kind::Reading)   == Presence::kPlaying,   "map/reading is Playing (no Reading type exists)");

    CHECK(Presence::fallbackAsset(Kind::Movie)     == QStringLiteral("movie"),     "asset/movie");
    CHECK(Presence::fallbackAsset(Kind::Episode)   == QStringLiteral("tv"),        "asset/episode");
    CHECK(Presence::fallbackAsset(Kind::LiveTv)    == QStringLiteral("livetv"),    "asset/live TV");
    CHECK(Presence::fallbackAsset(Kind::Music)     == QStringLiteral("music"),     "asset/music");
    CHECK(Presence::fallbackAsset(Kind::Audiobook) == QStringLiteral("audiobook"), "asset/audiobook");
    CHECK(Presence::fallbackAsset(Kind::Game)      == QStringLiteral("game"),      "asset/game");
    CHECK(Presence::fallbackAsset(Kind::PcGame)    == QStringLiteral("game"),      "asset/pc game shares the game key");
    CHECK(Presence::fallbackAsset(Kind::Reading)   == QStringLiteral("book"),      "asset/reading");

    CHECK(!Presence::build(Item{}, 0, 0, false, kNow).valid,
          "map/none: an empty item is not a card — the orchestrator clears rather than sending a blank");
    {
        Item titleless; titleless.kind = Kind::Movie;
        CHECK(!Presence::build(titleless, 0, 0, false, kNow).valid,
              "map/untitled: a kind with no title is not a card either");
    }

    // ---- §2 THE BYTE CLAMP -------------------------------------------------------------------------
    {
        CHECK(Presence::clampUtf8(QStringLiteral("short"), 128) == QStringLiteral("short"),
              "clamp/short: a title inside the budget is untouched");

        // 60 copies of a 3-byte character = 180 bytes, over the 128-byte budget.
        const QString cjk(60, QChar(0x6F22));       // 漢
        const QString cut = Presence::clampUtf8(cjk, Presence::kMaxFieldBytes);
        CHECK(cut.toUtf8().size() <= Presence::kMaxFieldBytes,
              "clamp/cjk: the result fits the BYTE budget");
        CHECK(cut.toUtf8().size() == 126 && cut.size() == 42,
              "clamp/cjk: ...by whole characters (42x3=126), not by cutting the 43rd in half");
        CHECK(QString::fromUtf8(cut.toUtf8()) == cut,
              "clamp/cjk: the result round-trips through UTF-8 — no orphaned continuation byte");

        // A 2-byte character straddling the boundary is the case a byte-count-only clamp gets wrong.
        const QString acc = QString(127, QLatin1Char('a')) + QChar(0x00E9);  // 127 + 2 bytes = 129
        const QString acut = Presence::clampUtf8(acc, Presence::kMaxFieldBytes);
        CHECK(acut.toUtf8().size() == 127 && acut.endsWith(QLatin1Char('a')),
              "clamp/straddle: a 2-byte char that will not fit is dropped whole, not halved");

        const Activity longCard = Presence::build([]{ Item i = movie();
            i.title = QString(300, QLatin1Char('x')); return i; }(), 0, 0, false, kNow);
        CHECK(longCard.details.toUtf8().size() <= Presence::kMaxFieldBytes,
              "clamp/build: build() applies the clamp to details");
    }

    // ---- §3 TIMESTAMPS -----------------------------------------------------------------------------
    {
        // A film 20 minutes into 100 has 80 minutes left, and Discord is told the wall-clock instant it ends.
        const Activity a = Presence::build(movie(), 1200.0, 6000.0, false, kNow);
        CHECK(a.endUnix == kNow + 4800 && a.startUnix == 0,
              "time/countdown: a film reports the instant it ends, and no start");

        const Activity paused = Presence::build(movie(), 1200.0, 6000.0, true, kNow);
        CHECK(paused.endUnix == 0 && paused.startUnix == 0,
              "time/paused: NEITHER timestamp survives a pause — Discord counts down client-side, so an "
              "end left set would run a stopped film to zero");
        CHECK(paused.state == QStringLiteral("Paused"),
              "time/paused: ...and the second line says so instead of the subtitle");

        const Activity resumed = Presence::build(movie(), 1800.0, 6000.0, false, kNow);
        CHECK(resumed.endUnix == kNow + 4200,
              "time/resume: resuming recomputes the end from the position resumed at, not the original one");

        // No duration (a live stream, an unknown length) has no end to count to.
        CHECK(Presence::build(movie(), 10.0, 0.0, false, kNow).endUnix == 0,
              "time/no duration: an unknown length yields no countdown rather than a fabricated one");

        Item chan; chan.kind = Kind::LiveTv; chan.title = QStringLiteral("BBC One");
        const Activity live = Presence::build(chan, 0.0, 0.0, false, kNow);
        CHECK(live.startUnix == kNow && live.endUnix == 0,
              "time/live: a channel counts UP — it has no end");

        Item g; g.kind = Kind::Game; g.title = QStringLiteral("Super Metroid");
        CHECK(Presence::build(g, 0.0, 0.0, false, kNow).startUnix == kNow,
              "time/game: a game counts up from now");

        CHECK(!Presence::hasCountdown(Kind::Game) && !Presence::hasCountdown(Kind::Reading)
              && !Presence::hasCountdown(Kind::LiveTv),
              "time/kinds: games, reading and live TV have no meaningful end");
        CHECK(Presence::hasCountdown(Kind::Movie) && Presence::hasCountdown(Kind::Music),
              "time/kinds: films and tracks do");
    }

    // ---- §4 ART AND BUTTONS ------------------------------------------------------------------------
    {
        const Activity a = Presence::build(movie(), 0, 0, false, kNow);
        CHECK(a.largeImage == QStringLiteral("https://img.example/poster.jpg"),
              "art/https: a public poster URL is used verbatim — no upload needed");
        CHECK(a.smallImage == QStringLiteral("movie"),
              "art/badge: the small badge always carries the kind's icon");

        Item local = movie();
        local.artUrl = QStringLiteral("C:/Users/me/Posters/blade.jpg");
        CHECK(Presence::build(local, 0, 0, false, kNow).largeImage == QStringLiteral("movie"),
              "art/local: a file path falls back to the kind's key — Discord's CDN cannot fetch a local file");

        Item http = movie();
        http.artUrl = QStringLiteral("http://img.example/poster.jpg");
        CHECK(Presence::build(http, 0, 0, false, kNow).largeImage == QStringLiteral("movie"),
              "art/plain http: only https is accepted");

        CHECK(a.buttons.size() == 2, "buttons/movie: IMDb plus the project link");
        CHECK(a.buttons.at(0).first == QStringLiteral("View on IMDb")
              && a.buttons.at(0).second == QStringLiteral("https://www.imdb.com/title/tt0083658/"),
              "buttons/imdb: built from the tt id the app already holds");

        Item ep = movie();
        ep.kind = Kind::Episode; ep.imdbId = QStringLiteral("tt1234567:3:4");
        CHECK(Presence::build(ep, 0, 0, false, kNow).buttons.at(0).second
                  == QStringLiteral("https://www.imdb.com/title/tt1234567/"),
              "buttons/episode: the season:episode suffix is stripped — it is not part of the title URL");

        Item g; g.kind = Kind::Game; g.title = QStringLiteral("Super Metroid");
        const Activity ga = Presence::build(g, 0, 0, false, kNow);
        CHECK(ga.buttons.size() == 1 && ga.buttons.at(0).first == QStringLiteral("Get EverythingBox"),
              "buttons/game: no IMDb id, so only the project link");

        for (Kind k : { Kind::Movie, Kind::Episode, Kind::LiveTv, Kind::Music, Kind::Audiobook,
                        Kind::Game, Kind::PcGame, Kind::Reading }) {
            Item i; i.kind = k; i.title = QStringLiteral("t"); i.imdbId = QStringLiteral("tt1");
            CHECK(Presence::build(i, 0, 0, false, kNow).buttons.size() <= Presence::kMaxButtons,
                  "buttons/cap: no card ever carries more than two");
        }
    }

    // ---- §5 EQUALITY, the basis of send-only-on-change ----------------------------------------------
    {
        // Two seconds later, the film ends two seconds later too — so the card is NOT equal and a frame is
        // owed. This is the arm that would break if endUnix were stored as "seconds remaining".
        CHECK(Presence::build(movie(), 1200.0, 6000.0, false, kNow)
              == Presence::build(movie(), 1200.0, 6000.0, false, kNow),
              "eq/identical: the same inputs build an equal card");
        CHECK(Presence::build(movie(), 1200.0, 6000.0, false, kNow)
              != Presence::build(movie(), 1200.0, 6000.0, true, kNow),
              "eq/pause: pausing changes the card");
        // The one that matters: a tick that does not move the end instant is not a change. At a steady one
        // second per second, position and now advance together and the end lands on the same instant.
        CHECK(Presence::build(movie(), 1200.0, 6000.0, false, kNow)
              == Presence::build(movie(), 1201.0, 6000.0, false, kNow + 1),
              "eq/steady tick: a second of ordinary playback builds an IDENTICAL card, so nothing is sent");
        CHECK(Presence::build(movie(), 1200.0, 6000.0, false, kNow)
              != Presence::build(movie(), 3000.0, 6000.0, false, kNow),
              "eq/seek: a seek moves the end instant, so a frame IS owed");
    }

    // ---- §6 IDLE ------------------------------------------------------------------------------------
    {
        const Activity i = Presence::idle(kNow - 3600);
        CHECK(i.valid && i.type == Presence::kPlaying, "idle/type");
        CHECK(i.details == QStringLiteral("Browsing") && i.state.isEmpty(), "idle/lines");
        CHECK(i.startUnix == kNow - 3600 && i.endUnix == 0,
              "idle/elapsed: counts up from when the session started");
        CHECK(i.largeImage == QStringLiteral("logo"), "idle/art is the app logo");
    }

    if (fails) { printf("PRESENCE-FAIL %d\n", fails); return 1; }
    printf("PRESENCE-OK\n");
    return 0;
}
```

- [ ] **Step 3: Register the probe in all three places**

In `native/CMakeLists.txt`, immediately after the `probe_scrobble` block (which ends with the `target_link_libraries(probe_scrobble ...)` line), add:

```cmake
    # Headless test for DISCORD RICH PRESENCE. Pure rules only — no socket, no Discord, no clock: Presence.cpp
    # takes `nowUnix` as a parameter precisely so this probe never has to wait for anything.
    add_executable(probe_presence tools/probe_presence.cpp
        src/core/Presence.cpp src/core/Presence.h)
    target_include_directories(probe_presence PRIVATE src src/core)
    target_link_libraries(probe_presence PRIVATE Qt6::Core)
```

In the same file, add the two sources to the app target beside the scrobbling block (after the `src/core/ListenBrainzClient.cpp src/core/ListenBrainzClient.h` line):

```cmake
        # DISCORD RICH PRESENCE: the pure rules; the seam, transport and orchestrator land in later tasks.
        src/core/Presence.cpp src/core/Presence.h
```

In `native/tools/run-headless-probes.sh`, inside the long `for p in ...` list, add `"probe_presence PRESENCE-OK"` immediately after `"probe_scrobble SCROBBLE-OK"`. **This file is CRLF — do not rewrite it, edit the one line in place.**

In `.github/workflows/ci.yml`, add `probe_presence` to the long `--target` list in the "Build probes" step, beside `probe_scrobble`. **This is the site that is easy to miss** — `probe_addon` was maintained for a long time while wired into neither the runner nor CI, so every assertion in it gated nothing.

- [ ] **Step 4: Configure and build the probe to verify it fails**

```bash
cmake --build build --config Release --target probe_presence
```

Expected: **compile failure**, `Presence.cpp` does not exist / undefined references to `Presence::build`.

- [ ] **Step 5: Write the implementation**

Create `native/src/core/Presence.cpp`:

```cpp
#include "Presence.h"
#include "AppBrand.h"

#include <QByteArray>
#include <QtGlobal>

namespace {

// A UTF-8 continuation byte is 10xxxxxx. Cutting on one leaves an orphaned fragment that makes Discord
// discard the whole payload, which presents as "presence stopped updating" and nothing else.
inline bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

} // namespace

bool Presence::Activity::operator==(const Activity& o) const
{
    return valid      == o.valid      && type       == o.type
        && details    == o.details    && state      == o.state
        && largeImage == o.largeImage && largeText  == o.largeText
        && smallImage == o.smallImage && smallText  == o.smallText
        && startUnix  == o.startUnix  && endUnix    == o.endUnix
        && buttons    == o.buttons;
}

int Presence::typeFor(Kind k)
{
    switch (k) {
    case Kind::Movie: case Kind::Episode: case Kind::LiveTv:  return kWatching;
    case Kind::Music: case Kind::Audiobook:                   return kListening;
    // Reading takes Playing because Discord has no fourth verb — the header reads "Playing EverythingBox"
    // and the body carries "Reading". Wrong in the header, honest in the two lines a human reads.
    case Kind::Game: case Kind::PcGame: case Kind::Reading:   return kPlaying;
    case Kind::None:                                          break;
    }
    return kPlaying;
}

QString Presence::fallbackAsset(Kind k)
{
    switch (k) {
    case Kind::Movie:     return QStringLiteral("movie");
    case Kind::Episode:   return QStringLiteral("tv");
    case Kind::LiveTv:    return QStringLiteral("livetv");
    case Kind::Music:     return QStringLiteral("music");
    case Kind::Audiobook: return QStringLiteral("audiobook");
    case Kind::Game: case Kind::PcGame: return QStringLiteral("game");
    case Kind::Reading:   return QStringLiteral("book");
    case Kind::None:      break;
    }
    return QStringLiteral("logo");
}

QString Presence::kindLabel(Kind k)
{
    switch (k) {
    case Kind::Movie:     return QStringLiteral("Movie");
    case Kind::Episode:   return QStringLiteral("TV");
    case Kind::LiveTv:    return QStringLiteral("Live TV");
    case Kind::Music:     return QStringLiteral("Music");
    case Kind::Audiobook: return QStringLiteral("Audiobook");
    case Kind::Game:      return QStringLiteral("Game");
    case Kind::PcGame:    return QStringLiteral("Game");
    case Kind::Reading:   return QStringLiteral("Reading");
    case Kind::None:      break;
    }
    return QString::fromLatin1(AppBrand::kName);
}

bool Presence::hasCountdown(Kind k)
{
    switch (k) {
    case Kind::Movie: case Kind::Episode: case Kind::Music: case Kind::Audiobook: return true;
    default: return false;
    }
}

QString Presence::clampUtf8(const QString& s, int maxBytes)
{
    if (maxBytes <= 0) return QString();
    const QByteArray u = s.toUtf8();
    if (u.size() <= maxBytes) return s;
    int cut = maxBytes;
    while (cut > 0 && isContinuation(u.at(cut))) --cut;
    return QString::fromUtf8(u.left(cut));
}

Presence::Activity Presence::build(const Item& item, double positionSec, double durationSec,
                                   bool paused, qint64 nowUnix)
{
    Activity a;
    if (item.kind == Kind::None || item.title.isEmpty()) return a;   // valid stays false: send nothing

    a.valid      = true;
    a.type       = typeFor(item.kind);
    a.details    = clampUtf8(item.title, kMaxFieldBytes);
    a.state      = paused ? QStringLiteral("Paused") : clampUtf8(item.subtitle, kMaxFieldBytes);
    a.largeImage = item.artUrl.startsWith(QLatin1String("https://")) ? item.artUrl
                                                                     : fallbackAsset(item.kind);
    a.largeText  = clampUtf8(item.title, kMaxFieldBytes);
    a.smallImage = fallbackAsset(item.kind);
    a.smallText  = kindLabel(item.kind);

    // A paused card carries NO timestamp at all. Discord runs the countdown client-side from `end`, so an end
    // left set through a pause counts a stopped film down to zero and then sits there lying.
    if (!paused) {
        const double pos = qMax(0.0, positionSec);
        if (hasCountdown(item.kind) && durationSec > 0.0 && pos < durationSec)
            a.endUnix = nowUnix + qint64(durationSec - pos + 0.5);
        else
            a.startUnix = nowUnix - qint64(pos);
    }

    if (!item.imdbId.isEmpty()) {
        // An episode's id is "ttShow:season:episode"; IMDb's title URL wants the show id alone.
        const QString tt = item.imdbId.section(QLatin1Char(':'), 0, 0);
        if (tt.startsWith(QLatin1String("tt")))
            a.buttons << qMakePair(QStringLiteral("View on IMDb"),
                                   QStringLiteral("https://www.imdb.com/title/%1/").arg(tt));
    }
    if (a.buttons.size() < kMaxButtons)
        a.buttons << qMakePair(QStringLiteral("Get EverythingBox"), QString::fromLatin1(AppBrand::kSiteUrl));
    return a;
}

Presence::Activity Presence::idle(qint64 sessionStartUnix)
{
    Activity a;
    a.valid      = true;
    a.type       = kPlaying;
    a.details    = QStringLiteral("Browsing");
    a.largeImage = QStringLiteral("logo");
    a.largeText  = QString::fromLatin1(AppBrand::kName);
    a.startUnix  = sessionStartUnix;
    a.buttons << qMakePair(QStringLiteral("Get EverythingBox"), QString::fromLatin1(AppBrand::kSiteUrl));
    return a;
}
```

- [ ] **Step 6: Build and run the probe**

```bash
cmake --build build --config Release --target probe_presence
```

Then run it:

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" && ./build/Release/probe_presence.exe
```

Expected: a list of `PASS` lines ending in `PRESENCE-OK`, exit status 0.

- [ ] **Step 7: Run the full gate**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Expected last line: `ALL HEADLESS PROBES PASSED`. Also confirm the runner is still syntactically whole:

```bash
bash -n native/tools/run-headless-probes.sh
```

Expected: no output.

- [ ] **Step 8: Commit**

```bash
git add native/src/core/Presence.h native/src/core/Presence.cpp native/tools/probe_presence.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: the pure rules behind a Discord presence card"
```

---

### Task 2: Settings keys, and keeping them off the sync bundle

**Files:**
- Modify: `native/src/core/Settings.h` (declarations, beside the scrobble block near line 207)
- Modify: `native/src/core/Settings.cpp` (definitions, beside `setScrobbleEnabled` near line 438)
- Modify: `native/src/core/CloudSync.cpp:260` (the device-local carve-out)
- Modify: `native/tools/probe_presence.cpp` (a new §7)
- Modify: `native/CMakeLists.txt` (probe_presence now needs Settings and its dependencies)

**Interfaces:**
- Consumes: `Presence::Kind` from Task 1.
- Produces: `Settings::discordEnabled()` / `setDiscordEnabled(bool)`, and for each of the five categories `discordMovies/Games/Music/Reading/LiveTv` plus `discordBrowsing`, each with a matching `set…(bool)`. Also `Settings::discordShows(Presence::Kind)` — the single predicate the orchestrator and both settings builders ask, so the mapping from kind to toggle exists once.

- [ ] **Step 1: Write the failing probe section**

Append to `native/tools/probe_presence.cpp`, immediately before the final `if (fails)` block. Add `#include "Settings.h"`, `#include <QCoreApplication>` and `#include <QTemporaryDir>` to the includes, and change `int main()` to `int main(int argc, char** argv)` with `QCoreApplication app(argc, argv);` as its first statement — `Settings` needs an application object for its store path.

```cpp
    // ---- §7 THE SETTINGS GATE -----------------------------------------------------------------------
    // Off by default, and each category answerable on its own. The defaults matter: this feature broadcasts
    // what somebody is watching to everyone who can see their profile, so it is opted into rather than out of.
    {
        CHECK(!Settings::discordEnabled(),
              "settings/default: presence is OFF until it is asked for");

        Settings::setDiscordEnabled(true);
        CHECK(Settings::discordMovies() && Settings::discordGames() && Settings::discordMusic()
              && Settings::discordReading() && Settings::discordLiveTv() && Settings::discordBrowsing(),
              "settings/categories: once the master is on, every category is on unless silenced");

        CHECK(Settings::discordShows(Kind::Movie) && Settings::discordShows(Kind::Episode),
              "gate/movies covers both films and episodes");
        Settings::setDiscordMovies(false);
        CHECK(!Settings::discordShows(Kind::Movie) && !Settings::discordShows(Kind::Episode),
              "gate/movies: silencing it silences both");
        CHECK(Settings::discordShows(Kind::Game),
              "gate/independent: silencing films leaves games alone");
        Settings::setDiscordMovies(true);

        CHECK(Settings::discordShows(Kind::Game) && Settings::discordShows(Kind::PcGame),
              "gate/games covers emulated and PC alike");
        CHECK(Settings::discordShows(Kind::Music) && Settings::discordShows(Kind::Audiobook),
              "gate/music covers audiobooks too");

        Settings::setDiscordEnabled(false);
        CHECK(!Settings::discordShows(Kind::Movie) && !Settings::discordShows(Kind::Game)
              && !Settings::discordShows(Kind::Music) && !Settings::discordShows(Kind::Reading)
              && !Settings::discordShows(Kind::LiveTv),
              "gate/master: the master switch overrides every category, whatever they say");
        CHECK(!Settings::discordShows(Kind::None),
              "gate/none is never shown");
    }
```

- [ ] **Step 2: Run the probe to verify it fails**

```bash
cmake --build build --config Release --target probe_presence
```

Expected: **compile failure**, `'discordEnabled' is not a member of 'Settings'`.

- [ ] **Step 3: Add the settings accessors**

In `native/src/core/Settings.h`, immediately after the scrobble declarations (the `void setScrobbleSpokenAudio(bool on);` line), add:

```cpp
    // ---- DISCORD RICH PRESENCE ---------------------------------------------------------------------
    // OFF by default and opted into once, because this broadcasts what somebody is watching to everyone who
    // can see their Discord profile. The five category switches sit UNDER the master: they are meaningless
    // while it is off, which is what discordShows() encodes so that no caller has to remember it.
    //
    // DEVICE-LOCAL (see CloudSync::isDeviceLocalKey). Whether the machine in the living room announces what
    // it is playing is a property of THAT machine, not of the account — turning presence on for a laptop
    // must not silently switch it on for a shared TV.
    bool discordEnabled();               void setDiscordEnabled(bool on);
    bool discordMovies();                void setDiscordMovies(bool on);    // films and episodes
    bool discordGames();                 void setDiscordGames(bool on);     // emulated and PC
    bool discordMusic();                 void setDiscordMusic(bool on);     // music and audiobooks
    bool discordReading();               void setDiscordReading(bool on);   // books, comics, PDFs
    bool discordLiveTv();                void setDiscordLiveTv(bool on);
    bool discordBrowsing();              void setDiscordBrowsing(bool on);  // the "Browsing" idle card

    // The ONE predicate that maps a kind onto its toggle. Both settings builders and the orchestrator ask
    // this rather than re-deriving the mapping, so "audiobooks follow the music switch" is written once.
    bool discordShows(Presence::Kind kind);
```

Add `#include "Presence.h"` to the top of `native/src/core/Settings.h`.

In `native/src/core/Settings.cpp`, immediately after `setScrobbleSpokenAudio`, add:

```cpp
// ---- DISCORD RICH PRESENCE -------------------------------------------------------------------------
// One prefix, so CloudSync's carve-out is a single startsWith and cannot drift from the key names.
static QString discordKey(const QString& leaf) { return QStringLiteral("discord/") + leaf; }

bool Settings::discordEnabled()
{ return store().value(discordKey(QStringLiteral("enabled")), false).toBool(); }
void Settings::setDiscordEnabled(bool on)
{ store().setValue(discordKey(QStringLiteral("enabled")), on); store().sync(); }

bool Settings::discordMovies()
{ return store().value(discordKey(QStringLiteral("movies")), true).toBool(); }
void Settings::setDiscordMovies(bool on)
{ store().setValue(discordKey(QStringLiteral("movies")), on); store().sync(); }

bool Settings::discordGames()
{ return store().value(discordKey(QStringLiteral("games")), true).toBool(); }
void Settings::setDiscordGames(bool on)
{ store().setValue(discordKey(QStringLiteral("games")), on); store().sync(); }

bool Settings::discordMusic()
{ return store().value(discordKey(QStringLiteral("music")), true).toBool(); }
void Settings::setDiscordMusic(bool on)
{ store().setValue(discordKey(QStringLiteral("music")), on); store().sync(); }

bool Settings::discordReading()
{ return store().value(discordKey(QStringLiteral("reading")), true).toBool(); }
void Settings::setDiscordReading(bool on)
{ store().setValue(discordKey(QStringLiteral("reading")), on); store().sync(); }

bool Settings::discordLiveTv()
{ return store().value(discordKey(QStringLiteral("livetv")), true).toBool(); }
void Settings::setDiscordLiveTv(bool on)
{ store().setValue(discordKey(QStringLiteral("livetv")), on); store().sync(); }

bool Settings::discordBrowsing()
{ return store().value(discordKey(QStringLiteral("browsing")), true).toBool(); }
void Settings::setDiscordBrowsing(bool on)
{ store().setValue(discordKey(QStringLiteral("browsing")), on); store().sync(); }

bool Settings::discordShows(Presence::Kind kind)
{
    if (!discordEnabled()) return false;
    switch (kind) {
    case Presence::Kind::Movie: case Presence::Kind::Episode:   return discordMovies();
    case Presence::Kind::Game:  case Presence::Kind::PcGame:    return discordGames();
    case Presence::Kind::Music: case Presence::Kind::Audiobook: return discordMusic();
    case Presence::Kind::Reading:                               return discordReading();
    case Presence::Kind::LiveTv:                                return discordLiveTv();
    case Presence::Kind::None:                                  break;
    }
    return false;
}
```

- [ ] **Step 4: Carve the keys out of the sync bundle**

In `native/src/core/CloudSync.cpp`, in `isDeviceLocalKey` (around line 260, beside the `if (Scrobble::isDeviceLocalKey(key)) return true;` line), add:

```cpp
    // Discord presence (see Settings.h): whether THIS machine announces what it is playing. A shared TV must
    // not start broadcasting because presence was switched on for a laptop on the same account.
    if (key.startsWith(QLatin1String("discord/"))) return true;
```

- [ ] **Step 5: Give the probe its new dependencies**

In `native/CMakeLists.txt`, replace the `probe_presence` block written in Task 1 with:

```cmake
    # Headless test for DISCORD RICH PRESENCE. Pure rules plus the settings gate — no socket and no Discord.
    # Settings.cpp pulls FormFactor.cpp, the same pairing probe_scrobble and probe_audiobookmarks need.
    add_executable(probe_presence tools/probe_presence.cpp
        src/core/Presence.cpp    src/core/Presence.h
        src/core/Settings.cpp    src/core/Settings.h
        src/core/ProfileStore.cpp src/core/ProfileStore.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h)
    target_include_directories(probe_presence PRIVATE src src/core src/theme2)
    target_link_libraries(probe_presence PRIVATE Qt6::Core)
```

- [ ] **Step 6: Build and run**

```bash
cmake --build build --config Release --target probe_presence
```

Then:

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" && ./build/Release/probe_presence.exe
```

Expected: all `PASS`, ending `PRESENCE-OK`.

- [ ] **Step 7: Run the full gate**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Expected last line: `ALL HEADLESS PROBES PASSED`. The `exe-folder contamination` gate is the one to watch here — if the probe wrote a real ini it will fail; the probe must use the app's normal test isolation (`EB_DATA_DIR`), which the runner already sets for every probe.

- [ ] **Step 8: Commit**

```bash
git add native/src/core/Settings.h native/src/core/Settings.cpp native/src/core/CloudSync.cpp native/tools/probe_presence.cpp native/CMakeLists.txt
git commit -m "feat: the Discord presence settings gate, device-local by design"
```

---

### Task 3: `PresenceTransport.h` and `PresenceController` — the orchestrator

**Files:**
- Create: `native/src/core/PresenceTransport.h`
- Create: `native/src/core/PresenceController.h`
- Create: `native/src/core/PresenceController.cpp`
- Modify: `native/tools/probe_presence.cpp` (a new §8)
- Modify: `native/CMakeLists.txt` (probe + app target)

**Interfaces:**
- Consumes: everything from Tasks 1 and 2.
- Produces: `struct PresenceTransport { virtual void setActivity(const Presence::Activity&); virtual void clearActivity(); virtual bool connected() const; }` and `class PresenceController : public QObject` with `setTransport(PresenceTransport*)`, `setItem(const Presence::Item&)`, `clearItem()`, `setPosition(double)`, `setDuration(double)`, `setPaused(bool)`, `settingsChanged()`, `QString statusLine() const`, signal `statusChanged()`.

- [ ] **Step 1: Write the seam**

Create `native/src/core/PresenceTransport.h`:

```cpp
// THE PRESENCE SEAM — the whole of what a presence service has to be able to do.
//
// Three verbs, because there are only three things the orchestrator ever wants: show this card, show nothing,
// and tell me whether you are reachable so the settings surface can say so. Everything that DECIDES what the
// card says lives in Presence.h; everything that decides WHEN lives in PresenceController.
//
// NOT A QObject, for ScrobbleProvider.h's reason: the orchestrator drives it directly through ordinary calls,
// making it a QObject would buy signals nobody wants, and it would cost probe_presence the ability to
// substitute a two-line recording fake.
#pragma once
#include "Presence.h"

struct PresenceTransport
{
    virtual ~PresenceTransport() = default;

    // Show this card. Called only when the card has actually CHANGED — the orchestrator does the comparing,
    // so an implementation may send unconditionally.
    virtual void setActivity(const Presence::Activity& activity) = 0;

    // Show nothing at all (the user switched presence off, or closed the last thing they had open).
    virtual void clearActivity() = 0;

    // Whether the service is reachable right now. FALSE IS ORDINARY: most users will not have Discord
    // running. It is a fact for the status line, never an error.
    virtual bool connected() const = 0;
};
```

- [ ] **Step 2: Write the failing probe section**

Append to `native/tools/probe_presence.cpp`, before the final `if (fails)` block. Add `#include "PresenceController.h"`, `#include "PresenceTransport.h"`, `#include <QEventLoop>`, `#include <QTimer>` and `#include <QDeadlineTimer>` to the includes, plus this helper above `main`:

```cpp
// A transport that records instead of connecting. This is why PresenceTransport is not a QObject.
struct FakeTransport : PresenceTransport
{
    QVector<Presence::Activity> sent;
    int  clears = 0;
    bool up     = true;

    void setActivity(const Presence::Activity& a) override { sent << a; }
    void clearActivity() override { ++clears; }
    bool connected() const override { return up; }
};

// Spin the event loop until `pred` holds or the deadline passes — the probe_scrobble idiom. The controller's
// coalescing floor is a QTimer, so the probe needs a running loop rather than a sleep.
template <typename Pred>
static void spinUntil(Pred pred, int ms)
{
    QDeadlineTimer deadline(ms);
    while (!pred() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}
```

Then the section:

```cpp
    // ---- §8 THE ORCHESTRATOR ------------------------------------------------------------------------
    {
        Settings::setDiscordEnabled(true);

        FakeTransport fake;
        PresenceController pc;
        pc.setTransport(&fake);

        Item m = movie();
        pc.setDuration(6000.0);
        pc.setPosition(1200.0);
        pc.setItem(m);
        spinUntil([&] { return !fake.sent.isEmpty(); }, 2000);
        CHECK(!fake.sent.isEmpty() && fake.sent.last().details == QStringLiteral("Blade Runner"),
              "orch/start: opening something sends its card");

        // THE RULE THE WHOLE THROTTLE RESTS ON. A second of ordinary playback moves the position and the
        // clock together, so the card is identical and NOTHING is sent. Without this, a per-second tick would
        // burn Discord's 5-per-20-seconds budget in four seconds and the card would freeze.
        const int afterStart = fake.sent.size();
        for (int s = 1; s <= 10; ++s) pc.setPosition(1200.0 + s);
        spinUntil([&] { return false; }, 600);
        CHECK(fake.sent.size() == afterStart,
              "orch/steady: ten seconds of ordinary playback send NOTHING — the card did not change");

        // A seek does change it, and is sent (after the coalescing floor).
        pc.setPosition(4000.0);
        spinUntil([&] { return fake.sent.size() > afterStart; }, 6000);
        CHECK(fake.sent.size() == afterStart + 1, "orch/seek: a seek sends exactly one card, not a burst");

        // A pause is a change too.
        const int beforePause = fake.sent.size();
        pc.setPaused(true);
        spinUntil([&] { return fake.sent.size() > beforePause; }, 6000);
        CHECK(fake.sent.last().state == QStringLiteral("Paused") && fake.sent.last().endUnix == 0,
              "orch/pause: the paused card reaches the transport with no countdown on it");

        // Silencing the category clears immediately, mid-playback.
        pc.setPaused(false);
        const int beforeGate = fake.clears;
        Settings::setDiscordMovies(false);
        pc.settingsChanged();
        spinUntil([&] { return fake.clears > beforeGate; }, 6000);
        CHECK(fake.clears > beforeGate,
              "orch/gate: switching a category off while it is playing clears the card at once");
        Settings::setDiscordMovies(true);

        // Closing it goes back to the browsing card...
        pc.settingsChanged();
        pc.clearItem();
        spinUntil([&] { return !fake.sent.isEmpty()
                            && fake.sent.last().details == QStringLiteral("Browsing"); }, 6000);
        CHECK(fake.sent.last().details == QStringLiteral("Browsing"),
              "orch/idle: closing the last thing falls back to the browsing card");

        // ...unless browsing itself is silenced, in which case there is no card at all.
        Settings::setDiscordBrowsing(false);
        const int beforeIdleOff = fake.clears;
        pc.settingsChanged();
        spinUntil([&] { return fake.clears > beforeIdleOff; }, 6000);
        CHECK(fake.clears > beforeIdleOff,
              "orch/idle off: with browsing silenced, an empty app shows nothing rather than 'Browsing'");
        Settings::setDiscordBrowsing(true);

        // The master switch clears everything regardless of category.
        pc.setItem(m);
        spinUntil([&] { return false; }, 600);
        const int beforeMaster = fake.clears;
        Settings::setDiscordEnabled(false);
        pc.settingsChanged();
        spinUntil([&] { return fake.clears > beforeMaster; }, 6000);
        CHECK(fake.clears > beforeMaster, "orch/master off: clears whatever was showing");

        // The status line is the only answer to "is this doing anything". It must distinguish all three.
        CHECK(pc.statusLine().contains(QStringLiteral("off"), Qt::CaseInsensitive),
              "status/off: says so when the master is off");
        Settings::setDiscordEnabled(true);
        pc.settingsChanged();
        fake.up = false;
        CHECK(!pc.statusLine().contains(QStringLiteral("off"), Qt::CaseInsensitive),
              "status/not running: on-but-unreachable does NOT read as 'off' — they are different problems "
              "with different fixes");
        fake.up = true;
        Settings::setDiscordEnabled(false);
    }
```

- [ ] **Step 3: Run the probe to verify it fails**

```bash
cmake --build build --config Release --target probe_presence
```

Expected: **compile failure**, `PresenceController.h: No such file or directory`.

- [ ] **Step 4: Write the orchestrator header**

Create `native/src/core/PresenceController.h`:

```cpp
// THE PRESENCE ORCHESTRATOR — what is showing, and when a new card is owed.
//
// It holds four facts (the item, the position, the duration, whether playback is paused), rebuilds the card
// from Presence::build whenever one of them moves, and sends it ONLY IF IT DIFFERS from the card last sent.
//
// THAT COMPARISON IS THE WHOLE THROTTLE. Discord accepts five updates per twenty seconds. A per-second
// position tick would exceed that in four seconds and the card would freeze — except that a second of
// ordinary playback moves the position and the clock by the same amount, so the end instant is unchanged and
// build() returns a card equal to the last one. Nothing is sent. What DOES change — a seek, a pause, a track
// boundary — is genuinely rare, and kFloorMs coalesces even those.
//
// The host tells it four things and reads one back; every hook point in the app is one of the setters below.
#pragma once
#include "Presence.h"

#include <QObject>
#include <QString>
#include <QTimer>

struct PresenceTransport;

class PresenceController : public QObject
{
    Q_OBJECT
public:
    explicit PresenceController(QObject* parent = nullptr);

    // Takes no ownership: the host owns the transport (MainWindow parents the DiscordPresence to itself),
    // exactly as Scrobbler::setProvider is used.
    void setTransport(PresenceTransport* transport);

    void setItem(const Presence::Item& item);  // something opened
    void clearItem();                          // ...and closed; falls back to the browsing card
    void setPosition(double seconds);
    void setDuration(double seconds);
    void setPaused(bool paused);

    // Re-read the settings gate and act on it now. Called from both settings builders on every toggle, so a
    // category switched off mid-film clears the card rather than waiting for the next boundary.
    void settingsChanged();

    // The one line both settings surfaces show. Never contains a title the user has silenced.
    QString statusLine() const;

signals:
    void statusChanged();

private:
    void rebuild();     // build -> compare -> send, or defer to the floor timer
    void deliver(const Presence::Activity& next);

    PresenceTransport* transport_ = nullptr;
    Presence::Item     item_;
    double             position_ = 0.0;
    double             duration_ = 0.0;
    bool               paused_   = false;
    qint64             sessionStart_ = 0;   // when this app run began; the browsing card counts from it

    Presence::Activity lastSent_;
    bool               anythingSent_ = false;
    QTimer             floor_;             // coalesces changes that arrive faster than kFloorMs
    bool               pending_ = false;

    // Discord allows five updates per twenty seconds. Four seconds between sends leaves headroom for the
    // occasional burst (a seek immediately followed by a pause) without ever approaching the ceiling.
    static constexpr int kFloorMs = 4000;
};
```

- [ ] **Step 5: Write the orchestrator implementation**

Create `native/src/core/PresenceController.cpp`:

```cpp
#include "PresenceController.h"
#include "PresenceTransport.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QDateTime>

PresenceController::PresenceController(QObject* parent)
    : QObject(parent)
    , sessionStart_(QDateTime::currentSecsSinceEpoch())
{
    floor_.setSingleShot(true);
    floor_.setInterval(kFloorMs);
    connect(&floor_, &QTimer::timeout, this, [this] {
        if (!pending_) return;
        pending_ = false;
        rebuild();
    });
}

void PresenceController::setTransport(PresenceTransport* transport)
{
    transport_ = transport;
    // A transport arriving after the app has already opened something must be told what is showing, or the
    // card stays blank until the next track boundary.
    anythingSent_ = false;
    rebuild();
}

void PresenceController::setItem(const Presence::Item& item)
{
    item_ = item;
    // A new item's clocks belong to it, not to whatever was playing before. Without this reset, opening a
    // 3-minute track while an hour-long film's duration is still held builds a countdown from the film.
    position_ = 0.0;
    duration_ = 0.0;
    paused_   = false;
    rebuild();
}

void PresenceController::clearItem()
{
    item_ = Presence::Item{};
    position_ = duration_ = 0.0;
    paused_ = false;
    rebuild();
}

void PresenceController::setPosition(double seconds) { position_ = seconds; rebuild(); }
void PresenceController::setDuration(double seconds) { duration_ = seconds; rebuild(); }
void PresenceController::setPaused(bool paused)      { paused_   = paused;  rebuild(); }

void PresenceController::settingsChanged()
{
    rebuild();
    emit statusChanged();
}

void PresenceController::rebuild()
{
    if (!transport_) return;

    // What SHOULD be showing, given the gate.
    Presence::Activity next;
    if (item_.kind != Presence::Kind::None && Settings::discordShows(item_.kind))
        next = Presence::build(item_, position_, duration_, paused_,
                               QDateTime::currentSecsSinceEpoch());
    else if (item_.kind == Presence::Kind::None && Settings::discordEnabled() && Settings::discordBrowsing())
        next = Presence::idle(sessionStart_);
    // else: next stays invalid -> clear

    if (anythingSent_ && next == lastSent_) return;   // nothing changed: send nothing at all

    if (floor_.isActive()) { pending_ = true; return; }   // too soon: coalesce
    deliver(next);
    floor_.start();
}

void PresenceController::deliver(const Presence::Activity& next)
{
    if (next.valid) transport_->setActivity(next);
    else            transport_->clearActivity();
    lastSent_     = next;
    anythingSent_ = true;
    emit statusChanged();
}

QString PresenceController::statusLine() const
{
    if (!Settings::discordEnabled())
        return QCoreApplication::translate("PresenceController",
            "Discord presence is off.");
    if (!transport_ || !transport_->connected())
        return QCoreApplication::translate("PresenceController",
            "Discord isn't running — your status will appear as soon as you start it.");
    if (lastSent_.valid && !lastSent_.details.isEmpty())
        return QCoreApplication::translate("PresenceController",
            "Connected to Discord — showing “%1”.").arg(lastSent_.details);
    return QCoreApplication::translate("PresenceController", "Connected to Discord.");
}
```

- [ ] **Step 6: Add the sources to CMake**

In `native/CMakeLists.txt`, extend the `probe_presence` block's source list with:

```cmake
        src/core/PresenceTransport.h
        src/core/PresenceController.cpp src/core/PresenceController.h
```

and add the same three lines to the app target beside `src/core/Presence.cpp`. Note `probe_presence` now needs `AUTOMOC` — it already has it if the file sits under the project-wide `set(CMAKE_AUTOMOC ON)`; if the link fails on `PresenceController::staticMetaObject`, add `set_target_properties(probe_presence PROPERTIES AUTOMOC ON)`.

- [ ] **Step 7: Build and run**

```bash
cmake --build build --config Release --target probe_presence
```

Then:

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" && ./build/Release/probe_presence.exe
```

Expected: all `PASS`, ending `PRESENCE-OK`.

- [ ] **Step 8: Run the full gate and commit**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Expected last line: `ALL HEADLESS PROBES PASSED`.

```bash
git add native/src/core/PresenceTransport.h native/src/core/PresenceController.h native/src/core/PresenceController.cpp native/tools/probe_presence.cpp native/CMakeLists.txt
git commit -m "feat: the presence orchestrator sends a card only when it changes"
```

---

### Task 4: `DiscordPresence` — the IPC transport

**Files:**
- Create: `native/src/core/DiscordPresence.h`
- Create: `native/src/core/DiscordPresence.cpp`
- Modify: `native/tools/probe_presence.cpp` (a new §9, framing only)
- Modify: `native/CMakeLists.txt` (probe needs `Qt6::Network`; app target gains the two files)

**Interfaces:**
- Consumes: `PresenceTransport`, `Presence::Activity`.
- Produces: `class DiscordPresence : public QObject, public PresenceTransport`, constructed as `DiscordPresence(const QString& applicationId, QObject* parent)`, signal `connectionChanged()`. Plus, for the probe, the two free functions `DiscordIpc::encodeFrame(int opcode, const QByteArray& json) -> QByteArray` and `DiscordIpc::activityJson(const Presence::Activity&) -> QJsonObject`.

- [ ] **Step 1: Write the failing probe section**

Append to `native/tools/probe_presence.cpp` before the final `if (fails)` block, adding `#include "DiscordPresence.h"`, `#include <QDateTime>`, `#include <QJsonArray>`, `#include <QJsonDocument>` and `#include <QJsonObject>` to the includes:

```cpp
    // ---- §9 THE WIRE ---------------------------------------------------------------------------------
    // No Discord is involved. What is asserted is the two things that are wrong silently: the frame header's
    // byte order and width, and the shape of the activity object Discord would be handed.
    {
        const QByteArray json = QByteArrayLiteral("{\"v\":1}");
        const QByteArray frame = DiscordIpc::encodeFrame(0, json);
        CHECK(frame.size() == 8 + json.size(),
              "wire/frame: an 8-byte header and nothing else before the payload");
        CHECK(static_cast<unsigned char>(frame.at(0)) == 0 && frame.at(1) == 0
              && frame.at(2) == 0 && frame.at(3) == 0,
              "wire/opcode: opcode 0 is the handshake");
        CHECK(static_cast<unsigned char>(frame.at(4)) == static_cast<unsigned char>(json.size())
              && frame.at(5) == 0 && frame.at(6) == 0 && frame.at(7) == 0,
              "wire/length: LITTLE-endian, 4 bytes — big-endian here makes Discord wait for a payload that "
              "never comes, which presents as a silent hang rather than an error");
        CHECK(frame.mid(8) == json, "wire/payload follows the header verbatim");

        const Activity a = Presence::build(movie(), 1200.0, 6000.0, false,
                                           QDateTime::currentSecsSinceEpoch());
        const QJsonObject o = DiscordIpc::activityJson(a);
        CHECK(o.value(QStringLiteral("type")).toInt() == Presence::kWatching, "wire/type");
        CHECK(o.value(QStringLiteral("details")).toString() == QStringLiteral("Blade Runner"), "wire/details");
        CHECK(o.contains(QStringLiteral("timestamps"))
              && o.value(QStringLiteral("timestamps")).toObject().contains(QStringLiteral("end")),
              "wire/timestamps: the countdown goes out as an END instant, not a duration");
        CHECK(o.value(QStringLiteral("assets")).toObject().value(QStringLiteral("large_image")).toString()
                  == QStringLiteral("https://img.example/poster.jpg"),
              "wire/assets: the external poster URL goes in large_image verbatim");
        CHECK(o.value(QStringLiteral("buttons")).toArray().size() == 2, "wire/buttons");

        // An empty field must be OMITTED, not sent as "". Discord rejects an empty string where it accepts
        // an absent key, and a rejected update is dropped whole and silently.
        Item bare; bare.kind = Kind::Game; bare.title = QStringLiteral("Tetris");
        const QJsonObject b = DiscordIpc::activityJson(
            Presence::build(bare, 0, 0, false, QDateTime::currentSecsSinceEpoch()));
        CHECK(!b.contains(QStringLiteral("state")),
              "wire/empty: a card with no second line omits `state` rather than sending an empty string");

        const QJsonObject pausedObj = DiscordIpc::activityJson(
            Presence::build(movie(), 1200.0, 6000.0, true, QDateTime::currentSecsSinceEpoch()));
        CHECK(!pausedObj.contains(QStringLiteral("timestamps")),
              "wire/paused: a paused card carries no timestamps object at all");
    }
```

- [ ] **Step 2: Run the probe to verify it fails**

```bash
cmake --build build --config Release --target probe_presence
```

Expected: **compile failure**, `DiscordPresence.h: No such file or directory`.

- [ ] **Step 3: Write the transport header**

Create `native/src/core/DiscordPresence.h`:

```cpp
// THE ONLY I/O IN THE PRESENCE FEATURE — Discord's local IPC socket.
//
// Discord's desktop client listens on a local socket named discord-ipc-0 through discord-ipc-9 (ten, because
// several Discord builds can run at once and each takes the first free one). On Windows those are named
// pipes; elsewhere they are unix sockets under the runtime dir. QLocalSocket speaks both, which is the whole
// reason this needs no SDK and no third-party dependency.
//
// DISCORD NOT RUNNING IS THE NORMAL CASE, NOT A FAILURE. Most users will not have it open, and many who do
// will start it after us. So a failed connect is silent: no notification, no log line, no error state — just
// a backoff and another attempt. The status line in Settings is the ONLY place this is ever surfaced.
//
// A NAMED HAZARD. A QLocalSocket destroyed inside its own readyRead emission is heap corruption, not a
// warning: Qt's frames resume on the socket after the slot returns, which is past where a QPointer can help.
// That was probe_uitest's "flaky" 3% rc=139. Nothing here deletes or reconnects the socket from inside one of
// its own slots — teardown is always deferred past the emission (see resetSocket).
#pragma once
#include "Presence.h"
#include "PresenceTransport.h"

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

class QLocalSocket;

// The pure half of the wire, split out so probe_presence can assert the framing and the payload shape
// without a socket, a Discord client or an application id.
namespace DiscordIpc
{
    // [opcode u32 LE][length u32 LE][payload]. Both fields are LITTLE-endian and both are 4 bytes; getting
    // either wrong makes Discord wait forever for a payload that never arrives, which looks like a hang.
    QByteArray  encodeFrame(int opcode, const QByteArray& json);

    // The `activity` object of a SET_ACTIVITY command. Empty fields are OMITTED rather than sent empty.
    QJsonObject activityJson(const Presence::Activity& a);
}

class DiscordPresence : public QObject, public PresenceTransport
{
    Q_OBJECT
public:
    // `applicationId` is the Discord application's snowflake. It is public information and is compiled in.
    // An empty id disables the transport entirely (nothing connects, nothing is sent) so a build made before
    // the application exists is inert rather than broken.
    explicit DiscordPresence(const QString& applicationId, QObject* parent = nullptr);
    ~DiscordPresence() override;

    void setActivity(const Presence::Activity& activity) override;
    void clearActivity() override;
    bool connected() const override;

signals:
    void connectionChanged();

private:
    void tryConnect();
    void onConnected();
    void onReadyRead();
    void onSocketGone();          // disconnect OR error: both mean "start over"
    void resetSocket();           // deferred teardown — never called synchronously from a socket slot
    void send(int opcode, const QJsonObject& payload);
    void flushPending();

    QString       appId_;
    QLocalSocket* socket_    = nullptr;
    int           nextPipe_  = 0;      // which discord-ipc-N to try next
    bool          handshook_ = false;
    QByteArray    inbox_;              // partial frames accumulate here
    QTimer        retry_;
    int           backoffMs_ = kBackoffMinMs;

    bool               hasPending_ = false;   // an activity that arrived while disconnected
    Presence::Activity pending_;
    bool               pendingIsClear_ = false;

    static constexpr int kPipeCount    = 10;
    static constexpr int kBackoffMinMs = 5000;
    static constexpr int kBackoffMaxMs = 60000;
};
```

- [ ] **Step 4: Write the transport implementation**

Create `native/src/core/DiscordPresence.cpp`:

```cpp
#include "DiscordPresence.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QUuid>

namespace {

// Discord's RPC opcodes.
constexpr int kOpHandshake = 0;
constexpr int kOpFrame     = 1;
constexpr int kOpClose     = 2;
constexpr int kOpPing      = 3;
constexpr int kOpPong      = 4;

constexpr int kHeaderBytes = 8;

// The socket's name for pipe N. On Windows QLocalSocket maps a bare name onto \\.\pipe\<name>; elsewhere the
// socket is a file in the runtime dir, and Snap and Flatpak installs put it one level deeper.
QStringList candidateNames(int n)
{
    const QString leaf = QStringLiteral("discord-ipc-%1").arg(n);
#ifdef Q_OS_WIN
    return { leaf };
#else
    QString base = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (base.isEmpty()) base = qEnvironmentVariable("TMPDIR");
    if (base.isEmpty()) base = QStringLiteral("/tmp");
    return { QDir(base).filePath(leaf),
             QDir(base).filePath(QStringLiteral("snap.discord/") + leaf),
             QDir(base).filePath(QStringLiteral("app/com.discordapp.Discord/") + leaf) };
#endif
}

void putLE32(QByteArray& out, quint32 v)
{
    out.append(char(v & 0xFF));
    out.append(char((v >> 8)  & 0xFF));
    out.append(char((v >> 16) & 0xFF));
    out.append(char((v >> 24) & 0xFF));
}

quint32 readLE32(const QByteArray& b, int at)
{
    return quint32(quint8(b.at(at)))
         | (quint32(quint8(b.at(at + 1))) << 8)
         | (quint32(quint8(b.at(at + 2))) << 16)
         | (quint32(quint8(b.at(at + 3))) << 24);
}

} // namespace

QByteArray DiscordIpc::encodeFrame(int opcode, const QByteArray& json)
{
    QByteArray out;
    out.reserve(kHeaderBytes + json.size());
    putLE32(out, quint32(opcode));
    putLE32(out, quint32(json.size()));
    out.append(json);
    return out;
}

QJsonObject DiscordIpc::activityJson(const Presence::Activity& a)
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), a.type);
    // Empty fields are OMITTED. Discord accepts an absent key where it rejects an empty string, and a
    // rejected update is dropped whole and without a word.
    if (!a.details.isEmpty()) o.insert(QStringLiteral("details"), a.details);
    if (!a.state.isEmpty())   o.insert(QStringLiteral("state"),   a.state);

    if (a.startUnix > 0 || a.endUnix > 0) {
        QJsonObject ts;
        if (a.startUnix > 0) ts.insert(QStringLiteral("start"), qint64(a.startUnix));
        if (a.endUnix   > 0) ts.insert(QStringLiteral("end"),   qint64(a.endUnix));
        o.insert(QStringLiteral("timestamps"), ts);
    }

    QJsonObject assets;
    if (!a.largeImage.isEmpty()) assets.insert(QStringLiteral("large_image"), a.largeImage);
    if (!a.largeText.isEmpty())  assets.insert(QStringLiteral("large_text"),  a.largeText);
    if (!a.smallImage.isEmpty()) assets.insert(QStringLiteral("small_image"), a.smallImage);
    if (!a.smallText.isEmpty())  assets.insert(QStringLiteral("small_text"),  a.smallText);
    if (!assets.isEmpty()) o.insert(QStringLiteral("assets"), assets);

    if (!a.buttons.isEmpty()) {
        QJsonArray arr;
        for (const auto& b : a.buttons) {
            QJsonObject j;
            j.insert(QStringLiteral("label"), b.first);
            j.insert(QStringLiteral("url"),   b.second);
            arr.append(j);
        }
        o.insert(QStringLiteral("buttons"), arr);
    }
    return o;
}

DiscordPresence::DiscordPresence(const QString& applicationId, QObject* parent)
    : QObject(parent), appId_(applicationId)
{
    retry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, &DiscordPresence::tryConnect);
    if (!appId_.isEmpty()) tryConnect();
}

DiscordPresence::~DiscordPresence() = default;

bool DiscordPresence::connected() const { return handshook_; }

void DiscordPresence::tryConnect()
{
    if (appId_.isEmpty() || socket_) return;

    socket_ = new QLocalSocket(this);
    connect(socket_, &QLocalSocket::connected,    this, &DiscordPresence::onConnected);
    connect(socket_, &QLocalSocket::readyRead,    this, &DiscordPresence::onReadyRead);
    connect(socket_, &QLocalSocket::disconnected, this, &DiscordPresence::onSocketGone);
    connect(socket_, &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError) { onSocketGone(); });

    // Walk the ten pipes. Failing to find one is ORDINARY — Discord is simply not running.
    const QStringList names = candidateNames(nextPipe_);
    socket_->connectToServer(names.first());
}

void DiscordPresence::onConnected()
{
    QJsonObject hs;
    hs.insert(QStringLiteral("v"), 1);
    hs.insert(QStringLiteral("client_id"), appId_);
    send(kOpHandshake, hs);
}

void DiscordPresence::onReadyRead()
{
    if (!socket_) return;
    inbox_.append(socket_->readAll());

    while (inbox_.size() >= kHeaderBytes) {
        const quint32 op  = readLE32(inbox_, 0);
        const quint32 len = readLE32(inbox_, 4);
        if (quint32(inbox_.size()) < kHeaderBytes + len) break;   // a partial frame; wait for the rest
        const QByteArray payload = inbox_.mid(kHeaderBytes, int(len));
        inbox_.remove(0, kHeaderBytes + int(len));

        if (op == kOpPing) {
            // Unanswered pings make Discord drop us.
            send(kOpPong, QJsonDocument::fromJson(payload).object());
        }
        else if (op == kOpClose) {
            onSocketGone();
            return;                     // the socket is being torn down; stop touching inbox_
        }
        else if (op == kOpFrame && !handshook_) {
            // The first frame after a successful handshake is READY.
            handshook_ = true;
            backoffMs_ = kBackoffMinMs;
            emit connectionChanged();
            flushPending();
        }
    }
}

void DiscordPresence::onSocketGone()
{
    const bool was = handshook_;
    handshook_ = false;
    inbox_.clear();
    resetSocket();

    // WALK ALL TEN PIPES BEFORE BACKING OFF. Discord takes the first free socket, so a second Discord build
    // (or a stale one) puts the live client on discord-ipc-1 or higher. Backing off between each attempt
    // would make that user wait a minute per pipe — up to ten minutes to be found at all — and it would look
    // exactly like presence being broken. A whole round of ten failed connects costs nothing, so the backoff
    // belongs BETWEEN rounds, not between pipes.
    nextPipe_ = (nextPipe_ + 1) % kPipeCount;
    if (nextPipe_ != 0) { retry_.start(0); return; }      // same round: try the next pipe at once

    backoffMs_ = qMin(backoffMs_ * 2, kBackoffMaxMs);
    // A client that was connected and just quit is likely to come back soon, so that case restarts at the
    // floor rather than inheriting a backoff grown while Discord was closed.
    retry_.start(was ? kBackoffMinMs : backoffMs_);
    if (was) { backoffMs_ = kBackoffMinMs; emit connectionChanged(); }
}

void DiscordPresence::resetSocket()
{
    if (!socket_) return;
    QLocalSocket* doomed = socket_;
    socket_ = nullptr;
    doomed->disconnect(this);       // no further slots on a socket we are done with
    doomed->abort();
    // NEVER a plain delete, and NEVER deleteLater alone. This can be reached from inside readyRead, and
    // deleteLater only defers past the INNERMOST delivery — Qt's own frames resume on the socket after the
    // slot returns. Hopping through the event loop with a zero-timer puts the destruction after every frame
    // that could still touch it. This is the probe_uitest rc=139 lesson; see the header.
    QTimer::singleShot(0, doomed, [doomed] { doomed->deleteLater(); });
}

void DiscordPresence::send(int opcode, const QJsonObject& payload)
{
    if (!socket_ || socket_->state() != QLocalSocket::ConnectedState) return;
    const QByteArray json  = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const QByteArray frame = DiscordIpc::encodeFrame(opcode, json);
    // ONE write. A frame split across two writes breaks the pipe.
    socket_->write(frame);
    socket_->flush();
}

void DiscordPresence::setActivity(const Presence::Activity& activity)
{
    pending_        = activity;
    pendingIsClear_ = false;
    hasPending_     = true;
    if (handshook_) flushPending();
}

void DiscordPresence::clearActivity()
{
    pending_        = Presence::Activity{};
    pendingIsClear_ = true;
    hasPending_     = true;
    if (handshook_) flushPending();
}

void DiscordPresence::flushPending()
{
    if (!hasPending_ || !handshook_) return;

    QJsonObject args;
    args.insert(QStringLiteral("pid"), qint64(QCoreApplication::applicationPid()));
    // A null activity is how SET_ACTIVITY says "show nothing".
    if (pendingIsClear_) args.insert(QStringLiteral("activity"), QJsonValue());
    else                 args.insert(QStringLiteral("activity"), DiscordIpc::activityJson(pending_));

    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"),   QStringLiteral("SET_ACTIVITY"));
    cmd.insert(QStringLiteral("args"),  args);
    cmd.insert(QStringLiteral("nonce"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    send(kOpFrame, cmd);
    hasPending_ = false;
}
```

- [ ] **Step 5: Add to CMake**

Extend the `probe_presence` block with `src/core/DiscordPresence.cpp src/core/DiscordPresence.h` and change its link line to:

```cmake
    target_link_libraries(probe_presence PRIVATE Qt6::Core Qt6::Network)
```

Add the same two sources to the app target beside `src/core/PresenceController.cpp`.

- [ ] **Step 6: Build and run**

```bash
cmake --build build --config Release --target probe_presence
```

Then:

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" && ./build/Release/probe_presence.exe
```

Expected: all `PASS`, ending `PRESENCE-OK`.

- [ ] **Step 7: Run the full gate and commit**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Expected last line: `ALL HEADLESS PROBES PASSED`.

```bash
git add native/src/core/DiscordPresence.h native/src/core/DiscordPresence.cpp native/tools/probe_presence.cpp native/CMakeLists.txt
git commit -m "feat: speak Discord's local IPC protocol over a QLocalSocket"
```

---

### Task 5: The settings rows, in both builders

**Files:**
- Modify: `native/src/ui/MainWindow.h` (the member, the status hook, the status-line getter)
- Modify: `native/src/ui/MainWindow.cpp` — themed declarations (after the `scrobble.status` row, ~line 18344), themed handlers (after the `scrobble.lburl` arm, ~line 18883), classic builder (after the scrobble status block, ~line 20510), and construction (~line 556, beside `scrobbler_ = new Scrobbler(this)`)

**Interfaces:**
- Consumes: `PresenceController`, `DiscordPresence`, the `Settings::discord*` accessors.
- Produces: `MainWindow::presence_` (a `PresenceController*`), `MainWindow::discordStatusLine() const`, `MainWindow::presenceStatusUpdate_` (a `std::function<void()>`, the classic panel's refresh hook), and the compiled-in constant `kDiscordAppId`.

- [ ] **Step 1: Add the members**

In `native/src/ui/MainWindow.h`, beside the `class Scrobbler* scrobbler_ = nullptr;` declaration, add:

```cpp
    // DISCORD RICH PRESENCE. The window reports five facts to it (see PresenceController.h) and reads one
    // line back out for both settings surfaces.
    class PresenceController* presence_ = nullptr;
```

Beside `scrobbleStatusUpdate_`, add:

```cpp
    // Re-armed from PresenceController::statusChanged, so a connection made while the classic panel is up
    // moves the line. QPointer-guarded at the install site, the traktStatusUpdate_ idiom.
    std::function<void()> presenceStatusUpdate_;
```

And beside `QString scrobbleStatusLine() const;`:

```cpp
    QString discordStatusLine() const;
```

- [ ] **Step 2: Construct it**

In `native/src/ui/MainWindow.cpp`, immediately after the scrobbler construction block (the `connect(scrobbler_, &Scrobbler::statusChanged, ...)` line near 561), add:

```cpp
    // --- Discord Rich Presence ---
    // The application id is public information (it names the app on every card, not the user) and is
    // compiled in so presence works out of the box. An EMPTY id makes the transport inert rather than
    // broken, which is what a build made before the application existed gets.
    presence_ = new PresenceController(this);
    presence_->setTransport(new DiscordPresence(QString::fromLatin1(kDiscordAppId), this));
    connect(presence_, &PresenceController::statusChanged, this,
            [this] { if (presenceStatusUpdate_) presenceStatusUpdate_(); });
```

Beside `kDiscordInvite` (near line 341), add:

```cpp
// The Discord APPLICATION this app announces itself as — the bold name on every Rich Presence card. Public
// information: it identifies the app, never the user. Empty until the application is registered, which makes
// the transport inert rather than broken.
static constexpr const char* kDiscordAppId = "";
```

Add `#include "../core/PresenceController.h"` and `#include "../core/DiscordPresence.h"` beside the `Scrobbler.h` include.

Add the status-line getter beside `MainWindow::scrobbleStatusLine`:

```cpp
QString MainWindow::discordStatusLine() const
{
    return presence_ ? presence_->statusLine() : tr("Discord presence is off.");
}
```

- [ ] **Step 3: Add the themed rows**

In `MainWindow::openGeneralSettings`, immediately after the `info(QStringLiteral("scrobble.status"), ...)` line, add:

```cpp
        // --- Discord Rich Presence ---
        // The twin of every row here lives in the QWidget builder below; a setting in one builder is simply
        // unreachable in the other mode. OFF by default: this announces what somebody is watching, by name,
        // to everyone who can see their Discord profile.
        sep(tr("Discord"));
        toggle(QStringLiteral("discord.on"), tr("Show what I'm doing on Discord"), Settings::discordEnabled());
        toggle(QStringLiteral("discord.movies"),   tr("Movies and TV"),        Settings::discordMovies());
        toggle(QStringLiteral("discord.games"),    tr("Games"),                Settings::discordGames());
        toggle(QStringLiteral("discord.music"),    tr("Music and audiobooks"), Settings::discordMusic());
        toggle(QStringLiteral("discord.reading"),  tr("Books and comics"),     Settings::discordReading());
        toggle(QStringLiteral("discord.livetv"),   tr("Live TV"),              Settings::discordLiveTv());
        toggle(QStringLiteral("discord.browsing"), tr("Just browsing"),        Settings::discordBrowsing());
        info(QStringLiteral("discord.hint"),
             tr("Your Discord profile shows what you're watching, playing or reading, with its artwork. "
                "Each category can be silenced on its own, and this machine's choice is its own — turning it "
                "on here doesn't turn it on anywhere else."), QString());
        info(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
```

- [ ] **Step 4: Add the themed handlers**

Immediately after the `scrobble.lburl` arm (which ends with its `setInfo(...scrobble.status...)` line), add:

```cpp
                // --- Discord presence. Every arm re-reads the status line: a row flipped while the panel is
                // up must not leave a line that contradicts it, and the controller acts on the change at once
                // so a category silenced mid-film clears the card rather than waiting for the next boundary.
                else if (id == QStringLiteral("discord.on")) {
                    Settings::setDiscordEnabled(on);
                    if (presence_) presence_->settingsChanged();
                    setInfo(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
                }
                else if (id == QStringLiteral("discord.movies")) {
                    Settings::setDiscordMovies(on);
                    if (presence_) presence_->settingsChanged();
                    setInfo(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
                }
                else if (id == QStringLiteral("discord.games")) {
                    Settings::setDiscordGames(on);
                    if (presence_) presence_->settingsChanged();
                    setInfo(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
                }
                else if (id == QStringLiteral("discord.music")) {
                    Settings::setDiscordMusic(on);
                    if (presence_) presence_->settingsChanged();
                    setInfo(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
                }
                else if (id == QStringLiteral("discord.reading")) {
                    Settings::setDiscordReading(on);
                    if (presence_) presence_->settingsChanged();
                    setInfo(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
                }
                else if (id == QStringLiteral("discord.livetv")) {
                    Settings::setDiscordLiveTv(on);
                    if (presence_) presence_->settingsChanged();
                    setInfo(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
                }
                else if (id == QStringLiteral("discord.browsing")) {
                    Settings::setDiscordBrowsing(on);
                    if (presence_) presence_->settingsChanged();
                    setInfo(QStringLiteral("discord.status"), tr("Discord"), discordStatusLine());
                }
```

- [ ] **Step 5: Add the classic twins**

Immediately after the classic scrobble block's two `connect(sbSpoken, ...)` lines and before the `// --- Profiles (issue #30)` comment, add:

```cpp
        // --- Discord Rich Presence: the twin of every themed row above. A user-facing setting has to exist
        // in BOTH surfaces or it is unreachable in one mode. ---
        v->addSpacing(12);
        auto* dcHeading = new QLabel(tr("Discord"));
        dcHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(dcHeading);
        auto* dcNote = new QLabel(tr("Your Discord profile shows what you're watching, playing or reading, "
                                     "with its artwork. Each category can be silenced on its own, and this "
                                     "machine's choice is its own — turning it on here doesn't turn it on "
                                     "anywhere else."));
        dcNote->setWordWrap(true);
        dcNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(dcNote);

        auto* dcStatus = new QLabel(discordStatusLine());
        dcStatus->setWordWrap(true);
        dcStatus->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));

        // One builder for all seven rows: the same key, the same setter and the same status refresh as the
        // themed twin, so the two surfaces cannot drift.
        auto dcRow = [this, v, dcStatus](const QString& label, bool checked, void (*setter)(bool)) {
            auto* cb = new QCheckBox(label);
            cb->setStyleSheet(QStringLiteral("font-size:15px;"));
            cb->setChecked(checked);
            v->addWidget(cb);
            connect(cb, &QCheckBox::toggled, this, [this, dcStatus, setter](bool c) {
                setter(c);
                if (presence_) presence_->settingsChanged();
                dcStatus->setText(discordStatusLine()); });
        };
        dcRow(tr("Show what I'm doing on Discord"), Settings::discordEnabled(),  &Settings::setDiscordEnabled);
        dcRow(tr("Movies and TV"),                  Settings::discordMovies(),   &Settings::setDiscordMovies);
        dcRow(tr("Games"),                          Settings::discordGames(),    &Settings::setDiscordGames);
        dcRow(tr("Music and audiobooks"),           Settings::discordMusic(),    &Settings::setDiscordMusic);
        dcRow(tr("Books and comics"),               Settings::discordReading(),  &Settings::setDiscordReading);
        dcRow(tr("Live TV"),                        Settings::discordLiveTv(),   &Settings::setDiscordLiveTv);
        dcRow(tr("Just browsing"),                  Settings::discordBrowsing(), &Settings::setDiscordBrowsing);

        v->addWidget(dcStatus);
        {
            // While THIS panel is up it owns the refresh hook — the scrobbleStatusUpdate_ idiom, QPointer
            // guarded so a connection made after the panel is destroyed writes nowhere.
            QPointer<QLabel> guard(dcStatus);
            presenceStatusUpdate_ = [this, guard] { if (guard) guard->setText(discordStatusLine()); };
        }
```

- [ ] **Step 6: Build the app**

```bash
cmake --build build --config Release --target everythingbox
```

Expected: builds clean, no warnings about the new code.

- [ ] **Step 7: Verify both surfaces are reachable**

Launch under the uitest channel and screenshot both settings surfaces:

```bash
EB_UITEST=1 ./build/Release/everythingbox.exe
```

Then in another shell, drive it to Settings ▸ General in the themed surface and again in the classic surface, and confirm the seven rows and the status line appear in both:

```bash
python native/tools/uitest.py shot discord-themed.png
```

Expected: the "Discord" section with seven toggles and a status line reading `Discord presence is off.`

- [ ] **Step 8: Run the gate and commit**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Expected last line: `ALL HEADLESS PROBES PASSED`.

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
git commit -m "feat: a Discord section in both settings builders"
```

---

### Task 6: The six hook points

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (the media item, position, duration, pause, stop)
- Modify: `native/src/launch/GameLauncher.h` / `.cpp` (two new signals at the play-session edges)
- Modify: `native/src/comic/ComicView.cpp:369`, `native/src/ebook/EbookView.cpp:706`, `native/src/pdf/PdfView.cpp:205` (reading)

**Interfaces:**
- Consumes: `PresenceController::setItem/clearItem/setPosition/setDuration/setPaused`, `Presence::Item`, `Presence::Kind`.
- Produces: `GameLauncher::playSessionBegan(const QString& title, const QString& system, const QString& artPath, bool isPcGame)` and `GameLauncher::playSessionEnded()`; `MainWindow::presenceItemForReader(...)` is NOT introduced — the readers emit through an existing signal, see Step 4.

- [ ] **Step 1: Feed position, duration and pause**

In `native/src/ui/MainWindow.cpp`, at the position callback (the line `if (scrobbler_) scrobbler_->positionTick(seconds);`, ~line 23431), add immediately after it:

```cpp
    if (presence_) presence_->setPosition(seconds);
```

The duration callback is `native/src/ui/MainWindow.cpp:23396` (`duration_ = seconds;`, with
`session_->setDuration(seconds);` on 23398). Add a third line beside them:

```cpp
    if (presence_) presence_->setDuration(seconds);
```

Wire the pause seam. `MpvWidget` already emits `pausedChanged(bool)` precisely because position ticks STOP arriving while paused (see the comment at `MpvWidget.h:187`). Beside the other `player_` connects in the constructor, add:

```cpp
    // #Discord: pause is exactly the state in which position ticks stop, so the card can only learn about it
    // from this signal — a rebuild driven by the next tick would learn it had been paused only on resume.
    connect(player_, &MpvWidget::pausedChanged, this,
            [this](bool paused) { if (presence_) presence_->setPaused(paused); });
```

- [ ] **Step 2: Announce the media item, and clear it**

There are exactly two choke points, and both already exist because scrobbling needed the same thing.

**Video (movies, episodes, live TV): `native/src/ui/MainWindow.cpp:16201`,** the `startScrobble(item.imdbStreamId);`
line in the catalog/local play path. `item` is the `MediaItem` in hand, and — importantly — `item.imdbStreamId`
is populated for local-library files too, not only catalog streams. Add immediately after it:

```cpp
        // #Discord: the same seam Trakt uses, but NOT via scrobbleImdb_ — startScrobble() early-returns when
        // Trakt is not connected, so reading the member would give the IMDb button only to Trakt users. The
        // id is taken from the item directly.
        if (presence_) {
            Presence::Item pi;
            pi.kind     = item.imdbStreamId.contains(QLatin1Char(':')) ? Presence::Kind::Episode
                                                                       : Presence::Kind::Movie;
            pi.title    = item.title;
            pi.subtitle = item.presenceSubtitle();   // year, or "S3E4 · <episode title>"
            pi.artUrl   = item.poster;
            pi.imdbId   = item.imdbStreamId;
            presence_->setItem(pi);
        }
```

`MediaItem` has no `presenceSubtitle()` yet — add one to `MediaItem`, or build the string inline from the
fields the item already carries (`year`, `season`, `episode`, `name`). Whichever is cleaner where it lands;
do not add a lookup.

For an IPTV channel the same play path runs with an empty `imdbStreamId`; gate on the channel case there and
set `Kind::LiveTv` with `subtitle = tr("Live TV")`.

**Music and audiobooks: `MainWindow::noteScrobbleTrack(const QString& path)`, `native/src/ui/MainWindow.cpp:5817`** —
the host's whole per-track duty, called from `PlaybackSession::trackChanged`, which is the one signal a
gapless advance produces. It already resolves a `Scrobble::Track` carrying artist, title, album and kind.

Note its **first line is `if (!scrobbler_) return;`** — presence must not sit behind that guard, or a build
with no scrobbler shows no music card. Restructure to:

```cpp
void MainWindow::noteScrobbleTrack(const QString& path)
{
    Scrobble::Track t;
    const bool named = scrobbleTrackFor(path, t);

    // #Discord first, and OUTSIDE the scrobbler guard: presence is a separate feature and must not depend on
    // whether a scrobbling provider happens to exist.
    if (presence_) {
        if (!named) presence_->clearItem();
        else {
            Presence::Item pi;
            pi.kind     = (t.kind == Scrobble::Kind::Music) ? Presence::Kind::Music
                                                            : Presence::Kind::Audiobook;
            pi.title    = t.title;
            pi.subtitle = t.album.isEmpty() ? t.artist
                                            : tr("%1 — %2").arg(t.artist, t.album);
            pi.artUrl   = musicArtUrlFor(path);   // whatever the now-playing surface already uses; empty is fine
            presence_->setItem(pi);
        }
    }

    if (!scrobbler_) return;
    if (!named) { scrobbler_->playbackStopped(); return; }
    scrobbler_->trackStarted(t);
}
```

**The stop edges.** Beside `if (scrobbler_) scrobbler_->playbackStopped();` (~line 1700) and in
`MainWindow::stopScrobble()` (`:13554`), add:

```cpp
    if (presence_) presence_->clearItem();
```

Also connect `PlaybackSession::queueCleared` so an emptied queue returns to the browsing card:

```cpp
    connect(session_, &PlaybackSession::queueCleared, this,
            [this] { if (presence_) presence_->clearItem(); });
```

- [ ] **Step 3: Announce games at the two play-session edges**

`GameLauncher::beginPlaySession` / `endPlaySession` (`native/src/launch/GameLauncher.cpp:982` and `:992`) are the single choke point every game launch already passes through — libretro, standalone emulator and PC alike. Add two signals to `native/src/launch/GameLauncher.h`, beside `restoreRequested`:

```cpp
    // Discord presence: a game session opened / closed. Emitted from begin/endPlaySession, which is the ONE
    // point every launch path already funnels through — hooking the individual open() arms instead would
    // miss whichever one is added next.
    void playSessionBegan(const QString& title, const QString& system, const QString& artPath, bool isPcGame);
    void playSessionEnded();
```

`beginPlaySession` currently takes only an identity, which is not enough to name a card. Widen it:

```cpp
void GameLauncher::beginPlaySession(const QString& identity, const QString& title,
                                    const QString& system, const QString& artPath, bool isPcGame)
{
    endPlaySession();
    if (identity.isEmpty()) return;
    PlayStats::markPlayed(identity);
    activePlayId_    = identity;
    activePlayStart_ = QDateTime::currentSecsSinceEpoch();
    emit playSessionBegan(title, system, artPath, isPcGame);
}
```

Give the four new parameters defaults in the header (`= QString()`, `= false`) so existing call sites keep compiling, then pass the real values at each `beginPlaySession` call — the title and system are already in hand there, beside the `RecentItem`. In `endPlaySession`, after `activePlayStart_ = 0;`, add:

```cpp
    emit playSessionEnded();
```

In `MainWindow.cpp`, beside the existing `connect(launcher_, &GameLauncher::restoreRequested, ...)` (~line 1780):

```cpp
    connect(launcher_, &GameLauncher::playSessionBegan, this,
            [this](const QString& title, const QString& system, const QString& art, bool isPc) {
                if (!presence_) return;
                Presence::Item pi;
                pi.kind     = isPc ? Presence::Kind::PcGame : Presence::Kind::Game;
                pi.title    = title;
                pi.subtitle = system;      // "SNES" / "Steam"
                pi.artUrl   = art;
                pi.system   = system;
                presence_->setItem(pi);
            });
    connect(launcher_, &GameLauncher::playSessionEnded, this,
            [this] { if (presence_) presence_->clearItem(); });
```

- [ ] **Step 4: Announce reading**

The three readers already accrue at their page-turn edges. Each gets one line beside its existing `ConsumptionStats::addPagesRead` call, routed through the host. Add a signal to each reader view, e.g. in `ComicView.h`:

```cpp
    // Discord presence: what is open and how far in. Emitted at the same page-turn edge the consumption
    // accrual uses, so reading needs no new timer and no new bookkeeping.
    void readingProgress(const QString& title, const QString& subtitle, const QString& artPath);
```

In `native/src/comic/ComicView.cpp`, immediately after line 369's accrual:

```cpp
        emit readingProgress(QFileInfo(path_).completeBaseName(),
                             tr("Reading · p. %1 of %2").arg(current_ + 1).arg(pageTotal()), QString());
```

In `native/src/ebook/EbookView.cpp`, after line 706:

```cpp
    emit readingProgress(book_->title(),
                         tr("Reading · p. %1 of %2").arg(globalPage()).arg(pageCount()), QString());
```

In `native/src/pdf/PdfView.cpp`, after line 205:

```cpp
    emit readingProgress(QFileInfo(path_).completeBaseName(),
                         tr("Reading · p. %1 of %2").arg(page1).arg(pageCount()), QString());
```

All three of these exist today: `ComicView::pageTotal()` (`ComicView.h:113`), `EbookView::pageCount()`
(`EbookView.h:130`) and `PdfView::pageCount()` (`PdfView.h:31`). None of the three views exposes a cover
path, so the art argument is empty and every reading card uses the `book` fallback icon — correct, and not
worth adding a cover accessor for. Then in `MainWindow.cpp`, beside where each reader is constructed, connect:

```cpp
    connect(comic_, &ComicView::readingProgress, this,
            [this](const QString& t, const QString& s, const QString& art) {
                if (!presence_) return;
                Presence::Item pi;
                pi.kind = Presence::Kind::Reading;
                pi.title = t; pi.subtitle = s; pi.artUrl = art;
                presence_->setItem(pi);
            });
```

...and the same for `ebook_` and `pdf_`. At each point the reader is closed (the same place the host returns Home from a reader), add:

```cpp
    if (presence_) presence_->clearItem();
```

- [ ] **Step 5: Build**

```bash
cmake --build build --config Release --target everythingbox
```

Expected: builds clean.

- [ ] **Step 6: Rebuild everything and check for errors**

A merge or a widened signature can break a target that was not named. Rebuild all probe targets and confirm none regressed:

```bash
cmake --build build --config Release --target probe_presence probe_scrobble probe_playback probe_stats
```

Expected: all four build.

- [ ] **Step 7: Run the gate**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Expected last line: `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 8: Commit**

```bash
git add native/src/ui/MainWindow.cpp native/src/launch/GameLauncher.h native/src/launch/GameLauncher.cpp native/src/comic/ComicView.h native/src/comic/ComicView.cpp native/src/ebook/EbookView.h native/src/ebook/EbookView.cpp native/src/pdf/PdfView.h native/src/pdf/PdfView.cpp
git commit -m "feat: feed the presence card from the app's existing playback and reading seams"
```

---

### Task 7: Live verification against a real Discord client

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (fill in `kDiscordAppId`)

**Interfaces:**
- Consumes: everything above, plus the Application ID from the repository owner.

This task is **blocked on the prerequisite** in the spec: the Discord application must exist and its eight fallback assets must be uploaded. Everything before this point is complete and gated without it.

- [ ] **Step 1: Fill in the application id**

Replace the empty `kDiscordAppId` in `native/src/ui/MainWindow.cpp` with the real snowflake:

```cpp
static constexpr const char* kDiscordAppId = "<the Application ID>";
```

- [ ] **Step 2: Build and deploy**

```bash
cmake --build build --config Release --target everythingbox
```

Deploy the **Release** binary to `C:\EverythingBox-app` (the debug DLLs are not present there) and relaunch.

- [ ] **Step 3: Walk the checklist with Discord running**

Each row is a card on the user's own Discord profile. Confirm by looking at it.

- [ ] Presence is **off** by default on a fresh profile; the profile shows nothing.
- [ ] Switching the master on with nothing playing shows **Browsing** with the app logo and a counting-up timer.
- [ ] Playing a streamed film shows its **real poster**, the title, and a counting-down "left" timer.
- [ ] Pausing that film **stops** the countdown and the second line reads `Paused`.
- [ ] Resuming restores a countdown matching the actual time left.
- [ ] Seeking updates the countdown within about four seconds.
- [ ] The **View on IMDb** button opens the right title; **Get EverythingBox** opens the site.
- [ ] An episode shows the show name and `S…E… · <title>`.
- [ ] A local-library film (no https art) shows the **movie** fallback icon, not a broken image.
- [ ] A SNES game shows the title, `SNES`, and counts up; closing the emulator returns to **Browsing**.
- [ ] A track shows artist and album and counts down; a gapless boundary moves the card to the next track.
- [ ] A comic shows `Reading · Ch. N` and updates on a page turn.
- [ ] Silencing one category mid-playback clears the card **immediately**; the others still work.
- [ ] Switching the master off clears the card and the status line says so.
- [ ] Quitting Discord while a film plays: the status line changes to "Discord isn't running"; **the app does not stall, warn or log**.
- [ ] Restarting Discord re-shows the current film's card **without** needing a new track boundary.
- [ ] A title with non-Latin characters (a CJK or accented film) shows correctly and does not freeze the card.

- [ ] **Step 4: Commit**

```bash
git add native/src/ui/MainWindow.cpp
git commit -m "feat: point Discord presence at the EverythingBox application"
```

- [ ] **Step 5: Merge**

```bash
git checkout main && git pull && git merge --no-ff feat/discord-rich-presence
BUILD_DIR=build bash native/tools/run-headless-probes.sh
git push
```

Expected: the gate passes on the merge commit before pushing.

---

## Notes for the implementer

**One field this plan invents:** Task 6 Step 2 uses `item.presenceSubtitle()`, which does not exist on
`MediaItem`. Add it, or inline the string from `year` / `season` / `episode` / `name`, whichever reads better
where it lands. It must not perform a lookup — every fact it needs is already on the item.

**One thing to watch for at review:** Discord has historically rejected a `details` or `state` of exactly one
character. This plan does not implement a minimum-length rule, because that behaviour is not in the current
documentation and inventing it would be guessing. If Task 7's live walk shows a one-character title failing to
appear, `Presence::build` is where to add the pad and `probe_presence` §2 is where to pin it.

**A decision this plan makes that the spec did not cover:** the `discord/` settings keys are carved out of the
cloud-sync bundle as device-local (Task 2 Step 4). Whether the machine in the living room announces what it is
playing is a property of that machine, not of the account, and syncing the master switch would let switching
presence on for a laptop silently switch it on for a shared TV. If that is not wanted, delete the four-line
change to `CloudSync.cpp` and the keys sync like any other preference.
