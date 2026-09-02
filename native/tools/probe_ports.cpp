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
// SECTIONS 12-17 ARE THE RECOMPS SECTION (issue #248, increment a) — `Games → Recomps`, the browse surface
// over this same catalog. The rail there is the INSTALL STATE: `not installed` / `needs ROM` / `installed` /
// `update available` is derived from four inputs and stored nowhere, so the only way it can be wrong is the
// derivation, and three of the four states need this machine to be in a condition no live drive can stage on
// demand. src/core/RecompRows.h is pure for exactly that reason and is driven here from fixtures.
//
// Expected values are hand-authored, never read back out of the code under test. Prints PORTS-OK on success;
// on any failure prints PORTS-FAIL <cond> and exits non-zero.
#include "NativePorts.h"
#include "RecompRows.h"   // issue #248: the Recomps section's row/state model, sections 12-17

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
            // #248: the licence the Recomps row and the card show. The shipped entry states it; an entry that
            // did not would show nothing rather than a guess, which is the next assertion but one.
            CHECK(z->port.license == QStringLiteral("GPL-3.0"));
            // ...and it pins no release and names no build engine, which is what makes it the PRE-BUILT tier
            // with no update claim. Both are absence tripwires: the states they unlock must not appear by
            // accident on the one entry this build ships.
            CHECK(z->port.releaseTag.isEmpty());
            CHECK(z->port.buildEngine.isEmpty());
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

    // ======================================================================================================
    // THE RECOMPS SECTION (issue #248, increment a) — src/core/RecompRows.h.
    //
    // The section is a browse surface over the catalog asserted above, and everything about it that can be
    // wrong is in the row model: which rows exist, in what order, and what each one says about this machine.
    // The last of those is the reason these sections are here rather than in a live drive — three of the four
    // states need the machine to be in a condition no drive can stage on demand (a port installed at a
    // release the catalogue has since moved past, a library with no matching dump), and a state that is only
    // ever seen in one condition is a state nobody has tested.
    // ======================================================================================================

    // ---- 12. the state derivation, all four states from fixture inputs ----------------------------------
    {
        using recomps::State;
        auto st = [](bool installed, bool match, const char* have, const char* want) {
            recomps::Facts f;
            f.installed = installed;
            f.libraryMatch = match;
            f.installedTag = QString::fromLatin1(have);
            f.catalogueTag = QString::fromLatin1(want);
            return recomps::deriveState(f);
        };

        // NOT INSTALLED, and the user has the game: the one row where "Install" is the obvious next step.
        CHECK(st(false, true, "", "") == State::NotInstalled);
        // NEEDS ROM: nothing in the library matches. A port is not a game — installing one here ends at the
        // port's own "give me the ROM" screen with nothing to give it, so the row says so instead.
        CHECK(st(false, false, "", "") == State::NeedsRom);
        // ...and that is decided by the LIBRARY, not by the install: a port already installed is `installed`
        // whether or not a matching dump is on this machine. Removing the game must not make an installed
        // program disappear from the section.
        CHECK(st(true, false, "", "") == State::Installed);
        CHECK(st(true, true, "", "") == State::Installed);

        // UPDATE AVAILABLE: installed, and the release EB recorded differs from the one the catalogue pins.
        CHECK(st(true, true, "1.2.1", "1.2.2") == State::UpdateAvailable);
        CHECK(st(true, false, "1.2.1", "1.2.2") == State::UpdateAvailable);   // the library is irrelevant here
        // ...and the SAME release is not an update. Case-insensitively and whitespace-insensitively: a tag is
        // written "v1.2.2" by a release and "V1.2.2 " by whatever wrote the file, and telling somebody their
        // software is out of date because of a capital V is a lie the row would tell every time it was drawn.
        CHECK(st(true, true, "1.2.2", "1.2.2") == State::Installed);
        CHECK(st(true, true, "V1.2.2", "v1.2.2") == State::Installed);
        CHECK(st(true, true, " 1.2.2 ", "1.2.2") == State::Installed);

        // AN UNKNOWN IS NOT A DIFFERENCE. Either tag missing means nobody knows which release this is, and
        // "installed" is the honest answer. This is the state of every port installed before EB started
        // recording the tag, and of every entry the catalogue pins nothing for — i.e. the shipped one.
        CHECK(st(true, true, "", "1.2.2") == State::Installed);
        CHECK(st(true, true, "1.2.1", "") == State::Installed);
        CHECK(st(true, true, "   ", "1.2.2") == State::Installed);

        // The two reserved states are never derived. Increment (c) produces them from a live build; a state
        // machine that could reach them today would be claiming a build this app cannot run.
        for (bool i : { false, true })
            for (bool m : { false, true })
                for (const char* h : { "", "1.0" })
                    for (const char* w : { "", "1.0", "2.0" })
                    {
                        const State s = st(i, m, h, w);
                        CHECK(s != State::Building && s != State::Ready);
                    }
    }

    // ---- 13. the tier, read off the entry rather than assumed --------------------------------------------
    {
        // Every entry this build ships is PRE-BUILT: N64 has no generic recompiler, so those titles are
        // published as release binaries. An entry naming a build engine is the other tier, and the row has to
        // say so rather than offering an Install this increment cannot perform.
        const ExternalEmulator* z = byId(QStringLiteral("zelda64recomp"));
        CHECK(z != nullptr);
        if (z) CHECK(recomps::tierOf(*z) == recomps::Tier::PreBuilt);

        ExternalEmulator selfBuilt = titleFromJson(QJsonDocument::fromJson(
            "{\"id\":\"psx1\",\"name\":\"Some PSX Game\",\"kind\":\"recomp\",\"platform\":\"psx\","
            "\"build\":{\"generate\":{\"engine\":\"psxrecomp\"}}}").object());
        CHECK(selfBuilt.port.buildEngine == QStringLiteral("psxrecomp"));
        CHECK(recomps::tierOf(selfBuilt) == recomps::Tier::SelfCompiled);
    }

    // ---- 14. the row list: one row per entry, grouped by system, sorted by title within a system ---------
    {
        auto entry = [](const char* id, const char* name, const char* platform, const char* owner) {
            ExternalEmulator e;
            e.id = QString::fromLatin1(id);
            e.port.name = QString::fromLatin1(name);
            e.port.platform = QString::fromLatin1(platform);
            e.displayName = QString::fromLatin1(owner);
            return e;
        };
        // Deliberately out of order in both dimensions, and the two N64 titles are deliberately adjacent in
        // the input so a builder that merely preserved catalog order would still look sorted for one of them.
        QList<ExternalEmulator> cat{
            entry("mm",  "The Legend of Zelda: Majora's Mask", "n64", "Zelda64Recomp"),
            entry("bk",  "Banjo-Kazooie",                      "n64", "banjo-team"),
            entry("sf",  "Star Fox 64",                        "n64", "starfox-team"),
            entry("cb",  "Crash Bandicoot",                    "psx", "crash-team"),
        };

        const QVector<recomps::Row> rows = recomps::buildRows(
            cat,
            [](const ExternalEmulator&) { return recomps::Facts{}; },
            [](const QString& id) { return id == QStringLiteral("n64") ? QStringLiteral("Nintendo 64")
                                                                      : QStringLiteral("PlayStation"); });

        // 2 headers + 4 ports, and nothing else: one row per entry, no entry dropped, none duplicated.
        CHECK(rows.size() == 6);
        if (rows.size() == 6)
        {
            // Systems in ascending ID order ("n64" < "psx"), NOT in the display language's order — the header
            // NAME comes from a resolver that may answer the same string for two ids, and sorting on it would
            // make the grouping depend on which language the app is running in.
            CHECK(rows[0].kind == recomps::Row::Kind::SystemHeader);
            CHECK(rows[0].systemId == QStringLiteral("n64"));
            CHECK(rows[0].title == QStringLiteral("Nintendo 64"));   // the resolver's name, not the raw id
            CHECK(rows[0].portId.isEmpty());                          // a label activates nothing
            // ...then that system's ports, by title, case-insensitively. "The Legend of Zelda…" sorts under T.
            CHECK(rows[1].kind == recomps::Row::Kind::Port);
            CHECK(rows[1].portId == QStringLiteral("bk"));
            CHECK(rows[2].portId == QStringLiteral("sf"));
            CHECK(rows[3].portId == QStringLiteral("mm"));
            CHECK(rows[1].systemId == QStringLiteral("n64"));
            CHECK(rows[4].kind == recomps::Row::Kind::SystemHeader);
            CHECK(rows[4].systemId == QStringLiteral("psx"));
            CHECK(rows[5].portId == QStringLiteral("cb"));
            // The row credits the upstream BY ITS OWN NAME. Never the recompilation toolchain's brand — its
            // developers asked a third-party launcher to stop using it, and this app does not either.
            CHECK(rows[1].creditedName == QStringLiteral("banjo-team"));
            CHECK(rows[3].creditedName == QStringLiteral("Zelda64Recomp"));
        }

        // An unresolvable system still groups, under its raw id rather than under a blank header.
        const QVector<recomps::Row> unnamed = recomps::buildRows(
            cat, [](const ExternalEmulator&) { return recomps::Facts{}; }, {});
        CHECK(unnamed.size() == 6);
        if (unnamed.size() == 6) CHECK(unnamed[0].title == QStringLiteral("n64"));

        // An entry with NO game binding is not a row. A catalog file can carry an id and nothing else; it
        // matches no game and can be installed for none, so listing it would be listing a typo.
        QList<ExternalEmulator> withJunk = cat;
        ExternalEmulator junk; junk.id = QStringLiteral("oops");
        withJunk << junk;
        CHECK(recomps::buildRows(withJunk, [](const ExternalEmulator&) { return recomps::Facts{}; }, {}).size() == 6);
    }

    // ---- 15. an unreadable catalogue is an ERROR ROW, never an empty section (#174) ----------------------
    {
        const QVector<recomps::Row> none =
            recomps::buildRows({}, [](const ExternalEmulator&) { return recomps::Facts{}; }, {});
        CHECK(none.size() == 1);
        // The distinction is the whole point: an empty grid says "there are no recomps", which is a different
        // statement from "the catalogue could not be read" and is false.
        if (none.size() == 1)
        {
            CHECK(none[0].kind == recomps::Row::Kind::Error);
            CHECK(!none[0].title.isEmpty());
            CHECK(none[0].portId.isEmpty());
        }
        // The same for a catalogue whose every entry is unusable — a file full of ids and nothing else reads
        // as broken, not as "no recomps exist".
        ExternalEmulator junk; junk.id = QStringLiteral("oops");
        const QVector<recomps::Row> allJunk =
            recomps::buildRows({ junk }, [](const ExternalEmulator&) { return recomps::Facts{}; }, {});
        CHECK(allJunk.size() == 1);
        if (allJunk.size() == 1) CHECK(allJunk[0].kind == recomps::Row::Kind::Error);
    }

    // ---- 16. `needs ROM` asks the SAME question the game row's verb is offered on -------------------------
    // The section and the verb must never disagree about whether a port applies to this machine: a row saying
    // "needs ROM" beside a game row already offering *Native port* is a bug that reads as a lie.
    {
        const ExternalEmulator* z = byId(QStringLiteral("zelda64recomp"));
        CHECK(z != nullptr);
        if (z)
        {
            const QVector<recomps::LibraryRom> hasIt{
                { QStringLiteral("nes"), QStringLiteral("Super Mario Bros."), QStringLiteral("C:/roms/nes/smb.nes") },
                { QStringLiteral("n64"), QStringLiteral("The Legend of Zelda_ Majora's Mask"),
                  QStringLiteral("C:/roms/n64/The Legend of Zelda_ Majora's Mask.7z") },
            };
            CHECK(recomps::libraryMatches(*z, hasIt));
            CHECK(recomps::deriveState({ false, recomps::libraryMatches(*z, hasIt), {}, {} })
                  == recomps::State::NotInstalled);

            // The same game filed under the WRONG console does not count. A port is bound to one game on one
            // system, and a library row's system is what says which.
            const QVector<recomps::LibraryRom> wrongSystem{
                { QStringLiteral("gc"), QStringLiteral("The Legend of Zelda_ Majora's Mask"),
                  QStringLiteral("C:/roms/gc/The Legend of Zelda_ Majora's Mask.iso") },
            };
            CHECK(!recomps::libraryMatches(*z, wrongSystem));
            CHECK(recomps::deriveState({ false, recomps::libraryMatches(*z, wrongSystem), {}, {} })
                  == recomps::State::NeedsRom);

            // A library of other N64 games is not a match either — this is the near-miss the whole match gate
            // exists for, asked once more from the section's side.
            const QVector<recomps::LibraryRom> otherGames{
                { QStringLiteral("n64"), QStringLiteral("Super Mario 64"), QStringLiteral("C:/roms/n64/Super Mario 64.z64") },
                { QStringLiteral("n64"), QStringLiteral("The Legend of Zelda_ Ocarina of Time"),
                  QStringLiteral("C:/roms/n64/The Legend of Zelda_ Ocarina of Time.z64") },
            };
            CHECK(!recomps::libraryMatches(*z, otherGames));
            CHECK(!recomps::libraryMatches(*z, {}));
        }
    }

    // ---- 17. the recorded release tag round-trips through the install folder ------------------------------
    // The one input with no other home: EmulatorManager writes this at install time because that is the only
    // moment the app knows which release it laid down.
    {
        const QString dir = QDir::tempPath() + QStringLiteral("/eb-probe-ports-install");
        QDir(dir).removeRecursively();
        CHECK(QDir().mkpath(dir));
        // Nothing recorded yet — an install that predates this file, which must read as "unknown", not "".
        CHECK(readInstalledTag(dir).isEmpty());
        CHECK(writeInstalledTag(dir, QStringLiteral("1.2.2")));
        CHECK(readInstalledTag(dir) == QStringLiteral("1.2.2"));
        // An empty tag is never recorded: writing "" would turn "we do not know" into "we checked and it is
        // blank", and the file's absence is the honest form of the first.
        CHECK(!writeInstalledTag(dir, QString()));
        CHECK(!writeInstalledTag(dir, QStringLiteral("   ")));
        CHECK(readInstalledTag(dir) == QStringLiteral("1.2.2"));   // ...and did not clobber what was there
        // It lives INSIDE the install folder, so Remove (a folder delete) takes it with it. An orphaned tag
        // file would make a fresh install of an older release read as up to date.
        CHECK(installedTagPath(dir).startsWith(dir));
        CHECK(QDir(dir).removeRecursively());
        CHECK(readInstalledTag(dir).isEmpty());
        CHECK(readInstalledTag(QString()).isEmpty());              // total: no install dir, no tag
    }

    if (failures == 0) std::printf("PORTS-OK\n");
    else               std::fprintf(stderr, "PORTS had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
