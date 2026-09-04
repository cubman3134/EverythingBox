// Headless check of LAUNCH RECIPES (src/core/LaunchRecipe.h, issue #190) — the per-system launch knowledge a
// retro COMPUTER needs, carried as data instead of as folklore in the user's head.
//
// FOUR THINGS HERE CAN FAIL SILENTLY, and each one is a rail below:
//
//  1. THE SHIPPED RECIPES ARE NOT EMBEDDED. A .qrc that reaches its target the wrong way produces no rcc
//     output and says nothing about it (native/CMakeLists.txt records that this is measured, not theoretical:
//     ports.qrc listed in qt_add_executable's source list embedded nothing). The only symptom would be "the
//     DOS game still doesn't launch" — indistinguishable from a bug in the engine. So every shipped recipe is
//     read OUT OF THE RESOURCE, exactly as the app reads it, and compared BYTE FOR BYTE with the file in the
//     source tree. A missing, stale or misaliased resource all fail here.
//
//  2. A MALFORMED RECIPE READS AS AN EMPTY ONE. "No recipe" is a legitimate, common answer (most systems have
//     none), so a parser that returned an empty recipe for broken bytes would make a corrupt file look
//     exactly like a system nobody has written a recipe for. parse() must say WHY, and does.
//
//  3. THE DOS EXECUTABLE PICK IS WRONG BUT PLAUSIBLE. Launching INSTALL.EXE instead of the game is not a
//     crash; it is a DOS installer asking which sound card you have. Every step of the documented priority
//     is pinned here, in order, INCLUDING the ambiguous case (which must stay ambiguous — a rule that
//     silently resolved it would replace a two-row menu with a coin flip) and including the guard that the
//     avoid-list can never remove the last candidate.
//
//  4. THE FIRMWARE MESSAGE DOESN'T NAME THE FILE. The whole firmware half of #190 is that the user is told
//     the exact file and the exact folder instead of getting a black screen. A message that says "firmware
//     missing" passes every test that isn't this one, so the wording is asserted against literals.
//
// Expected values are hand-authored oracles, never read back out of the code under test. Prints RECIPES-OK on
// success; on failure prints RECIPES-FAIL <cond> per failure and exits non-zero.
#include "LaunchRecipe.h"
#include "SystemCatalog.h"
#include "AmsdosCatalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "RECIPES-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(bytes);
    return true;
}

