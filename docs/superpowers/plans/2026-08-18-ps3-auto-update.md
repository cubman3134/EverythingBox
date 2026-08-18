# PS3 Game Auto-Update Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When EverythingBox launches a PS3 game in RPCS3, fetch the game's official Sony update PKG chain and install it into RPCS3 before boot, so the player runs the patched game with no manual step.

**Architecture:** All client-side, in `native/src/core/ps3/`. Six small, seam-isolated units (SFO parser, Title-ID reader, ver.xml feed parser, update installer, idempotency state, orchestration coordinator) tested by one pure-logic probe. Two glue changes wire it in: a default-on setting in both settings builders, and a hook in `EmulatorManager::launch()` that runs the coordinator for `rpcs3` before starting the game process. The update path never blocks a boot — every failure falls through to a normal unpatched launch.

**Tech Stack:** C++17, Qt6 (`Qt6::Core` for the units; `Qt6::Network` for the production fetch in the wiring task). Tests are a headless `probe_ps3update` executable (Qt offscreen, no display/GPU/network).

## Global Constraints

- **No AI attribution** anywhere in commits/PRs/issue bodies (no `Co-Authored-By`, no "Generated with", no tool name). Repo `CLAUDE.md`.
- **Stage by explicit path only** — `git add <path>`; NEVER `git add -A` / `git add .`. The working tree is shared with concurrent sessions.
- **A new probe is registered in ALL THREE places** or CI/the gate misses it:
  1. `native/CMakeLists.txt` — `add_executable(...)` + `target_include_directories` + `target_link_libraries` + `target_compile_definitions`.
  2. `native/tools/run-headless-probes.sh` — add `"probe_ps3update PS3UPDATE-OK"` to the pure-logic `for p in ...` list (~line 321).
  3. `.github/workflows/ci.yml` — add `probe_ps3update` to the `cmake --build build --target ...` list (~line 63).
- **The new setting goes in BOTH settings builders** in `native/src/ui/MainWindow.cpp` (the themed builder AND the QWidget builder) or it is unreachable. ROMs-folder is the precedent.
- **All UI through the nav kit** (`src/ui/nav`: NavOverlay/NavRing/Osk) — never `QDialog`/`QMessageBox`/`QInputDialog`/top-level windows.
- **The update path never blocks a boot.** Every failure stage (unreadable Title ID, network failure, empty/malformed feed, download failure, SHA mismatch, non-zero install exit) is caught, logged, and falls through to `startGameProcess`.
- **Sony endpoint (verified 2026-08-18):** `https://a0.ww.np.dl.playstation.net/tpl/np/{TITLEID}/{TITLEID}-ver.xml`, TLS **peer verification disabled** (cert CN mismatch). Empty body = no updates. Package `url`s are plain `http://`.
- **Version floor:** C++17. Follow existing `EmulatorManager` patterns (portable `binDir`, seed-if-absent); do not restructure the launch path beyond the added hook.
- **Build one probe, not all:** never a target-less `cmake --build`. `cmake --build build --config Release --target probe_ps3update`.

---

## File Structure

- Create: `native/src/core/ps3/Ps3Sfo.h` / `.cpp` — pure SFO byte → `TITLE_ID` parser.
- Create: `native/src/core/ps3/Ps3TitleId.h` / `.cpp` — locate + read the Title ID from a folder or `.pkg` rom path (uses `Ps3Sfo`).
- Create: `native/src/core/ps3/Ps3UpdateFeed.h` / `.cpp` — `Ps3UpdatePackage` + `parsePs3VerXml`.
- Create: `native/src/core/ps3/Ps3UpdateState.h` / `.cpp` — version compare + JSON idempotency store.
- Create: `native/src/core/ps3/Ps3UpdateInstaller.h` / `.cpp` — download → SHA-1 verify → `--installpkg`, seams injected.
- Create: `native/src/core/ps3/Ps3UpdateCoordinator.h` / `.cpp` — orchestration, seams injected.
- Create: `native/tools/probe_ps3update.cpp` — the single headless probe covering all six units.
- Modify: `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml` — register the probe.
- Modify: `native/src/ui/MainWindow.cpp` — the "Auto-install PS3 game updates" toggle in both settings builders.
- Modify: `native/src/core/EmulatorManager.cpp` (`launch()`) — run the coordinator for `rpcs3` before boot, on a background thread, with a nav-overlay note.

Header/source split follows the existing `native/src/core` convention. The six `ps3/` units have no Qt-Widgets or nav dependency — they link `Qt6::Core` only — so the probe builds without the app.

---

## Task 1: SFO parser + probe scaffold

**Files:**
- Create: `native/src/core/ps3/Ps3Sfo.h`, `native/src/core/ps3/Ps3Sfo.cpp`
- Create: `native/tools/probe_ps3update.cpp`
- Modify: `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml`

**Interfaces:**
- Produces: `std::optional<QString> Ps3Sfo::titleIdFromSfo(const QByteArray& sfo);` — returns the `TITLE_ID` value (e.g. `"BLUS31156"`), or `std::nullopt` if absent/malformed.
- Produces (probe helper, file-local): `QByteArray makeSfo(const QVector<QPair<QString,QString>>& kv);` — builds a valid PARAM.SFO with UTF-8 string entries, for tests.

- [ ] **Step 1: Write the failing probe**

Create `native/tools/probe_ps3update.cpp`:

```cpp
// Headless pure-logic probe for the PS3 auto-update units. Prints PS3UPDATE-OK on success.
// No display, no network, no process spawns — every external effect is an injected seam.
#include "core/ps3/Ps3Sfo.h"

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QPair>
#include <QtEndian>
#include <cstdio>
#include <optional>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #c, __LINE__); ++g_fail; } } while (0)

// Build a minimal valid PARAM.SFO holding the given UTF-8 string keys, so the parser is exercised
// against real bytes rather than a magic blob. Layout: 20-byte header, N index entries (16 bytes each),
// key table (null-terminated names), data table (null-terminated values padded to max len).
static QByteArray makeSfo(const QVector<QPair<QString, QString>>& kv)
{
    auto u16 = [](quint16 v) { char b[2]; qToLittleEndian(v, b); return QByteArray(b, 2); };
    auto u32 = [](quint32 v) { char b[4]; qToLittleEndian(v, b); return QByteArray(b, 4); };

    QByteArray keyTable, dataTable, index;
    QVector<quint32> keyOffs, dataOffs, dataLens, dataMax;
    for (const auto& p : kv)
    {
        QByteArray k = p.first.toUtf8();  k.append('\0');
        QByteArray d = p.second.toUtf8(); d.append('\0');
        const quint32 maxLen = static_cast<quint32>((d.size() + 15) & ~15); // pad to 16
        keyOffs.append(static_cast<quint32>(keyTable.size()));
        dataOffs.append(static_cast<quint32>(dataTable.size()));
        dataLens.append(static_cast<quint32>(d.size()));
        dataMax.append(maxLen);
        keyTable.append(k);
        dataTable.append(d);
        dataTable.append(QByteArray(static_cast<int>(maxLen) - d.size(), '\0'));
    }
    const quint32 entries = static_cast<quint32>(kv.size());
    const quint32 keyStart = 20 + entries * 16;
    const quint32 dataStart = keyStart + static_cast<quint32>(keyTable.size());
    for (quint32 i = 0; i < entries; ++i)
    {
        index += u16(static_cast<quint16>(keyOffs[i]));
        index += u16(0x0204); // utf8 null-terminated
        index += u32(dataLens[i]);
        index += u32(dataMax[i]);
        index += u32(dataOffs[i]);
    }
    QByteArray out;
    out.append('\0'); out.append("PSF", 3);       // magic \0PSF
    out += u32(0x00000101);                        // version 1.1
    out += u32(keyStart);
    out += u32(dataStart);
    out += u32(entries);
    out += index; out += keyTable; out += dataTable;
    return out;
}

static void testSfo()
{
    const QByteArray sfo = makeSfo({ { "APP_VER", "01.00" }, { "TITLE_ID", "BLUS31156" }, { "TITLE", "GTA V" } });
    auto id = Ps3Sfo::titleIdFromSfo(sfo);
    CHECK(id.has_value());
    CHECK(id.value_or(QString()) == QStringLiteral("BLUS31156"));

    CHECK(!Ps3Sfo::titleIdFromSfo(makeSfo({ { "TITLE", "No id here" } })).has_value());
    CHECK(!Ps3Sfo::titleIdFromSfo(QByteArray("not an sfo")).has_value());
    CHECK(!Ps3Sfo::titleIdFromSfo(QByteArray()).has_value());
}

int main()
{
    testSfo();
    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("PS3UPDATE-OK\n");
    return 0;
}
```

