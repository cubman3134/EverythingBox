# Preferred content language — Increment 1 (client) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Promote the client's subtitle-language setting into a general "Preferred content language" stored canonically (ISO-639-1 two-letter), driving mpv subtitle **and** audio track selection, and sent as `Accept-Language` on our own server's addon requests.

**Architecture:** A new header-only `LanguageCodes` provides the canonical mapping (3→2 migration, 2→mpv list). `Settings` gains `preferredLanguage()` over a new synced key `content/language`, migrated once from the legacy `subs/language`; the old subtitle accessors delegate to it. mpv's `slang`/`alang` and the `FILE_LOADED` language check use the canonical value; `AddonManager` sends `Accept-Language`; both settings builders move the control into a general "Language" section. Client-only.

**Tech Stack:** C++17 / Qt 6, mpv (libmpv), headless `probe_*` tests.

## Global Constraints

- **Canonical form = ISO-639-1 two-letter, lowercased; empty = "no preference".** Consumers map outward (mpv `"en,eng"`, later ranking names). Migrate `subs/language` (3-letter) once.
- **Soft preference, never a filter** — an empty/absent preference behaves exactly as today.
- **Both settings builders.** Any user-facing setting must appear in BOTH the themed (`ThemedPanelHost` `choice`) and classic (`QComboBox`) builders in `MainWindow.cpp` — the ROMs-folder precedent. Miss one = unreachable.
- **A new probe must be registered in THREE places** or it silently never runs: `native/CMakeLists.txt` (`add_executable` + links), `native/tools/run-headless-probes.sh` (the run loop), `.github/workflows/ci.yml` (the `--target` list). (CONTRIBUTING.md:288-305.)
- **Shared tree + version hook.** Stage by explicit path; never `git add -A`. Don't hand-edit the version lines in `native/CMakeLists.txt`/`native/src/main.cpp` — the pre-commit hook owns them (it may add them to your commit; that's expected).
- **No AI attribution** in commits.
- **Accept-Language goes only to OUR server** (the non-Stremio / our-provider request sites), never to third-party Stremio addons — matching exactly where `AppBrand::kConfigHeader` ("X-EB-Config") is already sent.

---

### Task 1: `LanguageCodes` + canonical `Settings` accessor + migration, with a probe

**Files:**
- Create: `native/src/core/LanguageCodes.h`
- Modify: `native/src/core/Settings.h` (declare `preferredLanguage`/`setPreferredLanguage`), `native/src/core/Settings.cpp` (define them + migration; delegate the old subtitle accessors)
- Create: `native/tools/probe_contentlang.cpp`
- Modify: `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml` (register the probe)

**Interfaces:**
- Produces: `namespace LanguageCodes { QString toCanonical(const QString&); QString toMpvLangList(const QString&); }` (header-only); `QString Settings::preferredLanguage(); void Settings::setPreferredLanguage(const QString&);` (key `content/language`, canonical 2-letter). `Settings::subtitleLanguage()`/`setSubtitleLanguage()` now delegate to these.

- [ ] **Step 1: Create `native/src/core/LanguageCodes.h`** (header-only, pure):

