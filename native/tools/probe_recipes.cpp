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

    if (failures == 0) std::printf("RECIPES-OK\n");
    else               std::fprintf(stderr, "RECIPES had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