- [ ] **Step 2: Create the header with no implementation, register the probe, build, verify it fails**

Create `native/src/core/ps3/Ps3Sfo.h`:

```cpp
#pragma once
#include <QByteArray>
#include <QString>
#include <optional>

// Parses a PS3 PARAM.SFO blob (the small key/value binary Sony stores game metadata in) and returns
// the TITLE_ID value. Pure: takes bytes, returns the id or nullopt on any malformation.
namespace Ps3Sfo {
std::optional<QString> titleIdFromSfo(const QByteArray& sfo);
}
```

Create `native/src/core/ps3/Ps3Sfo.cpp` with a stub so the link fails on behavior, not symbol:

```cpp
#include "core/ps3/Ps3Sfo.h"

namespace Ps3Sfo {
std::optional<QString> titleIdFromSfo(const QByteArray&) { return std::nullopt; }
}
```

Register the probe. In `native/CMakeLists.txt`, beside the other pure-logic probes (e.g. near `probe_useremulators`), add:

```cmake
    add_executable(probe_ps3update tools/probe_ps3update.cpp
        src/core/ps3/Ps3Sfo.cpp)
    target_include_directories(probe_ps3update PRIVATE src src/core)
    target_link_libraries(probe_ps3update PRIVATE Qt6::Core)
```

In `native/tools/run-headless-probes.sh`, append `"probe_ps3update PS3UPDATE-OK"` to the `for p in ...` list (~line 321).

In `.github/workflows/ci.yml` (~line 63), append `probe_ps3update` to the `cmake --build build --target ...` probe list.

Run: `cmake --build build --config Release --target probe_ps3update`
Then run the built exe.
Expected: builds, but the SFO checks FAIL (stub returns nullopt) — `main` returns 1, no `PS3UPDATE-OK`.

- [ ] **Step 3: Implement the parser**

Replace `native/src/core/ps3/Ps3Sfo.cpp`:

```cpp
#include "core/ps3/Ps3Sfo.h"
#include <QtEndian>

namespace Ps3Sfo {

std::optional<QString> titleIdFromSfo(const QByteArray& sfo)
{
    if (sfo.size() < 20) return std::nullopt;
    const auto* p = reinterpret_cast<const uchar*>(sfo.constData());
    // magic "\0PSF"
    if (!(p[0] == 0x00 && p[1] == 'P' && p[2] == 'S' && p[3] == 'F')) return std::nullopt;

    const quint32 keyStart  = qFromLittleEndian<quint32>(p + 8);
    const quint32 dataStart = qFromLittleEndian<quint32>(p + 12);
    const quint32 entries   = qFromLittleEndian<quint32>(p + 16);
    const int size = sfo.size();
    if (keyStart > static_cast<quint32>(size) || dataStart > static_cast<quint32>(size)) return std::nullopt;
    // guard the index table
    if (20 + static_cast<qint64>(entries) * 16 > size) return std::nullopt;

    for (quint32 i = 0; i < entries; ++i)
    {
        const uchar* e = p + 20 + i * 16;
        const quint16 keyOff  = qFromLittleEndian<quint16>(e + 0);
        const quint32 dataLen = qFromLittleEndian<quint32>(e + 4);
        const quint32 dataOff = qFromLittleEndian<quint32>(e + 12);

        // read the null-terminated key name
        const quint32 kpos = keyStart + keyOff;
        if (kpos >= static_cast<quint32>(size)) continue;
        int kend = static_cast<int>(kpos);
        while (kend < size && sfo[kend] != '\0') ++kend;
        const QByteArray key = sfo.mid(static_cast<int>(kpos), kend - static_cast<int>(kpos));
        if (key != "TITLE_ID") continue;

        const quint32 dpos = dataStart + dataOff;
        if (dpos > static_cast<quint32>(size)) return std::nullopt;
        int avail = size - static_cast<int>(dpos);
        int len = static_cast<int>(qMin<quint32>(dataLen, static_cast<quint32>(avail)));
        QByteArray val = sfo.mid(static_cast<int>(dpos), len);
        const int nul = val.indexOf('\0');
        if (nul >= 0) val.truncate(nul); // strings are null-terminated
        if (val.isEmpty()) return std::nullopt;
        return QString::fromLatin1(val); // Title IDs are ASCII
    }
    return std::nullopt;
}

} // namespace Ps3Sfo
```

- [ ] **Step 4: Build and verify the probe passes**

Run: `cmake --build build --config Release --target probe_ps3update`
Then run the built exe.
Expected: prints `PS3UPDATE-OK`, returns 0.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/ps3/Ps3Sfo.h native/src/core/ps3/Ps3Sfo.cpp native/tools/probe_ps3update.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: PS3 PARAM.SFO Title-ID parser + probe_ps3update harness"
```

---

## Task 2: ver.xml feed parser

**Files:**
- Create: `native/src/core/ps3/Ps3UpdateFeed.h`, `native/src/core/ps3/Ps3UpdateFeed.cpp`
- Modify: `native/tools/probe_ps3update.cpp`, `native/CMakeLists.txt`

**Interfaces:**
- Produces: `struct Ps3UpdatePackage { QString version; qint64 size = 0; QString sha1; QString url; QString ps3SystemVer; };`
- Produces: `QVector<Ps3UpdatePackage> Ps3UpdateFeed::parseVerXml(const QByteArray& xml);` — one entry per `<package>`, **sorted ascending by version**; empty body or malformed → empty vector.

- [ ] **Step 1: Write the failing test**

In `probe_ps3update.cpp`, add the include `#include "core/ps3/Ps3UpdateFeed.h"` and a `testFeed()` called from `main()` before the OK print:

