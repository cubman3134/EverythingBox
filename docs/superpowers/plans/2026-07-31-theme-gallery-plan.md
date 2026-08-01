# In-app theme gallery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `RegistryBrowser`'s dead `Themes` kind install a themes2 theme folder from the community registry, and reach it from both the classic and themed Appearance surfaces.

**Architecture:** A new pure `ThemeRegistry` unit (`native/src/core`, QtCore only) owns everything dangerous or non-obvious — index parsing, the `dir` entry shape, remote-path validation, the GitHub Trees API URL, and the atomic write into `themes2/<Name>/`. Both surfaces consume it, so they cannot disagree, and a headless probe can test it because none of it touches the network or a window. The two surfaces keep their own network fetch loops, matching how add-ons already work.

**Tech Stack:** C++17, Qt 6 (Core / Network / Widgets / Quick), CMake, bash probe suite, Python 3 for the existing drift gate.

## Global Constraints

- **No AI attribution in commits or PR bodies.** No `Co-Authored-By: Claude` trailer, no "Generated with Claude Code" line, no tool name in the message body (repo root `CLAUDE.md`).
- **Conventional commit prefixes** (`feat:`, `fix:`, `docs:`, `refactor:`) per `CONTRIBUTING.md`.
- **Never add `QDialog`, `QMessageBox`, `QInputDialog` or any top-level window to a navigable surface.** `RegistryBrowser` is an existing `QDialog` and is only ever hosted inline via `showDialogPanel` / `showDialogPage`, both of which set `Qt::Widget`.
- **`openAppearance()` has two builders — add to both.** A user-facing setting added to only one is unreachable on the other surface.
- **A new pure component gets a probe, registered in all three places:** `native/CMakeLists.txt`, the runner loop in `native/tools/run-headless-probes.sh`, and the `--target` list in `.github/workflows/ci.yml`. Missing one means the probe silently never runs.
- **The probe suite is offline** — no network, no keys. Nothing in a probe may make an HTTP request.
- **Build command:** `cmake --build build --config Release --target everythingbox` and `--target probe_themereg`. `--config Release` matters: the Windows generator is multi-config and a Debug-built probe in a Release tree reads as "not built".
- **Probe suite:** `BUILD_DIR=build bash native/tools/run-headless-probes.sh`.
- **Spec:** `docs/superpowers/specs/2026-07-31-theme-gallery-design.md`. **Related issue:** #131 (registry still serves the pre-#57 themes) — do not attempt to fix it here; it is a different repository.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `native/src/core/ThemeRegistry.h` (create) | The registry contract: `Entry`, `parseIndex`, `isSafeRelPath`, `treeApiUrl`, `filesUnder`, `installFiles`, the caps. |
| `native/src/core/ThemeRegistry.cpp` (create) | Its implementation. QtCore only — no Widgets, no Quick, no Network. |
| `native/tools/probe_themereg.cpp` (create) | Headless probe; prints `THEMEREG-OK`. |
| `native/CMakeLists.txt` (modify) | `ThemeRegistry.cpp` into the app target; `add_executable(probe_themereg …)`. |
| `native/tools/run-headless-probes.sh` (modify) | Probe in the runner loop; the new reachability gate. |
| `.github/workflows/ci.yml` (modify) | `probe_themereg` in the build-probes `--target` list. |
| `native/src/ui/RegistryBrowser.{h,cpp}` (modify) | The `Themes` path: `themes2` index key, `dir` entries, tree fetch, install via `ThemeRegistry`. |
| `native/src/ui/MainWindow.cpp` (modify) | `presentThemeRegistry()`, the `appr.browse` row, the classic button, reframed copy. |

Task order is bottom-up: the pure unit and its probe first (Tasks 1–2), then the classic surface it unblocks (Tasks 3–4), then the themed twin (Task 5), then the gate that pins both (Task 6).

---

### Task 1: `ThemeRegistry` — entry shape and path validation

The pure core. Parses a registry index into entries and decides whether a remote path may become a filename. No network, no UI.

**Files:**
- Create: `native/src/core/ThemeRegistry.h`
- Create: `native/src/core/ThemeRegistry.cpp`
- Create: `native/tools/probe_themereg.cpp`
- Modify: `native/CMakeLists.txt`
- Modify: `native/tools/run-headless-probes.sh`
- Modify: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: nothing (this is the base).
- Produces: `ThemeRegistry::Entry` (fields `name`, `author`, `description`, `dir`, `formFactors`; method `QString folder() const`); `QVector<Entry> ThemeRegistry::parseIndex(const QByteArray&)`; `bool ThemeRegistry::isSafeRelPath(const QString&)`. Tasks 2–5 all depend on these exact names.

- [ ] **Step 1: Write the failing probe**

Create `native/tools/probe_themereg.cpp`:

```cpp
// Headless check of ThemeRegistry (src/core/ThemeRegistry) — the pure core of the in-app theme gallery.
// Both Appearance surfaces (classic RegistryBrowser::Themes, themed presentThemeRegistry) parse the
// community registry index and validate remote paths through this one unit, so the contract is pinned here
// rather than twice in UI code that no probe can reach.
//
// The registry entry shape is `{name, author, description, dir: "themes2/<Name>"}`; the index array key is
// `themes2` (what github.com/cubman3134/everythingbox-themes actually serves) with `themes` accepted as the
// legacy spelling. Every path in a listing arrives over the network and is about to become a filename, so
// isSafeRelPath REJECTS rather than sanitises — a rewritten path is a guess about intent.
//
// Prints THEMEREG-OK on success; any failure prints THEMEREG-FAIL <cond> and exits non-zero.
#include "ThemeRegistry.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "THEMEREG-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // 1. The live index shape: a "themes2" array of dir-entries.
    {
        const QByteArray json = R"({"themes2":[
            {"name":"Grid","author":"EverythingBox","description":"A dense poster grid.","dir":"themes2/Grid"},
            {"name":"Night","author":"EverythingBox","description":"A dark desktop library.",
             "dir":"themes2/Night","formFactors":["desktop"]}]})";
        const QVector<ThemeRegistry::Entry> es = ThemeRegistry::parseIndex(json);
        CHECK(es.size() == 2);
        CHECK(es.value(0).name == QStringLiteral("Grid"));
        CHECK(es.value(0).author == QStringLiteral("EverythingBox"));
        CHECK(es.value(0).dir == QStringLiteral("themes2/Grid"));
        CHECK(es.value(0).folder() == QStringLiteral("Grid"));
        CHECK(es.value(0).formFactors.isEmpty());
        CHECK(es.value(1).folder() == QStringLiteral("Night"));
        CHECK(es.value(1).formFactors == QStringList{ QStringLiteral("desktop") });
    }

    // 2. The legacy "themes" key still parses — one line of compatibility, and the key the pre-existing
    //    RegistryBrowser code assumed. "themes2" WINS when both are present.
    {
        const QByteArray legacy = R"({"themes":[{"name":"Old","dir":"themes2/Old"}]})";
        CHECK(ThemeRegistry::parseIndex(legacy).size() == 1);
        CHECK(ThemeRegistry::parseIndex(legacy).value(0).folder() == QStringLiteral("Old"));

        const QByteArray both = R"({"themes2":[{"name":"New","dir":"themes2/New"}],
                                     "themes":[{"name":"Old","dir":"themes2/Old"}]})";
        const QVector<ThemeRegistry::Entry> es = ThemeRegistry::parseIndex(both);
        CHECK(es.size() == 1);
        CHECK(es.value(0).folder() == QStringLiteral("New"));
    }

    // 3. Junk in, nothing out — a malformed index must never yield a half-entry that later becomes a path.
    {
        CHECK(ThemeRegistry::parseIndex(QByteArray("not json at all")).isEmpty());
        CHECK(ThemeRegistry::parseIndex(QByteArray("{}")).isEmpty());
        CHECK(ThemeRegistry::parseIndex(QByteArray(R"({"themes2":"nope"})")).isEmpty());
        // An entry with no dir has nothing to install and is dropped, not kept with an empty folder.
        CHECK(ThemeRegistry::parseIndex(QByteArray(R"({"themes2":[{"name":"NoDir"}]})")).isEmpty());
        // The legacy flat colour-theme shape ("file"/"assets", no "dir") is NOT a themes2 entry.
        CHECK(ThemeRegistry::parseIndex(
                  QByteArray(R"({"themes2":[{"name":"Flat","file":"flat.json","assets":["a.png"]}]})")).isEmpty());
    }

    // 4. A traversing or absolute dir is dropped at parse time — folder() is the ONLY thing that ever becomes
    //    a directory name, so it must be a single plain segment or nothing.
    {
        const char* bad[] = { R"({"themes2":[{"name":"X","dir":"themes2/../../etc"}]})",
                              R"({"themes2":[{"name":"X","dir":"/abs/Grid"}]})",
                              R"({"themes2":[{"name":"X","dir":"C:/Windows/Grid"}]})",
                              R"({"themes2":[{"name":"X","dir":"themes2\\Grid"}]})",
                              R"({"themes2":[{"name":"X","dir":".."}]})",
                              R"({"themes2":[{"name":"X","dir":"themes2/"}]})" };
        for (const char* b : bad) CHECK(ThemeRegistry::parseIndex(QByteArray(b)).isEmpty());
    }

    // 5. isSafeRelPath — the gate every listed file passes before it becomes a filename.
    {
        CHECK(ThemeRegistry::isSafeRelPath(QStringLiteral("theme.json")));
        CHECK(ThemeRegistry::isSafeRelPath(QStringLiteral("sounds/move.wav")));
        CHECK(ThemeRegistry::isSafeRelPath(QStringLiteral("fonts/VarelaRound-Regular.ttf")));
        CHECK(ThemeRegistry::isSafeRelPath(QStringLiteral("a/b/c/d.png")));

        CHECK(!ThemeRegistry::isSafeRelPath(QString()));                                  // empty
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("../theme.json")));            // traversal
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("sounds/../../theme.json")));  // buried traversal
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("/etc/passwd")));              // absolute
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("C:/Windows/x.dll")));         // drive letter
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("sounds\\move.wav")));         // backslash
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("sounds//move.wav")));         // empty segment
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("sounds/")));                  // trailing slash
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral(".")));                        // dot segment
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("a/./b.png")));                // buried dot
        // Windows reserved device names, at any depth and with any extension. On Windows these do not name
        // files at all; writing one opens a DEVICE, and the failure is baffling rather than loud.
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("CON")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("con.wav")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("sounds/NUL.wav")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("COM1")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("lpt9.txt")));
    }

    if (failures == 0) std::printf("THEMEREG-OK\n");
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register the probe in all three places**

In `native/CMakeLists.txt`, next to the other pure-core probes (near the `probe_sync` block around line 359):

```cmake
    add_executable(probe_themereg tools/probe_themereg.cpp
        src/core/ThemeRegistry.cpp)
    target_include_directories(probe_themereg PRIVATE src src/core)
    target_link_libraries(probe_themereg PRIVATE Qt6::Core)
