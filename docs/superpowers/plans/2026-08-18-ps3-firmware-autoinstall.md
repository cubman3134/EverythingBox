# PS3 Firmware Auto-Install Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When launching a PS3 game, auto-detect missing RPCS3 firmware, download Sony's official PS3UPDAT.PUP, and install it headlessly via `rpcs3.exe --installfw` before the boot — never blocking a boot on failure.

**Architecture:** A new pure-logic unit `Ps3Firmware` (detection + updatelist parse + seam-injected check→fetch→download→install pipeline) beside the existing `src/core/ps3/` game-update units, exercised by a new headless probe `probe_ps3firmware`. `EmulatorManager::runPs3UpdateThenLaunch` gains a firmware step ahead of the existing game-update step on the same worker thread, reusing the existing `downloadPs3Pkg` streamed downloader and the bounded-`QProcess` install pattern.

**Tech Stack:** C++17, Qt6 (Core/Network), CMake, headless probe suite.

## Global Constraints

- **No AI attribution in commits** — no Co-Authored-By, no generated-by lines. Conventional prefixes (`feat:`, `fix:` …).
- **Never block a boot**: every firmware-install failure (fetch, parse, download, install, verify) falls through silently to `finishLocalLaunch` — RPCS3 then shows its own missing-firmware error. Result of the firmware step is ignored by the launch flow, exactly like `coord.maybeUpdate(rom)`.
- **Same infrastructure trust model as the PS3 game-update feature**: Sony endpoints with cert-CN mismatch → `QSslSocket::VerifyNone` + `ignoreSslErrors()`, 15 s transfer timeout + 20 s hard watchdog on the metadata fetch, streamed download with stall detection + 12 GB byte ceiling (reuse `downloadPs3Pkg` as-is), bounded process wait (30 s start / 600 s run / kill).
- **The updatelist feed offers no hash** — Sony's `ps3-updatelist.txt` has no SHA field, so there is no hash gate; RPCS3's `--installfw` validates the PUP internally and a corrupt file fails the install (which falls through). Say this in a comment where the fetch lives.
- **New probe registered in three places** (CONTRIBUTING "A new pure component gets a probe"): `native/CMakeLists.txt` (`add_executable` + link), the `for p in …` loop near the end of `native/tools/run-headless-probes.sh` (entry `"probe_ps3firmware PS3FIRMWARE-OK"`), and the `--target` list of the "Build probes" step in `.github/workflows/ci.yml`.
- **Every new assertion is mutation-proven with `native/tools/mutate.py --spec …`** (never a hand-rolled loop; the tree is CRLF and the driver handles that trap). All mutants must report KILLED; NOT APPLIED is a failed run.
- **Probe links Qt6::Core only**, prints `PS3FIRMWARE-OK` on success, non-zero + stderr on failure, no display/network/process spawns (all seams injected).
- **No new user-facing setting.** Firmware install is auto-on-launch for RPCS3 and runs regardless of the "Auto-install PS3 game updates" toggle (that toggle keeps gating only the game-update step).
- **Build config**: existing `build/` dir, VS 2026 generator, multi-config. Build with `cmake --build build --config Release --target <t> --parallel`. Probe exes land in `build/Release/`.
- Firmware presence test is `<binDir>/dev_flash/vsh/etc/version.txt` exists **and is non-empty** (the file RPCS3 itself reads for the installed-firmware version; RPCS3 is portable on Windows so `dev_flash` sits next to the exe).
- Sony endpoints (exact URLs):
  - updatelist: `https://fus01.ps3.update.playstation.net/update/ps3/list/us/ps3-updatelist.txt`
  - the PUP url comes verbatim from the `CDN=` field of that feed.

---

### Task 1: `Ps3Firmware` unit + `probe_ps3firmware` (registered in all three places)

**Files:**
- Create: `native/src/core/ps3/Ps3Firmware.h`
- Create: `native/src/core/ps3/Ps3Firmware.cpp`
- Create: `native/tools/probe_ps3firmware.cpp`
- Modify: `native/CMakeLists.txt` (immediately after the `probe_ps3update` block that ends near line 1123)
- Modify: `native/tools/run-headless-probes.sh` (the long `for p in "probe_navqml NAVQML-OK" …` list, ~line 321 — append one entry before the closing `; do`)
- Modify: `.github/workflows/ci.yml` ("Build probes" step, ~line 63 — append to the `--target` list)
- Create (scratch, not committed): mutation spec JSON — put it in the session scratchpad or `build/`, NOT in the repo tree.

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces (Task 2 relies on these exact signatures, namespace `Ps3Firmware`):
  - `bool installed(const QString& binDir)`
  - `struct Info { QString version; QString url; }`
  - `std::optional<Info> parseUpdateList(const QByteArray& body)`
  - `using FeedFetcher = std::function<std::optional<QByteArray>()>;`
  - `using Downloader  = std::function<bool(const QString& url, const QString& destPath)>;`
  - `using Installer   = std::function<int(const QString& rpcs3Exe, const QString& pupPath)>;`
  - `using Progress    = std::function<void(const QString& message)>;`
  - `bool maybeInstall(const QString& binDir, const QString& rpcs3Exe, const QString& tmpDir, const FeedFetcher& fetch, const Downloader& download, const Installer& install, const Progress& progress)`