```cpp
static void testFeed()
{
    // Verified real single-package feed (The Last of Us, BCUS98174).
    const QByteArray single =
        "<?xml version='1.0' encoding='UTF-8'?>"
        "<titlepatch status=\"alive\" titleid=\"BCUS98174\">"
        "<tag name=\"BCUS98174_T11\" popup=\"true\" signoff=\"true\">"
        "<package version=\"01.11\" size=\"284414928\" "
        "sha1sum=\"5f978c88721962b54f5b12053ee06f896ef3b4a1\" "
        "url=\"http://b0.ww.np.dl.playstation.net/tppkg/np/BCUS98174/BCUS98174_T11/x/patch.pkg\" "
        "ps3_system_ver=\"04.4000\"><paramsfo><TITLE>The Last of Us 1.11</TITLE></paramsfo></package>"
        "</tag></titlepatch>";
    auto one = Ps3UpdateFeed::parseVerXml(single);
    CHECK(one.size() == 1);
    if (one.size() == 1)
    {
        CHECK(one[0].version == QStringLiteral("01.11"));
        CHECK(one[0].size == 284414928LL);
        CHECK(one[0].sha1 == QStringLiteral("5f978c88721962b54f5b12053ee06f896ef3b4a1"));
        CHECK(one[0].url.startsWith(QStringLiteral("http://")));
    }

    // A multi-package chain, listed OUT of version order — must come back sorted ascending.
    const QByteArray chain =
        "<titlepatch titleid=\"BLUS31156\">"
        "<package version=\"01.11\" size=\"20\" sha1sum=\"bb\" url=\"http://h/b.pkg\"></package>"
        "<package version=\"01.05\" size=\"10\" sha1sum=\"aa\" url=\"http://h/a.pkg\"></package>"
        "</titlepatch>";
    auto many = Ps3UpdateFeed::parseVerXml(chain);
    CHECK(many.size() == 2);
    if (many.size() == 2)
    {
        CHECK(many[0].version == QStringLiteral("01.05")); // sorted ascending
        CHECK(many[1].version == QStringLiteral("01.11"));
    }

    CHECK(Ps3UpdateFeed::parseVerXml(QByteArray()).isEmpty());          // no updates = empty body
    CHECK(Ps3UpdateFeed::parseVerXml(QByteArray("<broken")).isEmpty()); // malformed = empty, not fatal
}
```

- [ ] **Step 2: Create the header + stub, add to CMake, build, verify fail**

Create `native/src/core/ps3/Ps3UpdateFeed.h`:

```cpp
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

struct Ps3UpdatePackage {
    QString version;
    qint64  size = 0;
    QString sha1;
    QString url;
    QString ps3SystemVer;
};

// Parses Sony's {TITLEID}-ver.xml feed into the update packages, sorted ascending by version.
// Empty body (Sony's "no updates" signal) or malformed XML both yield an empty vector.
namespace Ps3UpdateFeed {
QVector<Ps3UpdatePackage> parseVerXml(const QByteArray& xml);
}
```

Create `native/src/core/ps3/Ps3UpdateFeed.cpp` stub:

```cpp
#include "core/ps3/Ps3UpdateFeed.h"
namespace Ps3UpdateFeed {
QVector<Ps3UpdatePackage> parseVerXml(const QByteArray&) { return {}; }
}
```

In `native/CMakeLists.txt`, add `src/core/ps3/Ps3UpdateFeed.cpp` to `probe_ps3update`'s `add_executable` source list.

Run: `cmake --build build --config Release --target probe_ps3update`; run the exe.
Expected: FAIL (the single/chain checks fail; stub returns empty).

- [ ] **Step 3: Implement the parser**

Replace `native/src/core/ps3/Ps3UpdateFeed.cpp`:

```cpp
#include "core/ps3/Ps3UpdateFeed.h"
#include <QXmlStreamReader>
#include <algorithm>

namespace {
// Sony versions are "NN.NN". Compare numerically so 01.05 < 01.11 regardless of formatting quirks.
bool versionLess(const QString& a, const QString& b)
{
    const auto pa = a.split(QLatin1Char('.')); const auto pb = b.split(QLatin1Char('.'));
    const int amaj = pa.value(0).toInt(), amin = pa.value(1).toInt();
    const int bmaj = pb.value(0).toInt(), bmin = pb.value(1).toInt();
    if (amaj != bmaj) return amaj < bmaj;
    return amin < bmin;
}
}

namespace Ps3UpdateFeed {

QVector<Ps3UpdatePackage> parseVerXml(const QByteArray& xml)
{
    QVector<Ps3UpdatePackage> out;
    if (xml.trimmed().isEmpty()) return out;

    QXmlStreamReader r(xml);
    while (!r.atEnd())
    {
        if (r.readNext() == QXmlStreamReader::StartElement && r.name() == QLatin1String("package"))
        {
            const auto a = r.attributes();
            Ps3UpdatePackage p;
            p.version      = a.value(QLatin1String("version")).toString();
            p.size         = a.value(QLatin1String("size")).toLongLong();
            p.sha1         = a.value(QLatin1String("sha1sum")).toString();
            p.url          = a.value(QLatin1String("url")).toString();
            p.ps3SystemVer = a.value(QLatin1String("ps3_system_ver")).toString();
            if (!p.url.isEmpty()) out.append(p);
        }
    }
    if (r.hasError()) return {}; // malformed -> no updates, never fatal

    std::sort(out.begin(), out.end(),
              [](const Ps3UpdatePackage& x, const Ps3UpdatePackage& y) { return versionLess(x.version, y.version); });
    return out;
}

} // namespace Ps3UpdateFeed
```

- [ ] **Step 4: Build and verify pass**

Run: `cmake --build build --config Release --target probe_ps3update`; run the exe.
Expected: `PS3UPDATE-OK`.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/ps3/Ps3UpdateFeed.h native/src/core/ps3/Ps3UpdateFeed.cpp native/tools/probe_ps3update.cpp native/CMakeLists.txt
git commit -m "feat: parse Sony PS3 ver.xml update feed (sorted, empty-safe)"
```

---

## Task 3: version compare + idempotency state

**Files:**
- Create: `native/src/core/ps3/Ps3UpdateState.h`, `native/src/core/ps3/Ps3UpdateState.cpp`
- Modify: `native/tools/probe_ps3update.cpp`, `native/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from earlier tasks (self-contained; shares the version-compare idea but owns its own copy — see note).
- Produces:
  - `bool Ps3Version::less(const QString& a, const QString& b);` — public numeric "NN.NN" compare (extracted so both the feed sort and the state can share one definition).
  - `class Ps3UpdateState { public: explicit Ps3UpdateState(QString path); bool needsUpdate(const QString& titleId, const QString& latest) const; void markInstalled(const QString& titleId, const QString& version); };`

Note: extract `versionLess` from Task 2 into `Ps3Version::less` and have `Ps3UpdateFeed.cpp` call it, so there is one comparator. Do this as the first step here.

- [ ] **Step 1: Extract the shared comparator**

Create `native/src/core/ps3/Ps3Version.h`:

```cpp
#pragma once
#include <QString>
// Numeric compare of Sony "NN.NN" version strings (01.05 < 01.11).
namespace Ps3Version { bool less(const QString& a, const QString& b); }
```

Create `native/src/core/ps3/Ps3Version.cpp`:

```cpp
#include "core/ps3/Ps3Version.h"
namespace Ps3Version {
bool less(const QString& a, const QString& b)
{
    const auto pa = a.split(QLatin1Char('.')); const auto pb = b.split(QLatin1Char('.'));
    const int amaj = pa.value(0).toInt(), amin = pa.value(1).toInt();
    const int bmaj = pb.value(0).toInt(), bmin = pb.value(1).toInt();
    if (amaj != bmaj) return amaj < bmaj;
    return amin < bmin;
}
}
```

In `native/src/core/ps3/Ps3UpdateFeed.cpp`, delete the local anonymous `versionLess` and `#include "core/ps3/Ps3Version.h"`; change the sort comparator to `Ps3Version::less(x.version, y.version)`. Add `src/core/ps3/Ps3Version.cpp` to `probe_ps3update` sources in `native/CMakeLists.txt`.

- [ ] **Step 2: Write the failing test**

In `probe_ps3update.cpp`, add includes `#include "core/ps3/Ps3Version.h"` and `#include "core/ps3/Ps3UpdateState.h"`, plus `#include <QTemporaryDir>` and `#include <QDir>`. Add `testState()` called from `main()`:

```cpp
static void testState()
{
    CHECK(Ps3Version::less(QStringLiteral("01.05"), QStringLiteral("01.11")));
    CHECK(!Ps3Version::less(QStringLiteral("01.11"), QStringLiteral("01.11")));
    CHECK(!Ps3Version::less(QStringLiteral("02.00"), QStringLiteral("01.99")));

    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/ps3-updates.json");
    {
        Ps3UpdateState s(path);
        CHECK(s.needsUpdate(QStringLiteral("BLUS31156"), QStringLiteral("01.11"))); // unknown -> needs it
        s.markInstalled(QStringLiteral("BLUS31156"), QStringLiteral("01.11"));
        CHECK(!s.needsUpdate(QStringLiteral("BLUS31156"), QStringLiteral("01.11"))); // equal -> no
        CHECK(s.needsUpdate(QStringLiteral("BLUS31156"), QStringLiteral("01.12")));  // newer -> yes
    }
    {
        Ps3UpdateState reopened(path); // persisted across instances
        CHECK(!reopened.needsUpdate(QStringLiteral("BLUS31156"), QStringLiteral("01.11")));
    }
}
```

- [ ] **Step 3: Header + stub, add to CMake, build, verify fail**

Create `native/src/core/ps3/Ps3UpdateState.h`:

```cpp
#pragma once
#include <QString>
#include <QJsonObject>

// Per-Title-ID record of the highest update version already installed, persisted as a small JSON file.
// Makes the launch-time update check a no-op once a game is current.
class Ps3UpdateState {
public:
    explicit Ps3UpdateState(QString path);
    bool needsUpdate(const QString& titleId, const QString& latest) const;
    void markInstalled(const QString& titleId, const QString& version);
private:
    QString     path_;
    QJsonObject installed_; // titleId -> version
    void load();
    void save() const;
};
```

Create `native/src/core/ps3/Ps3UpdateState.cpp` stub (constructor stores path, methods no-op / return true):

```cpp
#include "core/ps3/Ps3UpdateState.h"
Ps3UpdateState::Ps3UpdateState(QString path) : path_(std::move(path)) {}
bool Ps3UpdateState::needsUpdate(const QString&, const QString&) const { return true; }
void Ps3UpdateState::markInstalled(const QString&, const QString&) {}
void Ps3UpdateState::load() {}
void Ps3UpdateState::save() const {}
```

Add `src/core/ps3/Ps3UpdateState.cpp` to `probe_ps3update` sources.
Run the build + exe. Expected: FAIL (the reopened/equal checks fail — stub always returns true, never persists).

- [ ] **Step 4: Implement**

Replace `native/src/core/ps3/Ps3UpdateState.cpp`:

```cpp
#include "core/ps3/Ps3UpdateState.h"
#include "core/ps3/Ps3Version.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <utility>

Ps3UpdateState::Ps3UpdateState(QString path) : path_(std::move(path)) { load(); }

void Ps3UpdateState::load()
{
    QFile f(path_);
    if (!f.open(QIODevice::ReadOnly)) return;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (doc.isObject()) installed_ = doc.object();
}

void Ps3UpdateState::save() const
{
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QFile f(path_);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(installed_).toJson(QJsonDocument::Compact));
}

bool Ps3UpdateState::needsUpdate(const QString& titleId, const QString& latest) const
{
    const QString have = installed_.value(titleId).toString();
    if (have.isEmpty()) return true;
    return Ps3Version::less(have, latest); // installed older than latest
}

void Ps3UpdateState::markInstalled(const QString& titleId, const QString& version)
{
    installed_.insert(titleId, version);
    save();
}
```

- [ ] **Step 5: Build, verify pass, commit**

Run the build + exe. Expected: `PS3UPDATE-OK`.

```bash
git add native/src/core/ps3/Ps3Version.h native/src/core/ps3/Ps3Version.cpp native/src/core/ps3/Ps3UpdateState.h native/src/core/ps3/Ps3UpdateState.cpp native/src/core/ps3/Ps3UpdateFeed.cpp native/tools/probe_ps3update.cpp native/CMakeLists.txt
git commit -m "feat: PS3 update idempotency state + shared version compare"
```

---

## Task 4: update installer (download → verify → install)

**Files:**
- Create: `native/src/core/ps3/Ps3UpdateInstaller.h`, `native/src/core/ps3/Ps3UpdateInstaller.cpp`
- Modify: `native/tools/probe_ps3update.cpp`, `native/CMakeLists.txt`

**Interfaces:**
- Consumes: `Ps3UpdatePackage` (Task 2).
- Produces:
```cpp
class Ps3UpdateInstaller {
public:
    // Downloads url to destPath; returns true on success (bytes on disk at destPath).
    using Downloader = std::function<bool(const QString& url, const QString& destPath)>;
    // Runs `rpcs3Exe --installpkg pkgPath`; returns the process exit code (0 = ok).
    using Installer  = std::function<int(const QString& rpcs3Exe, const QString& pkgPath)>;

    Ps3UpdateInstaller(QString rpcs3Exe, QString tmpDir, Downloader dl, Installer run);
    // Installs every package in order. Aborts (returns false) on the first download failure,
    // SHA-1 mismatch, or non-zero install exit; always cleans up its temp files.
    bool installAll(const QString& titleId, const QVector<Ps3UpdatePackage>& pkgs);
};
```

- [ ] **Step 1: Write the failing test**

In `probe_ps3update.cpp`, add `#include "core/ps3/Ps3UpdateInstaller.h"`, `#include <QCryptographicHash>`, `#include <QFile>`, `#include <QStringList>`. Add `testInstaller()`:

```cpp
static QString sha1Hex(const QByteArray& b)
{ return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha1).toHex()); }

static void testInstaller()
{
    QTemporaryDir dir; CHECK(dir.isValid());
    const QByteArray bodyA("PKG-A-BYTES"), bodyB("PKG-B-BYTES");

    // A stub downloader that writes canned bytes keyed by URL, and records order.
    QStringList installed;
    auto downloader = [&](const QString& url, const QString& dest) -> bool {
        const QByteArray body = url.endsWith(QStringLiteral("a.pkg")) ? bodyA : bodyB;
        QFile f(dest); if (!f.open(QIODevice::WriteOnly)) return false; f.write(body); return true;
    };
    auto runner = [&](const QString&, const QString& pkg) -> int { installed << pkg; return 0; };

    QVector<Ps3UpdatePackage> pkgs = {
        { QStringLiteral("01.05"), 0, sha1Hex(bodyA), QStringLiteral("http://h/a.pkg"), {} },
        { QStringLiteral("01.11"), 0, sha1Hex(bodyB), QStringLiteral("http://h/b.pkg"), {} },
    };

    Ps3UpdateInstaller good(QStringLiteral("rpcs3.exe"), dir.path(), downloader, runner);
    CHECK(good.installAll(QStringLiteral("BLUS31156"), pkgs));
    CHECK(installed.size() == 2);                       // both installed, in order
    if (installed.size() == 2) CHECK(installed[0].endsWith(QStringLiteral("a.pkg")) || installed[0].contains(QStringLiteral("01.05")));

    // Temp pkgs cleaned up afterwards.
    QDir d(dir.path());
    CHECK(d.entryList(QStringList() << QStringLiteral("*.pkg"), QDir::Files).isEmpty());

    // SHA mismatch on the second package aborts the whole update, nothing extra installed.
    installed.clear();
    QVector<Ps3UpdatePackage> bad = pkgs;
    bad[1].sha1 = QStringLiteral("deadbeef");
    Ps3UpdateInstaller mm(QStringLiteral("rpcs3.exe"), dir.path(), downloader, runner);
    CHECK(!mm.installAll(QStringLiteral("BLUS31156"), bad));
    CHECK(installed.size() == 1); // only the first (good) package's install ran before the abort
    CHECK(d.entryList(QStringList() << QStringLiteral("*.pkg"), QDir::Files).isEmpty()); // still cleaned up
}
```

- [ ] **Step 2: Header + stub, CMake, build, verify fail**

Create `native/src/core/ps3/Ps3UpdateInstaller.h`:

```cpp
#pragma once
#include "core/ps3/Ps3UpdateFeed.h"
#include <QString>
#include <QVector>
#include <functional>

class Ps3UpdateInstaller {
public:
    using Downloader = std::function<bool(const QString& url, const QString& destPath)>;
    using Installer  = std::function<int(const QString& rpcs3Exe, const QString& pkgPath)>;

    Ps3UpdateInstaller(QString rpcs3Exe, QString tmpDir, Downloader dl, Installer run);
    bool installAll(const QString& titleId, const QVector<Ps3UpdatePackage>& pkgs);

private:
    QString    rpcs3Exe_;
    QString    tmpDir_;
    Downloader download_;
    Installer  install_;
};
```

Create `native/src/core/ps3/Ps3UpdateInstaller.cpp` stub returning false. Add to CMake sources. Build + run. Expected: FAIL.

- [ ] **Step 3: Implement**

```cpp
#include "core/ps3/Ps3UpdateInstaller.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <utility>

Ps3UpdateInstaller::Ps3UpdateInstaller(QString rpcs3Exe, QString tmpDir, Downloader dl, Installer run)
    : rpcs3Exe_(std::move(rpcs3Exe)), tmpDir_(std::move(tmpDir)), download_(std::move(dl)), install_(std::move(run)) {}

namespace {
QString sha1Of(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha1);
    if (!h.addData(&f)) return {};
    return QString::fromLatin1(h.result().toHex());
}
}

bool Ps3UpdateInstaller::installAll(const QString& titleId, const QVector<Ps3UpdatePackage>& pkgs)
{
    QDir().mkpath(tmpDir_);
    QStringList temps;
    auto cleanup = [&] { for (const QString& t : temps) QFile::remove(t); };

    int n = 0;
    for (const Ps3UpdatePackage& p : pkgs)
    {
        const QString dest = QDir(tmpDir_).filePath(QStringLiteral("%1_%2_%3.pkg").arg(titleId, p.version).arg(n++));
        temps << dest;
        if (!download_(p.url, dest)) { cleanup(); return false; }
        if (!p.sha1.isEmpty() && sha1Of(dest).compare(p.sha1, Qt::CaseInsensitive) != 0) { cleanup(); return false; }
        if (install_(rpcs3Exe_, dest) != 0) { cleanup(); return false; }
    }
    cleanup();
    return true;
}
```

- [ ] **Step 4: Build, verify pass, commit**

Run + exe → `PS3UPDATE-OK`.

```bash
git add native/src/core/ps3/Ps3UpdateInstaller.h native/src/core/ps3/Ps3UpdateInstaller.cpp native/tools/probe_ps3update.cpp native/CMakeLists.txt
git commit -m "feat: PS3 update installer — download, SHA-1 verify, --installpkg, abort-on-corrupt"
```

---

## Task 5: Title-ID reader (folder + PKG)

**Files:**
- Create: `native/src/core/ps3/Ps3TitleId.h`, `native/src/core/ps3/Ps3TitleId.cpp`
- Modify: `native/tools/probe_ps3update.cpp`, `native/CMakeLists.txt`

**Interfaces:**
- Consumes: `Ps3Sfo::titleIdFromSfo` (Task 1).
- Produces:
  - `std::optional<QString> Ps3TitleId::titleIdFromPkgHeader(const QByteArray& header);` — parse the `content_id` at offset 0x30 of a PKG header → Title ID.
  - `std::optional<QString> Ps3TitleId::read(const QString& romPath);` — folder or `.pkg` rom path → Title ID; unknown format / unreadable → `nullopt`.

- [ ] **Step 1: Write the failing test**

Add `#include "core/ps3/Ps3TitleId.h"` to the probe. Add `testTitleId()` (reuses `makeSfo` from Task 1):

```cpp
static void testTitleId()
{
    // --- PKG header: magic "\x7FPKG", 36-byte content_id at 0x30 = "UP0001-BLUS31156_00-GTAVGTAVGTAVGTA"
    QByteArray pkg(0x60, '\0');
    pkg[0] = 0x7F; pkg[1] = 'P'; pkg[2] = 'K'; pkg[3] = 'G';
    const QByteArray cid = "UP0001-BLUS31156_00-GTAVGTAVGTAVGTA"; // 35 chars + implicit slack
    for (int i = 0; i < cid.size(); ++i) pkg[0x30 + i] = cid[i];
    auto fromHdr = Ps3TitleId::titleIdFromPkgHeader(pkg);
    CHECK(fromHdr.value_or(QString()) == QStringLiteral("BLUS31156"));
    CHECK(!Ps3TitleId::titleIdFromPkgHeader(QByteArray("not a pkg")).has_value());

    // --- folder game: <root>/PS3_GAME/PARAM.SFO
    QTemporaryDir dir; CHECK(dir.isValid());
    const QString root = dir.path() + QStringLiteral("/game");
    QDir().mkpath(root + QStringLiteral("/PS3_GAME"));
    {
        QFile f(root + QStringLiteral("/PS3_GAME/PARAM.SFO"));
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(makeSfo({ { "TITLE_ID", "BLUS31156" } }));
    }
    CHECK(Ps3TitleId::read(root).value_or(QString()) == QStringLiteral("BLUS31156"));
    // reading from the EBOOT path walks up to the game root
    QDir().mkpath(root + QStringLiteral("/PS3_GAME/USRDIR"));
    { QFile e(root + QStringLiteral("/PS3_GAME/USRDIR/EBOOT.BIN")); CHECK(e.open(QIODevice::WriteOnly)); e.write("x"); }
    CHECK(Ps3TitleId::read(root + QStringLiteral("/PS3_GAME/USRDIR/EBOOT.BIN")).value_or(QString()) == QStringLiteral("BLUS31156"));

    // --- .pkg file on disk
    const QString pkgPath = dir.path() + QStringLiteral("/game.pkg");
    { QFile f(pkgPath); CHECK(f.open(QIODevice::WriteOnly)); f.write(pkg); }
    CHECK(Ps3TitleId::read(pkgPath).value_or(QString()) == QStringLiteral("BLUS31156"));

    // --- unknown format -> nullopt (safe fallthrough)
    const QString isoPath = dir.path() + QStringLiteral("/game.iso");
    { QFile f(isoPath); CHECK(f.open(QIODevice::WriteOnly)); f.write("random iso bytes"); }
    CHECK(!Ps3TitleId::read(isoPath).has_value());
}
```

