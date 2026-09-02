// Headless check of NATIVE PORTS (src/core/NativePorts.h, issue #233) — a static recompilation of ONE game,
// shipped as its own PC executable, offered on that one game's row and on nothing else.
//
// THE RAIL THIS PROBE EXISTS FOR is the last clause. Every other part of a port is an ExternalEmulator that
// already works; the part that is new, and the part that is dangerous, is the MATCH. A port that matches a
// second game boots the wrong software off a 30 MB download the user did not ask for, and the row it appears
// on is the only place a person could notice. So the match is asserted from both sides: the bound game
// matches through every spelling the machine actually stores it under, and a curated set of near misses — the
// same series on the same console, the same title on another console, the same title in a region the entry
// does not accept — does not.
//
// It also pins the two things that would make the feature silently absent rather than wrong:
//   * the shipped catalog is EMBEDDED (resources/ports.qrc). A .qrc that reaches a target the wrong way is
//     not embedded and says nothing about it, and the symptom is a verb that never appears — which is not
//     hypothetical, it is what listing the .qrc in qt_add_executable's source list actually did. shippedPorts()
//     is read out of the resource here, exactly as the app reads it, and compared with the file in the source
//     tree so a stale or misaliased resource is caught too;
//   * a port declares NO `systems`, so EmulationTarget.h's boundEmulatorsFor can never offer it as an N64
//     emulator for every N64 game. That is an absence-of-behaviour tripwire and is labelled as one.
//
// The catalog is the RetComM per-title schema (github.com/TechnicallyComputers/retcomm-catalog, SCHEMA.md)
// plus one documented EB extension, `rom_delivery` — so the projection of that schema onto EB's standalone
// tier (asset globs -> artifact markers, release owner -> credited name, launch.* -> binaries) is asserted
// here too. That projection is the seam a later increment reads the real feed through.
//
// Expected values are hand-authored, never read back out of the code under test. Prints PORTS-OK on success;
// on any failure prints PORTS-FAIL <cond> and exits non-zero.
#include "NativePorts.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PORTS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
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