- [ ] **Step 1: Write the probe first (it is the failing test)**

Create `native/tools/probe_ps3firmware.cpp`:

```cpp
// Headless pure-logic probe for the PS3 firmware auto-install unit. Prints PS3FIRMWARE-OK on success.
// No display, no network, no process spawns — every external effect is an injected seam.
#include "core/ps3/Ps3Firmware.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <cstdio>
#include <optional>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #c, __LINE__); ++g_fail; } } while (0)

// Realistic two-record feed: a compatibility line (no CDN) followed by the image line. The parser must
// skip the first and return the CDN url + SystemSoftwareVersion of the second.
static const char* kRealFeed =
    "Dest=84;CompatibleSystemSoftwareVersion=4.9200-;\n"
    "Dest=84;ImageVersion=04.9200;SystemSoftwareVersion=4.9200;"
    "CDN=http://dus01.ps3.update.playstation.net/update/ps3/image/us/"
    "2024_0227_09799a2ba7bdcb84302b3ba09b5be4f8/PS3UPDAT.PUP;CDN_Timeout=30;\n";

static void testParse()
{
    auto info = Ps3Firmware::parseUpdateList(QByteArray(kRealFeed));
    CHECK(info.has_value());
    if (info)
    {
        CHECK(info->version == QStringLiteral("4.9200"));
        CHECK(info->url.startsWith(QStringLiteral("http://dus01.ps3.update.playstation.net/")));
        CHECK(info->url.endsWith(QStringLiteral("/PS3UPDAT.PUP")));
    }

    CHECK(!Ps3Firmware::parseUpdateList(QByteArray()).has_value());                     // empty body
    CHECK(!Ps3Firmware::parseUpdateList(QByteArray("Dest=84;ImageVersion=1;\n")).has_value()); // no CDN field
    CHECK(!Ps3Firmware::parseUpdateList(QByteArray("random junk not a feed")).has_value());
    CHECK(!Ps3Firmware::parseUpdateList(QByteArray("CDN=ftp://evil/PS3UPDAT.PUP;\n")).has_value()); // non-http url
}

// Create <root>/dev_flash/vsh/etc/version.txt with the given bytes.
static void seedVersionTxt(const QString& root, const QByteArray& bytes)
{
    QDir().mkpath(root + QStringLiteral("/dev_flash/vsh/etc"));
    QFile f(root + QStringLiteral("/dev_flash/vsh/etc/version.txt"));
    f.open(QIODevice::WriteOnly);
    f.write(bytes);
}

static void testInstalled()
{
    QTemporaryDir dir; CHECK(dir.isValid());
    CHECK(!Ps3Firmware::installed(dir.path()));       // no dev_flash at all
    seedVersionTxt(dir.path(), QByteArray());
    CHECK(!Ps3Firmware::installed(dir.path()));       // version.txt present but EMPTY = incomplete
    seedVersionTxt(dir.path(), QByteArray("04.9200"));
    CHECK(Ps3Firmware::installed(dir.path()));        // real content = installed
}

static void testMaybeInstall()
{
    const QString feed = QString::fromLatin1(kRealFeed);

    // Already installed -> no work at all: the feed is never even fetched.
    {
        QTemporaryDir dir; CHECK(dir.isValid());
        seedVersionTxt(dir.path(), QByteArray("04.9200"));
        int fetches = 0;
        auto fetch = [&]() -> std::optional<QByteArray> { ++fetches; return feed.toUtf8(); };
        CHECK(!Ps3Firmware::maybeInstall(dir.path(), QStringLiteral("rpcs3.exe"), dir.path(),
                                         fetch, nullptr, nullptr, nullptr));
        CHECK(fetches == 0);
    }

    // Happy path: missing firmware -> fetch -> download -> --installfw (which produces dev_flash) -> true.
    {
        QTemporaryDir dir; CHECK(dir.isValid());
        const QString tmp = dir.path() + QStringLiteral("/tmp");
        QString downloadedUrl, installedPup;
        QStringList notes;
        auto fetch    = [&]() -> std::optional<QByteArray> { return feed.toUtf8(); };
        auto download = [&](const QString& url, const QString& dest) {
            downloadedUrl = url;
            QFile f(dest); if (!f.open(QIODevice::WriteOnly)) return false; f.write("PUPBYTES"); return true;
        };
        auto install  = [&](const QString&, const QString& pup) {
            installedPup = pup;
            CHECK(QFile::exists(pup)); // the PUP must still be on disk when the installer runs
            seedVersionTxt(dir.path(), QByteArray("04.9200")); // simulate --installfw writing dev_flash
            return 0;
        };
        auto progress = [&](const QString& m) { notes << m; };
        CHECK(Ps3Firmware::maybeInstall(dir.path(), QStringLiteral("rpcs3.exe"), tmp,
                                        fetch, download, install, progress));
        CHECK(downloadedUrl.endsWith(QStringLiteral("/PS3UPDAT.PUP")));
        CHECK(installedPup.endsWith(QStringLiteral("PS3UPDAT.PUP")));
        CHECK(!notes.isEmpty());                 // told the user what's happening
        CHECK(!QFile::exists(installedPup));     // temp PUP cleaned up afterwards
    }

    // Fetch fails -> false, nothing downloaded or installed.
    {
        QTemporaryDir dir; CHECK(dir.isValid());
        int downloads = 0;
        auto fetch    = [&]() -> std::optional<QByteArray> { return std::nullopt; };
        auto download = [&](const QString&, const QString&) { ++downloads; return true; };
        CHECK(!Ps3Firmware::maybeInstall(dir.path(), QStringLiteral("rpcs3.exe"), dir.path(),
                                         fetch, download, nullptr, nullptr));
        CHECK(downloads == 0);
    }

    // Download fails -> false, installer never runs, no stray PUP left behind.
    {
        QTemporaryDir dir; CHECK(dir.isValid());
        const QString tmp = dir.path() + QStringLiteral("/tmp");
        int installs = 0;
        auto fetch    = [&]() -> std::optional<QByteArray> { return feed.toUtf8(); };
        auto download = [&](const QString&, const QString&) { return false; };
        auto install  = [&](const QString&, const QString&) { ++installs; return 0; };
        CHECK(!Ps3Firmware::maybeInstall(dir.path(), QStringLiteral("rpcs3.exe"), tmp,
                                         fetch, download, install, nullptr));
        CHECK(installs == 0);
        CHECK(!QFile::exists(QDir(tmp).filePath(QStringLiteral("PS3UPDAT.PUP"))));
    }

    // Installer exits non-zero -> false, temp PUP still cleaned up.
    {
        QTemporaryDir dir; CHECK(dir.isValid());
        const QString tmp = dir.path() + QStringLiteral("/tmp");
        auto fetch    = [&]() -> std::optional<QByteArray> { return feed.toUtf8(); };
        auto download = [&](const QString&, const QString& dest) {
            QFile f(dest); if (!f.open(QIODevice::WriteOnly)) return false; f.write("PUPBYTES"); return true;
        };
        auto install  = [&](const QString&, const QString&) { return 1; };
        CHECK(!Ps3Firmware::maybeInstall(dir.path(), QStringLiteral("rpcs3.exe"), tmp,
                                         fetch, download, install, nullptr));
        CHECK(!QFile::exists(QDir(tmp).filePath(QStringLiteral("PS3UPDAT.PUP"))));
    }

    // Installer exits 0 but produced NO dev_flash -> false (an exit code alone is not proof).
    {
        QTemporaryDir dir; CHECK(dir.isValid());
        const QString tmp = dir.path() + QStringLiteral("/tmp");
        auto fetch    = [&]() -> std::optional<QByteArray> { return feed.toUtf8(); };
        auto download = [&](const QString&, const QString& dest) {
            QFile f(dest); if (!f.open(QIODevice::WriteOnly)) return false; f.write("PUPBYTES"); return true;
        };
        auto install  = [&](const QString&, const QString&) { return 0; };
        CHECK(!Ps3Firmware::maybeInstall(dir.path(), QStringLiteral("rpcs3.exe"), tmp,
                                         fetch, download, install, nullptr));
    }
}

int main()
{
    testParse();
    testInstalled();
    testMaybeInstall();
    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("PS3FIRMWARE-OK\n");
    return 0;
}
```

