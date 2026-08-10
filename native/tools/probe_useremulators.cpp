// Headless check of the data-driven standalone-emulator registry (src/core/EmulatorRegistry, issue #52) —
// the JSON merge-over-builtin that lets <data>/emulators/*.json ADD a user's own emulator or OVERRIDE fields
// of a built-in one without a rebuild, mirroring SystemCatalog (#92).
//
// THE #1 RAIL is no regression: with no data files, all() must be byte-for-byte the compiled-in table. This
// proves it two ways — every built-in round-trips through the JSON schema to a byte-identical in-memory entry,
// and loadDataDir over an empty/absent dir returns the base untouched (what all() computes when the dir is
// empty).
//
// Then the merge itself: a data file ADDS a new emulator, OVERRIDES a single field of a built-in one (leaving
// its other fields intact — field-level, not whole-entry), and a MALFORMED file (bad JSON / wrong top-level
// type / an entry with no id) is logged and skipped while the base and its valid siblings survive. Then the
// two #52-specific rails: (a) a user entry with an ABSOLUTE `binary` resolves as installed with NO download —
// resolveBinaryFrom returns the path and hasInstallSource is false; (b) every built-in still has an install
// source (auto-install stays a built-in privilege). Finally the end-to-end path through all()/byId() with a
// seeded <data>/emulators dir, and the shipped example parsed to prove it is a valid, mergeable file. A live
// child-process launch of a real emulator is NOT exercised here (needs the emulator + a ROM) and is unverified.
//
// Expected values are hand-authored (an independent oracle), never read back out of toJson(), so a fixture
// cannot be a fixed point of the function under test. Prints USEREMU-OK on success; on any failure prints
// USEREMU-FAIL <cond> and exits non-zero.
#include "EmulatorRegistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "USEREMU-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(bytes);
    return true;
}