// The one port this build ships, built by hand so the assertions below have an oracle that is not the
// catalog file. Only the fields the behaviour depends on.
static ExternalEmulator majorasMaskPort()
{
    ExternalEmulator e;
    e.id = QStringLiteral("zelda64recomp");
    e.displayName = QStringLiteral("Zelda64Recomp");
    e.port.name = QStringLiteral("The Legend of Zelda: Majora's Mask");
    e.port.platform = QStringLiteral("n64");
    e.port.filenames = QStringList{ QStringLiteral("Legend of Zelda, The - Majora's Mask (USA).z64"),
                                    QStringLiteral("Zelda - Majora's Mask (USA).z64") };
    e.port.romDelivery = QStringLiteral("in_app_menu");
    return e;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace NativePorts;

    // ---- 1. title normalisation: the several spellings of ONE game collapse onto one key ------------------
    // The two that matter are real: the No-Intro dump name inside the archive, and the archive the library
    // actually stores ('_' because ':' is not a legal Windows filename character).
    const QString key = QStringLiteral("legend of zelda majora s mask");
    CHECK(normalizeTitle(QStringLiteral("Legend of Zelda, The - Majora's Mask (USA).z64")) == key);
    CHECK(normalizeTitle(QStringLiteral("The Legend of Zelda_ Majora's Mask.7z")) == key);
    CHECK(normalizeTitle(QStringLiteral("The Legend of Zelda: Majora's Mask")) == key);
    CHECK(normalizeTitle(QStringLiteral("Legend of Zelda, The - Majora's Mask (USA) (Rev A) [!]")) == key);
    // THE LIMIT, stated rather than discovered later: the No-Intro inversion is recognised by its COMMA. An
    // article sitting mid-title without one is left where it is, because "the" in the middle of a name is
    // usually a word ("Legend of the Mystical Ninja") and stripping every one of them would collide titles
    // that are genuinely different. A `rom_identity.filenames` entry is the answer for a spelling that needs it.
    CHECK(normalizeTitle(QStringLiteral("Legend of Zelda The - Majora's Mask")) != key);
    // ...and a DIFFERENT game does not. Same series, same console, eleven shared characters of prefix.
    CHECK(normalizeTitle(QStringLiteral("Legend of Zelda, The - Ocarina of Time (USA).z64")) != key);
    CHECK(normalizeTitle(QStringLiteral("Super Mario 64 (USA).z64")) != key);
    // Only a real extension is stripped, and "real" needs BOTH bounds — a length and an alphanumeric run.
    // A dot in the middle of a name is punctuation, and taking its tail for an extension eats a word:
    //   * "Sonic.Adventure"  -> the tail is alphanumeric, so only the LENGTH bound refuses it;
    //   * "Game.a b"         -> the tail is short, so only the ALPHANUMERIC bound refuses it.
    // Each mutation-tests the guard the other cannot reach.
    CHECK(normalizeTitle(QStringLiteral("Dr. Mario 64 (USA).z64")) == QStringLiteral("dr mario 64"));
    CHECK(normalizeTitle(QStringLiteral("Sonic.Adventure")) == QStringLiteral("sonic adventure"));
    CHECK(normalizeTitle(QStringLiteral("Game.a b")) == QStringLiteral("game a b"));
    // An empty/marker-only name yields an empty key, which matches() refuses outright (below).
    CHECK(normalizeTitle(QStringLiteral("(USA).z64")).isEmpty());

    // ---- 2. dump identity: regions and revisions are read, other groups are not -------------------------
    {
        const GameIdentity a = identify(QStringLiteral("Legend of Zelda, The - Majora's Mask (USA).z64"));
        CHECK(a.key == key);
        CHECK(a.regions == QStringList{ QStringLiteral("usa") });
        CHECK(a.revision.isEmpty());

        const GameIdentity b = identify(QStringLiteral("Legend of Zelda, The - Majora's Mask (Japan) (Rev A).z64"));
        CHECK(b.regions == QStringList{ QStringLiteral("japan") });
        CHECK(b.revision == QStringLiteral("rev a"));

        // A language list is not a region list, and neither is "[!]" — reading either as one would refuse
        // dumps that are perfectly fine.
        const GameIdentity c = identify(QStringLiteral("Some Game (En,Fr,De) [!].z64"));
        CHECK(c.regions.isEmpty());
        CHECK(c.revision.isEmpty());

        // A full path is accepted: only the file name is read.
        const GameIdentity d = identify(QStringLiteral("C:/roms/n64/The Legend of Zelda_ Majora's Mask.7z"));
        CHECK(d.key == key);
        CHECK(d.regions.isEmpty());   // the library's own name declares no region — and that is not a refusal
    }

    // ---- 3. what an entry accepts, derived from the schema it is written in -----------------------------
    // The RetComM schema has no region field: a No-Intro basename in `rom_identity.filenames` is the only
    // place the region is written, so that is where the region gate comes from.
    {
        const ExternalEmulator p = majorasMaskPort();
        CHECK(titleKeys(p).contains(key));
        CHECK(titleKeys(p).contains(QStringLiteral("zelda majora s mask")));  // via the second basename
        CHECK(acceptedRegions(p) == QStringList{ QStringLiteral("usa") });
        CHECK(acceptedRevisions(p).isEmpty());                                // no basename names a revision
        // An entry whose basenames name no region accepts any — the honest reading of an entry that never said.
        {
            ExternalEmulator q = p;
            q.port.filenames = QStringList{ QStringLiteral("Legend of Zelda, The - Majora's Mask.z64") };
            CHECK(acceptedRegions(q).isEmpty());
        }
    }

    // ---- 4. THE MATCH ---------------------------------------------------------------------------------
    {
        const ExternalEmulator p = majorasMaskPort();
        const QString n64 = QStringLiteral("n64");

        // Every spelling of the bound game, on the right system, matches.
        CHECK(matches(p, n64, identify(QStringLiteral("Legend of Zelda, The - Majora's Mask (USA).z64"))));
        CHECK(matches(p, n64, identify(QStringLiteral("The Legend of Zelda_ Majora's Mask.7z"))));
        CHECK(matches(p, n64, identify(QStringLiteral("Zelda - Majora's Mask.n64"))));   // via a basename hint
        CHECK(matches(p, QStringLiteral("N64"), identify(QStringLiteral("The Legend of Zelda_ Majora's Mask.7z"))));

        // A DIFFERENT GAME never matches. This is the rail.
        CHECK(!matches(p, n64, identify(QStringLiteral("Legend of Zelda, The - Ocarina of Time (USA).z64"))));
        CHECK(!matches(p, n64, identify(QStringLiteral("The Legend of Zelda_ Ocarina of Time.7z"))));
        CHECK(!matches(p, n64, identify(QStringLiteral("Super Mario 64 (USA).z64"))));
        CHECK(!matches(p, n64, identify(QStringLiteral("Majora's Mask 3D.3ds"))));
        CHECK(!matches(p, n64, identify(QStringLiteral("Legend of Zelda, The - Majora's Mask Redux.z64"))));

        // The same title on ANOTHER SYSTEM never matches: a port is bound to one game on one console.
        CHECK(!matches(p, QStringLiteral("snes"), identify(QStringLiteral("Legend of Zelda, The - Majora's Mask (USA).z64"))));
        CHECK(!matches(p, QString(), identify(QStringLiteral("Legend of Zelda, The - Majora's Mask (USA).z64"))));

        // REGION is a refusal, never a requirement: a declared region the entry does not accept is refused,
        // and a name that declares none is accepted (the port's own check is what speaks for it).
        CHECK(!matches(p, n64, identify(QStringLiteral("Legend of Zelda, The - Majora's Mask (Japan).z64"))));
        CHECK(!matches(p, n64, identify(QStringLiteral("Legend of Zelda, The - Majora's Mask (Europe).z64"))));
        CHECK(matches(p, n64, identify(QStringLiteral("Legend of Zelda, The - Majora's Mask.z64"))));
        // An entry whose basenames name no region takes the Japanese dump too.
        {
            ExternalEmulator any = p;
            any.port.filenames = QStringList{ QStringLiteral("Legend of Zelda, The - Majora's Mask.z64") };
            CHECK(matches(any, n64, identify(QStringLiteral("Legend of Zelda, The - Majora's Mask (Japan).z64"))));
        }
        // Revisions behave the same way, and the shipped entry names none (so every revision is taken).
        {
            ExternalEmulator rev = p;
            rev.port.filenames = QStringList{ QStringLiteral("Legend of Zelda, The - Majora's Mask (USA) (Rev A).z64") };
            CHECK(matches(rev, n64, identify(QStringLiteral("Legend of Zelda, The - Majora's Mask (USA) (Rev A).z64"))));
            CHECK(!matches(rev, n64, identify(QStringLiteral("Legend of Zelda, The - Majora's Mask (USA) (Rev B).z64"))));
            CHECK(matches(rev, n64, identify(QStringLiteral("Legend of Zelda, The - Majora's Mask (USA).z64"))));
        }
        // An entry that is NOT a port (no bound game name) matches nothing, whatever else it carries.
        {
            ExternalEmulator ares;
            ares.id = QStringLiteral("ares");
            ares.systems = QStringList{ n64 };
            CHECK(!ares.isNativePort());
            CHECK(!matches(ares, n64, identify(QStringLiteral("Legend of Zelda, The - Majora's Mask (USA).z64"))));
        }

        // matchesRow: the two strings a library row carries. Either may be the one that knows.
        CHECK(matchesRow(p, n64, QStringLiteral("The Legend of Zelda: Majora's Mask"), QString()));
        CHECK(matchesRow(p, n64, QString(), QStringLiteral("C:/roms/n64/Legend of Zelda, The - Majora's Mask (USA).z64")));
        CHECK(matchesRow(p, n64, QStringLiteral("The Legend of Zelda: Majora's Mask"),
                                 QStringLiteral("C:/roms/n64/The Legend of Zelda_ Majora's Mask.7z")));
        CHECK(!matchesRow(p, n64, QStringLiteral("The Legend of Zelda: Ocarina of Time"),
                                  QStringLiteral("C:/roms/n64/The Legend of Zelda_ Ocarina of Time.7z")));
        CHECK(!matchesRow(p, n64, QString(), QString()));
        // The FILE name is read as well as the title, so its region marker refuses a dump the title alone
        // would have taken.
        CHECK(!matchesRow(p, n64, QString(),
                          QStringLiteral("C:/roms/n64/Legend of Zelda, The - Majora's Mask (Japan).z64")));
    }

    // ---- 5. rom delivery: only the mode this increment implements is claimed ----------------------------
    CHECK(romDeliverySupported(QStringLiteral("in_app_menu")));
    CHECK(!romDeliverySupported(QStringLiteral("beside_exe")));   // increment 2
    CHECK(!romDeliverySupported(QStringLiteral("cli_path")));     // increment 2
    CHECK(!romDeliverySupported(QString()));                      // a catalog entry that declares nothing
    CHECK(!romDeliverySupported(QStringLiteral("IN_APP_MENU")));  // the parser lowercases; a raw call must not guess

    // ---- 6. the RetComM -> EverythingBox projection ----------------------------------------------------
    // An asset glob is reduced to the substring EmulatorManager::assetMatches actually tests, and the port's
    // credited name is the OWNER of release.github (RetComM's own rule for labelling a recomp author).
    CHECK(globToMarker(QStringLiteral("*-Windows.zip")) == QStringLiteral("-Windows.zip"));
    CHECK(globToMarker(QStringLiteral("*win64*")) == QStringLiteral("win64"));
    CHECK(globToMarker(QStringLiteral("bpe-*linux*")) == QStringLiteral("linux"));   // longest literal wins
    CHECK(globToMarker(QStringLiteral("plain.zip")) == QStringLiteral("plain.zip"));
    CHECK(globToMarker(QStringLiteral("*")).isEmpty());     // all wildcard: "no build for this OS"
    CHECK(globToMarker(QString()).isEmpty());
    CHECK(creditedName(QStringLiteral("Zelda64Recomp/Zelda64Recomp")) == QStringLiteral("Zelda64Recomp"));
    CHECK(creditedName(QStringLiteral("TechnicallyComputers/RetComM-Launcher")) == QStringLiteral("TechnicallyComputers"));
    CHECK(creditedName(QStringLiteral("noslash")) == QStringLiteral("noslash"));
    CHECK(releaseApiUrl(QStringLiteral("o/r")) == QStringLiteral("https://api.github.com/repos/o/r/releases/latest"));
    CHECK(releaseApiUrl(QString()).isEmpty());

    // ---- 7. the /releases/latest 404 fallback ----------------------------------------------------------
    // GitHub 404s /releases/latest for a repository whose only releases are prereleases, which several ports
    // are. The retry URL is the list endpoint, and the newest usable entry of that list is what gets read.
    {
        using namespace EmulatorRegistry;
        CHECK(releasesFallbackUrl(QStringLiteral("https://api.github.com/repos/o/r/releases/latest"))
              == QStringLiteral("https://api.github.com/repos/o/r/releases"));
        CHECK(releasesFallbackUrl(QStringLiteral("https://api.github.com/repos/o/r/releases/latest/"))
              == QStringLiteral("https://api.github.com/repos/o/r/releases"));
        CHECK(releasesFallbackUrl(QStringLiteral("https://api.github.com/repos/o/r/releases/latest?x=1"))
              == QStringLiteral("https://api.github.com/repos/o/r/releases"));
        // Not a /releases/latest: there is nothing to fall back to and the caller must not invent one.
        CHECK(releasesFallbackUrl(QStringLiteral("https://dolphin-emu.org/update/latest/beta/")).isEmpty());
        CHECK(releasesFallbackUrl(QStringLiteral("https://api.github.com/repos/o/r/releases")).isEmpty());
        CHECK(releasesFallbackUrl(QString()).isEmpty());

        const QJsonArray list = QJsonDocument::fromJson(
            "[{\"draft\":true,\"tag_name\":\"draft\"},"
            " {\"draft\":false,\"prerelease\":true,\"tag_name\":\"0.9.2\"},"
            " {\"draft\":false,\"tag_name\":\"0.9.1\"}]").array();
        CHECK(newestRelease(list).value(QStringLiteral("tag_name")).toString() == QStringLiteral("0.9.2"));
        CHECK(newestRelease(QJsonArray{}).isEmpty());
        // An all-draft list yields nothing rather than a release nobody can download.
        const QJsonArray drafts = QJsonDocument::fromJson("[{\"draft\":true,\"tag_name\":\"d\"}]").array();
        CHECK(newestRelease(drafts).isEmpty());
    }

    // ---- 8. the SHIPPED catalog, read the way the app reads it ------------------------------------------
    {
        const QList<ExternalEmulator> shipped = shippedPorts();
        // Non-empty means the .qrc is embedded at all — the failure mode with no other symptom.
        CHECK(!shipped.isEmpty());
        const ExternalEmulator* z = find(shipped, QStringLiteral("zelda64recomp"));
        CHECK(z != nullptr);
        if (z)
        {
            CHECK(z->isNativePort());
            CHECK(z->port.name == QStringLiteral("The Legend of Zelda: Majora's Mask"));
            CHECK(z->port.kind == QStringLiteral("recomp"));
            CHECK(z->port.platform == QStringLiteral("n64"));
            CHECK(z->port.romDelivery == QStringLiteral("in_app_menu"));
            CHECK(!z->port.authorNotes.isEmpty());   // in_app_menu tells the user what to pick, or it is mute
            // The port is credited by its release owner, not by the catalog's `name` (which is the GAME).
            CHECK(z->port.releaseRepo == QStringLiteral("Zelda64Recomp/Zelda64Recomp"));
            CHECK(z->displayName == QStringLiteral("Zelda64Recomp"));
            CHECK(z->updateJsonUrl
                  == QStringLiteral("https://api.github.com/repos/Zelda64Recomp/Zelda64Recomp/releases/latest"));
            // rom_identity is CARRIED, in HashVerify's own shapes, and NOT gated on in increment 1. These are
            // this machine's own dump of the game the port accepts.
            CHECK(z->port.sha1 == QStringList{ QStringLiteral("d6133ace5afaa0882cf214cf88daba39e266c078") });
            CHECK(z->port.md5 == QStringList{ QStringLiteral("2a0a8acb61538235bc1094d297fb6556") });
            CHECK(z->port.crc32 == QStringList{ QStringLiteral("b428d8a7") });
            CHECK(z->port.sizes == QList<qint64>{ 33554432 });
            CHECK(z->port.discSerials.isEmpty());        // N64 is not a disc platform
            CHECK(!z->port.requireCue);
            CHECK(z->port.installDirName == QStringLiteral("zelda64recomp"));
            // Artifact markers are what assetMatches() tests against the release's asset names
            // (Zelda64Recompiled-v1.2.2-Windows.zip / -Linux-X64.zip / -macOS.zip). A marker matching more
            // than one asset would install the wrong build for the platform — note the LEADING DASH, which is
            // what keeps "-Linux-X64.zip" off the Linux-Flatpak-X64 and Linux-ARM64 assets.
            CHECK(z->winArtifact == QStringLiteral("-Windows.zip"));
            CHECK(z->linuxArtifact == QStringLiteral("-Linux-X64.zip"));
            CHECK(z->macArtifact == QStringLiteral("-macOS.zip"));
            CHECK(z->winBinaries.contains(QStringLiteral("Zelda64Recompiled.exe")));
            CHECK(z->extensions.contains(QStringLiteral("z64")));   // dotless, per ExternalEmulator's contract
            CHECK(!z->extensions.contains(QStringLiteral(".z64")));
            CHECK(EmulatorRegistry::hasInstallSource(*z));
            // rom_delivery "in_app_menu": the port takes the ROM in its own UI, so it is launched with NO
            // arguments.
            CHECK(z->argsTemplate.isEmpty());
            // The real dump on this machine matches it, through both spellings the library holds.
            CHECK(matchesRow(*z, QStringLiteral("n64"), QStringLiteral("The Legend of Zelda_ Majora's Mask"),
                             QStringLiteral("C:/roms/n64/The Legend of Zelda_ Majora's Mask.7z")));
            CHECK(!matchesRow(*z, QStringLiteral("n64"), QStringLiteral("The Legend of Zelda_ Ocarina of Time"),
                              QStringLiteral("C:/roms/n64/The Legend of Zelda_ Ocarina of Time.7z")));

            // A PORT DECLARES NO `systems`. This is an absence-of-behaviour tripwire and is deliberate: the
            // moment a port names a system, EmulationTarget.h's boundEmulatorsFor offers it as a standalone
            // emulator for EVERY game on it — Zelda64Recomp on Super Mario 64. Nothing else in the tree
            // states this, so if a future catalog entry grows a `systems` list, this is what says no.
            CHECK(z->systems.isEmpty());
        }
        // ...and the resource is the SAME BYTES as the file in the source tree (a stale rcc, or an alias
        // pointing somewhere else, would pass every check above while shipping last week's catalog).
        {
            QFile res(shippedCatalogResource());
            QFile src(QStringLiteral(EB_PORTS_SOURCE_DIR) + QStringLiteral("/n64recomp.json"));
            CHECK(res.open(QIODevice::ReadOnly));
            CHECK(src.open(QIODevice::ReadOnly));
            if (res.isOpen() && src.isOpen()) CHECK(res.readAll() == src.readAll());
        }
    }

    // ---- 9. the <data>/ports override, and malformed-file survival -------------------------------------
    // The same merge <data>/systems and <data>/emulators get: field-level override of a shipped entry, a new
    // entry appended, a broken file logged and skipped without dropping anything.
    {
        const QString dir = QDir(AppPaths::dataDir()).filePath(QStringLiteral("ports"));
        // A renamed upstream, corrected without a rebuild: `release.github` alone. The derived fields — the
        // update URL and the CREDITED NAME — must follow it, or the card would credit the old project.
        CHECK(writeFile(dir + QStringLiteral("/10-override.json"),
                        "{\"id\":\"zelda64recomp\",\"release\":{\"github\":\"NewOwner/NewRepo\"}}"));
        CHECK(writeFile(dir + QStringLiteral("/20-new.json"),
                        // ...and it TRIES to grant itself a system binding, which the parser must not read.
                        "[{\"id\":\"myport\",\"name\":\"Some Game\",\"kind\":\"recomp\",\"platform\":\"nes\","
                        "\"systems\":[\"nes\"],"
                        "\"rom_delivery\":\"cli_path\",\"launch\":{\"windows\":\"my.exe\"}}]"));
        CHECK(writeFile(dir + QStringLiteral("/30-broken.json"), "{ this is not json"));

        QStringList warnings;
        const QList<ExternalEmulator> merged = loadPortsDir(dir, shippedPorts(),
                                                            [&](const QString& m) { warnings << m; });
        CHECK(warnings.size() == 1);                       // exactly the broken file, named
        const ExternalEmulator* z = find(merged, QStringLiteral("zelda64recomp"));
        CHECK(z != nullptr);
        if (z)
        {
            CHECK(z->port.releaseRepo == QStringLiteral("NewOwner/NewRepo"));       // the field it named...
            CHECK(z->displayName == QStringLiteral("NewOwner"));                     // ...and what derives from it
            CHECK(z->updateJsonUrl
                  == QStringLiteral("https://api.github.com/repos/NewOwner/NewRepo/releases/latest"));
            CHECK(z->port.name == QStringLiteral("The Legend of Zelda: Majora's Mask")); // ...and nothing else
            CHECK(z->port.platform == QStringLiteral("n64"));
            CHECK(z->winArtifact == QStringLiteral("-Windows.zip"));
            CHECK(matchesRow(*z, QStringLiteral("n64"), QString(),
                             QStringLiteral("Legend of Zelda, The - Majora's Mask (USA).z64")));
        }
        const ExternalEmulator* mine = find(merged, QStringLiteral("myport"));
        CHECK(mine != nullptr);
        if (mine)
        {
            CHECK(mine->isNativePort());
            CHECK(mine->port.platform == QStringLiteral("nes"));
            CHECK(mine->winBinaries.contains(QStringLiteral("my.exe")));
            // An entry with no `release` declares no update source, so the download machinery is never
            // entered — auto-install stays a released-catalog privilege here exactly as it is for emulators.
            CHECK(!EmulatorRegistry::hasInstallSource(*mine));
            // ...and it declares a rom delivery this build does not implement, which the UI must SAY rather
            // than silently pretend to honour.
            CHECK(!romDeliverySupported(mine->port.romDelivery));
            // A `systems` key in a PORT catalog file is not read. If it were, EmulationTarget.h's
            // boundEmulatorsFor would offer this port as a standalone emulator for every NES game — which is
            // the one thing a per-GAME binding exists to make unreachable, and a user-editable file must not
            // be able to undo it.
            CHECK(mine->systems.isEmpty());
        }
    }

    // ---- 10. the lookup the UI asks, end to end through all() -------------------------------------------
    // all() is memoised on first use, and section 9 has just seeded <data>/ports, so this reads the merged
    // catalog.
    {
        CHECK(byId(QStringLiteral("zelda64recomp")) != nullptr);
        CHECK(byId(QStringLiteral("no-such-port")) == nullptr);
        const ExternalEmulator* hit = portForGame(QStringLiteral("nes"), QStringLiteral("Some Game"), QString());
        CHECK(hit != nullptr && hit->id == QStringLiteral("myport"));
        CHECK(portForGame(QStringLiteral("n64"), QStringLiteral("Super Mario 64"),
                          QStringLiteral("C:/roms/n64/Super Mario 64.zip")) == nullptr);
        CHECK(portForGame(QString(), QStringLiteral("Some Game"), QString()) == nullptr);
    }

    // ---- 11. a port is invisible to the EMULATOR tier ---------------------------------------------------
    // Two separate registries, and this is the assertion that they stay separate: nothing the emulator side
    // enumerates can reach a port. EmulationTarget.h's boundEmulatorsFor is "every entry of
    // EmulatorRegistry::all() whose `systems` names this system" — spelt out here rather than called, so this
    // probe stays QtCore-only (that header links the LaunchOpts resolvers). Both halves are asserted: no port
    // is in the emulator registry, and the real system binding it must not disturb still works.
    {
        CHECK(EmulatorRegistry::byId(QStringLiteral("zelda64recomp")) == nullptr);
        QStringList boundToN64;
        for (const ExternalEmulator& e : EmulatorRegistry::all())
        {
            CHECK(!e.isNativePort());   // no emulator-registry entry carries a game binding
            if (e.systems.contains(QStringLiteral("n64"))) boundToN64 << e.id;
        }
        CHECK(!boundToN64.contains(QStringLiteral("zelda64recomp")));
        CHECK(boundToN64.contains(QStringLiteral("ares")));
        // ...and the emulator schema does not serialize a port binding at all, so a <data>/emulators file can
        // never mint one. (EmulatorRegistry::toJson has no `port` key by design — see the note there.)
        const ExternalEmulator* z = byId(QStringLiteral("zelda64recomp"));
        if (z) CHECK(!EmulatorRegistry::toJson(*z).contains(QStringLiteral("port")));
    }

    if (failures == 0) std::printf("PORTS-OK\n");
    else               std::fprintf(stderr, "PORTS had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