```cpp
#pragma once
#include <QString>
#include <QHash>
#include <QLatin1Char>

// Canonical language handling for the "preferred content language" setting.
// Canonical form = ISO-639-1 two-letter, lowercased. Empty = "no preference".
namespace LanguageCodes {

// ISO-639-2 (B and T variants) -> ISO-639-1, for the languages the settings UI offers.
inline const QHash<QString, QString>& iso3to1()
{
    static const QHash<QString, QString> m = {
        {QStringLiteral("eng"),QStringLiteral("en")},{QStringLiteral("spa"),QStringLiteral("es")},
        {QStringLiteral("fra"),QStringLiteral("fr")},{QStringLiteral("fre"),QStringLiteral("fr")},
        {QStringLiteral("deu"),QStringLiteral("de")},{QStringLiteral("ger"),QStringLiteral("de")},
        {QStringLiteral("ita"),QStringLiteral("it")},{QStringLiteral("por"),QStringLiteral("pt")},
        {QStringLiteral("nld"),QStringLiteral("nl")},{QStringLiteral("dut"),QStringLiteral("nl")},
        {QStringLiteral("rus"),QStringLiteral("ru")},{QStringLiteral("jpn"),QStringLiteral("ja")},
        {QStringLiteral("kor"),QStringLiteral("ko")},{QStringLiteral("zho"),QStringLiteral("zh")},
        {QStringLiteral("chi"),QStringLiteral("zh")},{QStringLiteral("ara"),QStringLiteral("ar")},
    };
    return m;
}

// ISO-639-1 -> the ISO-639-2 tag mpv most often sees on a track, so slang/alang match either.
inline const QHash<QString, QString>& iso1to3()
{
    static const QHash<QString, QString> m = {
        {QStringLiteral("en"),QStringLiteral("eng")},{QStringLiteral("es"),QStringLiteral("spa")},
        {QStringLiteral("fr"),QStringLiteral("fra")},{QStringLiteral("de"),QStringLiteral("deu")},
        {QStringLiteral("it"),QStringLiteral("ita")},{QStringLiteral("pt"),QStringLiteral("por")},
        {QStringLiteral("nl"),QStringLiteral("nld")},{QStringLiteral("ru"),QStringLiteral("rus")},
        {QStringLiteral("ja"),QStringLiteral("jpn")},{QStringLiteral("ko"),QStringLiteral("kor")},
        {QStringLiteral("zh"),QStringLiteral("zho")},{QStringLiteral("ar"),QStringLiteral("ara")},
    };
    return m;
}

// Any code (2- or 3-letter, any case) -> canonical 2-letter. Empty stays empty.
inline QString toCanonical(const QString& code)
{
    const QString c = code.trimmed().toLower();
    if (c.isEmpty()) return QString();
    if (c.size() == 2) return c;
    const auto it = iso3to1().constFind(c);
    return it != iso3to1().constEnd() ? it.value() : c.left(2);
}

// Canonical 2-letter -> mpv slang/alang list, e.g. "en" -> "en,eng". Empty stays empty.
inline QString toMpvLangList(const QString& canonical)
{
    const QString c = canonical.trimmed().toLower();
    if (c.isEmpty()) return QString();
    const auto it = iso1to3().constFind(c);
    return it != iso1to3().constEnd() ? (c + QLatin1Char(',') + it.value()) : c;
}

} // namespace LanguageCodes
```

- [ ] **Step 2: Declare the accessors** in `native/src/core/Settings.h` — right after the existing subtitle-language declarations (`:20-25`), add:

```cpp
    // The preferred CONTENT language (canonical ISO-639-1 two-letter, e.g. "en"; empty = no
    // preference). Governs subtitle + audio track selection and is sent to our server as
    // Accept-Language. Migrated once from the legacy 3-letter "subs/language".
    QString preferredLanguage();
    void setPreferredLanguage(const QString& code);
```

- [ ] **Step 3: Define them + migrate + delegate** in `native/src/core/Settings.cpp`. Add `#include "LanguageCodes.h"` near the other includes. Replace the existing `subtitleLanguage()`/`setSubtitleLanguage()` bodies (`:43-52`) and add the new pair:

```cpp
QString Settings::preferredLanguage()
{
    auto& s = store();
    QString cur = s.value(QStringLiteral("content/language")).toString();
    if (cur.isEmpty())
    {
        // One-shot migration from the legacy subtitle-only 3-letter key.
        const QString legacy = s.value(QStringLiteral("subs/language")).toString();
        if (!legacy.isEmpty())
        {
            cur = LanguageCodes::toCanonical(legacy);
            s.setValue(QStringLiteral("content/language"), cur);
            s.sync();
        }
    }
    return cur;
}

void Settings::setPreferredLanguage(const QString& code)
{
    store().setValue(QStringLiteral("content/language"), LanguageCodes::toCanonical(code));
    store().sync();
}

// Back-compat: the old subtitle-language accessors now read/write the unified content language.
QString Settings::subtitleLanguage() { return preferredLanguage(); }
void Settings::setSubtitleLanguage(const QString& code) { setPreferredLanguage(code); }
```

- [ ] **Step 4: Write the probe** `native/tools/probe_contentlang.cpp`. Migration must run FIRST (it writes the legacy key via a raw `QSettings` on the same ini path, before the `Settings::store()` singleton is first constructed):