static const ExternalEmulator* find(const QList<ExternalEmulator>& list, const QString& id)
{
    for (const ExternalEmulator& e : list) if (e.id == id) return &e;
    return nullptr;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace EmulatorRegistry;

    const QList<ExternalEmulator> builtin = builtinEmulators();
    CHECK(builtin.size() >= 15); // sanity: the table is present (15 shipped built-ins)

    // ================= 1. THE #1 RAIL: built-in round-trips through the JSON schema unchanged =============
    // Every built-in, serialized to JSON and parsed back, must be byte-identical (operator== compares all the
    // serialized fields; order-sensitive where order is load-bearing, e.g. the binaries find-rules). A single
    // field the serializer drops or reorders trips this for that entry.
    for (const ExternalEmulator& e : builtin)
    {
        const ExternalEmulator round = fromJson(toJson(e));
        CHECK(round == e);
        CHECK(round.id == e.id);
        CHECK(round.winBinaries == e.winBinaries);
        CHECK(round.argsTemplate == e.argsTemplate);
        CHECK(round.updateJsonUrl == e.updateJsonUrl);
    }

    // Spot-check two representative built-ins by hand (independent oracle), so a round-trip that is identity
    // for the WRONG reason (both directions dropping the same field) is still caught: dolphin keeps its args
    // template and Flatpak id; rpcs3 keeps its per-OS update URLs (which most emulators leave empty).
    {
        const ExternalEmulator* dolphin = find(builtin, QStringLiteral("dolphin"));
        CHECK(dolphin != nullptr);
        const QJsonObject j = toJson(*dolphin);
        CHECK(j.value(QStringLiteral("id")).toString() == QStringLiteral("dolphin"));
        CHECK(j.value(QStringLiteral("name")).toString() == QStringLiteral("Dolphin")); // displayName -> "name"
        CHECK(j.value(QStringLiteral("argsTemplate")).toString() == QStringLiteral("-b -e {rom} {fs}"));
        CHECK(j.value(QStringLiteral("flatpakAppId")).toString() == QStringLiteral("org.DolphinEmu.dolphin-emu"));
        const QJsonArray wb = j.value(QStringLiteral("winBinaries")).toArray();
        CHECK(wb.size() == 4);
        CHECK(wb.at(0).toString() == QStringLiteral("Dolphin-x64/Dolphin.exe")); // order preserved
        // A built-in carries no data-only fields, so the canonical form omits them entirely.
        CHECK(!j.contains(QStringLiteral("extensions")));
        CHECK(!j.contains(QStringLiteral("systems")));
        CHECK(!j.contains(QStringLiteral("binary")));       // 'binary' is input-only, never emitted

        const ExternalEmulator* rpcs3 = find(builtin, QStringLiteral("rpcs3"));
        CHECK(rpcs3 != nullptr);
        const QJsonObject r = toJson(*rpcs3);
        CHECK(r.value(QStringLiteral("winUpdateUrl")).toString()
              == QStringLiteral("https://api.github.com/repos/RPCS3/rpcs3-binaries-win/releases/latest"));
        CHECK(!r.contains(QStringLiteral("updateJsonUrl"))); // rpcs3's shared field is empty -> omitted
    }

    // ================= 2. no-regression: loadDataDir over nothing returns the base untouched ==============
    {
        QString warned;
        const QList<ExternalEmulator> emptyReal = loadDataDir(QString(), builtin, [&](const QString& m){ warned = m; });
        CHECK(emptyReal == builtin); // empty dir string -> base, verbatim

        QDir tmp(QDir::tempPath() + QStringLiteral("/eb-useremu-empty"));
        tmp.removeRecursively(); QDir().mkpath(tmp.path());
        const QList<ExternalEmulator> emptyDir = loadDataDir(tmp.path(), builtin);
        CHECK(emptyDir == builtin); // present-but-empty dir -> base, verbatim
        CHECK(warned.isEmpty());
        tmp.removeRecursively();

        const QList<ExternalEmulator> absent = loadDataDir(QDir::tempPath() + QStringLiteral("/eb-useremu-nope-xyz"), builtin);
        CHECK(absent == builtin); // non-existent dir -> base, verbatim (never a crash, never a drop)
    }

    // ================= 3. ADD a new user emulator (hand-authored oracle) ==================================
    {
        QJsonArray entries;
        // A user emulator: friendly `name`, explicit per-OS binaries, args template, optional metadata.
        const QByteArray add =
            "[{\"id\":\"myemu\",\"name\":\"My Emulator\",\"argsTemplate\":\"{fs} {rom}\","
            "\"fullscreenArgs\":\"-full\",\"winBinaries\":[\"C:/Games/myemu/myemu.exe\"],"
            "\"extensions\":[\"ABC\",\"def\"],\"systems\":[\"MySystem\"]}]";
        QString perr;
        CHECK(parseEntries(add, &entries, &perr));
        const QList<ExternalEmulator> merged = applyEntries(builtin, entries);
        CHECK(merged.size() == builtin.size() + 1);
        const ExternalEmulator* m = find(merged, QStringLiteral("myemu"));
        CHECK(m != nullptr);
        if (m) {
        CHECK(m->displayName == QStringLiteral("My Emulator"));               // `name` -> displayName
        CHECK(m->argsTemplate == QStringLiteral("{fs} {rom}"));
        CHECK(m->fullscreenArgs == QStringLiteral("-full"));
        CHECK(m->winBinaries == (QStringList{ QStringLiteral("C:/Games/myemu/myemu.exe") })); // path case preserved
        CHECK(m->extensions == (QStringList{ QStringLiteral("abc"), QStringLiteral("def") })); // lowercased at parse
        CHECK(m->systems == (QStringList{ QStringLiteral("mysystem") }));      // lowercased at parse
        CHECK(!hasInstallSource(*m));                                          // no update URL -> no auto-install
        }
        // The base is untouched by an add: a built-in picked at random still equals itself.
        const ExternalEmulator* dol = find(merged, QStringLiteral("dolphin"));
        CHECK(dol != nullptr && *dol == *find(builtin, QStringLiteral("dolphin")));
    }

    // ================= 3b. the `binary` shorthand fills the current-OS find-rule =========================
    // On this probe's platform, a lone `binary` string populates the OS-specific binaries list. (Built on
    // Windows in CI, so winBinaries; the assertion keys off the same OS macro the code uses.)
    {
        QJsonArray entries;
        CHECK(parseEntries("[{\"id\":\"shorthand\",\"name\":\"S\",\"binary\":\"C:/x/y/z.exe\"}]", &entries, nullptr));
        const QList<ExternalEmulator> merged = applyEntries(builtin, entries);
        const ExternalEmulator* s = find(merged, QStringLiteral("shorthand"));
        CHECK(s != nullptr);
#if defined(Q_OS_WIN)
        CHECK(s->winBinaries == (QStringList{ QStringLiteral("C:/x/y/z.exe") }));
#elif defined(Q_OS_MACOS)
        CHECK(s->macBinaries == (QStringList{ QStringLiteral("C:/x/y/z.exe") }));
#else
        CHECK(s->linuxBinaries == (QStringList{ QStringLiteral("C:/x/y/z.exe") }));
#endif
    }

    // ================= 4. OVERRIDE one field of a built-in — the rest survive (field-level) ===============
    {
        QJsonArray entries;
        // Swap dolphin's argsTemplate ONLY. Its binaries, name and update URL must be left as the built-in has.
        CHECK(parseEntries("[{\"id\":\"dolphin\",\"argsTemplate\":\"--boot {rom}\"}]", &entries, nullptr));
        const QList<ExternalEmulator> merged = applyEntries(builtin, entries);
        CHECK(merged.size() == builtin.size()); // an override does NOT append
        const ExternalEmulator* dol = find(merged, QStringLiteral("dolphin"));
        const ExternalEmulator* base = find(builtin, QStringLiteral("dolphin"));
        CHECK(dol != nullptr && base != nullptr);
        CHECK(dol->argsTemplate == QStringLiteral("--boot {rom}")); // overridden
        CHECK(dol->winBinaries == base->winBinaries);   // untouched — kills a whole-entry-replace mutant
        CHECK(dol->displayName == base->displayName);   // untouched
        CHECK(dol->updateJsonUrl == base->updateJsonUrl);
        CHECK(!dol->winBinaries.isEmpty());             // a full-replace would have emptied this
        CHECK(hasInstallSource(*dol));                  // overriding a built-in KEEPS its install privilege
    }

    // ================= 5. MALFORMED is logged and skipped; the base and valid siblings survive ============
    {
        // parseEntries verdicts (unit level).
        QJsonArray tmp; QString perr;
        CHECK(!parseEntries("{ this is not json", &tmp, &perr));   // unparseable -> false, with a reason
        CHECK(!perr.isEmpty());
        CHECK(!parseEntries("", &tmp, &perr));                      // empty bytes are a parse error, not "0 emulators"
        CHECK(parseEntries("[]", &tmp, &perr));                     // an empty ARRAY is a valid (empty) file
        CHECK(tmp.isEmpty());
        CHECK(parseEntries("{\"id\":\"solo\"}", &tmp, &perr));       // a bare object is accepted (wrapped)
        CHECK(tmp.size() == 1);

        // An array holding a junk entry (no id) beside a good one: the junk is skipped WITH a warning, the good
        // one applied.
        QJsonArray entries;
        CHECK(parseEntries("[{\"name\":\"no id here\"},{\"id\":\"okid\",\"argsTemplate\":\"{rom}\"}]", &entries, nullptr));
        QStringList warns;
        const QList<ExternalEmulator> merged = applyEntries(builtin, entries, [&](const QString& m){ warns << m; });
        CHECK(merged.size() == builtin.size() + 1);       // only the good entry landed
        CHECK(find(merged, QStringLiteral("okid")) != nullptr);
        CHECK(warns.size() == 1);                          // the id-less entry produced exactly one warning
        CHECK(warns.first().contains(QStringLiteral("no \"id\"")));

        // A whole DIRECTORY where one file is corrupt and one is good: base intact + good applied + a warning.
        QDir dir(QDir::tempPath() + QStringLiteral("/eb-useremu-mixed"));
        dir.removeRecursively(); QDir().mkpath(dir.path());
        CHECK(writeFile(dir.filePath(QStringLiteral("a-bad.json")), "{ broken"));
        CHECK(writeFile(dir.filePath(QStringLiteral("b-good.json")),
                        "[{\"id\":\"fromfile\",\"name\":\"From File\",\"argsTemplate\":\"{rom}\"}]"));
        QStringList dwarn;
        const QList<ExternalEmulator> fromDir = loadDataDir(dir.path(), builtin, [&](const QString& m){ dwarn << m; });
        CHECK(fromDir.size() == builtin.size() + 1);
        CHECK(find(fromDir, QStringLiteral("fromfile")) != nullptr);
        CHECK(find(fromDir, QStringLiteral("dolphin")) != nullptr); // base never dropped by a bad neighbour
        bool sawBad = false; for (const QString& w : dwarn) if (w.contains(QStringLiteral("a-bad.json"))) sawBad = true;
        CHECK(sawBad); // the corrupt file was named in a warning, not silently swallowed
        dir.removeRecursively();
    }

    // ================= 6. #52 rails: user entry resolves as installed with NO download ===================
    // resolveBinaryFrom returns an ABSOLUTE candidate verbatim when it exists (the user-points-at-their-own-
    // binary case) and "" when it does not; a RELATIVE candidate is looked up under the base dir. Combined with
    // hasInstallSource being false, a user entry is "installed" (isInstalled true) without ever downloading.
    {
        QDir root(QDir::tempPath() + QStringLiteral("/eb-useremu-bin"));
        root.removeRecursively(); QDir().mkpath(root.path());
        const QString absReal = root.filePath(QStringLiteral("myemu.exe"));
        CHECK(writeFile(absReal, "MZ")); // a real file at an absolute path
        const QString absMissing = root.filePath(QStringLiteral("nope.exe"));

        // Absolute candidate that exists -> returned verbatim (independent oracle: the path we just wrote).
        CHECK(resolveBinaryFrom(QStringList{ absReal }, QString()) == absReal);
        // Absolute candidate that does NOT exist -> "" (not installed; play() would report, never download).
        CHECK(resolveBinaryFrom(QStringList{ absMissing }, QString()).isEmpty());
        // Relative candidate is resolved under the base dir.
        CHECK(writeFile(root.filePath(QStringLiteral("sub/rel.exe")), "MZ"));
        CHECK(resolveBinaryFrom(QStringList{ QStringLiteral("sub/rel.exe") }, root.path())
              == root.path() + QStringLiteral("/sub/rel.exe"));
        // First existing match wins, in list order (a missing first candidate falls through to the real one).
        CHECK(resolveBinaryFrom(QStringList{ absMissing, absReal }, QString()) == absReal);

        // A user emulator built from JSON pointing at that absolute binary: hasInstallSource false AND the
        // binary resolves -> the two facts that make it "installed, no download". Build the JSON with Qt's
        // own writer (an independent oracle, NOT EmulatorRegistry::toJson) so the runtime temp path — which
        // may contain characters that need JSON escaping — is injected correctly.
        QJsonObject uo;
        uo.insert(QStringLiteral("id"), QStringLiteral("u"));
        uo.insert(QStringLiteral("name"), QStringLiteral("U"));
        QJsonArray ba; ba.push_back(absReal);
        uo.insert(QStringLiteral("winBinaries"), ba);
        uo.insert(QStringLiteral("macBinaries"), ba);
        uo.insert(QStringLiteral("linuxBinaries"), ba);
        QJsonArray uarr; uarr.push_back(uo);
        QJsonArray entries;
        CHECK(parseEntries(QJsonDocument(uarr).toJson(QJsonDocument::Compact), &entries, nullptr));
        const QList<ExternalEmulator> umerged = applyEntries(builtin, entries);
        const ExternalEmulator* u = find(umerged, QStringLiteral("u"));
        CHECK(u != nullptr);
        if (u) CHECK(!hasInstallSource(*u));
#if defined(Q_OS_WIN)
        if (u) CHECK(resolveBinaryFrom(u->winBinaries, QString()) == absReal);
#elif defined(Q_OS_MACOS)
        if (u) CHECK(resolveBinaryFrom(u->macBinaries, QString()) == absReal);
#else
        if (u) CHECK(resolveBinaryFrom(u->linuxBinaries, QString()) == absReal);
#endif
        root.removeRecursively();
    }

    // ================= 6b. auto-install stays a built-in privilege ========================================
    // EVERY built-in must have an install source (else the app couldn't offer to download it); this is the
    // property hasInstallSource keys off to distinguish a user entry. If a future built-in ships with no
    // update URL this trips — a deliberate tripwire, not just a passing assertion.
    for (const ExternalEmulator& e : builtin)
        CHECK(hasInstallSource(e));

    // ================= 7. the SHIPPED example file is a valid, mergeable data file ========================
#ifdef EB_USEREMU_EXAMPLE_DIR
    {
        const QList<ExternalEmulator> withExample = loadDataDir(QStringLiteral(EB_USEREMU_EXAMPLE_DIR), builtin);
        CHECK(withExample.size() == builtin.size() + 2); // mame-standalone, supermodel
        const ExternalEmulator* mame = find(withExample, QStringLiteral("mame-standalone"));
        CHECK(mame != nullptr && mame->displayName == QStringLiteral("MAME (standalone)"));
        CHECK(mame != nullptr && !hasInstallSource(*mame)); // a user example never auto-installs
        CHECK(mame != nullptr && mame->extensions.contains(QStringLiteral("chd")));
        // The example must not clobber a built-in: dolphin still equals its built-in self.
        const ExternalEmulator* dol = find(withExample, QStringLiteral("dolphin"));
        CHECK(dol != nullptr && *dol == *find(builtin, QStringLiteral("dolphin")));
    }
#endif

    // ================= 8. end-to-end through all()/byId() with seeded data ================================
    // Seed <data>/emulators (the probe's isolated AppPaths::dataDir) BEFORE the first all() call, then drive
    // the real accessors — this is the path GameLauncher/MainWindow use. all() caches on first use, so no
    // accessor is called above this point.
    {
        const QByteArray seed =
            "[{\"id\":\"seededemu\",\"name\":\"Seeded\",\"argsTemplate\":\"{rom}\","
            "\"binary\":\"/opt/seeded/seeded\"},"
            "{\"id\":\"dolphin\",\"argsTemplate\":\"--seed {rom}\"}]"; // also override a built-in via the real path
        CHECK(writeFile(dataEmulatorsDir() + QStringLiteral("/seed.json"), seed));

        // New user emulator reachable by id through the real merged accessor.
        const ExternalEmulator* s = byId(QStringLiteral("seededemu"));
        CHECK(s != nullptr && s->displayName == QStringLiteral("Seeded"));
        CHECK(s != nullptr && !hasInstallSource(*s));
        // The built-in override took effect through the real accessor...
        CHECK(byId(QStringLiteral("dolphin")) != nullptr);
        CHECK(byId(QStringLiteral("dolphin"))->argsTemplate == QStringLiteral("--seed {rom}"));
        // ...and the built-in's other fields are UNCHANGED even with data present (the #1 rail, live):
        CHECK(byId(QStringLiteral("dolphin"))->winBinaries == find(builtin, QStringLiteral("dolphin"))->winBinaries);
        CHECK(hasInstallSource(*byId(QStringLiteral("dolphin")))); // override kept its install source
        // A built-in the data never mentions is byte-for-byte itself.
        CHECK(byId(QStringLiteral("pcsx2")) != nullptr
              && *byId(QStringLiteral("pcsx2")) == *find(builtin, QStringLiteral("pcsx2")));
    }

    if (failures == 0) std::printf("USEREMU-OK\n");
    else               std::fprintf(stderr, "USEREMU had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
