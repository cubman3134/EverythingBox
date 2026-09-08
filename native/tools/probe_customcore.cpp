// Headless check of the load-any-core escape hatch (issue #98) — running a libretro core the catalogue does
// not bless. Driven against a FIXTURE STUB CORE built from source in-tree (tools/fixtures/customcore_fixture.c
// and customcore_notacore.c, built beside this probe): nothing is downloaded, and no binary is committed.
// Qt6::Core only, so it runs under the offscreen QPA in CI, and its data dir is the per-process scratch one
// (EB_ISOLATED_DATA_DIR), so the registry it writes cannot touch a real install.
//
// It pins:
//
//   * THE NO-CUSTOM-CORE IDENTITY. With an empty registry, candidateCoresFor(sys) IS sys->cores for EVERY
//     system in the catalogue, and the full enumeration + resolution of every system is snapshotted. After a
//     custom core is registered, every system the core does not claim is asserted byte-identical to that
//     snapshot, and the ONE system it does claim keeps cores[0], keeps its resolved default, and gains exactly
//     one appended target. This is #98's "nothing here weakens the curated path", in the shape #100's N=0
//     identity takes.
//   * INSPECTION. retro_get_system_info read off a real loaded library: name, version, and the extensions
//     split out of a deliberately messy valid_extensions string ("EBF|.ebfixture||nes" -> ebf/ebfixture/nes).
//   * SUPPORTS_NO_GAME. Learned from the SET_SUPPORT_NO_GAME the core declares during retro_set_environment —
//     the only place the libretro API offers it — and turned into a Run entry. A content-requiring core gets
//     no Run entry. The other half of that contract is asserted against the core itself: retro_load_game(NULL)
//     succeeds for the no-game core and is refused by the content core.
//   * THE SENTENCES. A file that is not a library, a library that is not a core, a core speaking a libretro
//     API version we do not, and a path with nothing at it each fail with a message naming the file — never a
//     crash, never a bare false. And a core that ASKS for something we do not provide (Vulkan, a camera) is
//     recorded and reported, advisory, without being refused.
//   * REGISTRATION AND PRECEDENCE. A registered core is APPENDED to the claiming system's candidates, so the
//     catalogue default still wins when nothing is chosen; and it wins when it IS chosen, both per system
//     (the default-core picker's lever) and per game (#51's override, whose resolveCore accepts exactly this
//     candidate list). Its picker row is tagged "(custom core)".
//   * THE ONE-TIME NOTICE. Due once, not due after acknowledgement, and still not due across a reload.
//   * NO DOWNLOADS. mayDownload() — the policy behind CoreManager's two fetch paths — is false for a custom
//     ref and true for a catalogue core, so this increment can never quietly grow the buildbot browser.
//
// Prints CUSTOMCORE-OK on success; any failure prints CUSTOMCORE-FAIL <cond> (line) and exits non-zero.
//
// FIXTURES ARE HAND-COMPUTED ORACLES: every expected id, name, extension list and candidate list is a literal
// derived from the fixture source and the SystemCatalog built-in table, never read back from the function
// under test.
#include "CustomCores.h"
#include "CustomCoreInstall.h"
#include "CoreInspect.h"
#include "EmulationTarget.h"
#include "SystemCatalog.h"
#include "LaunchOptionsStore.h"
#include "LibretroCore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QString>
#include <QStringList>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "CUSTOMCORE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using LaunchOpts::Override;

