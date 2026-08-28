# Riivolution disc composition — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Install a Wii file-replacement mod by composing a patched disc image offline, so the hack lands in the ROMs folder as an ordinary game.

**Architecture:** Four offline stages — extract the base disc, overlay the mod tree per its Riivolution XML, compose a new disc image, install it. Composition is done by a subprocess built from the Dolphin source RetroPark already vendors; no encryption, hash-tree, FST or container code is written here.

**Tech Stack:** C++17, Qt 6 (QProcess, QDir, QXmlStreamReader), MSBuild/VS2022 for the tool, headless probes for tests.

**Spec:** `docs/superpowers/specs/2026-08-28-riivolution-disc-composition-design.md`

## Global Constraints

- **This is the PUBLIC client repo. Never name a content source** in code, comments, commit messages or docs. The mod and the base game may be named; where it was distributed from may not.
- **No AI attribution** in commits — no `Co-Authored-By`, no "Generated with", no tool name. Conventional prefixes (`feat:`/`fix:`/`docs:`/`test:`) apply.
- **The working tree is SHARED with concurrent sessions.** Commit only your own paths, by pathspec (`git commit -- <paths>`). Never `git add -A`. A `pre-commit` hook bumps the version in `native/CMakeLists.txt` and `native/src/main.cpp` and will join your commit — that is expected and is not another session's work.
- **Byte-exact edits.** `native/tools/run-headless-probes.sh` is **CRLF**; `native/CMakeLists.txt` contains a lone CR at byte offset 7082. Editing either with a tool that normalises line endings breaks them silently. Append only, matching surrounding bytes.
- **A new probe must be registered in all three places** or it never runs: `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml`.
- **After every pathspec commit, clear the index for the two hook-touched files:**
  `git reset HEAD -- native/CMakeLists.txt native/src/main.cpp`. `git commit -- <pathspec>` commits from
  the working tree WITHOUT updating the index, so the pre-commit hook's staged version bump stays behind
  and presents as a staged *revert* of that bump — which the next session's commit would silently carry.
  This has already happened twice in this arc; it is a property of the hook plus a shared tree, not a
  mistake anyone made.
- **The base ROM is never modified.** Every stage reads it; nothing writes it.
- Never run a target-less `cmake --build build`. Always name a `--target`.
- A comment states what was **measured** versus assumed, and never describes only its successes.

---

## File Structure

**Created (client, `native/src/core/`):**
- `RiivolutionPatch.h` / `.cpp` — XML → a list of overlay operations. Pure; no I/O.
- `DiscOverlay.h` / `.cpp` — applies operations to an extracted tree. File operations only.
- `DiscCompose.h` / `.cpp` — the subprocess wrapper: extract, compose, staging, free-space, cleanup.

**Created (tests):**
- `native/tools/probe_riivolution.cpp` — covers all three units.

**Created (RetroPark, separate repo):**
- `patches/dolphin-2606-discio-directory-input.patch` — the one-hunk DiscIO fix.
- `docs/dolphin-disc-tool.md` — build recipe for the patched `DolphinTool.exe`.

**Modified (client):**
- `native/CMakeLists.txt` — probe target.
- `native/tools/run-headless-probes.sh` — runner list.
- `.github/workflows/ci.yml` — build target list.
- `native/src/ui/MainWindow.cpp` — the romhack install flow gains the compose branch.

---

### Task 1: The patched disc tool

**Repo:** `C:/Users/cubma/source/repos/RetroPark` (NOT the client repo).

**Files:**
- Create: `patches/dolphin-2606-discio-directory-input.patch`
- Create: `docs/dolphin-disc-tool.md`

**Interfaces:**
- Produces: a `DolphinTool.exe` whose `convert` accepts a **directory** as `-i`. Later tasks shell out to it as `<tool> extract -i <disc> -o <dir> -g -q` and `<tool> convert -i <dir> -o <out.rvz> -f rvz -c zstd -l 5 -b 131072`.

**Background you need.** `external/dolphin/` is **git-ignored** — the source tree is not committed, so the fix ships as a patch file plus a recipe, exactly like `docs/dolphin-build.md`. The tree is pinned to Dolphin tag **`2606`** (commit `6094cfcf7b`).

The defect: `DiscIO::CreateBlobReader` already knows how to open an extracted disc — `DirectoryBlobReader::Create(filename)` is in its `default:` branch — but the function opens the path as a file and reads a 4-byte magic *first*, so a directory returns `nullptr` before the switch is reached. Measured: `DolphinTool convert -i <dir>` prints `Error: The input file could not be opened.`

An earlier version of this plan added "while `Dolphin.exe -b -e <dir>` boots that same directory". That claim is **RETRACTED** — see the spec. Its only evidence was a GUI process still alive eight seconds after launch, which a modal error dialog produces too, and `DirectoryBlobReader`'s own `IsValidDirectoryBlob` accepts only a path ending in `/sys/main.dol`, so a bare directory was never going to be accepted by any path. What is true and is what this task rests on: `DirectoryBlobReader` reads an extracted disc when handed that file, so the fix resolves a directory to it.

- [ ] **Step 1: Confirm the unpatched failure**

```bash
cd "C:/EverythingBox-app/emulators/dolphin/Dolphin-x64"
./DolphinTool.exe extract -i "C:/EverythingBox-app/roms/gc/The Legend of Zelda_ Collector's Edition.rvz" -o /c/Users/cubma/AppData/Local/Temp/dt/out -g -q
./DolphinTool.exe convert -i "C:\Users\cubma\AppData\Local\Temp\dt\out" -o "C:\Users\cubma\AppData\Local\Temp\dt\r.rvz" -f rvz -c zstd -l 5 -b 131072
```
Expected: extract succeeds and writes `sys/` + `files/`; convert prints `Error: The input file could not be opened.`

- [ ] **Step 2: Write the patch**

In `external/dolphin`, edit `Source/Core/DiscIO/Blob.cpp`. At the top of `CreateBlobReader`, before the `DirectIOFile` is opened, add:

```cpp
std::unique_ptr<BlobReader> CreateBlobReader(const std::string& filename)
{
  // An extracted disc is a DIRECTORY, and the magic-number read below opens the path as a file, so a
  // directory failed that open and returned nullptr before ever reaching the DirectoryBlobReader case
  // in the switch. That case has always been there; it was simply unreachable through this function.
  // Measured on tag 2606: `DolphinTool convert -i <dir>` printed "The input file could not be opened".
  // (An earlier draft added "while Dolphin.exe -b -e <dir> booted the same directory"; that claim is
  // retracted -- IsValidDirectoryBlob accepts only a path ending in /sys/main.dol, so no path took it.)
  if (File::IsDirectory(filename))
    return DirectoryBlobReader::Create(filename);

  File::DirectIOFile file(filename, File::AccessMode::Read);
```

Leave the rest of the function unchanged.

- [ ] **Step 3: Build the tool**

Use `-` switches, not `/` switches — Git Bash mangles `/m` into `M:/`. Build the `.vcxproj` directly; `msbuild dolphin-emu.sln -t:DolphinTool` fails with MSB4057.