```

In `native/tools/run-headless-probes.sh`, append `"probe_themereg THEMEREG-OK"` to the `for p in …` loop at line 207 (immediately after `"probe_crashreport CRASHREPORT-OK"`, before the closing `; do`).

In `.github/workflows/ci.yml` line 52, append ` probe_themereg` to the end of the `cmake --build build --target …` list.

- [ ] **Step 3: Run the probe build to verify it fails**

```bash
cmake --build build --config Release --target probe_themereg
```

Expected: FAIL — `Cannot open include file: 'ThemeRegistry.h'`, and CMake errors that `src/core/ThemeRegistry.cpp` does not exist.

- [ ] **Step 4: Write the header**

Create `native/src/core/ThemeRegistry.h`:

```cpp
// ThemeRegistry — the pure core of the in-app theme gallery: what a community registry index means, and
// what may be written to disk because of one.
//
// A themes2 theme is a FOLDER (theme.json plus optional sounds/ and fonts/), and a registry entry names
// that folder rather than listing files: {name, author, description, dir: "themes2/<Name>"}. The dead
// RegistryBrowser::Themes path this replaces read a "file"/"assets" pair that no live entry has ever had.
//
// Everything here is QtCore-only and network-free so probe_themereg can pin it headlessly, and so BOTH
// Appearance builders share one copy of the parts that are easy to get subtly wrong: which index key to
// read, which paths may become filenames, and how a folder lands on disk without a half-install surviving.
#pragma once
#include <QByteArray>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ThemeRegistry {

// Refuse an entry whose listing is implausible for a theme. A registry is public and anyone may open a
// pull request against one; these are the bounds beyond which we stop rather than download.
constexpr int    kMaxFiles     = 64;
constexpr qint64 kMaxFileBytes = 8 * 1024 * 1024;

struct Entry {
    QString     name;          // display text ONLY — never used as a path
    QString     author;
    QString     description;
    QString     dir;           // "themes2/<Name>", relative to the index URL's directory
    QStringList formFactors;   // advisory note on the row; does not filter

    // The install folder: the last segment of `dir`. Empty when `dir` is unusable, which is the single
    // predicate callers check — parseIndex already drops those, so an Entry in hand always has one.
    QString folder() const;
};

// Parse a registry index. Reads "themes2" (what the registry serves) and falls back to "themes" (the key
// the pre-existing code assumed). Entries without a usable `dir` are DROPPED, so every returned Entry has
// a non-empty folder().
QVector<Entry> parseIndex(const QByteArray& json);

// May this relative path become a filename? Accepts only a relative path whose every segment is plain:
// no "." or ".." segment, no leading "/", no drive letter, no backslash, no empty segment, no Windows
// reserved device name. Rejects rather than sanitises: rewriting a hostile path guesses at intent, and no
// theme has a benign reason to ship one.
bool isSafeRelPath(const QString& rel);

} // namespace ThemeRegistry
```

- [ ] **Step 5: Write the implementation**

Create `native/src/core/ThemeRegistry.cpp`:

```cpp
#include "ThemeRegistry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace {

// Windows opens a DEVICE for these names at any extension, so a file called "con.wav" is not a file. They
// are rejected on every platform: a theme that installs on Linux and detonates on Windows is worse than
// one that is refused everywhere, and the registry is shared across both.
bool isReservedDeviceName(const QString& segment)
{
    static const QSet<QString> kReserved = {
        QStringLiteral("con"), QStringLiteral("prn"), QStringLiteral("aux"), QStringLiteral("nul"),
        QStringLiteral("com1"), QStringLiteral("com2"), QStringLiteral("com3"), QStringLiteral("com4"),
        QStringLiteral("com5"), QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
        QStringLiteral("com9"),
        QStringLiteral("lpt1"), QStringLiteral("lpt2"), QStringLiteral("lpt3"), QStringLiteral("lpt4"),
        QStringLiteral("lpt5"), QStringLiteral("lpt6"), QStringLiteral("lpt7"), QStringLiteral("lpt8"),
        QStringLiteral("lpt9") };
    const int dot = segment.indexOf(QLatin1Char('.'));
    const QString stem = (dot < 0 ? segment : segment.left(dot)).toLower();
    return kReserved.contains(stem);
}

// One path segment is plain: non-empty, not a dot-segment, no separator or drive-letter character, and not
// a reserved device name.
bool isPlainSegment(const QString& s)
{
    if (s.isEmpty()) return false;
    if (s == QLatin1String(".") || s == QLatin1String("..")) return false;
    if (s.contains(QLatin1Char('/')) || s.contains(QLatin1Char('\\')) || s.contains(QLatin1Char(':')))
        return false;
    if (isReservedDeviceName(s)) return false;
    return true;
}

} // namespace

namespace ThemeRegistry {

bool isSafeRelPath(const QString& rel)
{
    if (rel.isEmpty()) return false;
    if (rel.contains(QLatin1Char('\\'))) return false;      // no backslash anywhere, on any platform
    if (rel.startsWith(QLatin1Char('/'))) return false;     // absolute
    // A drive letter makes it absolute on Windows and is never valid in a registry path.
    if (rel.contains(QLatin1Char(':'))) return false;
    // split with KeepEmptyParts so "a//b" and "a/" are caught as empty segments rather than silently
    // collapsing into a valid-looking path.
    const QStringList parts = rel.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString& p : parts)
        if (!isPlainSegment(p)) return false;
    return true;
}

QString Entry::folder() const
{
    if (dir.isEmpty()) return QString();
    if (!isSafeRelPath(dir)) return QString();              // covers "..", absolute, drive letter, trailing /
    const int slash = dir.lastIndexOf(QLatin1Char('/'));
    const QString last = slash < 0 ? dir : dir.mid(slash + 1);
    return isPlainSegment(last) ? last : QString();
}

QVector<Entry> parseIndex(const QByteArray& json)
{
    QVector<Entry> out;
    const QJsonObject root = QJsonDocument::fromJson(json).object();

    // "themes2" is what the registry serves; "themes" is the legacy spelling. themes2 wins outright when
    // both are present rather than merging — two keys describing the same registry is a mistake, and
    // silently concatenating them would install from whichever the author forgot to delete.
    QJsonArray arr = root.value(QStringLiteral("themes2")).toArray();
    if (arr.isEmpty()) arr = root.value(QStringLiteral("themes")).toArray();

    for (const QJsonValue& v : arr)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        Entry e;
        e.name        = o.value(QStringLiteral("name")).toString();
        e.author      = o.value(QStringLiteral("author")).toString();
        e.description = o.value(QStringLiteral("description")).toString();
        e.dir         = o.value(QStringLiteral("dir")).toString();
        for (const QJsonValue& f : o.value(QStringLiteral("formFactors")).toArray())
            if (!f.toString().isEmpty()) e.formFactors << f.toString();

        // Drop anything without a usable folder, so no caller ever holds an Entry it cannot install. This
        // is also what rejects the legacy flat "file"/"assets" shape: it has no dir.
        if (e.folder().isEmpty()) continue;
        out << e;
    }
    return out;
}

} // namespace ThemeRegistry
```

- [ ] **Step 6: Build and run the probe**

```bash
cmake --build build --config Release --target probe_themereg
```

Then run it directly:

```bash
./build/Release/probe_themereg.exe
```

Expected: prints `THEMEREG-OK`, exit code 0. Any `THEMEREG-FAIL <cond> (line N)` line names the assertion to fix.

- [ ] **Step 7: Confirm the probe is actually wired into the suite**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh 2>&1 | grep -A2 "=== probe_themereg ==="
```