namespace {

// Wipe the registry back to "nothing has ever been loaded" and drop the cache, so each section starts from
// the same place regardless of what ran before it.
void clearRegistry()
{
    QFile::remove(CustomCores::registryPath());
    CustomCores::reload();
}

void setMode(const char* mode)
{
    if (mode) qputenv("EB_FIXTURE_CORE_MODE", QByteArray(mode));
    else      qunsetenv("EB_FIXTURE_CORE_MODE");
}

// One system's complete, observable behaviour under the emulation model. The identity rail compares these.
struct SystemSnapshot
{
    QStringList candidates;
    QStringList targetIdsA;   // emulationTargetsFor(retroPark=true,  standalone=true)
    QStringList targetIdsB;   // emulationTargetsFor(retroPark=false, standalone=false)
    QString     resolvedId;   // resolveEmulationTarget with NO overrides and NO per-system levers
    QString     launchCore;   // resolveLaunch's core under the same conditions
    int         launchEngine = 0;
};

SystemSnapshot snapshot(const GameSystem& sys)
{
    SystemSnapshot s;
    s.candidates = candidateCoresFor(&sys);
    for (const EmulationTarget& t : emulationTargetsFor(&sys, true, true))  s.targetIdsA << t.id;
    for (const EmulationTarget& t : emulationTargetsFor(&sys, false, false)) s.targetIdsB << t.id;
    s.resolvedId = resolveEmulationTarget(&sys, Override{}, QString(), QString(), EmuBackend::Libretro,
                                          true, true).id;
    const ResolvedLaunch rl = resolveLaunch(&sys, Override{}, QString(), QString(), EmuBackend::Libretro,
                                            true, false, true);
    s.launchCore   = rl.core;
    s.launchEngine = int(rl.engine);
    return s;
}

bool sameSnapshot(const SystemSnapshot& a, const SystemSnapshot& b)
{
    return a.candidates == b.candidates && a.targetIdsA == b.targetIdsA && a.targetIdsB == b.targetIdsB
        && a.resolvedId == b.resolvedId && a.launchCore == b.launchCore && a.launchEngine == b.launchEngine;
}

QMap<QString, SystemSnapshot> snapshotAll()
{
    QMap<QString, SystemSnapshot> out;
    for (const GameSystem& sys : SystemCatalog::systems()) out.insert(sys.id, snapshot(sys));
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // The fixture cores are MODULE libraries built beside this probe by the same CMake run (CMake points their
    // output directory at this executable's). They are found by NAME rather than handed in as a compile
    // definition on purpose: a path baked into a macro would carry the generator's own separators into a C
    // string literal, and a backslash there is an escape sequence, not a path.
    const QDir here(QCoreApplication::applicationDirPath());
    auto fixturePath = [&here](const char* base) {
        // The platform's own suffix first, then the others — an explicit list rather than a "*" glob, so the
        // MSVC import library and .pdb that sit beside a MODULE .dll can never be picked up instead.
        QStringList suffixes{ CustomCoreInstall::librarySuffix() };
        suffixes << QStringLiteral(".dll") << QStringLiteral(".so") << QStringLiteral(".dylib");
        for (const QString& sfx : suffixes)
        {
            const QString p = here.absoluteFilePath(QString::fromLatin1(base) + sfx);
            if (QFileInfo::exists(p)) return p;
        }
        return QString();
    };
    const QString fixture   = fixturePath("probe_customcore_fixture");
    const QString notACore  = fixturePath("probe_customcore_notacore");
    CHECK(QFileInfo::exists(fixture));
    CHECK(QFileInfo::exists(notACore));
    if (!QFileInfo::exists(fixture) || !QFileInfo::exists(notACore))
    {
        std::fprintf(stderr, "CUSTOMCORE: fixture core not built at %s / %s\n",
                     qUtf8Printable(fixture), qUtf8Printable(notACore));
        return 1;
    }

    clearRegistry();

    // ---- 1. Pure vocabulary: the "custom:<id>" ref -------------------------------------------------------
    {
        CHECK(CustomCores::refFor(QStringLiteral("mesen")) == QStringLiteral("custom:mesen"));
        CHECK(CustomCores::refFor(QString()).isEmpty());
        CHECK(CustomCores::isCustomRef(QStringLiteral("custom:mesen")));
        CHECK(!CustomCores::isCustomRef(QStringLiteral("custom:")));      // a prefix with no id is not a ref
        CHECK(!CustomCores::isCustomRef(QStringLiteral("fceumm")));
        CHECK(!CustomCores::isCustomRef(QString()));
        CHECK(CustomCores::idFromRef(QStringLiteral("custom:mesen")) == QStringLiteral("mesen"));
        CHECK(CustomCores::idFromRef(QStringLiteral("fceumm")).isEmpty());

        // Sanitising: lowercase, runs of anything else collapse to one '_', never leading/trailing.
        CHECK(CustomCores::sanitizeId(QStringLiteral("Mesen (nes/snes)")) == QStringLiteral("mesen_nes_snes"));
        CHECK(CustomCores::sanitizeId(QStringLiteral("  TIC-80  ")) == QStringLiteral("tic_80"));
        CHECK(CustomCores::sanitizeId(QStringLiteral("2048")) == QStringLiteral("2048"));
        CHECK(CustomCores::sanitizeId(QStringLiteral("!!!")).isEmpty());
        CHECK(CustomCores::sanitizeId(QString()).isEmpty());

        // No downloads, ever, for the custom tier. Catalogue cores are unaffected.
        CHECK(!CustomCores::mayDownload(QStringLiteral("custom:mesen")));
        CHECK(CustomCores::mayDownload(QStringLiteral("fceumm")));
        CHECK(CustomCores::mayDownload(QStringLiteral("mupen64plus_next")));
    }

    // ---- 2. Pure: valid_extensions splitting -------------------------------------------------------------
    {
        CHECK(CoreInspect::splitExtensions(QStringLiteral("EBF|.ebfixture||nes"))
              == (QStringList{ QStringLiteral("ebf"), QStringLiteral("ebfixture"), QStringLiteral("nes") }));
        CHECK(CoreInspect::splitExtensions(QString()).isEmpty());
        CHECK(CoreInspect::splitExtensions(QStringLiteral("|||")).isEmpty());
        CHECK(CoreInspect::splitExtensions(QStringLiteral(" gba | GBA ")) == (QStringList{ QStringLiteral("gba") }));
    }

    // ---- 3. Pure: the registry document round-trips, and a broken one costs nothing ----------------------
    {
        CustomCore c;
        c.id = QStringLiteral("eb_fixture_content");
        c.path = QStringLiteral("/cores/custom/x.dll");
        c.name = QStringLiteral("EB Fixture Content");
        c.version = QStringLiteral("0.1");
        c.extensions = QStringList{ QStringLiteral("ebf"), QStringLiteral("nes") };
        c.supportsNoGame = true;
        c.needFullpath = true;
        c.needs = QStringLiteral("wants a camera");
        c.addedAt = 1234567890123LL;
        CHECK(CustomCores::fromJson(CustomCores::toJson(c)) == c);

        CustomCoreRegistry r;
        r.noticeAcknowledged = true;
        r.cores << c;
        QString err;
        const CustomCoreRegistry back = CustomCores::parse(CustomCores::serialize(r), &err);
        CHECK(err.isEmpty());
        CHECK(back.noticeAcknowledged);
        CHECK(back.cores.size() == 1);
        CHECK(back.cores.value(0) == c);

        // Malformed => EMPTY, with a reason. Never a partial registry, never a crash.
        err.clear();
        const CustomCoreRegistry bad = CustomCores::parse(QByteArray("{ this is not json"), &err);
        CHECK(!err.isEmpty());
        CHECK(bad.cores.isEmpty());
        CHECK(!bad.noticeAcknowledged);
        err.clear();
        const CustomCoreRegistry wrongShape =
            CustomCores::parse(QByteArray("{\"noticeAcknowledged\":true,\"cores\":5}"), &err);
        CHECK(!err.isEmpty());
        CHECK(wrongShape.cores.isEmpty());
        CHECK(!wrongShape.noticeAcknowledged);
        // One bad row does not cost the others.
        err.clear();
        const CustomCoreRegistry mixed = CustomCores::parse(
            QByteArray("{\"cores\":[{\"name\":\"no id\"},{\"id\":\"ok\",\"path\":\"/p\",\"name\":\"OK\"}]}"), &err);
        CHECK(!err.isEmpty());
        CHECK(mixed.cores.size() == 1);
        CHECK(mixed.cores.value(0).id == QStringLiteral("ok"));
        // An absent/empty file is an empty registry and NOT an error.
        err.clear();
        CHECK(CustomCores::parse(QByteArray(), &err).cores.isEmpty());
        CHECK(err.isEmpty());
    }

    // ---- 4. THE NO-CUSTOM-CORE IDENTITY, taken before anything is registered -----------------------------
    clearRegistry();
    CHECK(CustomCores::all().isEmpty());
    for (const GameSystem& sys : SystemCatalog::systems())
        CHECK(candidateCoresFor(&sys) == sys.cores);      // the seam is the identity function
    const QMap<QString, SystemSnapshot> before = snapshotAll();
    CHECK(before.size() == SystemCatalog::systems().size());

    // ---- 5. Inspection: a content-requiring core --------------------------------------------------------
    setMode("content");
    CoreInspection contentInfo;
    {
        QString err;
        CHECK(CoreInspect::inspect(fixture, &contentInfo, &err));
        CHECK(err.isEmpty());
        CHECK(contentInfo.libraryName == QStringLiteral("EB Fixture Content"));
        CHECK(contentInfo.libraryVersion == QStringLiteral("0.1"));
        CHECK(contentInfo.extensions
              == (QStringList{ QStringLiteral("ebf"), QStringLiteral("ebfixture"), QStringLiteral("nes") }));
        CHECK(contentInfo.apiVersion == 1u);
        CHECK(!contentInfo.supportsNoGame);
        CHECK(contentInfo.unmet.isEmpty());
        CHECK(CoreInspect::unmetSentence(contentInfo).isEmpty());
    }

    // ---- 6. Inspection: a no-content (game-engine) core -------------------------------------------------
    setMode("nogame");
    CoreInspection engineInfo;
    {
        QString err;
        CHECK(CoreInspect::inspect(fixture, &engineInfo, &err));
        CHECK(err.isEmpty());
        CHECK(engineInfo.libraryName == QStringLiteral("EB Fixture Engine"));
        CHECK(engineInfo.extensions.isEmpty());
        CHECK(engineInfo.supportsNoGame);          // learned from SET_SUPPORT_NO_GAME, the only place it lives
    }

    // ---- 7. The sentences: every way a file can fail to be a core we can host ----------------------------
    {
        QString err;
        setMode("badapi");
        CHECK(!CoreInspect::inspect(fixture, nullptr, &err));
        CHECK(!err.isEmpty());
        CHECK(err.contains(QStringLiteral("42")));            // names the version the core reported
        CHECK(err.contains(QFileInfo(fixture).fileName()));   // and the file

        err.clear();
        setMode("content");
        CHECK(!CoreInspect::inspect(notACore, nullptr, &err));
        CHECK(!err.isEmpty());
        CHECK(err.contains(QFileInfo(notACore).fileName()));

        err.clear();
        const QString missing = QDir(CustomCores::customDir()).absoluteFilePath(QStringLiteral("nothing-here.bin"));
        CHECK(!CoreInspect::inspect(missing, nullptr, &err));
        CHECK(!err.isEmpty());

        err.clear();
        CHECK(!CoreInspect::inspect(QString(), nullptr, &err));
        CHECK(!err.isEmpty());

        // A file that exists but is not a library at all.
        const QString junkPath = QDir(CustomCores::customDir()).absoluteFilePath(QStringLiteral("junk.bin"));
        QFile junk(junkPath);
        CHECK(junk.open(QIODevice::WriteOnly));
        junk.write(QByteArray(64, 'J'));
        junk.close();
        err.clear();
        CHECK(!CoreInspect::inspect(junkPath, nullptr, &err));
        CHECK(!err.isEmpty());
        QFile::remove(junkPath);
    }

    // ---- 8. A core that asks for what we do not provide: recorded, reported, NOT refused ------------------
    {
        setMode("needs");
        QString err;
        CoreInspection greedy;
        CHECK(CoreInspect::inspect(fixture, &greedy, &err));   // it LOADS — advisory, never a refusal
        CHECK(err.isEmpty());
        CHECK(greedy.libraryName == QStringLiteral("EB Fixture Greedy"));
        CHECK(greedy.unmet.size() == 2);
        CHECK(greedy.unmet.join(QLatin1Char('|')).contains(QStringLiteral("Vulkan")));
        CHECK(greedy.unmet.join(QLatin1Char('|')).contains(QStringLiteral("camera")));
        const QString sentence = CoreInspect::unmetSentence(greedy);
        CHECK(!sentence.isEmpty());
        CHECK(sentence.contains(QStringLiteral("EB Fixture Greedy")));
        CHECK(sentence.contains(QStringLiteral("Vulkan")));
        // And the pure sentence for a clean core is empty, so nothing is ever said about an ordinary load.
        CHECK(CoreInspect::unmetSentence(CoreInspection{}).isEmpty());
    }

    // ---- 9. The id a core registers under, and the install ------------------------------------------------
    {
        CHECK(CustomCoreInstall::idFor(QStringLiteral("EB Fixture Content"), QStringLiteral("/x/whatever.dll"))
              == QStringLiteral("eb_fixture_content"));
        // No name at all -> the file's base name, with the buildbot's "_libretro" packaging marker dropped.
        CHECK(CustomCoreInstall::idFor(QString(), QStringLiteral("/x/mesen_libretro.dll"))
              == QStringLiteral("mesen"));
        CHECK(CustomCoreInstall::idFor(QString(), QStringLiteral("/x/2048_libretro.dll"))
              == QStringLiteral("2048"));
        CHECK(CustomCoreInstall::idFor(QString(), QStringLiteral("/x/!!!.dll")).isEmpty());
    }

    clearRegistry();
    setMode("content");
    CustomCore contentRec;
    {
        QString err;
        CHECK(CustomCoreInstall::loadFromFile(fixture, &contentRec, &err));
        CHECK(err.isEmpty());
        CHECK(contentRec.id == QStringLiteral("eb_fixture_content"));
        CHECK(contentRec.name == QStringLiteral("EB Fixture Content"));
        CHECK(contentRec.extensions.contains(QStringLiteral("nes")));
        CHECK(!contentRec.supportsNoGame);
        CHECK(contentRec.needs.isEmpty());
        // Copied into <data>/cores/custom, so clearing the folder it came from cannot break the registration.
        CHECK(QFileInfo(contentRec.path).absolutePath()
              == QFileInfo(CustomCores::customDir()).absoluteFilePath());
        CHECK(QFileInfo::exists(contentRec.path));
        // It is in the registry, resolvable, and tagged.
        CHECK(CustomCores::all().size() == 1);
        const QString ref = CustomCores::refFor(contentRec.id);
        CHECK(CustomCores::byRef(ref) != nullptr);
        CHECK(CustomCores::pathForRef(ref) == contentRec.path);
        CHECK(CustomCores::displayNameFor(ref) == QStringLiteral("EB Fixture Content (custom core)"));
        CHECK(CustomCores::displayNameFor(QStringLiteral("fceumm")).isEmpty());

        // Re-loading the same core is an UPDATE, not a second entry.
        CHECK(CustomCoreInstall::loadFromFile(fixture, nullptr, &err));
        CHECK(CustomCores::all().size() == 1);
    }
    const QString contentRef = CustomCores::refFor(contentRec.id);

    // ---- 10. Registration puts the core in the claiming system's candidates — APPENDED ------------------
    {
        const GameSystem* nes = SystemCatalog::byId(QStringLiteral("nes"));
        const GameSystem* gba = SystemCatalog::byId(QStringLiteral("gba"));
        CHECK(nes != nullptr);
        CHECK(gba != nullptr);
        if (!nes || !gba) { std::fprintf(stderr, "CUSTOMCORE: catalog missing nes/gba\n"); return 1; }

        // nes claims ".nes", which the fixture claims too -> appended AFTER both catalogue cores.
        CHECK(candidateCoresFor(nes)
              == (QStringList{ QStringLiteral("fceumm"), QStringLiteral("nestopia"), contentRef }));
        CHECK(candidateCoresFor(nes).value(0) == QStringLiteral("fceumm"));   // the default is untouched
        // gba claims ".gba" only -> the fixture does not claim it, so the list is exactly the catalogue's.
        CHECK(candidateCoresFor(gba) == gba->cores);

        // The picker offers it, last among the cores, tagged as custom.
        const QList<EmulationTarget> targets = emulationTargetsFor(nes, true, true);
        CHECK(targets.size() == 4);   // fceumm, nestopia, custom, retropark
        if (targets.size() == 4)
        {
            CHECK(targets[0].id == QStringLiteral("libretro:fceumm"));
            CHECK(targets[1].id == QStringLiteral("libretro:nestopia"));
            CHECK(targets[2].id == QStringLiteral("libretro:") + contentRef);
            CHECK(targets[2].ref == contentRef);
            CHECK(targets[2].displayName == QStringLiteral("EB Fixture Content (custom core)"));
            CHECK(targets[3].id == QStringLiteral("retropark"));
        }

        // ---- 11. It does NOT displace the default. Nothing chosen -> fceumm, exactly as before.
        CHECK(resolveEmulationTarget(nes, Override{}, QString(), QString(), EmuBackend::Libretro, true, true).id
              == QStringLiteral("libretro:fceumm"));
        CHECK(resolveLaunch(nes, Override{}, QString(), QString(), EmuBackend::Libretro, true, false, true).core
              == QStringLiteral("fceumm"));

        // ---- 12. The EXPLICIT choice wins — per game (#51) ...
        Override ov;
        ov.core = contentRef;
        ov.backend = QStringLiteral("libretro");
        CHECK(resolveEmulationTarget(nes, ov, QString(), QString(), EmuBackend::Libretro, true, true).ref
              == contentRef);
        const ResolvedLaunch perGame = resolveLaunch(nes, ov, QString(), QString(), EmuBackend::Libretro,
                                                     true, false, true);
        CHECK(perGame.engine == EmuEngine::Libretro);
        CHECK(perGame.core == contentRef);
        // ... and per system (the default-core picker's lever).
        CHECK(resolveEmulationTarget(nes, Override{}, contentRef, QString(), EmuBackend::Libretro, true, true).ref
              == contentRef);
        CHECK(resolveLaunch(nes, Override{}, contentRef, QString(), EmuBackend::Libretro, true, false, true).core
              == contentRef);
        // A per-game pick still beats a per-system one, unchanged by any of this.
        CHECK(resolveLaunch(nes, ov, QStringLiteral("nestopia"), QString(), EmuBackend::Libretro,
                            true, false, true).core == contentRef);

        // The round trip the picker relies on: selecting the offered target resolves back to it.
        Override sel;
        applyTargetToOverride(targets[2], sel);
        CHECK(sel.core == contentRef);
        CHECK(resolveEmulationTarget(nes, sel, QString(), QString(), EmuBackend::Libretro, true, true).id
              == targets[2].id);

        // A ref naming a core that is NOT registered is stale and ignored — the default stands, never an error.
        Override stale;
        stale.core = QStringLiteral("custom:not_registered");
        stale.backend = QStringLiteral("libretro");
        CHECK(resolveLaunch(nes, stale, QString(), QString(), EmuBackend::Libretro, true, false, true).core
              == QStringLiteral("fceumm"));
    }

    // ---- 13. IDENTITY, RE-CHECKED: only the claiming system moved ---------------------------------------
    {
        const QMap<QString, SystemSnapshot> after = snapshotAll();
        CHECK(after.size() == before.size());
        int changed = 0;
        for (auto it = before.constBegin(); it != before.constEnd(); ++it)
        {
            const SystemSnapshot& b = it.value();
            const SystemSnapshot& a = after.value(it.key());
            if (sameSnapshot(a, b)) continue;
            ++changed;
            CHECK(it.key() == QStringLiteral("nes"));               // the ONLY system claiming ".nes"
            CHECK(a.candidates.mid(0, b.candidates.size()) == b.candidates); // catalogue prefix intact, in order
            CHECK(a.candidates.size() == b.candidates.size() + 1);           // exactly one appended
            CHECK(a.resolvedId == b.resolvedId);                    // the resolved default did NOT move
            CHECK(a.launchCore == b.launchCore);
            CHECK(a.launchEngine == b.launchEngine);
        }
        CHECK(changed == 1);
    }

    // ---- 14. supports_no_game earns a Run entry; a content core does not ---------------------------------
    setMode("nogame");
    CustomCore engineRec;
    {
        QString err;
        CHECK(CustomCoreInstall::loadFromFile(fixture, &engineRec, &err));
        CHECK(err.isEmpty());
        CHECK(engineRec.id == QStringLiteral("eb_fixture_engine"));
        CHECK(engineRec.supportsNoGame);
        CHECK(engineRec.extensions.isEmpty());
        CHECK(CustomCores::all().size() == 2);

        const QList<CustomCore> runnable = CustomCores::runEntries(CustomCores::all());
        CHECK(runnable.size() == 1);
        CHECK(runnable.value(0).id == engineRec.id);

        // A core with no extensions claims no system, so it appears in NO candidate list — its only way in is
        // the Run entry, which is exactly why supports_no_game has to earn one.
        for (const GameSystem& sys : SystemCatalog::systems())
            CHECK(!candidateCoresFor(&sys).contains(CustomCores::refFor(engineRec.id)));

        // Pure, on hand-built input, so the rule is pinned independently of the fixture.
        CustomCore a; a.id = QStringLiteral("a"); a.supportsNoGame = false;
        CustomCore b; b.id = QStringLiteral("b"); b.supportsNoGame = true;
        CustomCore c; c.id = QStringLiteral("c"); c.supportsNoGame = true;
        const QList<CustomCore> picked = CustomCores::runEntries(QList<CustomCore>{ a, b, c });
        CHECK(picked.size() == 2);
        CHECK(picked.value(0).id == QStringLiteral("b"));
        CHECK(picked.value(1).id == QStringLiteral("c"));   // order preserved
        CHECK(CustomCores::runEntries(QList<CustomCore>{ a }).isEmpty());
    }

    // ---- 15. The no-content CONTRACT against the core itself ---------------------------------------------
    // retro_load_game(NULL), not a pointer to an empty struct. A core-side `if (!info)` is how every game-engine
    // core detects the case, so this is the difference between a Run entry that works and one that reports
    // "core rejected the game".
    {
        setMode("nogame");
        LibretroCore core;
        std::string err;
        CHECK(core.loadCore(engineRec.path.toStdString(), &err));
        if (core.coreLoaded())
        {
            CHECK(core.loadGame(std::string(), &err));
            CHECK(core.gameLoaded());
            core.runFrame();
            CHECK(!core.crashed());
            CHECK(core.hasFrame());
            CHECK(core.frameWidth() == 64u);
            CHECK(core.frameHeight() == 48u);
            core.unload();
        }
    }
    {
        setMode("content");
        LibretroCore core;
        std::string err;
        CHECK(core.loadCore(contentRec.path.toStdString(), &err));
        if (core.coreLoaded())
        {
            CHECK(!core.loadGame(std::string(), &err));   // a content core refuses NULL, and we report it
            CHECK(!core.gameLoaded());
            core.unload();
        }
    }

    // ---- 16. The one-time notice -------------------------------------------------------------------------
    {
        clearRegistry();
        CHECK(CustomCores::noticeDue(CustomCores::registry()));
        CHECK(!CustomCores::noticeText().isEmpty());
        CHECK(CustomCores::noticeText().contains(QStringLiteral("not curated")));
        CustomCores::acknowledgeNotice();
        CHECK(!CustomCores::noticeDue(CustomCores::registry()));
        CustomCores::reload();                       // and it survives a restart
        CHECK(!CustomCores::noticeDue(CustomCores::registry()));
        // Acknowledging twice is a no-op, and loading more cores never re-arms it.
        CustomCores::acknowledgeNotice();
        setMode("content");
        QString err;
        CHECK(CustomCoreInstall::loadFromFile(fixture, nullptr, &err));
        CHECK(!CustomCores::noticeDue(CustomCores::registry()));
        // Pure: the rule itself, independent of the store.
        CustomCoreRegistry fresh;
        CHECK(CustomCores::noticeDue(fresh));
        fresh.noticeAcknowledged = true;
        CHECK(!CustomCores::noticeDue(fresh));
    }

    // ---- 17. Removal, and the drop-it-in-the-folder scan --------------------------------------------------
    {
        clearRegistry();
        setMode("content");
        QString err;
        CHECK(CustomCoreInstall::loadFromFile(fixture, nullptr, &err));
        CHECK(CustomCores::all().size() == 1);
        // Now that the copy sits in <data>/cores/custom and IS registered, the scan must not offer it again.
        CHECK(!CustomCoreInstall::unregisteredInCustomDir().contains(CustomCores::all().value(0).path));
        CHECK(CustomCores::remove(CustomCores::all().value(0).id));
        CHECK(CustomCores::all().isEmpty());
        CHECK(!CustomCores::remove(QStringLiteral("nothing")));
        // With the record gone the file is still there, so the folder scan now offers it — the "drop a core in
        // the folder" path, which is the same load with no copy step.
        bool offered = false;
        for (const QString& p : CustomCoreInstall::unregisteredInCustomDir())
            if (QFileInfo(p).fileName() == QFileInfo(fixture).fileName()) offered = true;
        CHECK(offered);
        // And after registering it from there, the file is NOT copied onto itself.
        const QStringList inFolder = CustomCoreInstall::unregisteredInCustomDir();
        if (!inFolder.isEmpty())
        {
            CustomCore rec;
            CHECK(CustomCoreInstall::loadFromFile(inFolder.value(0), &rec, &err));
            CHECK(QFileInfo::exists(rec.path));
            CHECK(QFileInfo(rec.path).size() > 0);
        }
    }

    // ---- 18. And with everything removed again, the catalogue is back to itself ---------------------------
    {
        clearRegistry();
        const QMap<QString, SystemSnapshot> restored = snapshotAll();
        for (auto it = before.constBegin(); it != before.constEnd(); ++it)
            CHECK(sameSnapshot(restored.value(it.key()), it.value()));
    }

    setMode(nullptr);
    if (failures == 0) std::printf("CUSTOMCORE-OK\n");
    else               std::fprintf(stderr, "CUSTOMCORE: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