```bash
cd "C:/Users/cubma/source/repos/RetroPark/external/dolphin"
MSBUILD="/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe"
"$MSBUILD" Source/Core/DolphinTool/DolphinTool.vcxproj \
  -p:Configuration=Release -p:Platform=x64 \
  -p:SolutionDir='C:\Users\cubma\source\repos\RetroPark\external\dolphin\Source\' \
  -m -v:minimal -clp:Summary
```
Expected: `Build succeeded.` and a `DolphinTool.exe` under `external/dolphin/Binary/x64/`.

- [ ] **Step 4: Verify the round trip — this is the task's real gate**

```bash
T=/c/Users/cubma/AppData/Local/Temp/dt
"C:/Users/cubma/source/repos/RetroPark/external/dolphin/Binary/x64/DolphinTool.exe" \
  convert -i "C:\Users\cubma\AppData\Local\Temp\dt\out" -o "C:\Users\cubma\AppData\Local\Temp\dt\r.rvz" \
  -f rvz -c zstd -l 5 -b 131072
ls -la "$T/r.rvz"
"C:/Users/cubma/source/repos/RetroPark/external/dolphin/Binary/x64/DolphinTool.exe" header -i "$T/r.rvz"
```
Expected: convert succeeds, `r.rvz` is non-empty, and `header` prints the SAME game id and title as `header -i` on the original `.rvz`. Record both header outputs in the report — a composed image that opens but reports a different game is a failure, not a pass.

- [ ] **Step 5: Save the patch and the recipe**

```bash
cd "C:/Users/cubma/source/repos/RetroPark/external/dolphin"
git diff Source/Core/DiscIO/Blob.cpp > ../../patches/dolphin-2606-discio-directory-input.patch
```

Write `docs/dolphin-disc-tool.md` recording: the pinned tag (`2606`, `6094cfcf7b`), the `git apply` command, the MSBuild line from Step 3 verbatim, both header outputs from Step 4, and the fact that `external/dolphin/` is git-ignored so this document is the reproducible record.

- [ ] **Step 6: Commit (RetroPark repo)**

```bash
cd "C:/Users/cubma/source/repos/RetroPark"
git add patches/dolphin-2606-discio-directory-input.patch docs/dolphin-disc-tool.md
git commit -m "feat: let the disc tool compose from an extracted directory

DiscIO::CreateBlobReader already had a DirectoryBlobReader branch; it was
unreachable because the function reads a 4-byte magic first and a directory
fails that open. Measured on tag 2606: convert refused the directory outright.
DirectoryBlobReader itself is entered at <dir>/sys/main.dol, so the fix resolves
one path to the other."
```

---

### Task 2: RiivolutionPatch — the XML to a list of operations

**Files:**
- Create: `native/src/core/RiivolutionPatch.h`, `native/src/core/RiivolutionPatch.cpp`
- Create: `native/tools/probe_riivolution.cpp`
- Modify: `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml`

**Interfaces:**
- Produces: `RiivolutionPatch::parse(const QByteArray&) -> Parsed`, consumed by Task 3 and Task 4.

**Background.** The real document (measured) declares `root="/SuperMarioGravity_Demo"` on a `<patch id="smgra">`, then twelve `<folder disc="/StageData" external="StageData" create="true"/>` mappings — seven of which alias different locale directories onto one `EuEnglish` source — plus one `<savegame>` element. `<option>`/`<choice>` wrap the patch id.

- [ ] **Step 1: Write the header**

Create `native/src/core/RiivolutionPatch.h`:

```cpp
// A Riivolution document reduced to the overlay it describes. Pure: no file system, no network.
//
// Riivolution is a runtime loader — it substitutes files as the game reads them. We are composing a disc
// instead, so only the parts of the format that describe FILE SUBSTITUTION can be honoured. The two that
// cannot are handled explicitly rather than ignored quietly:
//
//   <savegame> redirects saves at runtime and has no meaning in a composed disc. Ignored, and the fact is
//   reported in `savegameIgnored` so a caller can say so rather than pretend the document was fully applied.
//
//   <memory> writes to RAM while the game runs and CANNOT be represented in a disc image at all. A document
//   using it is REFUSED. Composing it would produce a disc that builds, boots, and is subtly wrong, which is
//   worse than not installing it — so the refusal is the feature, not a limitation to work around later.
//
// Options are refused rather than guessed: see `parse`.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

namespace RiivolutionPatch
{
    struct Op
    {
        enum Kind { Folder, File };
        Kind kind = Folder;
        QString discPath;      // where it goes on the disc, e.g. "/StageData"
        QString externalPath;  // where it comes from, relative to the patch root, e.g. "StageData"
        bool create = false;   // the document's create= attribute
    };

    struct Parsed
    {
        bool ok = false;
        QString refusal;           // non-empty iff !ok; names the element that caused it
        QString root;              // the <patch root=> value, e.g. "/SuperMarioGravity_Demo"
        QVector<Op> ops;
        bool savegameIgnored = false;
    };

    // Parses a Riivolution document. Refuses — ok=false, with `refusal` naming why — when the document
    // contains a <memory> patch, or when it offers a CHOICE we cannot make: more than one <option>, or an
    // <option> with more than one <choice>. There is no UI for choosing, and picking silently would install
    // a different mod from the one the user believes they chose. A single-option, single-choice document
    // (the shape measured on the mod at hand) composes without asking anything.
    Parsed parse(const QByteArray& xml);
}
```

- [ ] **Step 2: Write the failing probe**

Create `native/tools/probe_riivolution.cpp`:

```cpp
// Headless test for RiivolutionPatch, DiscOverlay and DiscCompose's pure parts.
//
// The XML fixtures here are cut down from the real document shipped by the mod this work targets, so the
// element shapes, attribute names and the locale ALIASING (seven disc folders sourced from one external
// folder) are the measured article rather than an invention.
//
// Mutation targets: drop the <memory> refusal and case 3 passes when it must fail; drop the multi-choice
// refusal and case 4 passes; drop the containment check in DiscOverlay and case 7 writes outside the tree.
//
// Prints RIIVOLUTION-OK on success; RIIVOLUTION-FAIL (nonzero exit) on any miss.
#include "../src/core/RiivolutionPatch.h"
#include <QByteArray>
#include <cstdio>

static int failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

static QByteArray realShapeXml()
{
    return QByteArray(
        "<wiidisc version=\"1\" root=\"\">\n"
        "  <id game=\"SB4\"><region type=\"P\"/></id>\n"
        "  <options><section name=\"Super Mario Gravity\">\n"
        "    <option name=\"Demo\"><choice name=\"Enabled\"><patch id=\"smgra\"/></choice></option>\n"
        "  </section></options>\n"
        "  <patch id=\"smgra\" root=\"/SuperMarioGravity_Demo\">\n"
        "    <savegame external=\"SaveGame/{$__gameid}\" clone=\"false\"/>\n"
        "    <folder disc=\"/StageData\" external=\"StageData\" create=\"true\"/>\n"
        "    <folder disc=\"/LocalizeData/EuFrench\" external=\"LocalizeData/EuEnglish\" create=\"true\"/>\n"
        "  </patch>\n"
        "</wiidisc>\n");
}

int main()
{
    // 1. The measured shape parses, and the patch root is carried through.
    {
        const auto p = RiivolutionPatch::parse(realShapeXml());
        check(p.ok, "1: the real document shape parses");
        check(p.refusal.isEmpty(), "1: a parse that succeeded states no refusal");
        check(p.root == QStringLiteral("/SuperMarioGravity_Demo"), "1: patch root is carried through");
        check(p.ops.size() == 2, "1: both folder mappings survive");
    }

    // 2. Locale ALIASING: two different disc paths may share one external path. A map keyed by external
    //    path would silently collapse these, and six of the mod's twelve mappings would vanish.
    {
        const auto p = RiivolutionPatch::parse(realShapeXml());
        check(p.ops.size() == 2, "2: aliased mappings are not collapsed");
        check(p.ops[1].discPath == QStringLiteral("/LocalizeData/EuFrench"), "2: alias disc path kept");
        check(p.ops[1].externalPath == QStringLiteral("LocalizeData/EuEnglish"), "2: alias source kept");
    }

    // 3. <memory> is REFUSED, not ignored. A RAM patch cannot exist in a composed disc.
    {
        const auto p = RiivolutionPatch::parse(QByteArray(
            "<wiidisc version=\"1\"><options><section name=\"s\"><option name=\"o\">"
            "<choice name=\"c\"><patch id=\"p\"/></choice></option></section></options>"
            "<patch id=\"p\" root=\"/m\"><memory offset=\"0x80000000\" value=\"60000000\"/></patch></wiidisc>"));
        check(!p.ok, "3: a document with <memory> is refused");
        check(p.refusal.contains(QStringLiteral("memory")), "3: the refusal names the element");
    }

    // 4. A document that offers a real choice is refused: there is no UI to choose with, and choosing
    //    silently installs a different mod from the one the user thinks they picked.
    {
        const auto p = RiivolutionPatch::parse(QByteArray(
            "<wiidisc version=\"1\"><options><section name=\"s\"><option name=\"o\">"
            "<choice name=\"a\"><patch id=\"p\"/></choice>"
            "<choice name=\"b\"><patch id=\"q\"/></choice></option></section></options>"
            "<patch id=\"p\" root=\"/m\"/><patch id=\"q\" root=\"/n\"/></wiidisc>"));
        check(!p.ok, "4: a multi-choice document is refused");
        check(p.refusal.contains(QStringLiteral("choice")), "4: the refusal says a choice was needed");
    }

    // 5. <savegame> is ignored, and the caller is TOLD it was ignored.
    {
        const auto p = RiivolutionPatch::parse(realShapeXml());
        check(p.ok, "5: <savegame> does not fail the parse");
        check(p.savegameIgnored, "5: the caller is told <savegame> was dropped");
    }

    // 6. Malformed input is a refusal, never a crash and never a silent empty success.
    {
        const auto p = RiivolutionPatch::parse(QByteArray("<wiidisc><patch"));
        check(!p.ok, "6: malformed XML is refused");
        check(!p.refusal.isEmpty(), "6: a refusal always states a reason");
    }

    if (failures == 0) { std::printf("RIIVOLUTION-OK\n"); return 0; }
    std::fprintf(stderr, "RIIVOLUTION-FAIL (%d)\n", failures);
    return 1;
}
```

- [ ] **Step 3: Register the probe in all three places**

**(a)** In `native/CMakeLists.txt`, immediately after the `add_executable(probe_archiverom …)` block ending at line 2516, add — matching the surrounding indentation exactly:

```cmake
    add_executable(probe_riivolution tools/probe_riivolution.cpp
        src/core/RiivolutionPatch.cpp src/core/RiivolutionPatch.h)
    target_link_libraries(probe_riivolution PRIVATE Qt6::Core)
```

This block lists only the sources that exist at this task. Tasks 3 and 4 each extend it as they add a unit — naming a file CMake cannot find fails at configure time, before any compile error you were hoping to see.

**(b)** In `native/tools/run-headless-probes.sh`, in the `for p in …` list at line 676, append `"probe_riivolution RIIVOLUTION-OK"` immediately after `"probe_launchcontexts LAUNCHCONTEXTS-OK"`, before the closing `; do`. **This file is CRLF — do not let your editor rewrite the line.**

**(c)** In `.github/workflows/ci.yml` line 68, append ` probe_riivolution` to the end of the `--target` list.

Verify all three took:
```bash
grep -c probe_riivolution native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
bash -n native/tools/run-headless-probes.sh
file native/tools/run-headless-probes.sh
```
Expected: counts `1`, `1`, `1`; `bash -n` silent; `file` still reports CRLF line terminators.

- [ ] **Step 4: Build and watch it fail**

```bash
cmake --build build --config Release --target probe_riivolution
```
Expected: FAILS to compile — `RiivolutionPatch.cpp` does not exist yet.

- [ ] **Step 5: Implement the parser**

Create `native/src/core/RiivolutionPatch.cpp`:

```cpp
#include "RiivolutionPatch.h"
#include <QXmlStreamReader>

namespace
{
    RiivolutionPatch::Parsed refuse(const QString& why)
    {
        RiivolutionPatch::Parsed p;
        p.ok = false;
        p.refusal = why;
        return p;
    }
}

RiivolutionPatch::Parsed RiivolutionPatch::parse(const QByteArray& xml)
{
    Parsed out;
    QXmlStreamReader r(xml);

    int optionCount = 0;
    int maxChoicesInAnOption = 0;
    int choicesInCurrentOption = 0;
    bool sawPatch = false;

    while (!r.atEnd())
    {
        r.readNext();
        if (r.hasError()) break;
        if (!r.isStartElement()) continue;

        const QStringView name = r.name();

        if (name == QLatin1String("option"))
        {
            ++optionCount;
            choicesInCurrentOption = 0;
        }
        else if (name == QLatin1String("choice"))
        {
            ++choicesInCurrentOption;
            if (choicesInCurrentOption > maxChoicesInAnOption)
                maxChoicesInAnOption = choicesInCurrentOption;
        }
        else if (name == QLatin1String("memory"))
        {
            // Refused, not ignored: a RAM patch cannot be baked into a disc image, and a disc composed
            // without it would boot and misbehave with nothing to say why.
            return refuse(QStringLiteral("this mod uses a <memory> patch, which changes the game while it "
                                         "runs and cannot be written into a disc image"));
        }
        else if (name == QLatin1String("savegame"))
        {
            out.savegameIgnored = true;
        }
        else if (name == QLatin1String("patch"))
        {
            // A <patch id=.../> REFERENCE inside a <choice> carries no root; the definition does.
            const auto attrs = r.attributes();
            if (attrs.hasAttribute(QLatin1String("root")))
            {
                sawPatch = true;
                out.root = attrs.value(QLatin1String("root")).toString();
            }
        }
        else if (name == QLatin1String("folder") || name == QLatin1String("file"))
        {
            const auto attrs = r.attributes();
            Op op;
            op.kind = (name == QLatin1String("folder")) ? Op::Folder : Op::File;
            op.discPath = attrs.value(QLatin1String("disc")).toString();
            op.externalPath = attrs.value(QLatin1String("external")).toString();
            op.create = attrs.value(QLatin1String("create")).toString() == QLatin1String("true");
            // Appended in document order and never keyed by either path: the measured document maps SEVEN
            // different disc folders onto one external folder, so a map keyed by external path would drop
            // six of them and the mod would install missing most of its localisations.
            if (!op.discPath.isEmpty() && !op.externalPath.isEmpty()) out.ops.append(op);
        }
    }

    if (r.hasError())
        return refuse(QStringLiteral("this mod's Riivolution file could not be read: %1").arg(r.errorString()));
    if (!sawPatch)
        return refuse(QStringLiteral("this mod's Riivolution file declares no patch to apply"));
    if (optionCount > 1 || maxChoicesInAnOption > 1)
        return refuse(QStringLiteral("this mod offers a choice of options, and there is no way to ask which "
                                     "one you want yet"));

    out.ok = true;
    return out;
}
```