Expected: `PASS: probe_themereg`. If you see `(skip) probe_themereg not built`, the CMake registration is wrong; if the section is absent entirely, the runner-loop registration is wrong. A probe registered in fewer than three places gates nothing — that is the `probe_addon` failure `CONTRIBUTING.md` records.

- [ ] **Step 8: Commit**

```bash
git add native/src/core/ThemeRegistry.h native/src/core/ThemeRegistry.cpp native/tools/probe_themereg.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: ThemeRegistry — the themes2 registry entry shape and remote-path validation"
```

---

### Task 2: `ThemeRegistry` — tree listing and atomic install

The other half of the pure unit: turn a GitHub Trees API response into a validated file list, and write a downloaded folder into place without leaving a half-theme behind.

**Files:**
- Modify: `native/src/core/ThemeRegistry.h`
- Modify: `native/src/core/ThemeRegistry.cpp`
- Modify: `native/tools/probe_themereg.cpp`

**Interfaces:**
- Consumes: `ThemeRegistry::Entry`, `isSafeRelPath`, `kMaxFiles`, `kMaxFileBytes` from Task 1.
- Produces: `QString ThemeRegistry::treeApiUrl(const QString& indexUrl)`; `struct Listing { QStringList files; QString error; bool ok() const; }`; `Listing ThemeRegistry::filesUnder(const QByteArray& treeJson, const QString& dir)`; `bool ThemeRegistry::installFiles(const QString& themesRoot, const QString& folder, const QVector<QPair<QString, QByteArray>>& files, QString* error)`. Tasks 3 and 5 call all four.

- [ ] **Step 1: Write the failing probe additions**

In `native/tools/probe_themereg.cpp`, add these blocks immediately before the final `if (failures == 0)` line. Add `#include <QDir>` and `#include <QFile>` to the includes at the top.

```cpp
    // 6. treeApiUrl — a raw index URL becomes the Trees API URL for the same repo and branch. Anything that
    //    is not raw.githubusercontent.com yields "", which is how a user-added registry on another host is
    //    told apart from a GitHub one (it lists, but cannot be installed from in-app).
    {
        CHECK(ThemeRegistry::treeApiUrl(
                  QStringLiteral("https://raw.githubusercontent.com/cubman3134/everythingbox-themes/main/index.json"))
              == QStringLiteral("https://api.github.com/repos/cubman3134/everythingbox-themes/git/trees/main?recursive=1"));
        // A non-default branch survives.
        CHECK(ThemeRegistry::treeApiUrl(
                  QStringLiteral("https://raw.githubusercontent.com/o/r/dev/index.json"))
              == QStringLiteral("https://api.github.com/repos/o/r/git/trees/dev?recursive=1"));
        CHECK(ThemeRegistry::treeApiUrl(QStringLiteral("https://example.com/themes/index.json")).isEmpty());
        CHECK(ThemeRegistry::treeApiUrl(QStringLiteral("https://github.com/o/r/blob/main/index.json")).isEmpty());
        CHECK(ThemeRegistry::treeApiUrl(QString()).isEmpty());
        // Too few path segments to name a repo and a branch.
        CHECK(ThemeRegistry::treeApiUrl(QStringLiteral("https://raw.githubusercontent.com/o/index.json")).isEmpty());
    }

    // 7. filesUnder — keep the blobs under dir/, return them RELATIVE to dir, ignore everything else.
    {
        const QByteArray tree = R"({"truncated":false,"tree":[
            {"path":"README.md","type":"blob","size":10},
            {"path":"index.json","type":"blob","size":20},
            {"path":"themes2","type":"tree"},
            {"path":"themes2/Channels","type":"tree"},
            {"path":"themes2/Channels/theme.json","type":"blob","size":4898},
            {"path":"themes2/Channels/sounds","type":"tree"},
            {"path":"themes2/Channels/sounds/move.wav","type":"blob","size":6658},
            {"path":"themes2/Channels/fonts/VarelaRound-Regular.ttf","type":"blob","size":132748},
            {"path":"themes2/Night/theme.json","type":"blob","size":11136}]})";
        const ThemeRegistry::Listing l = ThemeRegistry::filesUnder(tree, QStringLiteral("themes2/Channels"));
        CHECK(l.ok());
        CHECK(l.error.isEmpty());
        QStringList got = l.files; got.sort();
        const QStringList want = { QStringLiteral("fonts/VarelaRound-Regular.ttf"),
                                   QStringLiteral("sounds/move.wav"),
                                   QStringLiteral("theme.json") };
        CHECK(got == want);
        // A sibling folder whose name merely PREFIXES this one must not bleed in.
        const ThemeRegistry::Listing n = ThemeRegistry::filesUnder(tree, QStringLiteral("themes2/Night"));
        CHECK(n.ok());
        CHECK(n.files == QStringList{ QStringLiteral("theme.json") });
    }

    // 8. Every refusal is a REASON, never an empty success. An empty file list that reads as "installed"
    //    is the exact failure the dead Themes path had ("Nothing to download for this entry.").
    {
        // truncated: the listing is incomplete, so a "complete" install would silently omit files.
        const QByteArray trunc = R"({"truncated":true,"tree":[
            {"path":"themes2/Grid/theme.json","type":"blob","size":10}]})";
        CHECK(!ThemeRegistry::filesUnder(trunc, QStringLiteral("themes2/Grid")).ok());

        // No theme.json at the folder root: not a theme.
        const QByteArray notheme = R"({"tree":[
            {"path":"themes2/Grid/sounds/move.wav","type":"blob","size":10}]})";
        CHECK(!ThemeRegistry::filesUnder(notheme, QStringLiteral("themes2/Grid")).ok());

        // A nested theme.json does not count as the folder's own.
        const QByteArray nested = R"({"tree":[
            {"path":"themes2/Grid/sub/theme.json","type":"blob","size":10}]})";
        CHECK(!ThemeRegistry::filesUnder(nested, QStringLiteral("themes2/Grid")).ok());

        // Folder absent from the tree entirely.
        const QByteArray missing = R"({"tree":[{"path":"themes2/Other/theme.json","type":"blob","size":10}]})";
        CHECK(!ThemeRegistry::filesUnder(missing, QStringLiteral("themes2/Grid")).ok());

        // An oversized file fails the WHOLE entry rather than being skipped — a theme missing its font is
        // a broken theme, and skipping quietly would install one.
        const QByteArray big = R"({"tree":[
            {"path":"themes2/Grid/theme.json","type":"blob","size":10},
            {"path":"themes2/Grid/huge.bin","type":"blob","size":9000000}]})";
        CHECK(!ThemeRegistry::filesUnder(big, QStringLiteral("themes2/Grid")).ok());

        // A traversing path anywhere in the folder fails the whole entry.
        const QByteArray evil = R"({"tree":[
            {"path":"themes2/Grid/theme.json","type":"blob","size":10},
            {"path":"themes2/Grid/../../../etc/passwd","type":"blob","size":10}]})";
        CHECK(!ThemeRegistry::filesUnder(evil, QStringLiteral("themes2/Grid")).ok());

        CHECK(!ThemeRegistry::filesUnder(QByteArray("not json"), QStringLiteral("themes2/Grid")).ok());
        // Never installable through a listing: an unusable dir.
        CHECK(!ThemeRegistry::filesUnder(QByteArray(R"({"tree":[]})"), QString()).ok());
    }

    // 9. The file cap. kMaxFiles + 1 blobs (theme.json included) is a refusal.
    {
        QByteArray many = R"({"tree":[{"path":"themes2/Big/theme.json","type":"blob","size":10})";
        for (int i = 0; i <= ThemeRegistry::kMaxFiles; ++i)
            many += QByteArray(",{\"path\":\"themes2/Big/f") + QByteArray::number(i)
                  + QByteArray(".png\",\"type\":\"blob\",\"size\":10}");
        many += "]}";
        CHECK(!ThemeRegistry::filesUnder(many, QStringLiteral("themes2/Big")).ok());
    }

    // 10. installFiles — the folder lands complete, WITH its subdirectories. The flattening both existing
    //     installers do (destDir + "/" + QFileInfo(rel).fileName()) would put sounds/move.wav at the theme
    //     root and leave every sound reference in the theme dangling.
    {
        const QString root = QDir::tempPath() + QStringLiteral("/eb-themereg-probe");
        QDir(root).removeRecursively();

        QVector<QPair<QString, QByteArray>> files;
        files << qMakePair(QStringLiteral("theme.json"), QByteArray("{\"name\":\"Probe\"}"));
        files << qMakePair(QStringLiteral("sounds/move.wav"), QByteArray("RIFFwave"));
        files << qMakePair(QStringLiteral("fonts/F.ttf"), QByteArray("ttf"));

        QString err;
        CHECK(ThemeRegistry::installFiles(root, QStringLiteral("Probe"), files, &err));
        CHECK(err.isEmpty());
        CHECK(QFile::exists(root + QStringLiteral("/Probe/theme.json")));
        CHECK(QFile::exists(root + QStringLiteral("/Probe/sounds/move.wav")));   // subpath PRESERVED
        CHECK(QFile::exists(root + QStringLiteral("/Probe/fonts/F.ttf")));
        CHECK(!QFile::exists(root + QStringLiteral("/Probe/move.wav")));         // NOT flattened

        // Re-installing the same folder replaces it wholesale rather than merging: a theme that dropped a
        // file must not keep the old one lying around.
        QVector<QPair<QString, QByteArray>> fewer;
        fewer << qMakePair(QStringLiteral("theme.json"), QByteArray("{\"name\":\"Probe2\"}"));
        CHECK(ThemeRegistry::installFiles(root, QStringLiteral("Probe"), fewer, &err));
        CHECK(QFile::exists(root + QStringLiteral("/Probe/theme.json")));
        CHECK(!QFile::exists(root + QStringLiteral("/Probe/sounds/move.wav")));

        // A refusal leaves NOTHING behind — not a partial folder, and not a damaged previous install. This
        // is what makes a failed install safe: availableThemes() picks up anything with a theme.json.
        QVector<QPair<QString, QByteArray>> bad;
        bad << qMakePair(QStringLiteral("theme.json"), QByteArray("{}"));
        bad << qMakePair(QStringLiteral("../escape.txt"), QByteArray("nope"));
        err.clear();
        CHECK(!ThemeRegistry::installFiles(root, QStringLiteral("Probe3"), bad, &err));
        CHECK(!err.isEmpty());
        CHECK(!QDir(root + QStringLiteral("/Probe3")).exists());
        CHECK(!QFile::exists(root + QStringLiteral("/escape.txt")));

        // A refused install must not have touched the theme that was already there.
        CHECK(ThemeRegistry::installFiles(root, QStringLiteral("Probe"), bad, &err) == false);
        CHECK(QFile::exists(root + QStringLiteral("/Probe/theme.json")));

        // An empty file set is a refusal, not an empty "success".
        CHECK(!ThemeRegistry::installFiles(root, QStringLiteral("Probe4"), {}, &err));
        // A folder name that is not a plain segment never becomes a directory.
        CHECK(!ThemeRegistry::installFiles(root, QStringLiteral("../evil"), files, &err));
        CHECK(!ThemeRegistry::installFiles(root, QString(), files, &err));

        QDir(root).removeRecursively();
    }
```

