// Headless check of the data-driven system catalog (src/core/SystemCatalog, issue #92) — the JSON
// merge-over-builtin that lets <data>/systems/*.json ADD systems or OVERRIDE fields of a built-in one
// without a rebuild, the same shape #52 asks for standalone emulators.
//
// THE #1 RAIL is no regression to built-in system resolution: with no data files, the catalog must resolve
// exactly as the compiled-in table does. This proves it two ways — the built-in table round-trips through
// the JSON schema to a byte-identical in-memory catalog, and loadDataDir over an empty/absent dir returns the
// base untouched (that is literally what systems() computes when <data>/systems is empty).
//
// Then the merge itself: a data file ADDS a new system, OVERRIDES a single field of a built-in one (leaving
// its other fields intact — field-level, not whole-entry), and a MALFORMED file (bad JSON / wrong top-level
// type / an entry with no id) is logged and skipped while the base and its valid siblings survive. Finally
// the end-to-end path through systems()/forExtension/forConsoleName with a seeded <data>/systems dir, and the
// shipped example (native/resources/systems/example-systems.json) parsed to prove it is a valid, mergeable
// file — a live launch on a real core is NOT exercised here (needs the core + a ROM) and is unverified.
//
// Expected values are hand-authored (an independent oracle), never read back out of toJson(), so a fixture
// cannot be a fixed point of the function under test. Prints SYSCATALOG-OK on success; on any failure prints
// SYSCATALOG-FAIL <cond> and exits non-zero.
#include "SystemCatalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "SYSCATALOG-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(bytes);
    return true;
}