```cpp
// Headless probe: canonical language mapping + the content/language setting migration.
#include "LanguageCodes.h"
#include "Settings.h"
#include "AppPaths.h"
#include "AppBrand.h"
#include <QCoreApplication>
#include <QSettings>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; \
    std::fprintf(stderr, "CONTENTLANG-FAIL %s (line %d)\n", #cond, __LINE__); } } while (0)

// MUST run before any Settings:: call, so the store() singleton reads the file with the legacy
// key already present and performs the one-shot migration.
static void testMigrationRunsFirst()
{
    const QString ini = AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);
    { QSettings s(ini, QSettings::IniFormat); s.setValue(QStringLiteral("subs/language"), QStringLiteral("spa")); s.sync(); }
    CHECK(Settings::preferredLanguage() == QStringLiteral("es"));   // migrated 3->2 on first read
}

static void testRoundTrip()
{
    Settings::setPreferredLanguage(QStringLiteral("fr"));
    CHECK(Settings::preferredLanguage() == QStringLiteral("fr"));
    Settings::setPreferredLanguage(QStringLiteral("spa"));          // canonicalized on write
    CHECK(Settings::preferredLanguage() == QStringLiteral("es"));
    Settings::setPreferredLanguage(QString());                     // empty = no preference
    CHECK(Settings::preferredLanguage().isEmpty());
    // Legacy alias still reflects the unified value.
    Settings::setSubtitleLanguage(QStringLiteral("de"));
    CHECK(Settings::subtitleLanguage() == QStringLiteral("de"));
    CHECK(Settings::preferredLanguage() == QStringLiteral("de"));
}

static void testCanonical()
{
    CHECK(LanguageCodes::toCanonical(QStringLiteral("spa")) == QStringLiteral("es"));
    CHECK(LanguageCodes::toCanonical(QStringLiteral("SPA")) == QStringLiteral("es"));
    CHECK(LanguageCodes::toCanonical(QStringLiteral("fre")) == QStringLiteral("fr"));
    CHECK(LanguageCodes::toCanonical(QStringLiteral("en")) == QStringLiteral("en"));
    CHECK(LanguageCodes::toCanonical(QString()).isEmpty());
    CHECK(LanguageCodes::toCanonical(QStringLiteral("xyz")) == QStringLiteral("xy")); // unknown 3 -> left2
}

static void testMpvList()
{
    CHECK(LanguageCodes::toMpvLangList(QStringLiteral("en")) == QStringLiteral("en,eng"));
    CHECK(LanguageCodes::toMpvLangList(QStringLiteral("es")) == QStringLiteral("es,spa"));
    CHECK(LanguageCodes::toMpvLangList(QString()).isEmpty());
    CHECK(LanguageCodes::toMpvLangList(QStringLiteral("xy")) == QStringLiteral("xy")); // unknown 2 -> as-is
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testMigrationRunsFirst();   // first — see comment above
    testRoundTrip();
    testCanonical();
    testMpvList();
    if (failures == 0) std::printf("CONTENTLANG-OK\n");
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 5: Register the probe (place 1 of 3) — `native/CMakeLists.txt`.** Copy the `add_executable(probe_audioout ...)` block (around `:911-927`) verbatim, rename the target and the source to `probe_contentlang` / `tools/probe_contentlang.cpp`, and add `src/core/LanguageCodes.h` to its source list. Keep the SAME `src/core/Settings.cpp`, `AppPaths`, `CloudSync`, `third_party/miniz.c`, the `MINIZ_NO_ZLIB_COMPATIBLE_NAMES` define, the `target_include_directories(... PRIVATE src src/core ...)` and `target_link_libraries(... PRIVATE Qt6::Core Qt6::Network Qt6::Gui)` — `Settings.cpp` pulls in CloudSync/AppPaths/miniz, so mirroring probe_audioout's deps exactly is required. (Do NOT touch the version lines the pre-commit hook manages.)

- [ ] **Step 6: Register the probe (place 2 of 3) — `native/tools/run-headless-probes.sh`.** Add `probe_contentlang CONTENTLANG-OK` to the `for p in ...` loop near `:321` (same shape as `probe_audioout AUDIOOUT-OK`).

- [ ] **Step 7: Register the probe (place 3 of 3) — `.github/workflows/ci.yml`.** Add `probe_contentlang` to the `--target` list in the "Build probes" step (near `:62-63`), alongside `probe_audioout`.

- [ ] **Step 8: Build + run the probe — expect PASS**

Build just this probe (configure once if needed), then run it:
```bash
cmake --build native/build --target probe_contentlang
./native/build/probe_contentlang   # (or native/build/Release/probe_contentlang.exe on Windows)
```
Expected: prints `CONTENTLANG-OK`, exit 0. (First run it should FAIL to build before Steps 1-3 exist, confirming the test drives the code.)

- [ ] **Step 9: Run the full headless probe suite** to confirm the new probe is wired and nothing regressed:
```bash
bash native/tools/run-headless-probes.sh
```
Expected: `probe_contentlang` reported PASS, suite green.

- [ ] **Step 10: Commit**
```bash
git add native/src/core/LanguageCodes.h native/src/core/Settings.h native/src/core/Settings.cpp \
        native/tools/probe_contentlang.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh \
        .github/workflows/ci.yml