- [ ] **Step 2: Header + stub, CMake, build, verify fail**

Create `native/src/core/ps3/Ps3TitleId.h`:

```cpp
#pragma once
#include <QByteArray>
#include <QString>
#include <optional>

// Reads a PS3 game's Title ID (e.g. "BLUS31156") from a rom path. Handles the two formats EverythingBox
// hands RPCS3 — an extracted/JB game folder (reads PS3_GAME/PARAM.SFO) and a .pkg (reads content_id).
// Any other format returns nullopt so the update step falls through to a normal boot.
namespace Ps3TitleId {
std::optional<QString> titleIdFromPkgHeader(const QByteArray& header);
std::optional<QString> read(const QString& romPath);
}
```

Create the stub `.cpp` (both return `std::nullopt`). Add to CMake sources. Build + run. Expected: FAIL.

- [ ] **Step 3: Implement**

```cpp
#include "core/ps3/Ps3TitleId.h"
#include "core/ps3/Ps3Sfo.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace Ps3TitleId {

std::optional<QString> titleIdFromPkgHeader(const QByteArray& header)
{
    if (header.size() < 0x30 + 36) return std::nullopt;
    const auto* p = reinterpret_cast<const uchar*>(header.constData());
    if (!(p[0] == 0x7F && p[1] == 'P' && p[2] == 'K' && p[3] == 'G')) return std::nullopt;
    QByteArray cid = header.mid(0x30, 36);
    const int nul = cid.indexOf('\0'); if (nul >= 0) cid.truncate(nul);
    // content_id "XXYYYY-{TITLEID}_00-..." -> take the segment between the first '-' and the first '_'.
    const int dash = cid.indexOf('-'); if (dash < 0) return std::nullopt;
    const int us = cid.indexOf('_', dash); if (us < 0) return std::nullopt;
    const QByteArray tid = cid.mid(dash + 1, us - dash - 1);
    if (tid.isEmpty()) return std::nullopt;
    return QString::fromLatin1(tid);
}

namespace {
// Read the SFO at <gameRoot>/PS3_GAME/PARAM.SFO or <gameRoot>/PARAM.SFO.
std::optional<QString> fromGameRoot(const QString& root)
{
    for (const QString& rel : { QStringLiteral("/PS3_GAME/PARAM.SFO"), QStringLiteral("/PARAM.SFO") })
    {
        QFile f(root + rel);
        if (f.open(QIODevice::ReadOnly))
        {
            auto id = Ps3Sfo::titleIdFromSfo(f.readAll());
            if (id) return id;
        }
    }
    return std::nullopt;
}
}

std::optional<QString> read(const QString& romPath)
{
    const QFileInfo fi(romPath);
    if (!fi.exists()) return std::nullopt;

    if (fi.isDir()) return fromGameRoot(QDir(romPath).absolutePath());

    if (fi.suffix().compare(QStringLiteral("pkg"), Qt::CaseInsensitive) == 0)
    {
        QFile f(romPath);
        if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
        return titleIdFromPkgHeader(f.read(0x60));
    }

    // A file inside a game tree (e.g. EBOOT.BIN): walk up to the game root (the dir that holds PS3_GAME,
    // or a PS3_GAME dir itself) and read the SFO there.
    QDir d = fi.absoluteDir();
    for (int hops = 0; hops < 6; ++hops)
    {
        if (auto id = fromGameRoot(d.absolutePath())) return id;
        if (d.dirName().compare(QStringLiteral("PS3_GAME"), Qt::CaseInsensitive) == 0)
            if (auto id = fromGameRoot(QFileInfo(d.absolutePath()).absolutePath())) return id;
        if (!d.cdUp()) break;
    }
    return std::nullopt;
}

} // namespace Ps3TitleId
```

- [ ] **Step 4: Build, verify pass, commit**

Run + exe → `PS3UPDATE-OK`.

```bash
git add native/src/core/ps3/Ps3TitleId.h native/src/core/ps3/Ps3TitleId.cpp native/tools/probe_ps3update.cpp native/CMakeLists.txt
git commit -m "feat: read PS3 Title ID from a game folder or .pkg"
```

---

## Task 6: orchestration coordinator

**Files:**
- Create: `native/src/core/ps3/Ps3UpdateCoordinator.h`, `native/src/core/ps3/Ps3UpdateCoordinator.cpp`
- Modify: `native/tools/probe_ps3update.cpp`, `native/CMakeLists.txt`

**Interfaces:**
- Consumes: `Ps3UpdateFeed::parseVerXml`, `Ps3UpdateState`, `Ps3UpdateInstaller`.
- Produces:
```cpp
class Ps3UpdateCoordinator {
public:
    using TitleIdReader = std::function<std::optional<QString>(const QString& romPath)>;
    using FeedFetcher   = std::function<std::optional<QByteArray>(const QString& titleId)>;
    using Progress      = std::function<void(const QString& message)>;

    Ps3UpdateCoordinator(TitleIdReader readId, FeedFetcher fetch,
                         Ps3UpdateState* state, Ps3UpdateInstaller* installer, Progress progress);
    // Runs the whole check→install pipeline for a rom. Returns true if it installed an update.
    // Any missing/failed stage short-circuits to false — never throws, never blocks the caller's boot.
    bool maybeUpdate(const QString& romPath);
};
```

- [ ] **Step 1: Write the failing test**

Add `#include "core/ps3/Ps3UpdateCoordinator.h"`. Add `testCoordinator()`:

```cpp
static void testCoordinator()
{
    QTemporaryDir dir; CHECK(dir.isValid());
    const QByteArray body("PKGDATA");
    const QByteArray feed =
        QByteArray("<titlepatch titleid=\"BLUS31156\"><package version=\"01.11\" size=\"7\" sha1sum=\"")
        + sha1Hex(body).toLatin1() + "\" url=\"http://h/a.pkg\"></package></titlepatch>";

    auto downloader = [&](const QString&, const QString& dest) {
        QFile f(dest); if (!f.open(QIODevice::WriteOnly)) return false; f.write(body); return true; };
    int installs = 0;
    auto runner = [&](const QString&, const QString&) { ++installs; return 0; };
    Ps3UpdateInstaller installer(QStringLiteral("rpcs3.exe"), dir.path(), downloader, runner);
    Ps3UpdateState state(dir.path() + QStringLiteral("/state.json"));

    QStringList notes;
    auto progress = [&](const QString& m) { notes << m; };

    // Happy path: reads id, fetches feed, installs, marks state.
    {
        Ps3UpdateCoordinator c(
            [](const QString&) { return std::optional<QString>(QStringLiteral("BLUS31156")); },
            [&](const QString&) { return std::optional<QByteArray>(feed); },
            &state, &installer, progress);
        CHECK(c.maybeUpdate(QStringLiteral("/any/rom")));
    }
    CHECK(installs == 1);
    CHECK(!notes.isEmpty()); // showed an "Updating…" note

    // Second run: state says current -> no work, no extra install.
    {
        Ps3UpdateCoordinator c(
            [](const QString&) { return std::optional<QString>(QStringLiteral("BLUS31156")); },
            [&](const QString&) { return std::optional<QByteArray>(feed); },
            &state, &installer, progress);
        CHECK(!c.maybeUpdate(QStringLiteral("/any/rom")));
    }
    CHECK(installs == 1); // unchanged

    // No Title ID -> falls through, no fetch/install.
    {
        int fetches = 0;
        Ps3UpdateCoordinator c(
            [](const QString&) { return std::optional<QString>(); },
            [&](const QString&) { ++fetches; return std::optional<QByteArray>(feed); },
            &state, &installer, progress);
        CHECK(!c.maybeUpdate(QStringLiteral("/any/rom")));
        CHECK(fetches == 0);
    }

    // Empty feed (Sony "no updates") -> falls through.
    {
        Ps3UpdateCoordinator c(
            [](const QString&) { return std::optional<QString>(QStringLiteral("BLUS40000")); },
            [&](const QString&) { return std::optional<QByteArray>(QByteArray()); },
            &state, &installer, progress);
        CHECK(!c.maybeUpdate(QStringLiteral("/any/rom")));
    }
}
```

- [ ] **Step 2: Header + stub, CMake, build, verify fail**

Create `native/src/core/ps3/Ps3UpdateCoordinator.h`:

```cpp
#pragma once
#include "core/ps3/Ps3UpdateState.h"
#include "core/ps3/Ps3UpdateInstaller.h"
#include <QByteArray>
#include <QString>
#include <functional>
#include <optional>

class Ps3UpdateCoordinator {
public:
    using TitleIdReader = std::function<std::optional<QString>(const QString& romPath)>;
    using FeedFetcher   = std::function<std::optional<QByteArray>(const QString& titleId)>;
    using Progress      = std::function<void(const QString& message)>;

    Ps3UpdateCoordinator(TitleIdReader readId, FeedFetcher fetch,
                         Ps3UpdateState* state, Ps3UpdateInstaller* installer, Progress progress);
    bool maybeUpdate(const QString& romPath);

private:
    TitleIdReader       readId_;
    FeedFetcher         fetch_;
    Ps3UpdateState*     state_;
    Ps3UpdateInstaller* installer_;
    Progress            progress_;
};
```

Create the stub `.cpp` (`maybeUpdate` returns false). Add to CMake sources. Build + run. Expected: FAIL (happy-path checks fail).

- [ ] **Step 3: Implement**

```cpp
#include "core/ps3/Ps3UpdateCoordinator.h"
#include "core/ps3/Ps3UpdateFeed.h"
#include <utility>

Ps3UpdateCoordinator::Ps3UpdateCoordinator(TitleIdReader readId, FeedFetcher fetch,
                                           Ps3UpdateState* state, Ps3UpdateInstaller* installer, Progress progress)
    : readId_(std::move(readId)), fetch_(std::move(fetch)), state_(state), installer_(installer), progress_(std::move(progress)) {}

bool Ps3UpdateCoordinator::maybeUpdate(const QString& romPath)
{
    const auto titleId = readId_ ? readId_(romPath) : std::nullopt;
    if (!titleId || titleId->isEmpty()) return false;

    const auto body = fetch_ ? fetch_(*titleId) : std::nullopt;
    if (!body || body->trimmed().isEmpty()) return false;

    const QVector<Ps3UpdatePackage> pkgs = Ps3UpdateFeed::parseVerXml(*body);
    if (pkgs.isEmpty()) return false;

    const QString latest = pkgs.last().version; // parseVerXml sorts ascending
    if (!state_ || !state_->needsUpdate(*titleId, latest)) return false;

    if (progress_) progress_(QStringLiteral("Updating game… v%1").arg(latest));
    if (!installer_ || !installer_->installAll(*titleId, pkgs)) return false;

    state_->markInstalled(*titleId, latest);
    return true;
}
```

- [ ] **Step 4: Build, verify pass, commit**

Run + exe → `PS3UPDATE-OK`.

```bash
git add native/src/core/ps3/Ps3UpdateCoordinator.h native/src/core/ps3/Ps3UpdateCoordinator.cpp native/tools/probe_ps3update.cpp native/CMakeLists.txt
git commit -m "feat: PS3 update coordinator orchestrating check→install with fallthrough"
```

---

## Task 7: settings toggle in both builders

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (themed settings builder AND QWidget settings builder)
- Test: `native/tools/probe_emusettings.cpp` if it covers persisted settings keys; otherwise assert via the existing settings storage in a small addition to that probe. (Locate the settings-key storage the app already uses — the same store ROMs-folder writes to — and add the `ps3AutoUpdate` bool default true.)

**Interfaces:**
- Consumes: nothing new.
- Produces: a persisted boolean setting keyed `ps3/autoUpdate` (default `true`), read by Task 8. Expose it through the same settings accessor the rest of the app uses (find how ROMs-folder is read/written and mirror it).

- [ ] **Step 1: Find the settings pattern**

Read `native/src/ui/MainWindow.cpp` for the ROMs-folder setting: how it is stored (the `QSettings`/config key), how it appears in the **themed** builder (sep/info/action rows) and in the **QWidget** builder. Note both call sites. This task mirrors that pattern exactly for a boolean toggle.

- [ ] **Step 2: Add the setting accessor + default**

Wherever app settings are centralized (the accessor ROMs-folder uses), add read/write for key `ps3/autoUpdate` defaulting to `true`. If settings are read inline via `QSettings`, define a helper next to the existing ones:

```cpp
// Whether EverythingBox auto-installs official Sony updates for a PS3 game before launching it in RPCS3.
bool ps3AutoUpdateEnabled() { return QSettings().value(QStringLiteral("ps3/autoUpdate"), true).toBool(); }
```

(Match the actual settings mechanism found in Step 1 — do not introduce a second settings system.)

- [ ] **Step 3: Add the toggle to BOTH builders**

In the **themed** settings builder, add a boolean row labeled "Auto-install PS3 game updates" that reads/writes `ps3/autoUpdate`, placed near the other emulation/ROMs settings, using the existing themed toggle-row idiom. In the **QWidget** settings builder, add the equivalent checkbox bound to the same key. Follow the ROMs-folder precedent for placement and wiring in each.

- [ ] **Step 4: Build the app settings probe (or a targeted check) and verify the key round-trips**

If `probe_emusettings` (or the nearest settings probe) can assert the default and round-trip, add:

```cpp
CHECK(QSettings().value(QStringLiteral("ps3/autoUpdate"), true).toBool() == true); // default on
```

Run that probe. Expected: pass. (If no probe covers app `QSettings`, verify by building the app target and confirming the toggle appears in both surfaces via the existing EB_UITEST harness — see the "Verify app GUI" memory; document the manual check in the commit body.)

- [ ] **Step 5: Commit**

```bash
git add native/src/ui/MainWindow.cpp
# add the probe file too if you extended one
git commit -m "feat: 'Auto-install PS3 game updates' setting in both settings builders (default on)"
```

---

## Task 8: wire the coordinator into EmulatorManager::launch()

**Files:**
- Modify: `native/src/core/EmulatorManager.cpp` (`launch()`), and its header if a new private helper is declared.
- Modify: `native/CMakeLists.txt` — add the six `src/core/ps3/*.cpp` to the **app** target's sources, and link `Qt6::Network` if not already linked.

**Interfaces:**
- Consumes: `Ps3UpdateCoordinator`, `Ps3TitleId::read`, `Ps3UpdateState`, `Ps3UpdateInstaller`, and `ps3AutoUpdateEnabled()` (Task 7).
- Produces: no new public interface; a private `EmulatorManager::runPs3UpdateThenLaunch(...)` helper.

This is thin glue over already-tested units; it is not unit-tested (it performs real network + process I/O). Keep the logic in the coordinator; this task only constructs production seams and threads the call.

- [ ] **Step 1: Add the app-side production seams**

In `EmulatorManager.cpp`, add a file-local helper section (guarded to this translation unit) that builds the production seams:

- **Feed fetch** (`Ps3UpdateCoordinator::FeedFetcher`): GET `https://a0.ww.np.dl.playstation.net/tpl/np/{TITLEID}/{TITLEID}-ver.xml` with peer verification disabled. Use `QNetworkAccessManager` with a `QNetworkRequest` whose `QSslConfiguration` sets `setPeerVerifyMode(QSslSocket::VerifyNone)`, run synchronously on the worker thread via a local `QEventLoop`. Return the body on HTTP 200 (empty body included → `QByteArray()`), `std::nullopt` on transport error.
- **Downloader** (`Ps3UpdateInstaller::Downloader`): GET the plain-`http://` package `url` to `destPath` with `QNetworkAccessManager` (streamed to a `QFile`); return true on 200 + complete write.
- **Installer** (`Ps3UpdateInstaller::Installer`): `QProcess::execute(rpcs3Exe, {"--installpkg", pkgPath})`; return its exit code. (RPCS3 installs the PKG into `dev_hdd0/game/{TITLEID}` under the portable `binDir` and exits.)
- **Title-ID reader**: `Ps3TitleId::read`.
- **State**: `Ps3UpdateState(AppPaths::dataDir() + "/ps3-updates.json")`.
- **Progress**: post the message to the nav overlay (NavOverlay) on the UI thread — reuse whatever transient-note mechanism the launch flow already uses; do NOT introduce a QDialog.

- [ ] **Step 2: Hook the RPCS3 branch of `launch()`**

In `launch()`, after `binDir` is known and before `startGameProcess`, add:

```cpp
#ifdef Q_OS_WIN
    const bool isRpcs3 = (em_.id == QStringLiteral("rpcs3"));
#else
    const bool isRpcs3 = (em_.id == QStringLiteral("rpcs3"));
#endif
    if (isRpcs3 && ps3AutoUpdateEnabled() && !rom_.isEmpty())
    {
        // Run the (blocking) update check on a worker thread so the UI thread stays responsive; show a
        // transient "Updating game…" note; then start the game. Any failure inside maybeUpdate() falls
        // through and we boot the unpatched game — the update path never prevents a launch.
        runPs3UpdateThenLaunch(binary, binDir);
        return;
    }
```

`runPs3UpdateThenLaunch` (new private method): capture `binary`/`binDir`/`rom_`, dispatch to a `QThread`/`QtConcurrent::run`:
1. Build the seams (Step 1) with `rpcs3Exe = binary`, `tmpDir = binDir + "/.eb-ps3-updates"`.
2. Construct the coordinator; call `maybeUpdate(rom_)` (result ignored — informational only).
3. Back on the UI thread (queued), call the normal `startGameProcess(binary, args, binDir, false)` path — reuse the exact args-building already in `launch()` (factor the args/program assembly so both the normal and post-update paths use it, rather than duplicating it).

Keep the refactor minimal: extract the args/program assembly from `launch()` into a small private helper both paths call, so there is no duplicated launch logic.

- [ ] **Step 3: Build the app target**

Run: `cmake --build build --config Release --target EverythingBox` (or the app target name in this tree).
Expected: compiles and links (with `Qt6::Network`).

- [ ] **Step 4: Re-run the full headless probe suite**

Run: `BUILD_DIR=build bash native/tools/run-headless-probes.sh`
Expected: ends with `ALL HEADLESS PROBES PASSED` (includes `probe_ps3update` → `PS3UPDATE-OK`).

- [ ] **Step 5: Commit**

```bash
git add native/src/core/EmulatorManager.cpp native/src/core/EmulatorManager.h native/CMakeLists.txt
git commit -m "feat: run PS3 auto-update before RPCS3 boot (background thread, nav-overlay note, boot never blocked)"
```

---

## Self-Review

**Spec coverage:**
- Client-side, auto-on-launch → Task 8 hook. ✅
- Install all packages in order → Task 4 `installAll` + Task 2 ascending sort. ✅
- Sony fetch cert-off, empty=no-update → Task 8 seam + Task 2/Task 6 empty handling. ✅
- Title ID from PARAM.SFO (folder) + PKG content_id; ISO deferred → Task 1 + Task 5, unknown→nullopt. ✅
- Download → SHA-1 verify → `--installpkg`, abort-on-corrupt → Task 4. ✅
- Idempotency state → Task 3. ✅
- Never block a boot → Task 6 fallthrough + Task 8 wiring. ✅
- Setting default-on in both builders → Task 7. ✅
- Probe registered in three places → Task 1 Step 2. ✅
- Out of scope (DLC, firmware, non-PS3, ISO, server) → nothing added. ✅

**Placeholder scan:** Task 7 references "the settings mechanism found in Step 1" and Task 8 says "reuse the transient-note mechanism the launch flow already uses" — these are genuine codebase-discovery steps, not code placeholders, because the exact ROMs-folder settings idiom and the nav-overlay note API must be read from the current tree rather than guessed. Every code step for the six pure units contains complete code.

**Type consistency:** `Ps3UpdatePackage` fields (`version/size/sha1/url/ps3SystemVer`) are consistent across Tasks 2, 4, 6. `titleIdFromSfo`, `parseVerXml`, `needsUpdate/markInstalled`, `installAll`, `read/titleIdFromPkgHeader`, `maybeUpdate` signatures match between their producing task and every consumer. The version comparator is defined once (`Ps3Version::less`, Task 3) and reused by the feed sort and the state.

**Note for the implementer of Tasks 7-8:** these two tasks require reading the current `MainWindow.cpp` settings builders and the launch/nav-overlay code before writing — they are integration glue whose exact idiom lives in the tree, not in this plan. The six units (Tasks 1-6) are fully specified and independently testable via `probe_ps3update`.