- [ ] **Step 2: Register the probe target in `native/CMakeLists.txt`**

Immediately after the existing `probe_ps3update` block (ends near line 1123, `target_link_libraries(probe_ps3update PRIVATE Qt6::Core)`), add:

```cmake
    # PS3 firmware auto-install pure-logic unit (src/core/ps3/Ps3Firmware): dev_flash presence check,
    # Sony ps3-updatelist.txt parse, and the seam-injected check→fetch→download→installfw pipeline —
    # headless-probed without display, network, or process spawns. Qt6::Core only.
    add_executable(probe_ps3firmware tools/probe_ps3firmware.cpp
        src/core/ps3/Ps3Firmware.cpp)
    target_include_directories(probe_ps3firmware PRIVATE src src/core)
    target_link_libraries(probe_ps3firmware PRIVATE Qt6::Core)
```

- [ ] **Step 3: Build — expect FAILURE (Ps3Firmware.h doesn't exist yet)**

Run: `cmake --build build --config Release --target probe_ps3firmware --parallel`
Expected: compile error, `Ps3Firmware.h: No such file or directory` (this is the TDD red).

- [ ] **Step 4: Write the implementation**

Create `native/src/core/ps3/Ps3Firmware.h`:

```cpp
#pragma once
#include <QByteArray>
#include <QString>
#include <functional>
#include <optional>

// PS3 firmware auto-install for RPCS3. RPCS3 cannot boot any game without Sony's console firmware
// (dev_flash), and a fresh auto-downloaded RPCS3 has none — historically a manual one-time user step.
// This unit detects a missing/incomplete dev_flash, locates the official PS3UPDAT.PUP via Sony's public
// updatelist feed, and drives a headless `rpcs3.exe --installfw` — with every external effect (network,
// disk download, process spawn) injected as a seam so the pipeline is probe-testable offline.
namespace Ps3Firmware {

struct Info {
    QString version; // SystemSoftwareVersion, e.g. "4.9200" (may be empty if the feed omits it)
    QString url;     // absolute http(s) url of PS3UPDAT.PUP, from the CDN= field
};

// Firmware present? True iff <binDir>/dev_flash/vsh/etc/version.txt exists and is non-empty — the file
// RPCS3 itself reads to display the installed firmware version (RPCS3 is portable on Windows, so
// dev_flash lives next to the exe). An empty file counts as incomplete: better to reinstall than to
// boot into RPCS3's missing-firmware error.
bool installed(const QString& binDir);

// Parses Sony's ps3-updatelist.txt: one record per line, each a run of ;-separated Key=Value fields.
// Returns version + url from the first line carrying an http(s) CDN= field (other lines are
// compatibility records without one); nullopt when no line qualifies (or the body is empty/garbage).
std::optional<Info> parseUpdateList(const QByteArray& body);

using FeedFetcher = std::function<std::optional<QByteArray>()>;
using Downloader  = std::function<bool(const QString& url, const QString& destPath)>;
using Installer   = std::function<int(const QString& rpcs3Exe, const QString& pupPath)>;
using Progress    = std::function<void(const QString& message)>;

// The whole pipeline: if firmware is already installed, does nothing. Otherwise fetch the updatelist,
// parse it, download the PUP into tmpDir, run the installer, delete the PUP, and report success only
// if dev_flash actually appeared (an installer exit code alone is not proof). Any failure returns
// false — callers treat that as "boot anyway" (RPCS3 then shows its own missing-firmware error).
bool maybeInstall(const QString& binDir, const QString& rpcs3Exe, const QString& tmpDir,
                  const FeedFetcher& fetch, const Downloader& download,
                  const Installer& install, const Progress& progress);

} // namespace Ps3Firmware
```

Create `native/src/core/ps3/Ps3Firmware.cpp`:

```cpp
#include "core/ps3/Ps3Firmware.h"

#include <QDir>
#include <QFile>
#include <QStringList>

namespace Ps3Firmware {

bool installed(const QString& binDir)
{
    QFile f(binDir + QStringLiteral("/dev_flash/vsh/etc/version.txt"));
    return f.exists() && f.size() > 0;
}

std::optional<Info> parseUpdateList(const QByteArray& body)
{
    const QStringList lines = QString::fromUtf8(body).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines)
    {
        QString version, url;
        const QStringList fields = line.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString& field : fields)
        {
            const int eq = field.indexOf(QLatin1Char('='));
            if (eq <= 0) continue;
            const QString key = field.left(eq).trimmed();
            const QString val = field.mid(eq + 1).trimmed();
            if (key == QLatin1String("SystemSoftwareVersion")) version = val;
            else if (key == QLatin1String("CDN"))              url = val;
        }
        if (url.startsWith(QLatin1String("http://")) || url.startsWith(QLatin1String("https://")))
            return Info{ version, url };
    }
    return std::nullopt;
}

bool maybeInstall(const QString& binDir, const QString& rpcs3Exe, const QString& tmpDir,
                  const FeedFetcher& fetch, const Downloader& download,
                  const Installer& install, const Progress& progress)
{
    if (installed(binDir)) return false;

    const auto body = fetch ? fetch() : std::nullopt;
    if (!body || body->trimmed().isEmpty()) return false;

    const auto info = parseUpdateList(*body);
    if (!info) return false;

    if (progress)
        progress(info->version.isEmpty()
                     ? QStringLiteral("Installing PS3 firmware…")
                     : QStringLiteral("Installing PS3 firmware… v%1").arg(info->version));

    QDir().mkpath(tmpDir);
    const QString pup = QDir(tmpDir).filePath(QStringLiteral("PS3UPDAT.PUP"));
    bool ok = download && download(info->url, pup);
    if (ok) ok = install && install(rpcs3Exe, pup) == 0;
    QFile::remove(pup); // the PUP is only a means to dev_flash — never leave ~230MB behind, pass or fail

    return ok && installed(binDir);
}

} // namespace Ps3Firmware
```

- [ ] **Step 5: Build and run the probe — expect PASS**

Run: `cmake --build build --config Release --target probe_ps3firmware --parallel`
Then: `build/Release/probe_ps3firmware.exe`
Expected: prints `PS3FIRMWARE-OK`, exit 0.

- [ ] **Step 6: Wire the probe into the runner and CI (the other two of the three places)**

1. `native/tools/run-headless-probes.sh` (~line 321): in the long `for p in …` list, append one entry after `"probe_ps3update PS3UPDATE-OK"` (inside the list, before `; do`):
   `"probe_ps3firmware PS3FIRMWARE-OK"`
2. `.github/workflows/ci.yml` "Build probes" step (~line 63): append ` probe_ps3firmware` to the end of the `--target` list (after `probe_ps3update`).

Then verify the script still parses: `bash -n native/tools/run-headless-probes.sh` (expected: silence, exit 0).

- [ ] **Step 7: Mutation-prove the probe's assertions**

Write a spec JSON **outside the repo tree** (e.g. `build/ps3fw-mutants.json` — `build/` is git-ignored):

```jsonc
{
  "build":    ["cmake", "--build", "build", "--config", "Release", "--target", "probe_ps3firmware", "--parallel"],
  "test":     ["build/Release/probe_ps3firmware.exe"],
  "artifact": "build/Release/probe_ps3firmware.exe",
  "sentinel": "PS3FIRMWARE-OK",
  "mutants": [
    { "name": "installed-accepts-empty-version-txt",
      "file": "native/src/core/ps3/Ps3Firmware.cpp",
      "find": "return f.exists() && f.size() > 0;",
      "replace": "return f.exists();",
      "count": 1, "expect": "killed" },
    { "name": "parse-ignores-cdn-key",
      "file": "native/src/core/ps3/Ps3Firmware.cpp",
      "find": "else if (key == QLatin1String(\"CDN\"))              url = val;",
      "replace": "else if (key == QLatin1String(\"CDNX\"))              url = val;",
      "count": 1, "expect": "killed" },
    { "name": "parse-accepts-non-http-url",
      "file": "native/src/core/ps3/Ps3Firmware.cpp",
      "find": "if (url.startsWith(QLatin1String(\"http://\")) || url.startsWith(QLatin1String(\"https://\")))",
      "replace": "if (!url.isEmpty())",
      "count": 1, "expect": "killed" },
    { "name": "maybeinstall-reinstalls-over-existing",
      "file": "native/src/core/ps3/Ps3Firmware.cpp",
      "find": "    if (installed(binDir)) return false;",
      "replace": "    if (false) return false;",
      "count": 1, "expect": "killed" },
    { "name": "maybeinstall-leaks-pup",
      "file": "native/src/core/ps3/Ps3Firmware.cpp",
      "find": "    QFile::remove(pup); // the PUP is only a means to dev_flash — never leave ~230MB behind, pass or fail",
      "replace": "    ;",
      "count": 1, "expect": "killed" },
    { "name": "maybeinstall-trusts-exit-code",
      "file": "native/src/core/ps3/Ps3Firmware.cpp",
      "find": "    return ok && installed(binDir);",
      "replace": "    return ok;",
      "count": 1, "expect": "killed" },
    { "name": "maybeinstall-ignores-installer-failure",
      "file": "native/src/core/ps3/Ps3Firmware.cpp",
      "find": "    if (ok) ok = install && install(rpcs3Exe, pup) == 0;",
      "replace": "    if (ok) ok = install && install(rpcs3Exe, pup) >= 0;",
      "count": 1, "expect": "killed" }
  ]
}
```

Run: `python native/tools/mutate.py --spec build/ps3fw-mutants.json`
Expected: all 7 mutants `KILLED`, exit 0. Any `SURVIVED`/`NOT APPLIED`: fix the assertion (or the find string) and re-run. Do not delete assertions to get green.

- [ ] **Step 8: Re-run the probe green, then commit**

Run: `build/Release/probe_ps3firmware.exe` → `PS3FIRMWARE-OK` (the mutation driver's final restore-build should have left a clean binary; this confirms).

```bash
git add native/src/core/ps3/Ps3Firmware.h native/src/core/ps3/Ps3Firmware.cpp \
        native/tools/probe_ps3firmware.cpp native/CMakeLists.txt \
        native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: PS3 firmware detect/parse/install unit + probe_ps3firmware"
```

(Do NOT commit the mutation spec JSON.)

---

### Task 2: Wire firmware auto-install into the RPCS3 launch flow

**Files:**
- Modify: `native/src/core/EmulatorManager.cpp` (include block ~line 35; comment ~line 931; launch gate ~lines 1248–1257; anon-namespace fetch helper ~lines 1289–1320; `runPs3UpdateThenLaunch` ~lines 1363–1423)
- Modify: `native/src/core/EmulatorManager.h` (doc comment on `runPs3UpdateThenLaunch`, line ~86)
- Modify: `native/CMakeLists.txt` (app source list, the block near lines 456–457 that lists the ps3 sources)

**Interfaces:**
- Consumes from Task 1: everything under `Ps3Firmware::` exactly as declared in Task 1's header (`installed`, `maybeInstall`, the four `using` seams).
- Produces: no new external interface; behavior change only.

**Requirements (verbatim from the spec):**
- Firmware step runs on the SAME worker thread as the game-update step, BEFORE it.
- Firmware step runs for every RPCS3 launch (not gated by `Settings::ps3AutoUpdate()`); the game-update step stays gated by `Settings::ps3AutoUpdate() && !rom.isEmpty()`.
- Read `Settings::ps3AutoUpdate()` on the UI thread and capture the bool (QSettings is not for cross-thread use).
- Reuse `downloadPs3Pkg` unchanged for the PUP download. Extract the shared body of `fetchPs3VerXml` into `fetchSonyTextFeed(url)` rather than duplicating the 25-line network dance.
- `--installfw` runs bounded exactly like `--installpkg`: `waitForStarted(30000)`, `waitForFinished(600000)`, kill + `waitForFinished(5000)` on timeout, return -1.
- All failures fall through to `finishLocalLaunch` (result of `maybeInstall` ignored).

- [ ] **Step 1: Add `Ps3Firmware.cpp/.h` to the app target's source list**

In `native/CMakeLists.txt`, in the app source list where the other ps3 sources sit (lines ~456–457), add one line beside them:

```cmake
        src/core/ps3/Ps3Firmware.cpp          src/core/ps3/Ps3Firmware.h
```

- [ ] **Step 2: Include the new header in `EmulatorManager.cpp`**

Next to the existing ps3 include (~line 35):

```cpp
#include "core/ps3/Ps3Firmware.h"          // auto-installs Sony's PS3UPDAT.PUP into RPCS3's dev_flash pre-boot
```

- [ ] **Step 3: Extract `fetchSonyTextFeed` and add `fetchPs3UpdateList`**

In the anon namespace (~line 1289), rework `fetchPs3VerXml` so the network body is shared. The existing body of `fetchPs3VerXml` (from `QNetworkAccessManager nam;` through `return out;`) moves VERBATIM into the new helper — only the `url` local is replaced by the parameter:

```cpp
// Synchronous HTTPS GET of a small Sony text feed with peer verification DISABLED (these endpoints'
// certificate CNs do not match their hosts, so the default handshake would fail). Runs on the calling
// worker thread via a local event loop, keeping the UI thread off the network. NoError -> body; any
// transport/HTTP error -> nullopt.
std::optional<QByteArray> fetchSonyTextFeed(const QString& url)
{
    QNetworkAccessManager nam;
    QNetworkRequest req{ QUrl(url) };
    // …the rest is the existing fetchPs3VerXml body, unchanged (UA header, 15s transfer timeout,
    // VerifyNone + ignoreSslErrors, 20s hard watchdog QTimer, local QEventLoop, NoError -> readAll)…
}

// The per-title update feed (empty body is Sony's "no updates" signal, handled by the coordinator).
std::optional<QByteArray> fetchPs3VerXml(const QString& titleId)
{
    return fetchSonyTextFeed(
        QStringLiteral("https://a0.ww.np.dl.playstation.net/tpl/np/%1/%1-ver.xml").arg(titleId));
}

// The console-firmware update list: one ;-separated record per line, the CDN= field carrying the
// PS3UPDAT.PUP url. Same endpoint family and trust model as ver.xml. This feed offers no hash — RPCS3
// validates the PUP internally on --installfw, so a corrupt download fails the install and the boot
// falls through to RPCS3's own missing-firmware error.
std::optional<QByteArray> fetchPs3UpdateList()
{
    return fetchSonyTextFeed(
        QStringLiteral("https://fus01.ps3.update.playstation.net/update/ps3/list/us/ps3-updatelist.txt"));
}
```

Keep the existing comment block above `fetchPs3VerXml` with the moved body (adjust wording from "Sony's ver.xml feed" to the generic helper as shown). Do not change `downloadPs3Pkg`.

- [ ] **Step 4: Widen the launch gate**

Replace the gate at ~lines 1248–1257 (comment + condition) with:

```cpp
    // RPCS3 only: before booting, make sure the PS3 console firmware is installed (auto-installing Sony's
    // official PS3UPDAT.PUP if dev_flash is missing) and install the game's official Sony update PKG chain
    // so the player runs the patched game with no manual step. Both run on a worker thread (real network +
    // process I/O), then the normal local-launch continuation runs on the UI thread. Every failure inside
    // falls through to a plain boot — this path never prevents a launch. `program` is the RPCS3 binary here
    // (the Flatpak sentinel returned above), so it doubles as the `--installfw` / `--installpkg` executable.
    if (em_.id == QStringLiteral("rpcs3"))
    {
        runPs3UpdateThenLaunch(program, args, binDir);
        return;
    }
```

- [ ] **Step 5: Add the firmware step to `runPs3UpdateThenLaunch`**

Replace the function body (~lines 1367–1423) with:

```cpp
void EmulatorManager::runPs3UpdateThenLaunch(const QString& program, const QStringList& args, const QString& binDir)
{
    const QString rom       = rom_;
    const QString rpcs3Exe  = program;
    const QString tmpDir    = binDir + QStringLiteral("/.eb-ps3-updates");
    const QString statePath = AppPaths::dataDir() + QStringLiteral("/ps3-updates.json");
    // Game updates keep their opt-out; the firmware step below has none (without firmware NOTHING boots).
    // Read the setting here on the UI thread — QSettings is not for cross-thread use — the worker only
    // sees the captured bool.
    const bool gameUpdates = Settings::ps3AutoUpdate() && !rom.isEmpty();

    if (gameUpdates) emit status(tr("Checking for PS3 game updates…"), -1);

    // Seed RPCS3's first-run config NOW, before the worker runs `--installfw` / `--installpkg`. On a fresh
    // RPCS3 install that is RPCS3's genuine first run, and without this seed its "Welcome to RPCS3" modal
    // (whose Exit button quits the app) would pop and block the bounded process wait for its full timeout.
    // The seed is idempotent (seed-if-absent), so finishLocalLaunch calling it again later is harmless.
    prepareFirstRunConfig(binDir);

    // Guard the cross-thread progress marshal against the manager being destroyed mid-update: the
    // queued lambda checks the QPointer before touching `this`.
    QPointer<EmulatorManager> self(this);
    QThread* worker = QThread::create([self, rom, rpcs3Exe, binDir, tmpDir, statePath, gameUpdates] {
        // Transient progress notes from both steps, marshalled to the UI thread via the existing status()
        // signal — but only if the manager is still alive (QPointer captured by value).
        auto note = [self](const QString& msg) {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, msg] { if (self) emit self->status(msg, -1); },
                                      Qt::QueuedConnection);
        };

        // Firmware first: RPCS3 cannot boot anything without dev_flash, and a fresh auto-downloaded
        // install has none. Result ignored — a failed fetch/download/install falls through and RPCS3
        // shows its own missing-firmware error, exactly like a failed game update falls through to an
        // unpatched boot.
        Ps3Firmware::maybeInstall(binDir, rpcs3Exe, tmpDir,
            [] { return fetchPs3UpdateList(); },
            [](const QString& url, const QString& dest) { return downloadPs3Pkg(url, dest); },
            [](const QString& exe, const QString& pup) {
                // Bounded run instead of QProcess::execute (which waits with no timeout): if the
                // installer wedges — e.g. on an unexpected modal — kill it after 10 min and return a
                // non-zero code so maybeInstall fails cleanly and the game still boots.
                QProcess proc;
                proc.start(exe, { QStringLiteral("--installfw"), pup });
                if (!proc.waitForStarted(30000)) return -1;
                if (!proc.waitForFinished(600000)) { proc.kill(); proc.waitForFinished(5000); return -1; }
                return proc.exitCode();
            },
            note);

        if (!gameUpdates) return;

        Ps3UpdateState state(statePath);
        Ps3UpdateInstaller installer(
            rpcs3Exe, tmpDir,
            [](const QString& url, const QString& dest) { return downloadPs3Pkg(url, dest); },
            [](const QString& exe, const QString& pkg) {
                // Bounded run instead of QProcess::execute (which waits with no timeout): if the
                // installer wedges — e.g. on an unexpected modal — kill it after 10 min and return a
                // non-zero code so installAll aborts cleanly and the game still boots.
                QProcess proc;
                proc.start(exe, { QStringLiteral("--installpkg"), pkg });
                if (!proc.waitForStarted(30000)) return -1;
                if (!proc.waitForFinished(600000)) { proc.kill(); proc.waitForFinished(5000); return -1; }
                return proc.exitCode();
            });
        Ps3UpdateCoordinator coord(
            [](const QString& p) { return Ps3TitleId::read(p); },
            [](const QString& titleId) { return fetchPs3VerXml(titleId); },
            &state, &installer, note);
        coord.maybeUpdate(rom); // result ignored — always fall through to a boot
    });
    // The thread frees itself when it finishes, regardless of the manager's lifetime — so if the
    // manager is destroyed mid-update (the continuation below auto-disconnects) the QThread doesn't leak.
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    // finished() is emitted from the worker; delivered queued to the UI thread, where the launch must
    // run. Bound to `this` as context so a destroyed manager correctly skips it (no boot after teardown).
    connect(worker, &QThread::finished, this, [this, program, args, binDir] {
        finishLocalLaunch(program, args, binDir);
    });
    worker->start();
}
```

Also update the function's lead comment (above ~line 1363) — it currently says "Run the PS3 auto-update pipeline"; make it:

```cpp
// Run the PS3 pre-boot pipeline (console-firmware auto-install, then the game's update-PKG chain) for the
// RPCS3 rom on a worker thread, then finish the normal launch on the UI thread. Informational only: every
// internal failure falls through — the game always boots (worst case into RPCS3's own firmware error).
// The worker thread has no Qt event loop of its own, but each seam spins a local QEventLoop for its
// network wait, so QNetworkAccessManager works there.
```

- [ ] **Step 6: Update the two stale comments**

1. `EmulatorManager.cpp` ~line 931, inside `prepareFirstRunConfig`'s rpcs3 branch: replace
   `// (PS3 firmware is still required to actually boot games — a separate one-time user step.)`
   with
   `// (PS3 firmware is auto-installed just before launch — see Ps3Firmware::maybeInstall in the launch path.)`
2. `EmulatorManager.h` line ~86: extend the doc comment on `runPs3UpdateThenLaunch` to mention the firmware step (whatever one-liner sits there now, make it cover "firmware auto-install + game update PKG chain, never blocks the boot").

- [ ] **Step 7: Build the app and both ps3 probes, run them**

```bash
cmake --build build --config Release --target EverythingBox probe_ps3update probe_ps3firmware --parallel
build/Release/probe_ps3update.exe
build/Release/probe_ps3firmware.exe
```

Expected: clean build (grep the output for `error`), `PS3UPDATE-OK`, `PS3FIRMWARE-OK`.

- [ ] **Step 8: Commit**

```bash
git add native/src/core/EmulatorManager.cpp native/src/core/EmulatorManager.h native/CMakeLists.txt
git commit -m "feat: auto-install PS3 firmware before RPCS3 boots a game"
```

---

## Post-task verification (controller, not a task)

1. Full Release rebuild of everything + grep for errors (harnesses-report-success rule).
2. Full headless gate: `BUILD_DIR=build bash native/tools/run-headless-probes.sh` → `ALL HEADLESS PROBES PASSED` (must include `probe_ps3firmware PS3FIRMWARE-OK` in its output — verify it RAN, not just that the suite passed).
3. Final whole-branch review (Fable), fix wave if needed.
4. Merge to main + push origin (no finish-branch menu — standing memory), deploy Release `EverythingBox.exe` to `C:\EverythingBox-app` (close the running app first; standing autonomy).