- [ ] **Step 2: Run the probe build to verify it fails**

```bash
cmake --build build --config Release --target probe_themereg
```

Expected: FAIL — `'treeApiUrl': is not a member of 'ThemeRegistry'`, plus the same for `Listing`, `filesUnder` and `installFiles`.

- [ ] **Step 3: Extend the header**

In `native/src/core/ThemeRegistry.h`, add to the `ThemeRegistry` namespace, after `isSafeRelPath`:

```cpp
// raw.githubusercontent.com/<owner>/<repo>/<branch>/index.json
//   -> api.github.com/repos/<owner>/<repo>/git/trees/<branch>?recursive=1
// Empty for any other host or a URL too short to name a repo and branch. An entry names a DIRECTORY, not a
// file list, so this is how the installer learns what is in one — from the repository itself, which is the
// only source that cannot drift from it (a `files: []` array in index.json would be a second copy of the
// same truth, maintained by hand, which is what issue #57 was about).
QString treeApiUrl(const QString& indexUrl);

// The outcome of reading a Trees API response. Either a usable file list or a user-facing reason there
// isn't one — never an empty list that reads as success.
struct Listing {
    QStringList files;   // paths RELATIVE to dir
    QString     error;   // non-empty => not installable, and this is what the row shows
    bool ok() const { return error.isEmpty(); }
};

// Filter a Trees API response to the blobs under `dir/`. Refuses (with a reason) when the response is
// truncated, `dir` holds no theme.json of its own, the folder is absent, any path is unsafe, there are
// more than kMaxFiles files, or any file exceeds kMaxFileBytes. One bad file fails the WHOLE entry: a
// theme installed without its font is a broken theme, and skipping quietly would produce one.
Listing filesUnder(const QByteArray& treeJson, const QString& dir);

// Write a downloaded theme folder into `themesRoot/folder`, replacing any existing folder of that name.
// Writes into a temp sibling and renames into place, so an interrupted or refused install never leaves a
// partial folder — ThemeEngine::availableThemes() offers anything holding a theme.json, and a theme with
// missing sounds and fonts would be selectable. Refuses an empty file set, an unsafe folder name, and any
// unsafe relative path, without having touched the existing install. Subdirectories are PRESERVED.
bool installFiles(const QString& themesRoot, const QString& folder,
                  const QVector<QPair<QString, QByteArray>>& files, QString* error);
```

- [ ] **Step 4: Extend the implementation**

In `native/src/core/ThemeRegistry.cpp`, add `#include <QDir>`, `#include <QFile>`, `#include <QFileInfo>` and `#include <QUrl>` to the includes, then add to the `ThemeRegistry` namespace:

```cpp
QString treeApiUrl(const QString& indexUrl)
{
    const QUrl u(indexUrl);
    if (u.host() != QLatin1String("raw.githubusercontent.com")) return QString();
    // /<owner>/<repo>/<branch>/<path...>  — four segments minimum.
    const QStringList p = u.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (p.size() < 4) return QString();
    return QStringLiteral("https://api.github.com/repos/%1/%2/git/trees/%3?recursive=1")
        .arg(p[0], p[1], p[2]);
}

Listing filesUnder(const QByteArray& treeJson, const QString& dir)
{
    Listing out;
    if (dir.isEmpty() || !isSafeRelPath(dir))
    { out.error = QStringLiteral("This entry does not name a usable folder."); return out; }

    const QJsonDocument doc = QJsonDocument::fromJson(treeJson);
    if (!doc.isObject())
    { out.error = QStringLiteral("The registry's file listing could not be read."); return out; }
    const QJsonObject root = doc.object();

    // A truncated tree is an INCOMPLETE listing. Installing from one would silently omit files and produce
    // a theme that looks installed and is not, which is worse than refusing.
    if (root.value(QStringLiteral("truncated")).toBool())
    { out.error = QStringLiteral("This registry is too large to list; install this theme by hand."); return out; }

    const QString prefix = dir + QLatin1Char('/');
    QStringList files;
    bool hasThemeJson = false;

    for (const QJsonValue& v : root.value(QStringLiteral("tree")).toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("type")).toString() != QLatin1String("blob")) continue;
        const QString path = o.value(QStringLiteral("path")).toString();
        if (!path.startsWith(prefix)) continue;             // prefix includes the '/', so a sibling whose
                                                            // name merely starts with dir cannot bleed in
        const QString rel = path.mid(prefix.size());
        if (!isSafeRelPath(rel))
        {
            out.files.clear();
            out.error = QStringLiteral("This theme lists an unsafe file path and will not be installed.");
            return out;
        }
        if (o.value(QStringLiteral("size")).toDouble() > double(kMaxFileBytes))
        {
            out.files.clear();
            out.error = QStringLiteral("This theme contains a file larger than 8 MB.");
            return out;
        }
        if (rel == QLatin1String("theme.json")) hasThemeJson = true;
        files << rel;
    }

    if (files.isEmpty())
    { out.error = QStringLiteral("This theme's folder is empty or missing from the registry."); return out; }
    if (!hasThemeJson)
    { out.error = QStringLiteral("This folder has no theme.json, so it is not a theme."); return out; }
    if (files.size() > kMaxFiles)
    { out.error = QStringLiteral("This theme contains more than %1 files.").arg(kMaxFiles); return out; }

    out.files = files;
    return out;
}

bool installFiles(const QString& themesRoot, const QString& folder,
                  const QVector<QPair<QString, QByteArray>>& files, QString* error)
{
    auto fail = [error](const QString& msg) { if (error) *error = msg; return false; };

    if (folder.isEmpty() || !isPlainSegment(folder))
        return fail(QStringLiteral("Unusable theme folder name."));
    if (files.isEmpty())
        return fail(QStringLiteral("Nothing was downloaded for this theme."));

    // VALIDATE EVERYTHING BEFORE WRITING ANYTHING. filesUnder has already checked these, but installFiles
    // is the function that turns a string into a filename and it does not get to assume its caller.
    bool hasThemeJson = false;
    for (const auto& f : files)
    {
        if (!isSafeRelPath(f.first)) return fail(QStringLiteral("Unsafe file path: %1").arg(f.first));
        if (f.first == QLatin1String("theme.json")) hasThemeJson = true;
    }
    if (!hasThemeJson) return fail(QStringLiteral("The download has no theme.json."));

    const QString dest = themesRoot + QLatin1Char('/') + folder;
    const QString tmp  = dest + QStringLiteral(".installing");

    QDir().mkpath(themesRoot);
    QDir(tmp).removeRecursively();                 // a previous run that died mid-install
    if (!QDir().mkpath(tmp)) return fail(QStringLiteral("Could not create %1").arg(tmp));

    for (const auto& f : files)
    {
        const QString target = tmp + QLatin1Char('/') + f.first;
        if (!QDir().mkpath(QFileInfo(target).absolutePath()))
        { QDir(tmp).removeRecursively(); return fail(QStringLiteral("Could not create a folder for %1").arg(f.first)); }
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        { QDir(tmp).removeRecursively(); return fail(QStringLiteral("Could not write %1").arg(f.first)); }
        if (out.write(f.second) != qint64(f.second.size()))
        { out.close(); QDir(tmp).removeRecursively(); return fail(QStringLiteral("Could not write %1").arg(f.first)); }
        out.close();
    }

    // Swap in. The old folder goes to a second temp name first, so a rename that fails leaves the previous
    // theme intact rather than deleting it and then failing to put the new one there.
    const QString old = dest + QStringLiteral(".replacing");
    QDir(old).removeRecursively();
    const bool hadOld = QDir(dest).exists();
    if (hadOld && !QDir().rename(dest, old))
    { QDir(tmp).removeRecursively(); return fail(QStringLiteral("Could not replace the existing %1.").arg(folder)); }
    if (!QDir().rename(tmp, dest))
    {
        if (hadOld) QDir().rename(old, dest);       // put it back
        QDir(tmp).removeRecursively();
        return fail(QStringLiteral("Could not install into %1.").arg(dest));
    }
    QDir(old).removeRecursively();
    if (error) error->clear();
    return true;
}
```