- [ ] **Step 6: Build and run the probe**

```bash
cmake --build build --config Release --target probe_riivolution
./build/Release/probe_riivolution.exe
```
Expected: `RIIVOLUTION-OK`, exit 0.

- [ ] **Step 7: Mutation-check the two refusals**

Comment out the `<memory>` refusal, rebuild, run: case 3 must fail. Restore. Change `maxChoicesInAnOption > 1` to `> 99`, rebuild, run: case 4 must fail. Restore, rebuild, confirm `RIIVOLUTION-OK`. Report both.

- [ ] **Step 8: Commit**

```bash
git add native/src/core/RiivolutionPatch.h native/src/core/RiivolutionPatch.cpp \
        native/tools/probe_riivolution.cpp
git commit -m "feat: read a Riivolution file as the overlay it describes

Refuses a <memory> patch outright rather than composing a disc that boots and
is quietly wrong, and refuses a document offering a choice rather than picking
one silently. <savegame> is ignored and the caller is told so." -- \
        native/src/core/RiivolutionPatch.h native/src/core/RiivolutionPatch.cpp \
        native/tools/probe_riivolution.cpp native/CMakeLists.txt \
        native/tools/run-headless-probes.sh .github/workflows/ci.yml
```

---

### Task 3: DiscOverlay — apply the operations to an extracted tree

**Files:**
- Create: `native/src/core/DiscOverlay.h`, `native/src/core/DiscOverlay.cpp`
- Modify: `native/tools/probe_riivolution.cpp` (add cases 7–9)

**Interfaces:**
- Consumes: `RiivolutionPatch::Op`, `RiivolutionPatch::Parsed` from Task 2.
- Produces: `DiscOverlay::apply(...) -> Result`, consumed by Task 4.

**Background.** `DolphinTool extract -g` writes a Wii disc as `<root>/DATA/files/…` plus `<root>/DATA/sys/…`; a GameCube disc extracts as `<root>/files` + `<root>/sys`. The mod's own patcher script targets `ISOfiles\DATA\files`, which is the same layout. A disc path of `/StageData` therefore lands at `<root>/DATA/files/StageData`.

- [ ] **Step 1: Write the header**

Create `native/src/core/DiscOverlay.h`:

```cpp
// Applies a Riivolution overlay to a disc tree that DolphinTool already extracted.
//
// Every path is CONTAINED: an `external` or `disc` attribute comes out of an archive we did not build, so
// a mapping of disc="/../../Windows" must be refused rather than obeyed. Containment is checked on the
// RESOLVED, canonical path, because "a/../../b" only escapes once resolved.
#pragma once
#include "RiivolutionPatch.h"
#include <QString>

namespace DiscOverlay
{
    struct Result
    {
        bool ok = false;
        QString error;      // non-empty iff !ok
        int filesWritten = 0;
    };

    // `discRoot` is the directory DolphinTool extracted into. `modRoot` is the directory holding the mod's
    // replacement tree. `parsed.root` names the subdirectory of `modRoot` the operations are relative to.
    // Returns ok=false, writing nothing further, on the first operation that cannot be applied safely.
    Result apply(const QString& discRoot, const QString& modRoot, const RiivolutionPatch::Parsed& parsed);

    // Where a disc path lands inside an extracted tree: the Wii layout ("DATA/files") when discRoot has a
    // DATA directory, the GameCube layout ("files") otherwise. Exposed for the probe.
    QString discFilesRoot(const QString& discRoot);
}
```

- [ ] **Step 2: Extend the probe target with this task's unit**

In `native/CMakeLists.txt`, extend the `probe_riivolution` block added in Task 2 to read:

```cmake
    add_executable(probe_riivolution tools/probe_riivolution.cpp
        src/core/RiivolutionPatch.cpp src/core/RiivolutionPatch.h
        src/core/DiscOverlay.cpp     src/core/DiscOverlay.h)
    target_link_libraries(probe_riivolution PRIVATE Qt6::Core)
```

- [ ] **Step 3: Add the failing probe cases**

Append to `native/tools/probe_riivolution.cpp`, before the final `if (failures == 0)`. Add `#include "../src/core/DiscOverlay.h"`, `#include <QDir>`, `#include <QFile>`, `#include <QTemporaryDir>` at the top:

```cpp
    // 7. CONTAINMENT: a disc path that climbs out of the tree is refused, and nothing is written outside it.
    {
        QTemporaryDir tmp;
        const QString discRoot = tmp.filePath(QStringLiteral("disc"));
        const QString modRoot  = tmp.filePath(QStringLiteral("mod"));
        QDir().mkpath(discRoot + QStringLiteral("/DATA/files"));
        QDir().mkpath(modRoot + QStringLiteral("/m/Evil"));
        QFile f(modRoot + QStringLiteral("/m/Evil/x.bin"));
        f.open(QIODevice::WriteOnly); f.write("x"); f.close();

        RiivolutionPatch::Parsed p;
        p.ok = true;
        p.root = QStringLiteral("/m");
        RiivolutionPatch::Op op;
        op.kind = RiivolutionPatch::Op::Folder;
        op.discPath = QStringLiteral("/../../escaped");
        op.externalPath = QStringLiteral("Evil");
        op.create = true;
        p.ops.append(op);

        const auto r = DiscOverlay::apply(discRoot, modRoot, p);
        check(!r.ok, "7: a disc path escaping the tree is refused");
        check(!QFile::exists(tmp.filePath(QStringLiteral("escaped/x.bin"))), "7: nothing was written outside");
    }

    // 8. The Wii layout is chosen by what the extraction actually produced, not assumed.
    {
        QTemporaryDir tmp;
        QDir().mkpath(tmp.filePath(QStringLiteral("wii/DATA/files")));
        QDir().mkpath(tmp.filePath(QStringLiteral("gc/files")));
        check(DiscOverlay::discFilesRoot(tmp.filePath(QStringLiteral("wii")))
                  .endsWith(QStringLiteral("DATA/files")), "8: a Wii tree resolves to DATA/files");
        check(DiscOverlay::discFilesRoot(tmp.filePath(QStringLiteral("gc")))
                  .endsWith(QStringLiteral("files")), "8: a GameCube tree resolves to files");
    }

    // 9. A real overlay lands where the disc path says, and REPLACES an existing file — the mod's whole
    //    purpose. A copy that refused to overwrite would leave the stock game with extra files beside it.
    {
        QTemporaryDir tmp;
        const QString discRoot = tmp.filePath(QStringLiteral("disc"));
        const QString modRoot  = tmp.filePath(QStringLiteral("mod"));
        QDir().mkpath(discRoot + QStringLiteral("/DATA/files/LayoutData"));
        QDir().mkpath(modRoot + QStringLiteral("/m/LayoutData"));
        QFile stock(discRoot + QStringLiteral("/DATA/files/LayoutData/TitleLogo.arc"));
        stock.open(QIODevice::WriteOnly); stock.write("STOCK"); stock.close();
        QFile mod(modRoot + QStringLiteral("/m/LayoutData/TitleLogo.arc"));
        mod.open(QIODevice::WriteOnly); mod.write("MODDED"); mod.close();

        RiivolutionPatch::Parsed p;
        p.ok = true;
        p.root = QStringLiteral("/m");
        RiivolutionPatch::Op op;
        op.kind = RiivolutionPatch::Op::Folder;
        op.discPath = QStringLiteral("/LayoutData");
        op.externalPath = QStringLiteral("LayoutData");
        op.create = true;
        p.ops.append(op);

        const auto r = DiscOverlay::apply(discRoot, modRoot, p);
        check(r.ok, "9: a well-formed overlay applies");
        check(r.filesWritten == 1, "9: one file was written");
        QFile back(discRoot + QStringLiteral("/DATA/files/LayoutData/TitleLogo.arc"));
        back.open(QIODevice::ReadOnly);
        check(back.readAll() == QByteArray("MODDED"), "9: the mod's file REPLACED the stock one");
    }
```