static const GameSystem* find(const QList<GameSystem>& list, const QString& id)
{
    for (const GameSystem& s : list) if (s.id == id) return &s;
    return nullptr;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace SystemCatalog;

    const QList<GameSystem> builtin = builtinSystems();
    CHECK(builtin.size() > 40); // sanity: the table is present

    // ================= 1. THE #1 RAIL: built-in round-trips through the JSON schema unchanged =============
    // Every built-in system, serialized to JSON and parsed back, must be byte-identical (operator== compares
    // all eight fields, order-sensitive for extensions/cores where order is load-bearing). A single field the
    // serializer drops or reorders trips this for that system.
    for (const GameSystem& s : builtin)
    {
        const GameSystem round = fromJson(toJson(s));
        CHECK(round == s);
        CHECK(round.id == s.id);
        CHECK(round.extensions == s.extensions);
        CHECK(round.cores == s.cores);
        CHECK(round.externalEmulator == s.externalEmulator);
    }

    // Spot-check two representative systems by hand (independent oracle), so a round-trip that is identity for
    // the WRONG reason (e.g. both directions dropping the same field) is still caught: psx keeps its core
    // ORDER and its externalEmulator; nes keeps its four extensions.
    {
        const GameSystem* psx = find(builtin, QStringLiteral("psx"));
        CHECK(psx != nullptr);
        const QJsonObject j = toJson(*psx);
        CHECK(j.value(QStringLiteral("id")).toString() == QStringLiteral("psx"));
        CHECK(j.value(QStringLiteral("externalEmulator")).toString() == QStringLiteral("duckstation"));
        const QJsonArray cores = j.value(QStringLiteral("cores")).toArray();
        CHECK(cores.size() == 3);
        CHECK(cores.at(0).toString() == QStringLiteral("swanstation")); // [0] is the default core — order matters
        // A built-in carries NO data-only fields, so the canonical form omits them entirely.
        CHECK(!j.contains(QStringLiteral("folderAliases")));
        CHECK(!j.contains(QStringLiteral("consoleHints")));
        CHECK(!j.contains(QStringLiteral("bios")));
    }

    // ================= 2. no-regression: loadDataDir over nothing returns the base untouched ==============
    {
        QString warned;
        const QList<GameSystem> emptyReal = loadDataDir(QString(), builtin, [&](const QString& m){ warned = m; });
        CHECK(emptyReal == builtin); // empty dir string -> base, verbatim

        QDir tmp(QDir::tempPath() + QStringLiteral("/eb-syscat-empty"));
        tmp.removeRecursively(); QDir().mkpath(tmp.path());
        const QList<GameSystem> emptyDir = loadDataDir(tmp.path(), builtin);
        CHECK(emptyDir == builtin); // present-but-empty dir -> base, verbatim
        CHECK(warned.isEmpty());
        tmp.removeRecursively();

        const QList<GameSystem> absent = loadDataDir(QDir::tempPath() + QStringLiteral("/eb-syscat-nope-xyz"), builtin);
        CHECK(absent == builtin); // non-existent dir -> base, verbatim (never a crash, never a drop)
    }

    // ================= 3. ADD a new system (hand-authored oracle) =========================================
    {
        QJsonArray entries;
        const QByteArray add =
            "[{\"id\":\"foolander\",\"name\":\"Foolander\",\"extensions\":[\"FOO\",\"bar\"],"
            "\"cores\":[\"foocore\",\"barcore\"],\"folderAliases\":[\"Foo\"],\"consoleHints\":[\"Foolander\"]}]";
        QString perr;
        CHECK(parseEntries(add, &entries, &perr));
        const QList<GameSystem> merged = applyEntries(builtin, entries);
        CHECK(merged.size() == builtin.size() + 1);
        const GameSystem* f = find(merged, QStringLiteral("foolander"));
        CHECK(f != nullptr);
        CHECK(f->name == QStringLiteral("Foolander"));
        CHECK(f->extensions == (QStringList{ QStringLiteral("foo"), QStringLiteral("bar") })); // lowercased at parse
        CHECK(f->cores == (QStringList{ QStringLiteral("foocore"), QStringLiteral("barcore") })); // case preserved
        CHECK(f->folderAliases == (QStringList{ QStringLiteral("foo") }));
        CHECK(f->consoleHints == (QStringList{ QStringLiteral("foolander") }));
        // The base is untouched by an add: a built-in picked at random still equals itself.
        const GameSystem* nes = find(merged, QStringLiteral("nes"));
        CHECK(nes != nullptr && *nes == *find(builtin, QStringLiteral("nes")));
    }

    // ================= 4. OVERRIDE one field of a built-in — the rest survive (field-level) ===============
    {
        QJsonArray entries;
        // Swap gba's default core ONLY. Its extensions and name must be left exactly as the built-in has them.
        CHECK(parseEntries("[{\"id\":\"gba\",\"cores\":[\"newgbacore\"]}]", &entries, nullptr));
        const QList<GameSystem> merged = applyEntries(builtin, entries);
        CHECK(merged.size() == builtin.size()); // an override does NOT append
        const GameSystem* gba = find(merged, QStringLiteral("gba"));
        const GameSystem* base = find(builtin, QStringLiteral("gba"));
        CHECK(gba != nullptr && base != nullptr);
        CHECK(gba->cores == (QStringList{ QStringLiteral("newgbacore") })); // overridden
        CHECK(gba->extensions == base->extensions);  // untouched — kills a whole-entry-replace mutant
        CHECK(gba->name == base->name);              // untouched
        CHECK(!gba->extensions.isEmpty());           // a full-replace would have emptied this
    }

    // ================= 5. MALFORMED is logged and skipped; the base and valid siblings survive ============
    {
        // parseEntries verdicts (unit level).
        QJsonArray tmp; QString perr;
        CHECK(!parseEntries("{ this is not json", &tmp, &perr));   // unparseable -> false, with a reason
        CHECK(!perr.isEmpty());
        CHECK(!parseEntries("", &tmp, &perr));                      // empty bytes are a parse error, not "0 systems"
        CHECK(parseEntries("[]", &tmp, &perr));                     // an empty ARRAY is a valid (empty) file
        CHECK(tmp.isEmpty());
        CHECK(parseEntries("{\"id\":\"solo\"}", &tmp, &perr));       // a bare object is accepted (wrapped)
        CHECK(tmp.size() == 1);

        // An array holding a junk entry (no id) beside a good one: the junk is skipped WITH a warning, the good
        // one applied.
        QJsonArray entries;
        CHECK(parseEntries("[{\"name\":\"no id here\"},{\"id\":\"okid\",\"cores\":[\"c\"]}]", &entries, nullptr));
        QStringList warns;
        const QList<GameSystem> merged = applyEntries(builtin, entries, [&](const QString& m){ warns << m; });
        CHECK(merged.size() == builtin.size() + 1);       // only the good entry landed
        CHECK(find(merged, QStringLiteral("okid")) != nullptr);
        CHECK(warns.size() == 1);                          // the id-less entry produced exactly one warning
        CHECK(warns.first().contains(QStringLiteral("no \"id\"")));

        // A whole DIRECTORY where one file is corrupt and one is good: base intact + good applied + a warning.
        QDir dir(QDir::tempPath() + QStringLiteral("/eb-syscat-mixed"));
        dir.removeRecursively(); QDir().mkpath(dir.path());
        CHECK(writeFile(dir.filePath(QStringLiteral("a-bad.json")), "{ broken"));
        CHECK(writeFile(dir.filePath(QStringLiteral("b-good.json")),
                        "[{\"id\":\"fromfile\",\"name\":\"From File\",\"cores\":[\"z\"]}]"));
        QStringList dwarn;
        const QList<GameSystem> fromDir = loadDataDir(dir.path(), builtin, [&](const QString& m){ dwarn << m; });
        CHECK(fromDir.size() == builtin.size() + 1);
        CHECK(find(fromDir, QStringLiteral("fromfile")) != nullptr);
        CHECK(find(fromDir, QStringLiteral("nes")) != nullptr); // base never dropped by a bad neighbour
        bool sawBad = false; for (const QString& w : dwarn) if (w.contains(QStringLiteral("a-bad.json"))) sawBad = true;
        CHECK(sawBad); // the corrupt file was named in a warning, not silently swallowed
        dir.removeRecursively();
    }

    // ================= 6. the SHIPPED example file is a valid, mergeable data file ========================
    // Tie the repo's native/resources/systems/example-systems.json to this gate: it must parse and add its
    // systems with the cores it names, or the FLEX breadth content has rotted.
#ifdef EB_SYSCATALOG_EXAMPLE_DIR
    {
        const QList<GameSystem> withExample = loadDataDir(QStringLiteral(EB_SYSCATALOG_EXAMPLE_DIR), builtin);
        CHECK(withExample.size() == builtin.size() + 4); // msx, pc88, x68000, wasm4
        const GameSystem* msx = find(withExample, QStringLiteral("msx"));
        CHECK(msx != nullptr && msx->cores.value(0) == QStringLiteral("bluemsx"));
        const GameSystem* x68 = find(withExample, QStringLiteral("x68000"));
        CHECK(x68 != nullptr && x68->cores.value(0) == QStringLiteral("px68k"));
        CHECK(x68 != nullptr && x68->extensions.contains(QStringLiteral("xdf")));
        // A shipped example must not collide-and-clobber a built-in extension: gba still resolves to gba.
        const GameSystem* gba = find(withExample, QStringLiteral("gba"));
        CHECK(gba != nullptr && gba->extensions == find(builtin, QStringLiteral("gba"))->extensions);
    }
#endif

    // ================= 7. end-to-end through systems()/forExtension/forConsoleName with seeded data =======
    // Seed <data>/systems (the probe's isolated AppPaths::dataDir) BEFORE the first systems() call, then drive
    // the real accessors — this is the path RomLibrary/GameLauncher use. systems() caches on first use, so no
    // accessor is called above this point.
    {
        const QByteArray seed =
            "[{\"id\":\"myst\",\"name\":\"Mystation\",\"extensions\":[\"mys\"],\"cores\":[\"mystcore\"],"
            "\"folderAliases\":[\"mystation\"],\"consoleHints\":[\"mystation\"]},"
            "{\"id\":\"gba\",\"cores\":[\"overridden_gba\"]}]"; // also override a built-in via the real path
        CHECK(writeFile(dataSystemsDir() + QStringLiteral("/seed.json"), seed));

        // New system reachable by extension, id and console hint.
        const GameSystem* byExt = forExtension(QStringLiteral("mys"));
        CHECK(byExt != nullptr && byExt->id == QStringLiteral("myst"));
        CHECK(byId(QStringLiteral("myst")) != nullptr);
        const GameSystem* byConsole = forConsoleName(QStringLiteral("Mystation"));
        CHECK(byConsole != nullptr && byConsole->id == QStringLiteral("myst"));
        CHECK(byId(QStringLiteral("myst"))->folderAliases.contains(QStringLiteral("mystation")));

        // The built-in override took effect through the real accessor...
        CHECK(byId(QStringLiteral("gba"))->cores.value(0) == QStringLiteral("overridden_gba"));
        // ...and built-in resolution is otherwise UNCHANGED even with data present (the #1 rail, live):
        CHECK(byId(QStringLiteral("gba"))->extensions.contains(QStringLiteral("gba")));
        const GameSystem* nesByExt = forExtension(QStringLiteral("nes"));
        CHECK(nesByExt != nullptr && nesByExt->id == QStringLiteral("nes"));
        const GameSystem* psxByName = forConsoleName(QStringLiteral("PlayStation"));
        CHECK(psxByName != nullptr && psxByName->id == QStringLiteral("psx"));
        const GameSystem* gcByName = forConsoleName(QStringLiteral("GameCube"));
        CHECK(gcByName != nullptr && gcByName->id == QStringLiteral("gc"));
        // A data extension never displaces a built-in one: forExtension returns the first (built-in) match
        // because data is appended after the base. .iso is first claimed by the saturn built-in (listed before
        // gc/psp), and still is with data present — first-match-wins is unchanged.
        const GameSystem* isoOwner = forExtension(QStringLiteral("iso"));
        CHECK(isoOwner != nullptr && isoOwner->id == QStringLiteral("saturn"));
    }

    if (failures == 0) std::printf("SYSCATALOG-OK\n");
    else               std::fprintf(stderr, "SYSCATALOG had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