`isPlainSegment` lives in the anonymous namespace from Task 1 and is already visible here.

- [ ] **Step 5: Build and run the probe**

```bash
cmake --build build --config Release --target probe_themereg
```

```bash
./build/Release/probe_themereg.exe
```

Expected: `THEMEREG-OK`, exit 0.

- [ ] **Step 6: Run the full suite**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Expected: no `FAIL:` lines. The `exe-folder contamination` gate matters here specifically — `installFiles` writes to disk, and the probe must clean up after itself (it removes its temp root at the end of block 10, and every probe's `dataDir()` is isolated anyway).

- [ ] **Step 7: Commit**

```bash
git add native/src/core/ThemeRegistry.h native/src/core/ThemeRegistry.cpp native/tools/probe_themereg.cpp
git commit -m "feat: ThemeRegistry — tree listing, caps and atomic folder install"
```

---

### Task 3: Teach `RegistryBrowser` the themes2 shape

Rewire the dead `Themes` path onto `ThemeRegistry`. After this task the classic browser works; it is still unreachable, which Task 4 fixes.

**Files:**
- Modify: `native/src/ui/RegistryBrowser.h`
- Modify: `native/src/ui/RegistryBrowser.cpp`
- Modify: `native/CMakeLists.txt` (add `ThemeRegistry.cpp` to the app target)

**Interfaces:**
- Consumes: `ThemeRegistry::{Entry, parseIndex, treeApiUrl, filesUnder, installFiles, Listing}` from Tasks 1–2; `ThemeEngine::themesRoot()` from `native/src/theme2/ThemeEngine.h`.
- Produces: a working `RegistryBrowser(RegistryBrowser::Themes, nullptr, parent)`. Task 4 constructs exactly this.

- [ ] **Step 1: Add `ThemeRegistry.cpp` to the app target**

In `native/CMakeLists.txt`, find the source list containing `src/ui/RegistryBrowser.cpp src/ui/RegistryBrowser.h` (line 161) and add alongside the other `src/core` entries:

```cmake
        src/core/ThemeRegistry.cpp src/core/ThemeRegistry.h
```

- [ ] **Step 2: Declare the new members**

In `native/src/ui/RegistryBrowser.h`, add to the private section (after `bool isInstalled(const QJsonObject& entry) const;`):

```cpp
    // Themes: the entry names a FOLDER, so the file list comes from the registry repo's own tree rather
    // than from the index. Cached per registry for this dialog's lifetime — one API call per install, and
    // unauthenticated GitHub allows 60 an hour per IP.
    void installThemeEntry(const QJsonObject& entry, const QString& indexUrl);
    QByteArray treeFor(const QString& indexUrl, QString* error);
    QHash<QString, QByteArray> treeCache_;
```

and add `#include <QHash>` at the top.

- [ ] **Step 3: Point the Themes path at `themes2`**

In `native/src/ui/RegistryBrowser.cpp`:

Add the includes:

```cpp
#include "../core/ThemeRegistry.h"
#include "../theme2/ThemeEngine.h"
```

Replace `localDirFor` (lines 192–197) so the Themes branch names the directory the engine actually reads:

```cpp
QString RegistryBrowser::localDirFor(const QString& id) const
{
    // Themes install as FOLDERS under the themes2 root ThemeEngine::availableThemes() scans. The old
    // <dataDir>/themes was the legacy flat colour-theme directory and nothing reads it.
    if (kind_ == Themes) return ThemeEngine::themesRoot() + QStringLiteral("/") + id;
    return AppPaths::dataDir() + QStringLiteral("/addons/") + id;
}
```

Replace the `Themes` branch of `isInstalled` (lines 211–216) with:

```cpp
    if (kind_ == Themes)
    {
        // A folder already on disk is installed — the same predicate ThemeEngine::availableThemes() uses,
        // so the gallery cannot claim something is installed that the picker will not list. This is also
        // what keeps the bundled Channels/Night/Triple from being replaced by the registry's older copies
        // of them (issue #131): they exist, so they are never offered.
        const QString folder = ThemeRegistry::Entry{ {}, {}, {},
            entry.value(QStringLiteral("dir")).toString(), {} }.folder();
        return !folder.isEmpty() && QFile::exists(localDirFor(folder) + QStringLiteral("/theme.json"));
    }
```

Gate the remote-add-on branch on the kind, immediately below it (replacing the bare `if (isRemoteEntry(entry))`):

```cpp
    if (kind_ == Addons && isRemoteEntry(entry)) // remote addon: "installed" = its URL is already in the source list
```

In `fetchOne`, replace the array-key line (line 244) so the Themes path reads what the registry serves:

```cpp
            const QJsonArray entries = root.value(kind_ == Themes ? QStringLiteral("themes2")
                                                                  : QStringLiteral("addons")).toArray();
            // The legacy "themes" spelling, for a registry that still uses it.
            const QJsonArray fallback = (kind_ == Themes && entries.isEmpty())
                ? root.value(QStringLiteral("themes")).toArray() : QJsonArray();
            for (const QJsonValue& e : (entries.isEmpty() ? fallback : entries))
```

and delete the old `for (const QJsonValue& e : entries)` line it replaces.

In `renderEntry`, gate the remote branch the same way (line 287):

```cpp
        if (kind_ == Addons && isRemoteEntry(entry) && addons_)
```

Still in `renderEntry`, surface `formFactors` as an advisory note so the classic card carries the same
information as the themed row. Insert immediately after the `desc` label is added to `texts` (line 274):

```cpp
    // Advisory only — a theme declaring a form factor is still installable anywhere, which is how the
    // engine treats it. Shown so the card says what the theme was built for.
    QStringList ff;
    for (const QJsonValue& f : entry.value(QStringLiteral("formFactors")).toArray())
        if (!f.toString().isEmpty()) ff << f.toString();
    if (!ff.isEmpty())
    {
        auto* forLbl = new QLabel(tr("built for %1").arg(ff.join(QStringLiteral(", "))));
        forLbl->setStyleSheet(QStringLiteral("color:#999; font-size:11px;"));
        texts->addWidget(forLbl);
    }
```

- [ ] **Step 4: Route Themes installs through `ThemeRegistry`**

In `installEntry`, replace the `if (kind_ == Themes) { … }` block (lines 350–356) and let the add-on path stand:

```cpp
    if (kind_ == Themes) { installThemeEntry(entry, indexUrl); return; }

    const QString id = entry.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) { status_->setText(tr("Entry has no id.")); return; }
    destDir = localDirFor(id);
    for (const QJsonValue& fv : entry.value(QStringLiteral("files")).toArray()) files << fv.toString();
```

(the surrounding `const QString base`, `QStringList files`, `QString destDir` declarations and everything from `if (files.isEmpty())` down are unchanged).

Then add the two new methods at the end of the file, before `updateRepoLink`:

```cpp
// The registry repo's file tree, fetched once per registry per dialog. An entry names a directory, so this
// is how we learn what is in it — from the repository itself, which cannot drift from what it holds.
QByteArray RegistryBrowser::treeFor(const QString& indexUrl, QString* error)
{
    if (treeCache_.contains(indexUrl)) return treeCache_.value(indexUrl);

    const QString api = ThemeRegistry::treeApiUrl(indexUrl);
    if (api.isEmpty())
    {
        // A user-added registry may be anywhere. It still LISTS — the user can read what the theme is and
        // go install it by hand — but we cannot enumerate a folder without the API.
        if (error) *error = tr("This registry isn't hosted on GitHub, so themes can't be installed from "
                               "here. Download the folder from the registry and drop it into %1.")
                                .arg(QDir::toNativeSeparators(ThemeEngine::themesRoot()));
        return QByteArray();
    }

    QString err;
    QByteArray body;
    {
        QNetworkRequest req((QUrl(api)));
        req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
        req.setRawHeader("Accept", "application/vnd.github+json");
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = nam_->get(req);
        QEventLoop loop;
        QTimer to; to.setSingleShot(true);
        connect(&to, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        to.start(20000);
        loop.exec();
        if (!reply->isFinished() || reply->error() != QNetworkReply::NoError)
        {
            err = reply->isFinished() ? reply->errorString() : tr("timed out");
            reply->abort(); reply->deleteLater();
        }
        else { body = reply->readAll(); reply->deleteLater(); }
    }
    if (!err.isEmpty())
    {
        // Rate limiting lands here too (GitHub allows 60 unauthenticated calls an hour per IP).
        if (error) *error = tr("Couldn't read the registry's file list: %1").arg(err);
        return QByteArray();
    }

    treeCache_.insert(indexUrl, body);
    return body;
}

// Install one themes2 entry: list the folder from the repo tree, download every file, then hand the whole
// set to ThemeRegistry::installFiles, which writes it atomically. Nothing touches themes2/<Name> until
// every byte is in hand — a half-installed theme would still be offered by the picker.
void RegistryBrowser::installThemeEntry(const QJsonObject& entry, const QString& indexUrl)
{
    ThemeRegistry::Entry e;
    e.name = entry.value(QStringLiteral("name")).toString();
    e.dir  = entry.value(QStringLiteral("dir")).toString();
    const QString folder = e.folder();
    if (folder.isEmpty()) { status_->setText(tr("This entry doesn't name a usable theme folder.")); return; }

    QString err;
    const QByteArray tree = treeFor(indexUrl, &err);
    if (tree.isEmpty()) { status_->setText(err); return; }

    const ThemeRegistry::Listing listing = ThemeRegistry::filesUnder(tree, e.dir);
    if (!listing.ok()) { status_->setText(listing.error); return; }

    const QString base = baseUrl(indexUrl);
    QVector<QPair<QString, QByteArray>> blobs;
    for (const QString& rel : listing.files)
    {
        // Percent-encode each segment: a theme may ship a font or sound with a space in its name.
        QStringList enc;
        for (const QString& seg : rel.split(QLatin1Char('/')))
            enc << QString::fromUtf8(QUrl::toPercentEncoding(seg));
        const QString url = base + QStringLiteral("/") + e.dir + QStringLiteral("/") + enc.join(QLatin1Char('/'));

        const QString tmp = QDir::tempPath() + QStringLiteral("/eb-theme-dl.tmp");
        QString derr;
        if (!downloadTo(url, tmp, &derr))
        { status_->setText(tr("Download failed: %1\n%2").arg(rel, derr)); QFile::remove(tmp); return; }
        QFile f(tmp);
        if (!f.open(QIODevice::ReadOnly))
        { status_->setText(tr("Download failed: %1").arg(rel)); QFile::remove(tmp); return; }
        blobs << qMakePair(rel, f.readAll());
        f.close();
        QFile::remove(tmp);
    }

    if (!ThemeRegistry::installFiles(ThemeEngine::themesRoot(), folder, blobs, &err))
    { status_->setText(err); return; }

    installed_ = true;
    status_->setText(tr("Installed “%1”. Pick it from the theme list.").arg(e.name));
}
```

Add `#include <QHash>` and `#include <QPair>` to the includes if not already present.

- [ ] **Step 5: Build**

```bash
cmake --build build --config Release --target everythingbox
```

Expected: builds clean. A `ThemeEngine.h` include error means the app target's include directories need `src/theme2` — it is already there for the other UI sources, so the more likely cause is a typo in the relative include path.

- [ ] **Step 6: Run the suite**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Expected: no `FAIL:` lines. The old-brand gate is the one most likely to trip on new user-facing strings — the current name is `EverythingBox`.

- [ ] **Step 7: Commit**

```bash
git add native/src/ui/RegistryBrowser.h native/src/ui/RegistryBrowser.cpp native/CMakeLists.txt
git commit -m "fix: RegistryBrowser installs a themes2 theme folder instead of a format nothing serves"
```

---

### Task 4: Reach the gallery from the classic Appearance panel

**Files:**
- Modify: `native/src/ui/MainWindow.cpp:5626-5640` (the classic builder's `share` label block)

**Interfaces:**
- Consumes: `RegistryBrowser(RegistryBrowser::Themes, nullptr, this)` from Task 3; `MainWindow::showDialogPanel` (`MainWindow.cpp:9078`).
- Produces: nothing later tasks consume. Task 6's gate greps for the `RegistryBrowser::Themes` construction added here.

- [ ] **Step 1: Reframe the instructions and add the button**

In `native/src/ui/MainWindow.cpp`, replace the `share` label block (lines 5626–5640) with:

```cpp
        // Getting more themes: the gallery is the way in, the GitHub link is how you contribute one (and
        // the fallback for a registry the gallery can't install from). Before this, the hand-copy was the
        // ONLY documented route, which is not performable on a TV box with no browser and no file manager.
        auto* share = new QLabel(tr(
            "<b>Get more themes &amp; share yours.</b> "
            "Browse the community registry below, or at "
            "<a href=\"https://github.com/cubman3134/everythingbox-themes\">github.com/cubman3134/everythingbox-themes</a>. "
            "Themes live in <code>%1</code> — each is a folder with a <code>theme.json</code>, so you can also "
            "add one by dropping its folder in there. "
            "To <b>share</b> yours, add the folder under <code>themes2/</code> in that repo with an "
            "<code>index.json</code> entry and open a pull request (see <code>THEME_FORMAT.md</code> for the format).")
            .arg(ThemeEngine::themesRoot()));
        share->setTextFormat(Qt::RichText);
        share->setWordWrap(true);
        share->setOpenExternalLinks(true); // the GitHub link opens in the browser
        share->setStyleSheet(QStringLiteral("margin-top:12px;"));
        v->addWidget(share);

        // The in-app gallery. Hosted INLINE via showDialogPanel (it sets Qt::Widget), never as a top-level
        // window — this panel lives inside panelRing_ and a real dialog would be unreachable with a D-pad.
        // Same hosting LibraryView::browseAddons uses for the add-on browser.
        auto* browse = new QPushButton(tr("Browse community themes…"));
        browse->setMinimumHeight(40);
        connect(browse, &QPushButton::clicked, this, [this] {
            auto* dlg = new RegistryBrowser(RegistryBrowser::Themes, nullptr, this);
            showDialogPanel(tr("Browse Themes"), dlg, [this, dlg](int) {
                // Re-render Appearance either way: availableThemes() reads the directory live, so a newly
                // installed theme is in the list as soon as the panel is rebuilt.
                if (dlg->installedSomething()) openAppearance();
            }, [this] { openAppearance(); });
        });
        v->addWidget(browse);
```

- [ ] **Step 2: Build**

```bash
cmake --build build --config Release --target everythingbox
```

Expected: builds clean.

- [ ] **Step 3: Verify in the running app**

Launch with the UI-test harness so no window needs focus:

```bash
EB_UITEST=1 ./build/Release/everythingbox.exe
```

Drive it with `python native/tools/uitest.py` to reach Settings ▸ Appearance with the themed home **off**, and confirm: the "Browse community themes…" button is present and focusable, activating it opens the browser inline (no separate OS window appears), the list shows the registry's themes, `Grid` / `Lumen` / `Midnight` offer **Install**, and `Channels` / `Night` / `Triple` show **Installed ✓** and are disabled. Install `Lumen`, go Back, and confirm it appears in the theme list.

Expected: `<dataDir>/themes2/Lumen/theme.json` exists afterwards.

- [ ] **Step 4: Commit**

```bash
git add native/src/ui/MainWindow.cpp
git commit -m "feat: reach the theme gallery from the classic Appearance panel"
```

---

### Task 5: Reach the gallery from the themed Appearance panel

The themed twin, modelled on `presentAddonRegistry` and sharing its lifetime discipline.

**Files:**
- Modify: `native/src/ui/MainWindow.h` (declare `presentThemeRegistry`)
- Modify: `native/src/ui/MainWindow.cpp` (define it next to `presentAddonRegistry` ~line 4174; add the `appr.browse` row at ~5499–5505 and its handler at ~5552)

**Interfaces:**
- Consumes: `ThemeRegistry::{Entry, parseIndex, treeApiUrl, filesUnder, installFiles}` from Tasks 1–2; `registryDownloadTo` and `registryBaseUrl` from the anonymous namespace at `MainWindow.cpp:4026-4069`; `PanelRow` from `native/src/theme2/PanelModel.h`.
- Produces: `void MainWindow::presentThemeRegistry()`. Task 6's gate greps for its declaration, definition and call.

- [ ] **Step 1: Declare it**

In `native/src/ui/MainWindow.h`, beside the `presentAddonRegistry` declaration, inside the same `#ifdef EB_HAVE_QML` guard:

```cpp
    void presentThemeRegistry();   // the themed twin of RegistryBrowser(Themes) — the theme gallery
```

- [ ] **Step 2: Define it**

In `native/src/ui/MainWindow.cpp`, after `installRegistryEntry` ends (line 4216) and before the closing `#endif // EB_HAVE_QML`:

```cpp
// The theme gallery as a nested themed panel — the twin of the classic RegistryBrowser(Themes). Same shape
// as presentAddonRegistry: a status Info row, one Action row per entry, a 15 s guard so "Loading…" cannot
// stick, and a themedPanelIsTop gate so a fetch that lands after the user navigated away is dropped.
//
// A theme entry names a FOLDER ({dir: "themes2/<Name>"}), so installing means listing that folder from the
// registry repo's own tree and writing it whole — see ThemeRegistry, which both surfaces share so they
// cannot disagree about what a registry says or what may become a filename.
void MainWindow::presentThemeRegistry()
{
    if (!docNam_) docNam_ = new QNetworkAccessManager(this);

    QVector<PanelRow> loading;
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("treg.status"); r.label = tr("Registry");
      r.value = tr("Loading…"); loading << r; }
    themedPanelHost_->present(tr("Browse Themes"), loading, [](const QString&, const QString&) {}, [this] {
        openAppearance();
    });
    stack_->setCurrentWidget(themedPanelHost_);
    updateNavForPage();

    QSettings iniStore(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile), QSettings::IniFormat);
    QStringList regs;
    regs << QStringLiteral("https://raw.githubusercontent.com/cubman3134/everythingbox-themes/main/index.json");
    for (const QString& u : iniStore.value(QStringLiteral("registry/themesExtras")).toStringList())
        if (!u.trimmed().isEmpty() && !regs.contains(u.trimmed())) regs << u.trimmed();

    struct ThemeFetch { int pending = 0; QVector<QPair<ThemeRegistry::Entry, QString>> entries; };
    auto st = std::make_shared<ThemeFetch>();
    st->pending = regs.size();

    auto finish = [this, st] {
        if (!themedPanelIsTop(tr("Browse Themes"))) return;   // navigated away while fetching — drop
        QVector<PanelRow> rows;
        if (st->entries.isEmpty())
        {
            PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("treg.status"); r.label = tr("Registry");
            r.value = tr("No themes found — the registry may be unreachable."); rows << r;
        }
        else
        {
            { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("treg.status"); r.label = tr("Registry");
              r.value = tr("%n theme(s) available.", "", int(st->entries.size())); rows << r; }
            for (int i = 0; i < st->entries.size(); ++i)
            {
                const ThemeRegistry::Entry& e = st->entries[i].first;
                // Installed = the folder is on disk, the same predicate availableThemes() uses. The bundled
                // themes are therefore never offered, which is what keeps the registry's older copies of
                // them (issue #131) from replacing the ones already in the app.
                const bool installed =
                    QFile::exists(ThemeEngine::themesRoot() + QStringLiteral("/") + e.folder()
                                  + QStringLiteral("/theme.json"));
                PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("treg:") + QString::number(i);
                r.label = e.name.isEmpty() ? e.folder() : e.name;
                QString sub = e.author.isEmpty() ? QString() : tr("by %1").arg(e.author);
                if (!e.formFactors.isEmpty())
                    sub += (sub.isEmpty() ? QString() : QStringLiteral(" · ")) + e.formFactors.join(QStringLiteral(", "));
                r.value = installed ? tr("Installed ✓") : sub;
                r.enabled = !installed;
                rows << r;
            }
        }
        themedPanelHost_->replaceTop(tr("Browse Themes"), rows,
            [this, st](const QString& id, const QString&) {
                if (!id.startsWith(QStringLiteral("treg:"))) return;
                const int i = id.mid(5).toInt();
                if (i < 0 || i >= st->entries.size()) return;
                installThemeRegistryEntry(st->entries[i].first, st->entries[i].second, id);
            },
            [this] { openAppearance(); });
    };

    QTimer::singleShot(15000, this, [st, finish] { if (st->pending > 0) { st->pending = 0; finish(); } });

    for (const QString& indexUrl : regs)
    {
        QNetworkRequest req((QUrl(indexUrl)));
        req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = docNam_->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, indexUrl, st, finish] {
            reply->deleteLater();
            if (st->pending <= 0) return;   // already finished (timeout) — ignore a late arrival
            if (reply->error() == QNetworkReply::NoError)
                for (const ThemeRegistry::Entry& e : ThemeRegistry::parseIndex(reply->readAll()))
                    st->entries << qMakePair(e, indexUrl);
            if (--st->pending <= 0) finish();
        });
    }
}

// Install one theme entry from the themed gallery: list its folder from the registry repo's tree, download
// every file, then write the set atomically. Blocking, like the add-on twin (installRegistryEntry); the row
// shows "Installing…" while it runs.
//
// The tree is fetched per INSTALL here rather than cached per registry as the classic browser does. That is
// still one API call per install — the figure the 60-an-hour budget rests on — and the alternative is a
// cache member on MainWindow outliving the panel that filled it, which is more state than one call is worth.
void MainWindow::installThemeRegistryEntry(const ThemeRegistry::Entry& entry, const QString& indexUrl,
                                           const QString& rowId)
{
    auto setRow = [this, rowId, &entry](const QString& value, bool enabled) {
        PanelRow r; r.kind = PanelRow::Action; r.id = rowId; r.label = entry.name;
        r.value = value; r.enabled = enabled; themedPanelHost_->updateRow(rowId, r);
    };
    setRow(tr("Installing…"), false);

    const QString folder = entry.folder();
    if (folder.isEmpty())
    { updatePanelInfo(QStringLiteral("treg.status"), tr("This entry doesn't name a usable theme folder."));
      setRow(tr("Retry"), true); return; }

    const QString api = ThemeRegistry::treeApiUrl(indexUrl);
    if (api.isEmpty())
    { updatePanelInfo(QStringLiteral("treg.status"),
                      tr("This registry isn't hosted on GitHub, so themes can't be installed from here."));
      setRow(tr("Manual"), false); return; }

    const QString treeTmp = QDir::tempPath() + QStringLiteral("/eb-theme-tree.tmp");
    QString err;
    if (!registryDownloadTo(docNam_, api, treeTmp, &err))
    { updatePanelInfo(QStringLiteral("treg.status"), tr("Couldn't read the registry's file list: %1").arg(err));
      setRow(tr("Retry"), true); return; }
    QByteArray tree;
    { QFile f(treeTmp); if (f.open(QIODevice::ReadOnly)) tree = f.readAll(); }
    QFile::remove(treeTmp);

    const ThemeRegistry::Listing listing = ThemeRegistry::filesUnder(tree, entry.dir);
    if (!listing.ok())
    { updatePanelInfo(QStringLiteral("treg.status"), listing.error); setRow(tr("Retry"), true); return; }

    const QString base = registryBaseUrl(indexUrl);
    QVector<QPair<QString, QByteArray>> blobs;
    for (const QString& rel : listing.files)
    {
        QStringList enc;
        for (const QString& seg : rel.split(QLatin1Char('/')))
            enc << QString::fromUtf8(QUrl::toPercentEncoding(seg));
        const QString url = base + QStringLiteral("/") + entry.dir + QStringLiteral("/") + enc.join(QLatin1Char('/'));
        const QString tmp = QDir::tempPath() + QStringLiteral("/eb-theme-dl.tmp");
        if (!registryDownloadTo(docNam_, url, tmp, &err))
        { updatePanelInfo(QStringLiteral("treg.status"), tr("Download failed: %1").arg(rel));
          QFile::remove(tmp); setRow(tr("Retry"), true); return; }
        QFile f(tmp);
        if (f.open(QIODevice::ReadOnly)) blobs << qMakePair(rel, f.readAll());
        f.close();
        QFile::remove(tmp);
    }

    if (!ThemeRegistry::installFiles(ThemeEngine::themesRoot(), folder, blobs, &err))
    { updatePanelInfo(QStringLiteral("treg.status"), err); setRow(tr("Retry"), true); return; }

    setRow(tr("Installed ✓"), false);
    updatePanelInfo(QStringLiteral("treg.status"),
                    tr("Installed \"%1\". Pick it from Theme… on Appearance.").arg(entry.name));
}
```

Declare `installThemeRegistryEntry` in `native/src/ui/MainWindow.h` beside `presentThemeRegistry`, inside the same `#ifdef EB_HAVE_QML` guard, and add `#include "../core/ThemeRegistry.h"` to `MainWindow.h` (the signature names the type):

```cpp
    void installThemeRegistryEntry(const ThemeRegistry::Entry& entry, const QString& indexUrl,
                                   const QString& rowId);
```

- [ ] **Step 3: Add the row**

In `native/src/ui/MainWindow.cpp`, in the themed Appearance builder, insert one line immediately before the existing `action(QStringLiteral("appr.gallery"), …)` at line 5505:

```cpp
        action(QStringLiteral("appr.browse"), tr("Browse community themes…"));
```

and update the neighbouring `appr.community` Info row (line 5503–5504) so it stops presenting the browser as the only route:

```cpp
        info(QStringLiteral("appr.community"),
             tr("Browse the registry below, or share your own at github.com/cubman3134/everythingbox-themes."),
             QString());
```

- [ ] **Step 4: Handle it**

In the same builder's `onAct` lambda, add a branch immediately before the `appr.gallery` branch at line 5552:

```cpp
                else if (id == QStringLiteral("appr.browse")) {
                    presentThemeRegistry();
                }
```

- [ ] **Step 5: Build**

```bash
cmake --build build --config Release --target everythingbox
```

Expected: builds clean.

- [ ] **Step 6: Verify in the running app**

```bash
EB_UITEST=1 ./build/Release/everythingbox.exe
```

Drive it with `python native/tools/uitest.py` to Settings ▸ Appearance with the themed home **on**. Confirm: "Browse community themes…" is a focusable row, activating it shows "Loading…" then the entry list, D-pad up/down moves through the rows and Back returns to Appearance, `Grid` / `Lumen` / `Midnight` are enabled and `Channels` / `Night` / `Triple` read "Installed ✓" disabled. Install `Midnight`, Back to Appearance, open `Theme…`, and confirm it is in the picker with a live preview.

Expected: `<dataDir>/themes2/Midnight/theme.json` and `<dataDir>/themes2/Midnight/sounds/move.wav` both exist — the second one is the flattening bug, and its absence is how you know the subpath handling works.

- [ ] **Step 7: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
git commit -m "feat: reach the theme gallery from the themed Appearance panel"
```

---

### Task 6: The two-builders reachability gate

A source-level gate so a later edit cannot quietly drop the gallery from one surface. Written to the standard of the neighbouring gates: comments stripped first, a floor that reports when it has scanned nothing, and positive assertions rather than a blacklist.

**Files:**
- Modify: `native/tools/run-headless-probes.sh`
- Modify: `CONTRIBUTING.md`

**Interfaces:**
- Consumes: the `appr.browse` row id and `presentThemeRegistry` from Task 5; the `RegistryBrowser::Themes` construction from Task 4.
- Produces: nothing.

- [ ] **Step 1: Write the gate**

In `native/tools/run-headless-probes.sh`, insert immediately before the `=== exe-folder contamination ===` section (which must stay last, since it fingerprints the whole run):

```bash
# Appearance theme-gallery reachability gate. openAppearance() has TWO builders — a themed one (PanelRows)
# and a classic one (QWidgets) — and CONTRIBUTING.md names that split as the thing most often half-done. The
# gallery is a fresh instance of it: the registry browser supported a Themes kind for a long time while being
# constructed with Addons at its ONE call site, so there was no theme gallery at all and nothing said so.
#
# This asserts the property rather than the prose: both builders still reach the registry. The themed side
# needs presentThemeRegistry to be DEFINED and CALLED (a definition nothing calls is exactly the dead-code
# state this replaces); the classic side needs a RegistryBrowser::Themes construction.
#
# Comments are stripped FIRST, and that is load-bearing rather than tidiness: this whole section is
# introduced by prose naming every symbol it greps for, so a gate reading the raw file would go on passing
# after someone deleted the code and left a comment describing it. That is precisely how an assertion ends
# up gating nothing — the same trap the probe data-dir isolation gate documents.
echo "=== appearance theme-gallery reachability ==="
GAL_MW="$HERE/../src/ui/MainWindow.cpp"
GAL_MWH="$HERE/../src/ui/MainWindow.h"
if [ ! -f "$GAL_MW" ] || [ ! -f "$GAL_MWH" ]; then
  echo "FAIL: appearance theme-gallery reachability (MainWindow sources not found under $HERE/../src/ui)"
  fail=1
else
  gal_bad=0
  gal_src="$(sed -E 's://.*$::' "$GAL_MW")"

  # Floor: did this gate scan the right file at all? A gate that walks the wrong tree prints PASS, which is
  # worse than no gate — it reports a rule as enforced. openAppearance is the function both builders live in.
  if ! printf '%s\n' "$gal_src" | grep -q 'void MainWindow::openAppearance'; then
    echo "  MainWindow.cpp has no openAppearance definition — this gate scanned the wrong file or the"
    echo "  builders moved. Treat a PASS as meaningless until the path is fixed."
    gal_bad=1
  fi

  # Themed builder: the row that opens the gallery, and a handler that actually calls it.
  printf '%s\n' "$gal_src" | grep -q '"appr\.browse"' \
    || { echo "  the themed Appearance builder no longer offers an appr.browse row — the gallery is"; \
         echo "  unreachable on the themed surface"; gal_bad=1; }
  printf '%s\n' "$gal_src" | grep -q 'void MainWindow::presentThemeRegistry' \
    || { echo "  presentThemeRegistry is no longer defined — the themed gallery panel is gone"; gal_bad=1; }
  # Called, not merely defined. The definition line is excluded so it cannot satisfy its own call check.
  printf '%s\n' "$gal_src" | grep -v 'void MainWindow::presentThemeRegistry' \
    | grep -q 'presentThemeRegistry()' \
    || { echo "  presentThemeRegistry is defined but never called — a themed panel nothing opens is the"; \
         echo "  exact dead-code state this gate exists to prevent"; gal_bad=1; }

  # Classic builder: the only way its gallery opens is a Themes-kind RegistryBrowser.
  printf '%s\n' "$gal_src" | grep -q 'RegistryBrowser::Themes' \
    || { echo "  the classic Appearance builder no longer constructs RegistryBrowser::Themes — the gallery"; \
         echo "  is unreachable on the classic surface"; gal_bad=1; }

  if [ "$gal_bad" -eq 0 ]; then
    echo "PASS: appearance theme-gallery reachability (both builders reach the theme registry)"
  else
    echo "FAIL: appearance theme-gallery reachability — a user-facing surface exists on only one of"
    echo "  openAppearance()'s two builders. See the two-settings-builders rule in CONTRIBUTING.md."
    fail=1
  fi
fi
echo
```

- [ ] **Step 2: Verify the gate passes**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh 2>&1 | grep -A4 "=== appearance theme-gallery reachability ==="
```

Expected: `PASS: appearance theme-gallery reachability (both builders reach the theme registry)`.

- [ ] **Step 3: Verify the gate actually fails — mutation-test it**

A gate that has never been seen red is a guess. Break each assertion in turn and confirm the suite goes red, restoring after each:

```bash
sed -i 's/"appr\.browse"/"appr.browseX"/' native/src/ui/MainWindow.cpp
BUILD_DIR=build bash native/tools/run-headless-probes.sh 2>&1 | grep -c "FAIL: appearance theme-gallery"
git checkout native/src/ui/MainWindow.cpp
```

Expected: `1` from the grep, then a clean checkout. Repeat for `RegistryBrowser::Themes` (rename it in the classic builder) and for the call check (comment out the `presentThemeRegistry();` line in `onAct` — the gate must still fail, proving the definition alone does not satisfy it).

- [ ] **Step 4: Document the gate**

In `CONTRIBUTING.md`, in the paragraph at lines 68–77 that lists the source-level gates, add the new one to the sentence:

```
that scan the tree: the QML no-direct-selection-writes gate, the RetroView
`.srm` path gate, the probe data-dir isolation wiring gate, the bundled-theme /
registry drift gate below, the Appearance theme-gallery reachability gate, and
the old-brand gate.
```

And under the `### `openGeneralSettings()` has two builders — add to both` section, append:

```
The theme gallery is the worked example, and the reason there is now a gate:
`RegistryBrowser` carried a `Themes` kind for a long time while its only
construction passed `Addons`, so there was no in-app theme gallery on either
surface and nothing said so. `=== appearance theme-gallery reachability ===`
asserts that both builders still reach the theme registry — the themed one via
`presentThemeRegistry`, the classic one via a `RegistryBrowser::Themes`
construction.
```

- [ ] **Step 5: Full suite and build**

```bash
cmake --build build --config Release --target everythingbox probe_themereg
```

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Expected: no `FAIL:` lines anywhere in the output.

- [ ] **Step 6: Commit**

```bash
git add native/tools/run-headless-probes.sh CONTRIBUTING.md
git commit -m "test: gate that both Appearance builders reach the theme gallery"
```

---

## Verification

After Task 6, the whole change is verified by:

1. `cmake --build build --config Release --target everythingbox probe_themereg` — clean.
2. `BUILD_DIR=build bash native/tools/run-headless-probes.sh` — no `FAIL:` lines; `PASS: probe_themereg` and `PASS: appearance theme-gallery reachability` both present.
3. The two manual passes from Tasks 4 and 5 (classic and themed), each ending with a real theme folder on disk including its `sounds/` subfolder.

The network and UI paths are not probe-covered — the suite is deliberately offline — so those manual passes are the evidence for them. Say so plainly when reporting completion rather than implying the suite covers the install.

## Follow-ups (not in this plan)

- **#131** — republish the drifted Channels, Night and Triple to the registry. Until it closes, an "Update this theme" action cannot be offered, because the only update available for those three is a downgrade.
- **Uninstall.** Sharper than it looks: `AssetBootstrap` re-extracts the bundled three on every version bump, so uninstall would not stick for them.