- [ ] **Step 4: Build and watch it fail**

```bash
cmake --build build --config Release --target probe_riivolution
```
Expected: FAILS to compile — `DiscOverlay.cpp` does not exist.

- [ ] **Step 5: Implement the overlay**

Create `native/src/core/DiscOverlay.cpp`:

```cpp
#include "DiscOverlay.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

namespace
{
    // True when `child` is inside `root` once both are resolved. Resolution is the point: "a/../../b" only
    // escapes after it is resolved, so comparing the unresolved strings would accept it.
    bool contained(const QString& root, const QString& child)
    {
        const QString r = QDir::cleanPath(QDir(root).absolutePath()) + QLatin1Char('/');
        const QString c = QDir::cleanPath(QDir(child).absolutePath());
        return c.startsWith(r, Qt::CaseInsensitive);
    }

    bool copyOver(const QString& from, const QString& to, int* written, QString* error)
    {
        QDir().mkpath(QFileInfo(to).absolutePath());
        if (QFile::exists(to) && !QFile::remove(to))
        {
            *error = QStringLiteral("could not replace %1").arg(to);
            return false;
        }
        if (!QFile::copy(from, to))
        {
            *error = QStringLiteral("could not write %1").arg(to);
            return false;
        }
        ++*written;
        return true;
    }
}

QString DiscOverlay::discFilesRoot(const QString& discRoot)
{
    // Decided by what the extraction produced rather than by the system we think we are patching: a Wii
    // disc extracts under DATA/, a GameCube disc does not.
    const QString wii = discRoot + QStringLiteral("/DATA/files");
    if (QFileInfo(wii).isDir()) return wii;
    return discRoot + QStringLiteral("/files");
}

DiscOverlay::Result DiscOverlay::apply(const QString& discRoot, const QString& modRoot,
                                       const RiivolutionPatch::Parsed& parsed)
{
    Result out;
    const QString filesRoot = discFilesRoot(discRoot);
    if (!QFileInfo(filesRoot).isDir())
    {
        out.error = QStringLiteral("the extracted disc has no files directory at %1").arg(filesRoot);
        return out;
    }

    QString patchRoot = modRoot;
    if (!parsed.root.isEmpty())
        patchRoot = modRoot + QLatin1Char('/') + parsed.root.mid(parsed.root.startsWith(QLatin1Char('/')) ? 1 : 0);

    for (const auto& op : parsed.ops)
    {
        const QString src = patchRoot + QLatin1Char('/') + op.externalPath;
        const QString dst = filesRoot + QLatin1Char('/')
                            + op.discPath.mid(op.discPath.startsWith(QLatin1Char('/')) ? 1 : 0);

        // Both ends are checked. The source comes out of an archive we did not build and the destination
        // is built from an attribute in that same archive, so either can be shaped to escape.
        if (!contained(modRoot, src) || !contained(filesRoot, dst))
        {
            out.error = QStringLiteral("this mod tries to write outside the game's own files (%1)")
                            .arg(op.discPath);
            return out;
        }

        if (!QFileInfo(src).exists())
        {
            // Not an error: a document may map a folder the distribution does not ship.
            continue;
        }

        if (op.kind == RiivolutionPatch::Op::File)
        {
            if (!copyOver(src, dst, &out.filesWritten, &out.error)) return out;
            continue;
        }

        QDirIterator it(src, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString one = it.next();
            const QString rel = QDir(src).relativeFilePath(one);
            const QString target = dst + QLatin1Char('/') + rel;
            if (!contained(filesRoot, target))
            {
                out.error = QStringLiteral("this mod tries to write outside the game's own files (%1)").arg(rel);
                return out;
            }
            if (!copyOver(one, target, &out.filesWritten, &out.error)) return out;
        }
    }

    out.ok = true;
    return out;
}
```

- [ ] **Step 6: Build and run**

```bash
cmake --build build --config Release --target probe_riivolution
./build/Release/probe_riivolution.exe
```
Expected: `RIIVOLUTION-OK`, exit 0.

- [ ] **Step 7: Mutation-check containment**

Make `contained` `return true;`, rebuild, run: case 7 must fail (`a disc path escaping the tree is refused`). Restore, rebuild, confirm `RIIVOLUTION-OK`. Report the mutation and the case that caught it.

- [ ] **Step 8: Commit**

```bash
git add native/src/core/DiscOverlay.h native/src/core/DiscOverlay.cpp native/tools/probe_riivolution.cpp
git commit -m "feat: overlay a mod's files onto an extracted disc tree

Both ends of every mapping are containment-checked on the resolved path: the
attributes come out of an archive we did not build, so either end can be shaped
to climb out of the tree." -- \
        native/src/core/DiscOverlay.h native/src/core/DiscOverlay.cpp native/tools/probe_riivolution.cpp
```

---

### Task 4: DiscCompose — the subprocess wrapper