static QByteArray readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // The systems this build ships a recipe for. Hand-listed (not globbed off disk) so that DELETING a recipe
    // file, or forgetting its .qrc line, is a failure rather than a quietly shorter loop.
    const QStringList shipped = {
        QStringLiteral("msdos"), QStringLiteral("amiga"), QStringLiteral("atarist"),
        QStringLiteral("c64"),   QStringLiteral("vic20"), QStringLiteral("zxspectrum"),
        QStringLiteral("amstradcpc"), QStringLiteral("apple2"),
        QStringLiteral("pc98"),  QStringLiteral("x1"),
        QStringLiteral("msx"),
    };

    // ---- 1. embedded == source, and every shipped recipe parses ------------------------------------------
    {
        const QString srcDir = QString::fromUtf8(EB_RECIPES_SOURCE_DIR);
        for (const QString& id : shipped)
        {
            const QByteArray embedded = readAll(LaunchRecipes::shippedResource(id));
            const QByteArray onDisk   = readAll(srcDir + QStringLiteral("/") + id + QStringLiteral(".json"));
            if (embedded.isEmpty())
                std::fprintf(stderr, "RECIPES-FAIL %s.json is not embedded in this build (.qrc not wired up)\n",
                             qUtf8Printable(id));
            CHECK(!embedded.isEmpty());
            CHECK(!onDisk.isEmpty());
            CHECK(embedded == onDisk);   // a stale or misaliased resource

            LaunchRecipe r;
            QString err;
            CHECK(LaunchRecipes::parse(embedded, &r, &err));
            CHECK(err.isEmpty());
            CHECK(r.systemId == id);                       // the file's own id matches its name / its alias
            CHECK(!r.summary.isEmpty());                   // every recipe explains itself to the folder README
            CHECK(!r.cores.isEmpty());                     // a recipe that names no core says nothing
            // A recipe for a system that does not exist is dead data the user can never reach. Checked
            // against the BUILT-IN table, not systems(), so a <data> file cannot mask a missing system.
            bool known = false;
            for (const GameSystem& s : SystemCatalog::builtinSystems()) if (s.id == id) { known = true; break; }
            CHECK(known);
            // Every core a recipe names must be a candidate core of that system, or the options it seeds are
            // pushed at a core that will never run. (This catches an upstream core rename in review.)
            for (const GameSystem& s : SystemCatalog::builtinSystems())
            {
                if (s.id != id) continue;
                for (const RecipeCore& rc : r.cores)
                    CHECK(rc.core == QLatin1String("*") || s.cores.contains(rc.core));
            }
            // No recipe may declare a content presentation we cannot read: a typo would degrade to today's
            // behaviour in the app, which is safe but silent, so it is caught HERE instead.
            for (const RecipeCore& rc : r.cores)
                for (const RecipeContentRule& rule : rc.content)
                    CHECK(rule.present != Presentation::Unknown);
        }
    }

    // ---- 2. malformed is an ERROR, never an empty recipe ---------------------------------------------------
    {
        LaunchRecipe r;
        QString err;
        CHECK(!LaunchRecipes::parse(QByteArray("{ this is not json"), &r, &err));
        CHECK(!err.isEmpty());

        err.clear();
        CHECK(!LaunchRecipes::parse(QByteArray("[{\"system\":\"msdos\"}]"), &r, &err));  // an ARRAY, not an object
        CHECK(err.contains(QLatin1String("not a JSON object")));

        err.clear();
        CHECK(!LaunchRecipes::parse(QByteArray("{\"summary\":\"no id\"}"), &r, &err));
        CHECK(err.contains(QLatin1String("system")));

        // ...and a well-formed minimal recipe IS accepted (so the checks above are refusing the right things).
        err.clear();
        CHECK(LaunchRecipes::parse(QByteArray("{\"system\":\"msdos\"}"), &r, &err));
        CHECK(r.systemId == QLatin1String("msdos"));
        CHECK(r.cores.isEmpty());
    }

    // ---- 3. the <data> override wins, and a broken override costs only itself -------------------------------
    {
        QTemporaryDir tmp;
        CHECK(tmp.isValid());
        const QString dataDir = tmp.path() + QStringLiteral("/systems/recipes");

        // No user file -> the shipped recipe.
        LaunchRecipe base = LaunchRecipes::load(QStringLiteral("msdos"), dataDir);
        CHECK(base.systemId == QLatin1String("msdos"));
        CHECK(base.folderIsGame);
        const RecipeCore* baseCore = LaunchRecipes::coreFor(base, QStringLiteral("dosbox_pure"));
        CHECK(baseCore != nullptr);

        // A user file -> it WINS outright (a whole-file override, like a <data>/systems entry replacing a row).
        CHECK(writeFile(dataDir + QStringLiteral("/msdos.json"),
                        QByteArray("{\"system\":\"msdos\",\"summary\":\"mine\",\"folderIsGame\":false,"
                                   "\"cores\":[{\"core\":\"dosbox_pure\",\"options\":{\"dosbox_pure_cycles\":\"max\"}}]}")));
        LaunchRecipe mine = LaunchRecipes::load(QStringLiteral("msdos"), dataDir);
        CHECK(mine.summary == QLatin1String("mine"));
        CHECK(!mine.folderIsGame);
        const RecipeCore* mineCore = LaunchRecipes::coreFor(mine, QStringLiteral("dosbox_pure"));
        CHECK(mineCore != nullptr);
        if (mineCore) CHECK(mineCore->options.value(QStringLiteral("dosbox_pure_cycles")) == QLatin1String("max"));

        // A user file that does NOT parse -> reported, ignored, shipped recipe stands. A typo must never cost
        // the user a launch that worked yesterday.
        QStringList warnings;
        CHECK(writeFile(dataDir + QStringLiteral("/msdos.json"), QByteArray("{ oops")));
        LaunchRecipe fallback = LaunchRecipes::load(QStringLiteral("msdos"), dataDir,
                                                    [&](const QString& m) { warnings << m; });
        CHECK(warnings.size() == 1);
        CHECK(fallback.folderIsGame);   // the shipped one again
        CHECK(fallback.summary == base.summary);

        // A system with no recipe anywhere is an ordinary null, not an error.
        LaunchRecipe none = LaunchRecipes::load(QStringLiteral("nes"), dataDir);
        CHECK(none.isNull());
    }

    // ---- 4. core lookup + content presentation --------------------------------------------------------------
    {
        LaunchRecipe r;
        QString err;
        CHECK(LaunchRecipes::parse(
            QByteArray("{\"system\":\"msdos\",\"cores\":["
                       "{\"core\":\"dosbox_pure\",\"content\":[{\"when\":\"archive\",\"present\":\"asIs\"},"
                                                              "{\"when\":\"folder\",\"present\":\"executable\"}]},"
                       "{\"core\":\"*\",\"content\":[{\"when\":\"archive\",\"present\":\"extract\"}]}]}"),
            &r, &err));
        const RecipeCore* pure = LaunchRecipes::coreFor(r, QStringLiteral("dosbox_pure"));
        const RecipeCore* other = LaunchRecipes::coreFor(r, QStringLiteral("dosbox_core"));
        CHECK(pure != nullptr && other != nullptr);
        CHECK(pure && pure->core == QLatin1String("dosbox_pure"));
        CHECK(other && other->core == QLatin1String("*"));   // the any-core fallback, not the exact entry
        if (pure)
        {
            CHECK(LaunchRecipes::presentationFor(*pure, ContentKind::Archive) == Presentation::AsIs);
            CHECK(LaunchRecipes::presentationFor(*pure, ContentKind::Folder) == Presentation::Executable);
            // A shape the recipe says NOTHING about must read as Unknown, NOT as AsIs. This is the one that
            // matters: on an archive, "asIs" and "unstated" are opposite actions (hand the ZIP over vs
            // extract it as the app always has), so collapsing them would silently turn extraction off for
            // every system that has a recipe at all.
            CHECK(LaunchRecipes::presentationFor(*pure, ContentKind::File) == Presentation::Unknown);
        }
        if (other) CHECK(LaunchRecipes::presentationFor(*other, ContentKind::Archive) == Presentation::Extract);

        // An unreadable spelling is Unknown at the parse level (the caller decides it means "as before").
        CHECK(LaunchRecipes::presentationFromString(QStringLiteral("mount-as-drive-q")) == Presentation::Unknown);
        CHECK(LaunchRecipes::presentationFromString(QStringLiteral("asis")) == Presentation::AsIs); // case-insensitive
    }

    // ---- 5. the DOS executable pick, step by documented step -------------------------------------------------
    {
        const LaunchRecipe dos = LaunchRecipes::load(QStringLiteral("msdos"), QString());
        const RecipeExecutables& ex = dos.executables;
        CHECK(ex.extensions.contains(QStringLiteral("exe")));
        CHECK(ex.extensions.contains(QStringLiteral("bat")));
        CHECK(ex.extensions.contains(QStringLiteral("com")));
        CHECK(ex.avoid.contains(QStringLiteral("install")));
        CHECK(ex.avoid.contains(QStringLiteral("setup")));

        // (a) exactly one program -> no question asked.
        {
            const auto p = LaunchRecipes::chooseExecutable(
                { QStringLiteral("DOOM.EXE"), QStringLiteral("DOOM1.WAD"), QStringLiteral("README.TXT") },
                QStringLiteral("Doom"), ex);
            CHECK(p.chosen == QLatin1String("DOOM.EXE"));
            CHECK(!p.ambiguous());
        }
        // (b) a .BAT beats a .EXE — the DOS convention.
        {
            const auto p = LaunchRecipes::chooseExecutable(
                { QStringLiteral("START.BAT"), QStringLiteral("GAME.EXE") }, QStringLiteral("Whatever"), ex);
            CHECK(p.chosen == QLatin1String("START.BAT"));
        }
        // (c) no .BAT -> the program named after the folder.
        {
            const auto p = LaunchRecipes::chooseExecutable(
                { QStringLiteral("KEEN.EXE"), QStringLiteral("MODEM.EXE") }, QStringLiteral("KEEN"), ex);
            CHECK(p.chosen == QLatin1String("KEEN.EXE"));
        }
        // (d) the avoid-list demotes an installer out of the running BEFORE the .BAT rule sees it — otherwise
        //     INSTALL.BAT, which every second DOS ZIP carries, would win every time.
        {
            const auto p = LaunchRecipes::chooseExecutable(
                { QStringLiteral("INSTALL.BAT"), QStringLiteral("PRINCE.EXE") }, QStringLiteral("Prince"), ex);
            CHECK(p.chosen == QLatin1String("PRINCE.EXE"));
        }
        // (e) ...but the avoid-list may never remove the LAST candidate: a folder whose only program is an
        //     installer still has to be launchable.
        {
            const auto p = LaunchRecipes::chooseExecutable(
                { QStringLiteral("INSTALL.EXE"), QStringLiteral("DATA.DAT") }, QStringLiteral("Thing"), ex);
            CHECK(p.chosen == QLatin1String("INSTALL.EXE"));
        }
        // (f) a launcher at the top of the folder beats one buried in a sub-directory.
        {
            const auto p = LaunchRecipes::chooseExecutable(
                { QStringLiteral("GAME.EXE"), QStringLiteral("UTILS/EDIT.EXE"), QStringLiteral("UTILS/CONV.EXE") },
                QStringLiteral("Game"), ex);
            CHECK(p.chosen == QLatin1String("GAME.EXE"));
        }
        // (g) THE AMBIGUOUS CASE — and it must STAY ambiguous. Two unrelated .EXEs, neither named after the
        //     folder, no .BAT: the app has no basis to choose and must ask. The candidate list is what the
        //     picker shows, so its contents and order are pinned too.
        {
            const auto p = LaunchRecipes::chooseExecutable(
                { QStringLiteral("ZOOL.EXE"), QStringLiteral("ARENA.EXE"), QStringLiteral("SOUND.DAT") },
                QStringLiteral("Compilation"), ex);
            CHECK(p.chosen.isEmpty());
            CHECK(p.ambiguous());
            CHECK(p.candidates == QStringList({ QStringLiteral("ARENA.EXE"), QStringLiteral("ZOOL.EXE") }));
        }
        // (h) nothing that looks like a program at all -> no pick, no menu; the caller falls back to handing
        //     the folder to the core.
        {
            const auto p = LaunchRecipes::chooseExecutable(
                { QStringLiteral("DISK1.IMG"), QStringLiteral("NOTES.TXT") }, QStringLiteral("X"), ex);
            CHECK(p.chosen.isEmpty());
            CHECK(p.candidates.isEmpty());
            CHECK(!p.ambiguous());
        }
        // (i) an explicit `prefer` name wins outright, ahead of every later rule (no DOS recipe uses this
        //     today; the rule exists for a system whose launcher has a fixed name, and is pinned so a future
        //     recipe can rely on it).
        {
            RecipeExecutables custom = ex;
            custom.prefer = { QStringLiteral("play") };
            const auto p = LaunchRecipes::chooseExecutable(
                { QStringLiteral("START.BAT"), QStringLiteral("PLAY.EXE") }, QStringLiteral("Thing"), custom);
            CHECK(p.chosen == QLatin1String("PLAY.EXE"));
        }

        // folderLooksLikeGame: the scan-side half of the same rule.
        CHECK(LaunchRecipes::folderLooksLikeGame(dos, { QStringLiteral("GAME.EXE"), QStringLiteral("G.DAT") }));
        CHECK(!LaunchRecipes::folderLooksLikeGame(dos, { QStringLiteral("A.WAD"), QStringLiteral("B.DAT") }));
        // ...and it is OFF for a system whose recipe does not claim folders (a c64 sub-folder is not a game).
        LaunchRecipe noFolders = dos; noFolders.folderIsGame = false;
        CHECK(!LaunchRecipes::folderLooksLikeGame(noFolders, { QStringLiteral("GAME.EXE") }));
    }

    // ---- 6. firmware: what is missing, and the sentence the user reads ---------------------------------------
    {
        LaunchRecipe r;
        QString err;
        CHECK(LaunchRecipes::parse(
            QByteArray("{\"system\":\"amiga\",\"cores\":[{\"core\":\"puae\",\"firmware\":["
                       "{\"purpose\":\"Kickstart 1.3 (A500)\",\"files\":[\"kick34005.A500\",\"kick34005.a500.rom\"]},"
                       "{\"purpose\":\"IPF disk-image support\",\"files\":[\"capsimg.dll\"],\"required\":false}"
                       "]}]}"),
            &r, &err));
        const RecipeCore* puae = LaunchRecipes::coreFor(r, QStringLiteral("puae"));
        CHECK(puae != nullptr);
        if (!puae) { std::fprintf(stderr, "RECIPES had %d failure(s)\n", ++failures); return 1; }

        // Nothing present -> the REQUIRED entry is reported and the optional one is not.
        auto none = LaunchRecipes::missingFirmware(*puae, [](const QString&) { return false; });
        CHECK(none.size() == 1);
        CHECK(none.value(0).purpose == QLatin1String("Kickstart 1.3 (A500)"));

        // ANY of the acceptable spellings satisfies it — the alternative name, not just the first.
        auto second = LaunchRecipes::missingFirmware(*puae, [](const QString& f) {
            return f == QLatin1String("kick34005.a500.rom");
        });
        CHECK(second.isEmpty());

        // The message names the exact FILE and the exact FOLDER. This wording is the feature.
        const QString msg = LaunchRecipes::firmwareMessage(QStringLiteral("Lemmings"),
                                                           QStringLiteral("Commodore Amiga"), none,
                                                           QStringLiteral("C:/EB/system"));
        CHECK(msg.contains(QLatin1String("Lemmings")));
        CHECK(msg.contains(QLatin1String("Kickstart 1.3 (A500)")));
        CHECK(msg.contains(QLatin1String("kick34005.A500")));       // the file, spelt exactly
        CHECK(msg.contains(QLatin1String("C:/EB/system")));         // the folder, spelt exactly
        CHECK(msg.contains(QLatin1String("kick34005.a500.rom")));   // the alternative is offered, not hidden
        CHECK(msg.contains(QLatin1String("can't download")));       // and we say why we won't fetch it
        // No missing firmware -> no message at all (an empty string is the "nothing to say" spelling).
        CHECK(LaunchRecipes::firmwareMessage(QStringLiteral("Lemmings"), QStringLiteral("Commodore Amiga"),
                                             {}, QStringLiteral("C:/EB/system")).isEmpty());
    }

    // ---- 7. the two systems this increment wires end to end ----------------------------------------------------
    {
        // MS-DOS: an ARCHIVE goes to dosbox-pure UNEXTRACTED (the core reads ZIPs natively and keeps its
        // modifications in a save file beside them; extracting it first is what #190 is fixing), and a FOLDER
        // is presented as the program inside it (dosbox-pure mounts an .EXE/.COM/.BAT's own directory as C:
        // and auto-runs it — verified in the core's source, dosbox_pure_libretro.cpp).
        const LaunchRecipe dos = LaunchRecipes::load(QStringLiteral("msdos"), QString());
        const RecipeCore* pure = LaunchRecipes::coreFor(dos, QStringLiteral("dosbox_pure"));
        CHECK(pure != nullptr);
        if (pure)
        {
            CHECK(LaunchRecipes::presentationFor(*pure, ContentKind::Archive) == Presentation::AsIs);
            CHECK(LaunchRecipes::presentationFor(*pure, ContentKind::Folder) == Presentation::Executable);
            CHECK(pure->firmware.isEmpty());        // DOS needs no firmware from the user
            // The one option we seed is the one that makes a game's own dosbox.conf be honoured.
            CHECK(pure->options.value(QStringLiteral("dosbox_pure_conf")) == QLatin1String("inside"));
        }
        CHECK(dos.folderIsGame);

        // Amiga: a WHDLoad .lha IS the content — puae launches it directly (its own .info declares `lha`), so
        // it is handed over as it is. A .zip around it keeps today's extraction, stated explicitly rather
        // than left to the default, because for an archive silence and "asIs" are opposite actions.
        const LaunchRecipe amiga = LaunchRecipes::load(QStringLiteral("amiga"), QString());
        const RecipeCore* uae = LaunchRecipes::coreFor(amiga, QStringLiteral("puae"));
        CHECK(uae != nullptr);
        if (uae)
        {
            CHECK(LaunchRecipes::presentationFor(*uae, ContentKind::File) == Presentation::AsIs);
            CHECK(LaunchRecipes::presentationFor(*uae, ContentKind::Archive) == Presentation::Extract);
            CHECK(!uae->firmware.isEmpty());
            bool namesAKickstart = false;
            for (const RecipeFirmware& fw : uae->firmware)
                for (const QString& f : fw.files)
                    if (f.startsWith(QLatin1String("kick"), Qt::CaseInsensitive)) namesAKickstart = true;
            CHECK(namesAKickstart);
            CHECK(uae->options.contains(QStringLiteral("puae_use_whdload")));
        }
        CHECK(!amiga.folderIsGame);   // an Amiga game is a file, not a folder

        // Atari ST: the ONE per-system core-option seed that used to be a hardcoded C++ table in RetroView
        // (hatari's TOS auto-detection resolves to an invalid path once the TOS is present) is now this line
        // of data. If this assertion fails, that workaround has been lost and every ST game boots to a
        // "no TOS" screen again.
        const LaunchRecipe st = LaunchRecipes::load(QStringLiteral("atarist"), QString());
        const RecipeCore* hatari = LaunchRecipes::coreFor(st, QStringLiteral("hatari"));
        CHECK(hatari != nullptr);
        if (hatari) CHECK(hatari->options.value(QStringLiteral("hatari_tosimage")) == QLatin1String("default"));
    }


    // ---- 8. INCREMENT 2: the rest of the retro computers, wired ------------------------------------------
    //
    // Increment 1 shipped the recipe engine and wired MS-DOS and Amiga through it. Everything below is the
    // part that was data-only or absent: the MSX system id (a recipe for it could not even be shipped before,
    // because rail 1 asserts every recipe names a REAL system), the two core entries increment 1 named as
    // gaps, per-extension content presentation for every computer system, the per-game core override reaching
    // the archive decision, and the Amstrad CPC boot command read out of a disk's own catalogue.
    {
        // --- MSX exists, is reachable, and stole nothing -------------------------------------------------
        // The id had to be ADDED for this recipe to mean anything: blueMSX was only ever a fallback core for
        // sg1000/coleco, so "MSX" was not a system the app could route a game to. Three routes have to work
        // (id, console name, extension) because a game arrives by all three: a folder named msx/, a shelf
        // labelled "MSX2", and a loose .mx1.
        CHECK(SystemCatalog::byId(QStringLiteral("msx")) != nullptr);
        const GameSystem* msxSys = SystemCatalog::byId(QStringLiteral("msx"));
        if (msxSys)
        {
            CHECK(msxSys->cores.value(0) == QLatin1String("bluemsx"));   // cores[0] is the default
            CHECK(msxSys->cores.contains(QStringLiteral("fmsx")));
            CHECK(msxSys->externalEmulator.isEmpty());                   // in-process libretro, not a child process
        }
        const GameSystem* byName = SystemCatalog::forConsoleName(QStringLiteral("MSX2"));
        CHECK(byName != nullptr && byName->id == QLatin1String("msx"));
        const GameSystem* byExt = SystemCatalog::forExtension(QStringLiteral("mx1"));
        CHECK(byExt != nullptr && byExt->id == QLatin1String("msx"));
        // ...and it did NOT take an extension off a system that already had it. .dsk is Apple II's (Amstrad's
        // .dsk already routes by hint for exactly this reason), and .cas is the Atari 800's. A new system
        // that quietly re-routes an existing one is the failure mode this table's comments warn about.
        const GameSystem* dsk = SystemCatalog::forExtension(QStringLiteral("dsk"));
        CHECK(dsk != nullptr && dsk->id == QLatin1String("apple2"));
        const GameSystem* cas = SystemCatalog::forExtension(QStringLiteral("cas"));
        CHECK(cas != nullptr && cas->id == QLatin1String("atari800"));

        // The MSX recipe itself. blueMSX does not ship its machine definitions or its software database, and
        // the core reads them from Machines/ and Databases/ AT THE ROOT of the libretro system folder
        // (blueMSX-libretro libretro.c:1196-1199 builds exactly those two paths from the system dir). Its own
        // .info marks both as required (firmware0/1_opt = "false"). So this is a firmware requirement in
        // precisely the sense #190 means: something the user has to put somewhere, named exactly.
        const LaunchRecipe msx = LaunchRecipes::load(QStringLiteral("msx"), QString());
        CHECK(!msx.isNull());
        const RecipeCore* blue = LaunchRecipes::coreFor(msx, QStringLiteral("bluemsx"));
        CHECK(blue != nullptr);
        if (blue)
        {
            CHECK(blue->options.value(QStringLiteral("bluemsx_msxtype")) == QLatin1String("Auto"));
            bool namesMachines = false, namesDatabases = false;
            for (const RecipeFirmware& fw : blue->firmware)
                for (const QString& f : fw.files)
                {
                    if (f.startsWith(QLatin1String("Machines/")))  namesMachines  = true;
                    if (f.startsWith(QLatin1String("Databases/"))) namesDatabases = true;
                }
            CHECK(namesMachines);
            CHECK(namesDatabases);
            // The requirement has to be REQUIRED, or the launch proceeds to a black screen instead of a
            // sentence — which is the entire point of the firmware half of this issue.
            CHECK(!LaunchRecipes::missingFirmware(*blue, [](const QString&) { return false; }).isEmpty());
        }
        // fMSX boots as an MSX2+ unless told otherwise (fmsx_mode's first value IS the default, and the
        // core's own fallback is MSX2+ too), so the ROMs it needs are the MSX2+ pair, not MSX.ROM — naming
        // the wrong pair would send the user hunting for a file the core never opens.
        const RecipeCore* fmsx = LaunchRecipes::coreFor(msx, QStringLiteral("fmsx"));
        CHECK(fmsx != nullptr);
        if (fmsx)
        {
            CHECK(fmsx->options.value(QStringLiteral("fmsx_mode")) == QLatin1String("MSX2+"));
            QStringList required;
            for (const RecipeFirmware& fw : fmsx->firmware)
                if (fw.required) required += fw.files;
            CHECK(required.contains(QStringLiteral("MSX2P.ROM")));
            CHECK(required.contains(QStringLiteral("MSX2PEXT.ROM")));
            CHECK(required.contains(QStringLiteral("DISK.ROM")));   // no disk starts without it
            // Casing is load-bearing: fMSX chdirs into the system dir and opens the bare name (fMSX/MSX.c:
            // 688-780), so on a case-sensitive filesystem "msx2p.rom" is simply a different file.
            for (const QString& f : required) CHECK(f == f.toUpper());
        }

        // --- the two core entries increment 1 named as gaps ----------------------------------------------
        // dosbox_core is the OTHER MS-DOS core, and it is the opposite of dosbox-pure on the one decision
        // that matters: it has no archive support at all (its supported_extensions carry no `zip`, and
        // retro_load_game has a .conf branch, a disc branch and "hand it to DOSBox as a command line" — no
        // archive branch anywhere). Handing it an unextracted .zip cannot work, so its rule must say
        // `extract` in as many words rather than inheriting dosbox-pures.
        const LaunchRecipe dos2 = LaunchRecipes::load(QStringLiteral("msdos"), QString());
        const RecipeCore* dbc = LaunchRecipes::coreFor(dos2, QStringLiteral("dosbox_core"));
        CHECK(dbc != nullptr);
        if (dbc)
        {
            CHECK(LaunchRecipes::presentationFor(*dbc, ContentKind::Archive) == Presentation::Extract);
            CHECK(LaunchRecipes::presentationFor(*dbc, ContentKind::Folder)  == Presentation::Executable);
            CHECK(dbc->options.value(QStringLiteral("dosbox_core_machine")) == QLatin1String("svga_s3"));
            CHECK(dbc->options.value(QStringLiteral("dosbox_core_mount_c_as")) == QLatin1String("content"));
            // Keys are prefixed at runtime ("dosbox_core_" + short key), so a recipe written against the
            // SHORT keys would silently set nothing at all.
            for (auto it = dbc->options.constBegin(); it != dbc->options.constEnd(); ++it)
                CHECK(it.key().startsWith(QLatin1String("dosbox_core_")));
        }
        // puae2021 is built from the same source as puae (libretro-uae branch 2.6.1, Makefile TARGET_NAME
        // := puae2021) and carries the SAME puae_* option namespace and the same Kickstart table, so the
        // Amiga recipe must cover it — otherwise a user who switches cores loses the Kickstart message and
        // gets a black screen back.
        const LaunchRecipe amiga2 = LaunchRecipes::load(QStringLiteral("amiga"), QString());
        const RecipeCore* uae2021 = LaunchRecipes::coreFor(amiga2, QStringLiteral("puae2021"));
        const RecipeCore* uae1    = LaunchRecipes::coreFor(amiga2, QStringLiteral("puae"));
        CHECK(uae2021 != nullptr);
        CHECK(uae1 != nullptr);
        if (uae2021 && uae1)
        {
            CHECK(uae2021->options == uae1->options);       // same keys, same values: same core, twice
            CHECK(uae2021->firmware.size() == uae1->firmware.size());
            CHECK(!LaunchRecipes::missingFirmware(*uae2021, [](const QString&) { return false; }).isEmpty());
            CHECK(LaunchRecipes::presentationFor(*uae2021, ContentKind::File) == Presentation::AsIs);
        }

        // --- every computer system says something readable about a FILE ----------------------------------
        // A recipe that is silent about a shape degrades to the pre-#190 behaviour, which is correct but
        // means the system was never actually wired. These nine ARE wired, so each ones default core must
        // answer AsIs for a plain file — the shape every one of their declared extensions arrives in.
        const QStringList wired = { QStringLiteral("c64"), QStringLiteral("vic20"), QStringLiteral("zxspectrum"),
                                    QStringLiteral("amstradcpc"), QStringLiteral("apple2"), QStringLiteral("msx"),
                                    QStringLiteral("pc98"), QStringLiteral("x1"), QStringLiteral("atarist") };
        for (const QString& id : wired)
        {
            const GameSystem* sys = SystemCatalog::byId(id);
            const LaunchRecipe r  = LaunchRecipes::load(id, QString());
            CHECK(sys != nullptr);
            CHECK(!r.isNull());
            if (!sys || r.isNull()) continue;
            const RecipeCore* def = LaunchRecipes::coreFor(r, sys->cores.value(0));
            CHECK(def != nullptr);
            if (!def) continue;
            CHECK(LaunchRecipes::presentationFor(*def, ContentKind::File) == Presentation::AsIs);
            CHECK(LaunchRecipes::presentationFor(*def, ContentKind::Archive) == Presentation::Extract);
            // A computer whose content is a disk/tape is never a FOLDER game — only MS-DOS is.
            CHECK(!r.folderIsGame);
            // ...and every one of them seeds at least one core option or names firmware, or the recipe is
            // carrying no launch knowledge at all and the system is only nominally wired.
            CHECK(!def->options.isEmpty() || !def->firmware.isEmpty());
        }

        // The PC-98 casing question, settled from the cores own source: np2kai builds its BIOS path from the
        // single lowercase literal str_biosrom = "bios.rom" (common/strres.c:67, opened at bios/bios.c:441-
        // 442) and never tries an uppercase spelling. Listing "np2kai/BIOS.ROM" as an acceptable alternative
        // would make the app report a file as PRESENT on a case-sensitive filesystem that the core cannot
        // open. The FONT is the opposite case and genuinely is tried in four spellings (font/font.c:192-195),
        // so there the any-of list is correct.
        const LaunchRecipe pc98 = LaunchRecipes::load(QStringLiteral("pc98"), QString());
        const RecipeCore* np2 = LaunchRecipes::coreFor(pc98, QStringLiteral("np2kai"));
        CHECK(np2 != nullptr);
        if (np2)
        {
            bool sawBios = false, sawUpperBios = false, fontSpellings = false;
            for (const RecipeFirmware& fw : np2->firmware)
            {
                for (const QString& f : fw.files)
                {
                    if (f == QLatin1String("np2kai/bios.rom")) sawBios = true;
                    if (f == QLatin1String("np2kai/BIOS.ROM")) sawUpperBios = true;
                }
                if (fw.files.size() >= 4 && fw.files.value(0).contains(QLatin1String("FONT"))) fontSpellings = true;
            }
            CHECK(sawBios);
            CHECK(!sawUpperBios);
            CHECK(fontSpellings);
        }

        // --- the per-game core override reaches the ARCHIVE decision --------------------------------------
        // Increment 1 read the SYSTEMs core for this one decision, because it runs before the item key
        // exists. The consequence was concrete and unrecoverable: a DOS game moved onto dosbox_core still got
        // dosbox-pures "hand the .zip over whole" treatment, and dosbox_core cannot read a .zip at all.
        const QStringList dosCores = { QStringLiteral("dosbox_pure"), QStringLiteral("dosbox_core") };
        // Nothing overridden, nothing configured -> the system default, which DOES take the archive whole.
        CHECK(LaunchRecipes::archiveAsIsForLaunch(dos2, QString(), QString(), dosCores));
        // A per-GAME override onto the other core -> extract. This is the case increment 1 could not see.
        CHECK(!LaunchRecipes::archiveAsIsForLaunch(dos2, QStringLiteral("dosbox_core"), QString(), dosCores));
        // A per-SYSTEM configured core does the same...
        CHECK(!LaunchRecipes::archiveAsIsForLaunch(dos2, QString(), QStringLiteral("dosbox_core"), dosCores));
        // ...and the per-GAME override beats it in both directions.
        CHECK(!LaunchRecipes::archiveAsIsForLaunch(dos2, QStringLiteral("dosbox_core"),
                                                   QStringLiteral("dosbox_pure"), dosCores));
        CHECK(LaunchRecipes::archiveAsIsForLaunch(dos2, QStringLiteral("dosbox_pure"),
                                                  QStringLiteral("dosbox_core"), dosCores));
        // A stale override naming a core this system does not offer is IGNORED, not obeyed — the same rule
        // LaunchOpts::resolveCore applies everywhere else. Obeying it would resolve to a core entry that does
        // not exist and silently change the presentation.
        CHECK(LaunchRecipes::archiveAsIsForLaunch(dos2, QStringLiteral("mame"), QString(), dosCores));
        // And the resolution rule on its own, so a failure says which half broke.
        CHECK(LaunchRecipes::recipeCoreFor(QString(), QString(), dosCores) == QLatin1String("dosbox_pure"));
        CHECK(LaunchRecipes::recipeCoreFor(QStringLiteral("dosbox_core"), QString(), dosCores)
              == QLatin1String("dosbox_core"));
        CHECK(LaunchRecipes::recipeCoreFor(QString(), QStringLiteral("dosbox_core"), dosCores)
              == QLatin1String("dosbox_core"));
        CHECK(LaunchRecipes::recipeCoreFor(QStringLiteral("mame"), QString(), dosCores)
              == QLatin1String("dosbox_pure"));
        CHECK(LaunchRecipes::recipeCoreFor(QString(), QString(), QStringList()).isEmpty());
        // A system with no recipe never takes an archive whole, whatever anyone overrides.
        CHECK(!LaunchRecipes::archiveAsIsForLaunch(LaunchRecipe{}, QStringLiteral("dosbox_pure"),
                                                   QString(), dosCores));
    }

    // ---- 9. the Amstrad CPC boot command, derived from a disks own catalogue -----------------------------
    //
    // A CPC boots to BASIC and waits for a RUN command naming a file. cap32 derives that name from the disks
    // AMSDOS catalogue itself, through a documented precedence chain (libretro/dsk/loader.c), and
    // AmsdosCatalog.h is a faithful re-implementation of that chain — faithful ON PURPOSE, because a frontend
    // that derived a DIFFERENT command from the cores would be describing a launch that is not about to
    // happen.
    //
    // Everything here runs against a .dsk built byte by byte below: a standard CPCEMU DATA-format image with
    // one track of nine 512-byte sectors, catalogue entries in sectors &C1..&C4. No file, no core, no disk.
    {
        struct Ent { const char* name; const char* ext; bool hidden; };
        auto makeDsk = [](const QList<Ent>& entries) {
            const int kSectors = 9, kSectorSize = 512, kTrackLen = 0x100 + kSectors * kSectorSize;
            QByteArray dsk(0x100, '\0');
            const QByteArray sig = "MV - CPCEMU Disk-File\r\nDisk-Info\r\n";
            dsk.replace(0, int(sig.size()), sig);
            dsk[0x30] = char(1);                                   // 1 track is enough for a catalogue
            dsk[0x31] = char(1);                                   // side 0 only
            dsk[0x32] = char(kTrackLen & 0xFF);
            dsk[0x33] = char((kTrackLen >> 8) & 0xFF);

            QByteArray track(0x100, '\0');
            const QByteArray tsig = "Track-Info\r\n";
            track.replace(0, int(tsig.size()), tsig);
            track[0x10] = char(0);                                 // track 0
            track[0x11] = char(0);                                 // side 0
            track[0x14] = char(2);                                 // sector size code 2 == 512 bytes
            track[0x15] = char(kSectors);
            for (int i = 0; i < kSectors; ++i)
            {
                const int si = 0x18 + i * 8;
                track[si + 0] = char(0);                           // C
                track[si + 1] = char(0);                           // H
                track[si + 2] = char(0xC1 + i);                    // R: &C1.. == the DATA format id
                track[si + 3] = char(2);                           // N
            }

            // The four catalogue sectors, 16 entries of 32 bytes each. Unused entries carry user 0xE5, which
            // is what AMSDOS actually writes and what makes "deleted" distinguishable from "user 0".
            QByteArray cat(4 * kSectorSize, char(0xE5));
            for (int i = 0; i < entries.size() && i < 64; ++i)
            {
                const Ent& e = entries.at(i);
                QByteArray raw(32, char(0));
                raw[0] = char(0);                                  // user 0
                const QByteArray nm = QByteArray(e.name).leftJustified(8, ' ', true);
                const QByteArray ex = QByteArray(e.ext).leftJustified(3, ' ', true);
                raw.replace(1, 8, nm);
                raw.replace(9, 3, ex);
                if (e.hidden) raw[10] = char(quint8(raw.at(10)) | 0x80);   // CATALOGUE_ATTRIB: system/hidden
                raw[15] = char(0x10);                              // a non-zero record count: a real extent
                cat.replace(i * 32, 32, raw);
            }

            QByteArray data(kSectors * kSectorSize, char(0xE5));
            data.replace(0, int(cat.size()), cat);
            return dsk + track + data;
        };

        // (a) one program on the disk: that is the game, no ambiguity anywhere.
        {
            const AmsdosCatalog::Catalogue c = AmsdosCatalog::read(makeDsk({ { "GAME", "BAS", false } }));
            CHECK(c.ok);
            CHECK(!c.cpm);
            CHECK(c.entries.size() == 1);
            CHECK(AmsdosCatalog::names(c) == QStringList{ QStringLiteral("GAME.BAS") });
            CHECK(AmsdosCatalog::bootCommand(c) == QLatin1String("RUN\"GAME.BAS"));
        }
        // (b) the conventional loader name wins outright, even when it is not first and not the only .BAS.
        //     DISC.* / DISK.* is the CPC convention the whole platform settled on; picking the alphabetically
        //     or physically first .BAS instead is exactly the plausible-but-wrong answer this pins against.
        {
            const AmsdosCatalog::Catalogue c = AmsdosCatalog::read(makeDsk({
                { "AAALOAD", "BAS", false }, { "DISC", "BAS", false }, { "ZZ", "BIN", false } }));
            CHECK(c.entries.size() == 3);
            CHECK(AmsdosCatalog::bootCommand(c) == QLatin1String("RUN\"DISC.BAS"));
        }
        // (c) nothing conventional: the first .BAS in catalogue order beats a .BIN that comes before it.
        {
            const AmsdosCatalog::Catalogue c = AmsdosCatalog::read(makeDsk({
                { "ZZZ", "BIN", false }, { "AAA", "BAS", false } }));
            CHECK(AmsdosCatalog::bootCommand(c) == QLatin1String("RUN\"AAA.BAS"));
        }
        // (d) a .BIN is still runnable when there is no .BAS at all.
        {
            const AmsdosCatalog::Catalogue c = AmsdosCatalog::read(makeDsk({
                { "ONE", "BIN", false }, { "TWO", "BIN", false } }));
            CHECK(AmsdosCatalog::bootCommand(c) == QLatin1String("RUN\"ONE.BIN"));
        }
        // (e) a single HIDDEN entry and nothing listed: still the game. Hidden is how a lot of commercial CPC
        //     disks keep their loader out of the CAT listing, so refusing to run it would refuse the disk.
        {
            const AmsdosCatalog::Catalogue c = AmsdosCatalog::read(makeDsk({ { "SECRET", "BAS", true } }));
            CHECK(c.entries.size() == 1);
            CHECK(c.entries.first().hidden);
            CHECK(AmsdosCatalog::bootCommand(c) == QLatin1String("RUN\"SECRET.BAS"));
        }
        // (f) files whose extension the CPC cannot run are not in the catalogue at all, so they can never be
        //     offered as the thing to boot.
        {
            const AmsdosCatalog::Catalogue c = AmsdosCatalog::read(makeDsk({
                { "NOTES", "TXT", false }, { "PIC", "SCR", false } }));
            CHECK(c.ok);
            CHECK(c.entries.isEmpty());
            // ...and THIS is the case worth telling the user about: an empty command means the core will type
            // CAT and drop them at a listing. An engine that guessed something here would boot a data file.
            CHECK(AmsdosCatalog::bootCommand(c).isEmpty());
        }
        // (g) an extension-less program (a plain "LOADER") is runnable, and beats a .BIN.
        {
            const AmsdosCatalog::Catalogue c = AmsdosCatalog::read(makeDsk({
                { "PICTURE", "BIN", false }, { "LOADER", "", false } }));
            CHECK(AmsdosCatalog::bootCommand(c) == QLatin1String("RUN\"LOADER."));
        }
        // (h) not a disk image at all -> not ok, no command, and NO crash. This parser is fed whatever the
        //     user dropped in the folder, so every length in it is bounds-checked.
        {
            CHECK(!AmsdosCatalog::read(QByteArray()).ok);
            CHECK(!AmsdosCatalog::read(QByteArray("not a disk at all")).ok);
            CHECK(!AmsdosCatalog::read(QByteArray(0x400, 'x')).ok);
            CHECK(AmsdosCatalog::bootCommand(AmsdosCatalog::read(QByteArray("nonsense"))).isEmpty());
            // A well-formed header that lies about its track size must not read past the buffer.
            const QByteArray truncated = makeDsk({ { "GAME", "BAS", false } }).left(0x400);
            CHECK(!AmsdosCatalog::read(truncated).ok);
        }
        // (i) the recipe is what turns this on, and it is on for cap32 (which takes a boot command) and off
        //     for crocods (which does not) — so the mechanism can never be applied to a core that has no way
        //     to receive it.
        const LaunchRecipe cpc = LaunchRecipes::load(QStringLiteral("amstradcpc"), QString());
        const RecipeCore* cap32 = LaunchRecipes::coreFor(cpc, QStringLiteral("cap32"));
        const RecipeCore* croc  = LaunchRecipes::coreFor(cpc, QStringLiteral("crocods"));
        CHECK(cap32 != nullptr);
        CHECK(croc != nullptr);
        if (cap32)
        {
            CHECK(LaunchRecipes::bootCommandFor(*cap32) == QLatin1String("amsdos"));
            CHECK(cap32->options.value(QStringLiteral("cap32_autorun")) == QLatin1String("enabled"));
        }
        if (croc) CHECK(LaunchRecipes::bootCommandFor(*croc).isEmpty());
    }

    if (failures == 0) std::printf("RECIPES-OK\n");
    else               std::fprintf(stderr, "RECIPES had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