git commit -m "feat: canonical preferred-content-language setting + LanguageCodes mapping (probe-tested)"
```

---

### Task 2: Wire the client consumers — mpv slang/alang + Accept-Language

**Files:**
- Modify: `native/src/video/MpvWidget.cpp` (slang+alang from the canonical value; fix the `FILE_LOADED` language compare)
- Modify: `native/src/addons/AddonManager.cpp` (send `Accept-Language` to our server)

**Interfaces:**
- Consumes: `LanguageCodes::toCanonical`, `LanguageCodes::toMpvLangList`, `Settings::preferredLanguage()` (Task 1).

- [ ] **Step 1: mpv slang + alang.** In `native/src/video/MpvWidget.cpp`, add `#include "core/LanguageCodes.h"` (match the existing include style for `Settings.h`). Replace the slang block (`:421-422`):

```cpp
    const QString lang = Settings::preferredLanguage().trimmed();
    if (!lang.isEmpty())
    {
        const QByteArray list = LanguageCodes::toMpvLangList(lang).toUtf8();
        mpv_set_option_string(mpv, "slang", list.constData());  // preferred subtitle language
        mpv_set_option_string(mpv, "alang", list.constData());  // preferred audio language (new)
    }
```

- [ ] **Step 2: Fix the `FILE_LOADED` language compare** (`:322` and `:343`) so it works with the canonical value (the old `.left(2)` compare assumed 3-letter on both sides and would break now that the stored value is 2-letter). Change the `want` computation and the per-track compare:

```cpp
        const QString want = LanguageCodes::toCanonical(Settings::preferredLanguage());
        // ... inside the track loop, replace the wantSub test:
                if (!want.isEmpty() && LanguageCodes::toCanonical(l) == want) wantSub = true;
```
(`l` is the track's `track-list/N/lang`; canonicalizing both sides makes `"spa"` match `"es"`, etc.)

- [ ] **Step 3: Send `Accept-Language` to our server.** In `native/src/addons/AddonManager.cpp`, ensure `#include "core/Settings.h"` is present (add if missing). Add a small file-static helper next to `remoteConfigHeader` (`:288`):

```cpp
// Headers attached only to OUR OWN server (non-Stremio) addon requests: the per-caller config
// blob and the preferred content language. Never sent to third-party Stremio addons.
static void applyServerHeaders(QNetworkRequest& rq, const LoadedAddon* src)
{
    const QByteArray cfg = remoteConfigHeader(src);
    if (!cfg.isEmpty()) rq.setRawHeader(AppBrand::kConfigHeader, cfg);
    const QString lang = Settings::preferredLanguage();
    if (!lang.isEmpty()) rq.setRawHeader("Accept-Language", lang.toUtf8());
}
```
Then replace the five inline config-header sites so they route through it:
  - `:1098` (catalog): `if (!stremio) applyServerHeaders(rq, src);`
  - `:1145` (detail): `if (!stremio) applyServerHeaders(rq, src);`
  - `:1187` (meta): `if (!stremio) applyServerHeaders(rq, src);`
  - `:1733` (doc-bridge search): `applyServerHeaders(rq, prov);`
  - `:1793` (stream resolve): `applyServerHeaders(rq, src);`
(Leave the blocking helper at `:193` and the unrelated TorBox `Authorization` / update `If-None-Match` headers untouched.)

- [ ] **Step 4: Build the app + rerun the probe suite.** No new unit test — the mapping/value logic is already covered by `probe_contentlang`; this task is wiring that cannot be exercised headlessly (mpv track selection, a live network request).
```bash
cmake --build native/build --target EverythingBox   # app builds clean
bash native/tools/run-headless-probes.sh            # still green (no regressions)
```
Expected: app compiles; suite green.

- [ ] **Step 5: Integration check against the running server** (proves the header actually goes out). With the local server running, watch its request log while the client makes a request — or, as a wire-level stand-in, confirm the server accepts and logs `Accept-Language` (the server forwards it to sources in Increments 2-3). A quick manual stand-in:
```bash
curl -s -H "Accept-Language: es" "http://localhost:7000/catalog/manga:manga/search=Naruto.json" -o /dev/null -w "%{http_code}\n"
```
Expected: `200` (the header is accepted; manga does not yet consume it — that's Increment 2). Note in the commit that end-to-end track selection is GUI-verified.

- [ ] **Step 6: Commit**
```bash
git add native/src/video/MpvWidget.cpp native/src/addons/AddonManager.cpp
git commit -m "feat: drive mpv slang+alang from preferred language and send Accept-Language to our server"
```

---

### Task 3: Move the control into a general "Language" section — both builders

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (themed builder: relocate + relabel + rewire the language `choice`; classic builder: same for the `QComboBox`; both to canonical 2-letter and `preferredLanguage`)

**Interfaces:**
- Consumes: `Settings::preferredLanguage()` / `setPreferredLanguage()` (Task 1).

- [ ] **Step 1: Themed builder — 2-letter table + general placement.** In `openGeneralSettings()`'s themed arm, change the language table (`:12249-12258`) to canonical 2-letter codes and update the current-value lookup to `preferredLanguage()`:

```cpp
        const QList<QPair<QString, QString>> langs = {
            { tr("Any / no preference"), QString() }, { QStringLiteral("English"), QStringLiteral("en") },
            { QStringLiteral("Spanish"), QStringLiteral("es") }, { QStringLiteral("French"), QStringLiteral("fr") },
            { QStringLiteral("German"), QStringLiteral("de") }, { QStringLiteral("Italian"), QStringLiteral("it") },
            { QStringLiteral("Portuguese"), QStringLiteral("pt") }, { QStringLiteral("Dutch"), QStringLiteral("nl") },
            { QStringLiteral("Russian"), QStringLiteral("ru") }, { QStringLiteral("Japanese"), QStringLiteral("ja") },
            { QStringLiteral("Korean"), QStringLiteral("ko") }, { QStringLiteral("Chinese"), QStringLiteral("zh") },
            { QStringLiteral("Arabic"), QStringLiteral("ar") },
        };
        const QString curLang = Settings::preferredLanguage();
```
(The rest of the display/custom round-trip block at `:12260-12270` is unchanged — it already derives `curLangDisp`/`langOpts`/`langOptPairs` from `langs`+`curLang`.)

- [ ] **Step 2: Themed — move the row out of "Subtitles" into a new "Language" section.** Delete the `choice(QStringLiteral("subs.lang"), ...)` line from the Subtitles section (`:12729`). Add a new section right after the Display section (`sep(tr("Display"))` at `:12551` and its rows), before the next section:

```cpp
        sep(tr("Language"));
        choice(QStringLiteral("content.lang"), tr("Preferred content language"), langOpts, curLangDisp);
```
Keep the `sep(tr("Subtitles"))` + `toggle(subs.on, ...)` where they are (just without the language choice).

- [ ] **Step 3: Themed — update the write handler.** In the id-dispatch (`:13089-13094`), replace the `subs.lang` branch with a `content.lang` branch that writes `preferredLanguage`:

```cpp
                else if (id == QStringLiteral("content.lang")) {
                    QString code = val;
                    for (const auto& p : langOptPairs) if (p.first == val) { code = p.second; break; }
                    Settings::setPreferredLanguage(code);
                }
```

- [ ] **Step 4: Classic builder — 2-letter table, general placement, rewire, ungate.** In the classic `showPanel(tr("General"), ...)` arm: change the language table (`:14003-14010`) to the SAME 2-letter list as Step 1; read current via `Settings::preferredLanguage()`; save via `setPreferredLanguage`; and MOVE the `langRow` block (`:13999-14024`) out of the Subtitles section (`:13989+`) to just after the Display section (before "Attract mode" at `:13305`), under its own heading. Remove the subtitle-checkbox gating (`lang->setEnabled(on->isChecked());` and the `connect(on, &QCheckBox::toggled, lang, &QComboBox::setEnabled);`). The relocated block:

```cpp
        auto* langHeading = new QLabel(tr("Language"));
        langHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(langHeading);
        auto* langRow = new QHBoxLayout();
        langRow->addWidget(new QLabel(tr("Preferred content language:")));
        auto* lang = new QComboBox();
        lang->setMinimumHeight(34);
        const QList<QPair<QString, QString>> langs = {
            { tr("Any / no preference"), QString() }, { QStringLiteral("English"), QStringLiteral("en") },
            { QStringLiteral("Spanish"), QStringLiteral("es") }, { QStringLiteral("French"), QStringLiteral("fr") },
            { QStringLiteral("German"), QStringLiteral("de") }, { QStringLiteral("Italian"), QStringLiteral("it") },
            { QStringLiteral("Portuguese"), QStringLiteral("pt") }, { QStringLiteral("Dutch"), QStringLiteral("nl") },
            { QStringLiteral("Russian"), QStringLiteral("ru") }, { QStringLiteral("Japanese"), QStringLiteral("ja") },
            { QStringLiteral("Korean"), QStringLiteral("ko") }, { QStringLiteral("Chinese"), QStringLiteral("zh") },
            { QStringLiteral("Arabic"), QStringLiteral("ar") },
        };
        const QString cur = Settings::preferredLanguage();
        bool found = false;
        for (const auto& l : langs) { lang->addItem(l.first, l.second); if (l.second == cur) found = true; }
        if (!found && !cur.isEmpty()) lang->addItem(tr("%1 (custom)").arg(cur), cur);
        lang->setCurrentIndex(qMax(0, lang->findData(cur)));
        connect(lang, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [lang](int idx) { Settings::setPreferredLanguage(lang->itemData(idx).toString()); });
        langRow->addWidget(lang, 1);
        v->addLayout(langRow);
```
(Delete the original `langs`/`langRow`/gating block from the Subtitles section so it is not duplicated.)

- [ ] **Step 5: Build + rerun the probe suite.**
```bash
cmake --build native/build --target EverythingBox
bash native/tools/run-headless-probes.sh
```
Expected: compiles; suite green.

- [ ] **Step 6: GUI verification** (both surfaces show the control and it persists). Per the app's UITEST harness (`EB_UITEST=1` + `native/tools/uitest.py`): open Settings → General in each builder, confirm a "Language → Preferred content language" row exists (not under Subtitles), pick "Spanish", reopen, confirm it reads "Spanish", and confirm the INI has `content/language=es`. If a settings-surface probe exists that enumerates rows, extend it to assert the `content.lang`/Language row in both builders instead of manual-only.

- [ ] **Step 7: Commit**
```bash
git add native/src/ui/MainWindow.cpp
git commit -m "feat: General > Language 'Preferred content language' control in both settings builders"
```

---

## Self-review

**Spec coverage (Increment 1):** promote language control to General, both builders (spec) → Task 3. Canonical 2-letter + migration from `subs/language` (spec) → Task 1. Drive mpv `slang` + `alang` (spec) → Task 2 Steps 1-2. Send `Accept-Language` on our server's requests (spec) → Task 2 Step 3. Mapping `en`→`"en,eng"` (spec) → `LanguageCodes::toMpvLangList`, Task 1. Probe covering get/set + migration + mapping (spec) → Task 1 probe (3-place registration). GUI/both-builders verification (spec, "two builders" rule) → Task 3 Step 6. ✅

**Placeholder scan:** every code step is complete; the CMake step points at the real `probe_audioout` block to mirror (exact deps), not a vague "add build config"; the FILE_LOADED trap is fixed explicitly (Task 2 Step 2); the untestable-headless parts (mpv track pick, live header) are called out with concrete build + integration + GUI checks, not hand-waved.

**Type consistency:** `LanguageCodes::toCanonical`/`toMpvLangList` and `Settings::preferredLanguage`/`setPreferredLanguage` are defined in Task 1 and consumed unchanged in Tasks 2-3. Key `content/language` is the single stored key throughout; `subtitleLanguage()` delegates (Task 1) so any residual caller stays correct. Both builders use the identical 2-letter `langs` table and `setPreferredLanguage` writer. The sentinel `CONTENTLANG-OK` matches between the probe `main()` and the runner registration.