**Files:**
- Create: `native/src/core/DiscCompose.h`, `native/src/core/DiscCompose.cpp`
- Modify: `native/tools/probe_riivolution.cpp` (add cases 10–11)
- Modify: `native/CMakeLists.txt` (add `DiscCompose.cpp` to the probe target's sources)

**Interfaces:**
- Consumes: `DiscOverlay::apply`, `RiivolutionPatch::parse`.
- Produces: `DiscCompose::composePatchedDisc(...) -> Outcome`, consumed by Task 5.

**Background.** Measured free space on the target machine was **6.7 GB**, against a base game stored as a 1.4 GB archive. The decompressed disc, the extracted tree and the output image must coexist, so the estimate must be checked before any work starts.

- [ ] **Step 1: Write the header**

Create `native/src/core/DiscCompose.h`:

```cpp
// Composing a patched disc image, by driving the disc tool as a subprocess.
//
// Nothing here knows how a Wii disc is encrypted, hashed or laid out. The tool is built from the same
// Dolphin source the app boots discs with, so composition is done by the code that already reads them.
//
// Staging lives OUTSIDE the ROMs folder. A half-made disc inside it would be found by the library scan and
// shown as a playable game, which is the one outcome this must never produce.
#pragma once
#include <QString>

namespace DiscCompose
{
    struct Outcome
    {
        bool ok = false;
        QString error;        // non-empty iff !ok; phrased for the user
        QString outputPath;   // the composed image, when ok
    };

    // Bytes that must be free before starting: the extracted tree is about the size of the disc, and the
    // composed image is at most that again. Measured deliberately as a MULTIPLE of the source rather than a
    // fixed number, because the two disc generations this covers differ by an order of magnitude in size.
    qint64 requiredFreeBytes(qint64 discBytes);

    // True when `dir`'s volume has at least `needed` bytes free.
    bool hasFreeSpace(const QString& dir, qint64 needed);

    // Extract `discPath`, overlay the mod at `modRoot`, compose to `outputPath`. `toolPath` is the disc
    // tool. `stagingParent` must not be inside the ROMs folder. The base ROM is only ever read.
    Outcome composePatchedDisc(const QString& toolPath, const QString& discPath, const QString& modRoot,
                               const QByteArray& riivolutionXml, const QString& outputPath,
                               const QString& stagingParent);
}
```

- [ ] **Step 2: Add the failing probe cases**

Append to `native/tools/probe_riivolution.cpp` (add `#include "../src/core/DiscCompose.h"`):

```cpp
    // 10. The space estimate scales with the disc, and always demands more than the disc itself — an
    //     estimate that did not could pass and then run out mid-compose, which is the failure this exists
    //     to prevent.
    {
        const qint64 fourGiB = 4LL * 1024 * 1024 * 1024;
        check(DiscCompose::requiredFreeBytes(fourGiB) > fourGiB, "10: the estimate exceeds the disc itself");
        check(DiscCompose::requiredFreeBytes(fourGiB) > DiscCompose::requiredFreeBytes(fourGiB / 4),
              "10: the estimate scales with disc size");
    }

    // 11. A refused document composes NOTHING, and says why in the words the parser used. A tool that ran
    //     anyway would produce a disc missing the very patches that caused the refusal.
    {
        QTemporaryDir tmp;
        const auto o = DiscCompose::composePatchedDisc(
            QStringLiteral("no-such-tool.exe"), QStringLiteral("no-such.iso"), tmp.path(),
            QByteArray("<wiidisc version=\"1\"><options><section name=\"s\"><option name=\"o\">"
                       "<choice name=\"c\"><patch id=\"p\"/></choice></option></section></options>"
                       "<patch id=\"p\" root=\"/m\"><memory offset=\"0\" value=\"0\"/></patch></wiidisc>"),
            tmp.filePath(QStringLiteral("out.rvz")), tmp.path());
        check(!o.ok, "11: a refused document does not compose");
        check(o.error.contains(QStringLiteral("memory")), "11: the refusal reaches the caller intact");
        check(!QFile::exists(tmp.filePath(QStringLiteral("out.rvz"))), "11: no output file was left behind");
    }
```

- [ ] **Step 3: Add the source to the probe target**

In `native/CMakeLists.txt`, extend the `probe_riivolution` block from Task 2 to read:

```cmake
    add_executable(probe_riivolution tools/probe_riivolution.cpp
        src/core/RiivolutionPatch.cpp src/core/RiivolutionPatch.h
        src/core/DiscOverlay.cpp     src/core/DiscOverlay.h
        src/core/DiscCompose.cpp     src/core/DiscCompose.h)
    target_link_libraries(probe_riivolution PRIVATE Qt6::Core)
```

- [ ] **Step 4: Build and watch it fail**

```bash
cmake --build build --config Release --target probe_riivolution
```
Expected: FAILS to compile — `DiscCompose.cpp` does not exist.

- [ ] **Step 5: Implement**

Create `native/src/core/DiscCompose.cpp`:

```cpp
#include "DiscCompose.h"
#include "DiscOverlay.h"
#include "RiivolutionPatch.h"
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStorageInfo>
#include <QUuid>

namespace
{
    // The tool is given a generous ceiling rather than none: composing a disc is minutes of work, but a
    // process that has stopped making progress must not hold the install open forever.
    constexpr int kToolTimeoutMs = 45 * 60 * 1000;

    bool runTool(const QString& tool, const QStringList& args, QString* error)
    {
        QProcess p;
        p.start(tool, args);
        if (!p.waitForStarted(30000))
        {
            *error = QStringLiteral("the disc tool could not be started");
            return false;
        }
        if (!p.waitForFinished(kToolTimeoutMs))
        {
            p.kill();
            p.waitForFinished(5000);
            *error = QStringLiteral("the disc tool stopped responding");
            return false;
        }
        if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
        {
            // The tool reports its own reason on stderr; carrying it through beats inventing one.
            const QString said = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
            *error = said.isEmpty() ? QStringLiteral("the disc tool failed") : said;
            return false;
        }
        return true;
    }
}

qint64 DiscCompose::requiredFreeBytes(qint64 discBytes)
{
    // Extracted tree (about one disc) + composed image (at most one disc) + headroom. Deliberately a
    // multiple: a Wii disc is roughly an order of magnitude larger than a GameCube one, so a fixed figure
    // would be far too small for one and absurd for the other.
    return discBytes * 2 + (512LL * 1024 * 1024);
}

bool DiscCompose::hasFreeSpace(const QString& dir, qint64 needed)
{
    const QStorageInfo info(dir);
    if (!info.isValid() || !info.isReady()) return false;
    return info.bytesAvailable() >= needed;
}

DiscCompose::Outcome DiscCompose::composePatchedDisc(const QString& toolPath, const QString& discPath,
                                                     const QString& modRoot, const QByteArray& riivolutionXml,
                                                     const QString& outputPath, const QString& stagingParent)
{
    Outcome out;

    // The document is read FIRST, before any disc is extracted. A refusal must cost nothing: extracting a
    // 4 GB disc and then discovering the mod cannot be composed would be minutes of work thrown away.
    const auto parsed = RiivolutionPatch::parse(riivolutionXml);
    if (!parsed.ok)
    {
        out.error = parsed.refusal;
        return out;
    }

    const qint64 discBytes = QFileInfo(discPath).size();
    const qint64 needed = requiredFreeBytes(discBytes);
    if (!hasFreeSpace(stagingParent, needed))
    {
        out.error = QStringLiteral("there is not enough free space to build this hack — it needs about "
                                   "%1 GB free").arg(needed / (1024.0 * 1024 * 1024), 0, 'f', 1);
        return out;
    }

    const QString staging = QDir(stagingParent).absoluteFilePath(
        QStringLiteral("disc-compose-") + QUuid::createUuid().toString(QUuid::Id128));
    if (!QDir().mkpath(staging))
    {
        out.error = QStringLiteral("could not create a working folder to build this hack in");
        return out;
    }

    const QString tree = staging + QStringLiteral("/tree");
    bool ok = runTool(toolPath, {QStringLiteral("extract"), QStringLiteral("-i"), discPath,
                                 QStringLiteral("-o"), tree, QStringLiteral("-g"), QStringLiteral("-q")},
                      &out.error);

    if (ok)
    {
        const auto overlaid = DiscOverlay::apply(tree, modRoot, parsed);
        ok = overlaid.ok;
        if (!ok) out.error = overlaid.error;
    }

    if (ok)
    {
        ok = runTool(toolPath, {QStringLiteral("convert"), QStringLiteral("-i"), tree,
                                QStringLiteral("-o"), outputPath, QStringLiteral("-f"), QStringLiteral("rvz"),
                                QStringLiteral("-c"), QStringLiteral("zstd"),
                                // -l is REQUIRED whenever -c is not "none": without it the tool exits
                                // with "Compression level must be set when compression type is not
                                // 'none'". Measured in Task 1; the stock tool never reached this check
                                // because it refused the directory first, so the omission was invisible.
                                QStringLiteral("-l"), QStringLiteral("5"),
                                QStringLiteral("-b"), QStringLiteral("131072")},
                     &out.error);
    }

    // Cleaned on every path, success or failure. The staging tree is disc-sized; leaving one behind after a
    // failed install would quietly consume the space the next attempt needs.
    QDir(staging).removeRecursively();

    if (!ok)
    {
        // A partial image is worse than none: it would be found by the library scan and shown as playable.
        QFile::remove(outputPath);
        return out;
    }

    out.ok = true;
    out.outputPath = outputPath;
    return out;
}
```

- [ ] **Step 6: Build and run**

```bash
cmake --build build --config Release --target probe_riivolution
./build/Release/probe_riivolution.exe
```
Expected: `RIIVOLUTION-OK`, exit 0.

- [ ] **Step 7: Mutation-check the refusal ordering**

Move the `RiivolutionPatch::parse` block to *after* the `runTool(extract)` call, rebuild, run: case 11 must fail (the tool is invoked and the error is no longer the parser's). Restore, rebuild, confirm `RIIVOLUTION-OK`. This pins that a refusal costs no disc extraction.

- [ ] **Step 8: Commit**

```bash
git add native/src/core/DiscCompose.h native/src/core/DiscCompose.cpp native/tools/probe_riivolution.cpp
git commit -m "feat: compose a patched disc by driving the disc tool

The document is read before anything is extracted, so a mod that cannot be
composed costs no disc-sized work. Staging is cleaned on every exit path and a
failed compose removes its own output, which the library scan would otherwise
show as a playable game." -- \
        native/src/core/DiscCompose.h native/src/core/DiscCompose.cpp \
        native/tools/probe_riivolution.cpp native/CMakeLists.txt
```

---

### Task 5: Wire composition into the romhack install flow

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` — `applyRomhack` (`:14717`) and the pending-romhack path around `downloadBaseRomThenApply` (`:14652`)

**Interfaces:**
- Consumes: `DiscCompose::composePatchedDisc`, `RomhackInstall::destinationFor`.

**Background.** `applyRomhack` currently assumes `RomPatch` can apply the downloaded payload to the base ROM. A Riivolution distribution has no patch: it is an archive holding a Riivolution XML and a replacement tree. This task routes that shape to `DiscCompose` and leaves every existing path untouched.

The disc tool ships beside the Dolphin the app already manages; resolve it through the same emulator directory `EmulatorManager` uses (`binDir` for the `dolphin` id), and treat a missing tool as a refusal with a message naming what is missing — never as a silent no-op.

- [ ] **Step 1: Read the surrounding code**

```bash
sed -n '14640,14760p' native/src/ui/MainWindow.cpp
```
Understand how `PendingRomhack` reaches `applyRomhack`, how failures are reported to the user, and how a successful install writes its gamelist entry. Do not change any of it.

- [ ] **Step 2: Add the routing**

In `applyRomhack`, before the existing `RomPatch` call, detect the Riivolution shape and route it. A distribution is a Riivolution mod when the extracted payload contains a `.xml` whose root element is `wiidisc`:

```cpp
    // A Riivolution distribution carries no patch at all — a replacement tree plus an XML saying where each
    // folder belongs. RomPatch has nothing to apply to it, so it is composed into a new disc instead. The
    // shape is decided by the CONTENT of the xml (its root element), never by its file name.
    const QString riivolutionXmlPath = findRiivolutionXml(payloadDir);
    if (!riivolutionXmlPath.isEmpty())
    {
        composeRiivolutionHack(riivolutionXmlPath, payloadDir, baseRom, req);
        return;
    }
```

Add the two helpers beside `applyRomhack`:

```cpp
// The Riivolution document inside an extracted distribution, or an empty string. Identified by its ROOT
// ELEMENT, not its name: the distributions measured put it under a "Riivolution" folder, but nothing in the
// format requires that and a mod that named it otherwise would silently fall through to the patch path.
static QString findRiivolutionXml(const QString& payloadDir)
{
    QDirIterator it(payloadDir, {QStringLiteral("*.xml")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        QXmlStreamReader r(&f);
        while (!r.atEnd() && !r.isStartElement()) r.readNext();
        if (r.isStartElement() && r.name() == QLatin1String("wiidisc")) return path;
    }
    return QString();
}
```

```cpp
void MainWindow::composeRiivolutionHack(const QString& xmlPath, const QString& payloadDir,
                                        const QString& baseRom, const PendingRomhack& req)
{
    QFile xml(xmlPath);
    if (!xml.open(QIODevice::ReadOnly))
    {
        notifier_->notify(tr("Couldn't read this hack's Riivolution file."));
        return;
    }

    const QString tool = discToolPath();
    if (tool.isEmpty())
    {
        // Named, not silent: without this the install simply would not happen and nothing would say why.
        notifier_->notify(tr("The disc tool needed to build Wii hacks isn't installed yet."));
        return;
    }

    const QString dest = RomhackInstall::destinationFor(baseRom, req.title, QFileInfo(baseRom).absolutePath());
    if (dest.isEmpty())
    {
        notifier_->notify(tr("Couldn't work out where to install this hack."));
        return;
    }

    // Staging deliberately OUTSIDE the ROMs folder: RomLibrary::scan walks that folder recursively, so a
    // part-built disc tree inside it would be scanned, and a part-written image would be offered as a game.
    const QString staging = QStandardPaths::writableLocation(QStandardPaths::TempLocation);

    notifier_->notify(tr("Building %1 — this can take several minutes.").arg(req.title));
    const auto outcome = DiscCompose::composePatchedDisc(tool, baseRom, payloadDir, xml.readAll(),
                                                         dest, staging);
    if (!outcome.ok)
    {
        notifier_->notify(outcome.error);
        return;
    }

    finishRomhackInstall(outcome.outputPath, req);
}
```

And `discToolPath()`, which resolves the tool beside the Dolphin the app already manages:

```cpp
// The disc tool ships beside the managed Dolphin. Returns an empty string when it is not there, which the
// caller reports as a named refusal -- an install that silently does nothing is the one outcome worse than
// an install that says it cannot run yet.
static QString discToolPath()
{
    const QString binDir = EmulatorManager::binDirFor(QStringLiteral("dolphin"));
    if (binDir.isEmpty()) return QString();
    const QString exe = binDir + QStringLiteral("/DolphinTool.exe");
    return QFileInfo(exe).isExecutable() ? exe : QString();
}
```

If `EmulatorManager` exposes no `binDirFor`, use whichever accessor it already provides for an emulator's
install directory -- read the file rather than adding a new one, and say in your report which you used.

- [ ] **Step 3: Build the app**

```bash
cmake --build build --config Release --target everythingbox
```
Expected: `Build succeeded`, no new warnings in `MainWindow.cpp`.

- [ ] **Step 4: Run the whole probe suite**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: every probe passes, `probe_riivolution` among them, and the run's verdict matches the previous run's.

- [ ] **Step 5: Commit**

```bash
git add native/src/ui/MainWindow.cpp
git commit -m "feat: install a Wii file-replacement hack by composing a disc

A Riivolution distribution ships no patch, so RomPatch had nothing to apply and
these hacks could be listed but never installed. The shape is recognised by the
xml's root element rather than its file name, and staging stays outside the ROMs
folder so a part-built disc is never scanned as a game." -- native/src/ui/MainWindow.cpp
```

---

### Task 6: Ship the patched disc tool with the app

**Files:**
- Modify: `native/src/core/EmulatorManager.cpp` (the `dolphin` branch of `prepareFirstRunConfig`, around `:1041`)
- Modify: `native/src/ui/MainWindow.cpp` (`discToolPath`, if the seeded location differs from where it looks)
- Create: a build/packaging step placing the built `DolphinTool.exe` where the app can seed it from

**Why this task exists.** Task 5 shipped `discToolPath()`, which finds `DolphinTool.exe` in the managed
Dolphin install. That is the **stock** tool, and Task 1 measured that the stock tool refuses a directory as
`convert` input. So an install currently proceeds and fails at the convert step with the tool's own error
instead of a named refusal, and `discToolPath()` cannot tell the two builds apart without launching one.
The feature does not work end to end until the patched build reaches `emulators/dolphin/`.

**The decision, already taken:** ship it, seeded the way the app already seeds `portable.txt` and
`Dolphin.ini` for this emulator.

**A licensing obligation that is part of this task, not an afterthought.** Dolphin is GPLv2+. Distributing
a modified binary requires offering the corresponding source. The RetroPark repo already commits the exact
patch, the pinned tag (`2606`, `6094cfcf7b`) and the build recipe, which is most of that discharged — but
this task must make the offer discoverable from the app, not merely true in another repository. Put the
patch, the tag and a pointer to the recipe somewhere a user of the shipped app can reach.

- [ ] **Step 1: Decide and document where the seeded tool lives**

Read `EmulatorManager::prepareFirstRunConfig`'s `dolphin` branch (`native/src/core/EmulatorManager.cpp:1041`)
and `resolveBinary` (which already searches recursively under `emulators/<id>/`, because emulators extract
into version-named subfolders). The seeded tool must land where `discToolPath()` looks. Write down which
you chose and why before writing code.

- [ ] **Step 2: Make the app prefer the patched tool over the stock one**

`discToolPath()` must not return a tool that will fail at convert. Two builds share a filename, so
distinguish them by **where they came from**, not by probing behaviour: prefer the seeded copy at a name the
stock archive does not use, and fall back to nothing rather than to the stock tool. A refusal the user can
read beats an install that dies minutes in.

- [ ] **Step 3: Commit**

Include the licensing material. Commit by pathspec, then
`git reset HEAD -- native/CMakeLists.txt native/src/main.cpp`.

---

### Task 7: Move composition off the GUI thread

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (`composeRiivolutionHack`)
- Modify: `native/src/core/DiscCompose.h` / `.cpp` if a cancellation hook is needed

**Why this task exists.** `DiscCompose::composePatchedDisc` runs extract, overlay and convert synchronously,
with a 45-minute ceiling, on the GUI thread. On a big-screen controller UI a frozen app is indistinguishable
from a crashed one, and there is no way to cancel. Task 5 deferred one event-loop turn so the "Building…"
note paints before the freeze; that shortens nothing.

**Follow the pattern the app already has** rather than inventing one — archive extraction runs on a worker
`QThread` in `GameLauncher::open`, and it honours `QThread::currentThread()->isInterruptionRequested()` so
app quit aborts it promptly. Read that path first and mirror it.

- [ ] **Step 1: Read the existing worker pattern**

`native/src/core/ArchiveRom.cpp` and `GameLauncher::open`. Note how interruption is requested and checked,
and how completion is marshalled back to the GUI thread.

- [ ] **Step 2: Move the compose onto a worker, marshal the outcome back**

The install's completion (`finishRomhackInstall`) must still run on the GUI thread. Nothing in `DiscCompose`
may touch a widget.

- [ ] **Step 3: Make it cancellable, and make cancellation leave nothing behind**

An interrupted compose must remove its staging tree and its `.part` output — the same guarantee the failure
paths already give. `DiscCompose` cleans staging on every exit path today; interruption must not become the
exception.

- [ ] **Step 4: Verify**

Build `everythingbox`, run the full probe suite (`BUILD_DIR=build bash native/tools/run-headless-probes.sh`),
and report the verdict. Commit by pathspec, then
`git reset HEAD -- native/CMakeLists.txt native/src/main.cpp`.

---

### Task 8: The live gate

**Files:** none — measurement only, then an edit to this plan recording what happened.

**This task is run by the repository owner, not by an implementer.** No unit test reaches a real disc, and the failure that matters most — an overlay that silently did not land — produces a disc that boots perfectly.

- [ ] **Step 1: Confirm free space before starting**

```bash
df -h /c | tail -1
```
The base game is stored as a 1.4 GB archive; its decompressed disc, the extracted tree and the output image must coexist. If the free figure is under about 15 GB, point staging at another drive first.

- [ ] **Step 2: Record the base ROM's hash**

```bash
sha1sum "C:/EverythingBox-app/roms/gc/Super Mario Galaxy 2.7z"
```

- [ ] **Step 3: Install the hack from the app**

Open Super Mario Galaxy 2, choose the Super Mario Gravity hack from the romhack shelf, and install it.

Expected: a progress note, then a new entry in the library.

- [ ] **Step 4: Confirm the base ROM was not touched**

```bash
sha1sum "C:/EverythingBox-app/roms/gc/Super Mario Galaxy 2.7z"
```
Expected: byte-identical to Step 2.

- [ ] **Step 5: Confirm the overlay actually landed — the real gate**

Launch the installed hack. **Booting is not the pass condition**: a stock disc boots too, so booting proves only that composition produced a valid image. The pass condition is the mod's **own title screen** — the overlay replaces `LayoutData/TitleLogo.arc`, so the title screen is the cheapest place the substitution becomes visible. A stock Super Mario Galaxy 2 title screen means the disc composed correctly and the overlay did nothing, which is the failure this step exists to catch.

- [ ] **Step 6: Confirm nothing was left behind**

```bash
ls "$(cygpath "$TEMP")" | grep disc-compose
```
Expected: no output. Staging is cleaned on every exit path.

- [ ] **Step 7: Record what happened**

Append a `## Measured` section to this file: the free space before and after, both hashes from Steps 2 and 4, which title screen appeared, and the wall-clock time the compose took. Commit it.

---

## Done when

- The disc tool composes an image from an extracted directory, and its `header` output matches the source disc's.
- A Riivolution document becomes a list of overlay operations, with `<memory>` refused and a real choice refused, both mutation-verified.
- An overlay lands where the disc path says, replaces what is already there, and cannot write outside the tree — mutation-verified.
- A refused document costs no disc extraction, and a failed compose leaves neither staging nor a part-written image.
- Super Mario Gravity installs onto Super Mario Galaxy 2, the base ROM is byte-identical afterwards, and the hack's own title screen appears.

## Out of scope

- Riivolution at runtime, on hardware or through an in-process core.
- Dolphin texture packs, a third install shape.
- Any change to `RomLibrary` scanning, or to how the romhack capability stages or serves files.
- A UI for choosing between a mod's options. Documents offering a choice are refused until one exists.
