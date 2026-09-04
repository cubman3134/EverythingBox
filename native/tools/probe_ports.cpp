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
// SECTIONS 18-24 ARE THE RETCOMM FEED AND THE ROM-IDENTITY GATE (issue #248, increment b) — the published
// `catalog.zip` read as a second feed and merged over the in-tree catalogue, and the digest gate that decides
// whether the user owns the game. Neither can be driven live: the feed's failure modes are somebody else's
// publish, and the gate's are a machine holding a particular wrong dump. Fixture catalogues are written with
// miniz IN THIS PROCESS so each malformed case differs from the good one by the byte that matters, and the
// gate is driven from hand-authored digests. See the note above section 18.
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
#include "RecompFeed.h"   // issue #248 (b): the RetComM feed's parse/merge/cache, sections 18-24

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVector>
#include <cstdio>
#include <cstring>

#include "miniz.h"   // sections 18-24 WRITE their fixture catalogues; no binary fixture is committed

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

// A catalog.zip, built in memory (issue #248 (b), sections 18-24). Written rather than committed for the
// reason probe_decopack does the same: every malformed case below differs from the good one by exactly the
// change it is about, and a committed binary fixture hides that difference in a blob nobody can read.
// Members are added in name order so two fixtures that describe the same catalogue are the same bytes.
static QByteArray makeZip(const QHash<QString, QByteArray>& members)
{
    mz_zip_archive z;
    std::memset(&z, 0, sizeof(z));
    if (!mz_zip_writer_init_heap(&z, 0, 0)) return {};
    QStringList names = members.keys();
    names.sort();
    for (const QString& n : names)
    {
        const QByteArray name = n.toUtf8();
        const QByteArray body = members.value(n);
        mz_zip_writer_add_mem(&z, name.constData(), body.constData(), size_t(body.size()),
                              MZ_DEFAULT_COMPRESSION);
    }
    void* buf = nullptr;
    size_t sz = 0;
    QByteArray out;
    if (mz_zip_writer_finalize_heap_archive(&z, &buf, &sz) && buf)
        out = QByteArray(static_cast<const char*>(buf), int(sz));
    mz_zip_writer_end(&z);
    if (buf) mz_free(buf);
    return out;
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

    // ======================================================================================================
    // SECTIONS 18-24 ARE THE RETCOMM FEED AND THE ROM-IDENTITY GATE (issue #248, increment b).
    //
    // Two rails, and neither can be driven live on demand.
    //
    // THE FEED is a zip published by somebody else. Every way it can be broken — a truncated download, an
    // index that lists a title whose file did not land, a publish that shipped HTML from a captive portal —
    // has to present as an ERROR ROW rather than as a section that quietly got shorter (#174), and none of
    // those can be staged against the real repository. The fixture catalogues below are WRITTEN with miniz in
    // this process, so each malformed case differs from the good one by exactly the byte that matters.
    //
    // THE GATE decides whether the user owns the game, and it is the part of this feature that can be wrong
    // in a way nobody notices: a gate that is too generous offers a build against the wrong dump, and a gate
    // that is too strict tells somebody to go and find a game already on their disk. It is asserted per digest
    // KIND (an author publishes only the one their own check uses), and — the assertion this increment exists
    // for — a TITLE MATCH WITH A WRONG DIGEST IS NOT A MATCH, which is a behaviour change from increment (a)
    // and is asserted against increment (a)'s own function so the change is visible rather than described.
    //
    // And the cache rule: NOTHING here hashes. pathsNeedingHash() is the only function that names a file and
    // it opens none of them; a warm cache yields no work at all, which is what stops an open section hashing
    // a 660 MB disc image on every redraw.
    // ======================================================================================================

    // ---- 18. the recompiler engines, and the licence that is NOT read out of the feed ---------------------
    // A licence taken from a document the licensor does not control is not a licence notice. The table is
    // hard-coded, checked against each project's own LICENSE file, and an engine that is not in it shows
    // nothing rather than a guess.
    {
        using RecompFeed::engineInfo;
        const QString poly = QStringLiteral("PolyForm Noncommercial 1.0.0");
        CHECK(engineInfo(QStringLiteral("psxrecomp")).license == poly);
        CHECK(engineInfo(QStringLiteral("snesrecomp")).license == poly);
        CHECK(engineInfo(QStringLiteral("gbarecomp")).license == poly);
        // #248 named psxrecomp's terms specifically, and the row is where a person reads them.
        CHECK(engineInfo(QStringLiteral("psxrecomp")).homepage
              == QStringLiteral("https://github.com/TechnicallyComputers/psxrecomp"));
        CHECK(engineInfo(QStringLiteral("snesrecomp")).homepage.contains(QStringLiteral("snesrecomp")));
        CHECK(engineInfo(QStringLiteral("gbarecomp")).homepage.contains(QStringLiteral("gbarecomp")));
        // Spelling: a catalogue writes the engine name however it likes.
        CHECK(engineInfo(QStringLiteral("PsxRecomp")).license == poly);
        CHECK(engineInfo(QStringLiteral("  psxrecomp  ")).license == poly);
        // An engine this build has never checked: empty, deliberately. Both fields, so no row can show a
        // link to a page nobody looked at either.
        CHECK(engineInfo(QStringLiteral("n64recomp")).license.isEmpty());
        CHECK(engineInfo(QStringLiteral("n64recomp")).homepage.isEmpty());
        CHECK(engineInfo(QString()).id.isEmpty());
        CHECK(engineInfo(QString()).license.isEmpty());
    }

    // ---- 19. the published catalogue parses, in its real shape --------------------------------------------
    // `index.json` + `titles/<id>.json`, the layout retcomm-catalog's own README and SCHEMA.md describe. The
    // manifests below are cut down from the real `twisted-metal4-psx` and `tomba-psx` entries: every field the
    // row model reads is present, and every value here was hand-copied rather than read back out of the code.
    const QByteArray goodIndex = QByteArrayLiteral(
        "{\"schema_version\":1,\"name\":\"RetComM supported titles\","
        "\"catalog_date\":\"2026-09-02T14:03:33Z\",\"release_tag\":\"v2026.09.02.140333.26\","
        "\"platform_defaults\":{\"psx\":{\"bios_identity\":{\"required\":true}}},"
        "\"titles\":[\"twisted-metal4-psx\",\"tomba-psx\"]}");
    const QByteArray tm4 = QByteArrayLiteral(
        "{\"id\":\"twisted-metal4-psx\",\"name\":\"Twisted Metal4\",\"kind\":\"recomp\",\"platform\":\"psx\","
        "\"description\":\"Twisted Metal 4\","
        "\"homepage\":\"https://github.com/TechnicallyComputers/TwistedMetal4Recomp\","
        "\"author_notes\":\"Use USA Redump image (Rev 1)\","
        "\"rom_identity\":{\"crc32\":[\"020e8ae1\"],\"md5\":[\"0e5d822f108fcef31057645721d3d710\"],"
        "\"sha1\":[\"64e226413b27ea12639589b3ca2806976c795c8b\"],"
        "\"sha256\":[\"1be99e1d98c5b5e2d7572f3b46aa63c43ddd6f19f1b5cf179ffd853261a139f1\"],"
        "\"sizes\":[384615504],\"track_counts\":[23],\"require_cue\":true,"
        "\"filenames\":[\"Twisted Metal 4 (USA) (Rev 1).cue\"]},"
        "\"rom_extensions\":[\".cue\",\".bin\"],"
        "\"release\":{\"github\":\"TechnicallyComputers/TwistedMetal4Recomp\","
        "\"asset_glob\":{\"windows\":\"*windows*\",\"linux\":\"*linux*\",\"macos\":\"*macos*\"},"
        "\"allow_prerelease\":true},"
        "\"install_dir_name\":\"TwistedMetal4Recomp\","
        "\"launch\":{\"windows\":\"TwistedMetal4_Recompiled.exe\",\"linux\":\"TwistedMetal4_Recompiled\"},"
        "\"build\":{\"enabled\":true,\"generate\":{\"engine\":\"psxrecomp\",\"config\":\"game.toml\"}}}");
    const QByteArray tomba = QByteArrayLiteral(
        "{\"id\":\"tomba-psx\",\"name\":\"Tomba!\",\"kind\":\"recomp\",\"platform\":\"psx\","
        "\"rom_identity\":{\"crc32\":[],\"md5\":[],\"sha1\":[\"aaaabbbbccccddddeeeeffff0000111122223333\"],"
        "\"sha256\":[],\"disc_serials\":[\"SCUS-94236\"]},"
        "\"rom_extensions\":[\".cue\",\".bin\"],"
        "\"release\":{\"github\":\"TechnicallyComputers/TombaRecomp\"},"
        "\"launch\":{\"windows\":\"Tomba_Recompiled.exe\"},"
        "\"build\":{\"enabled\":true,\"generate\":{\"engine\":\"psxrecomp\"}}}");

    QHash<QString, QByteArray> goodMembers;
    goodMembers.insert(QStringLiteral("index.json"), goodIndex);
    goodMembers.insert(QStringLiteral("titles/twisted-metal4-psx.json"), tm4);
    goodMembers.insert(QStringLiteral("titles/tomba-psx.json"), tomba);
    const QByteArray goodZip = makeZip(goodMembers);
    CHECK(!goodZip.isEmpty());

    {
        const RecompFeed::Feed feed = RecompFeed::parseCatalogZip(goodZip);
        CHECK(feed.ok());
        CHECK(feed.shapeError.isEmpty());
        CHECK(feed.titles.size() == 2);
        // The release identity RetComM itself compares on, so a later "is the remote newer" check has it.
        CHECK(feed.releaseTag == QStringLiteral("v2026.09.02.140333.26"));
        CHECK(feed.catalogDate == QStringLiteral("2026-09-02T14:03:33Z"));

        const ExternalEmulator* t = find(feed.titles, QStringLiteral("twisted-metal4-psx"));
        CHECK(t != nullptr);
        if (t)
        {
            CHECK(t->isNativePort());
            CHECK(t->port.name == QStringLiteral("Twisted Metal4"));
            CHECK(t->port.platform == QStringLiteral("psx"));
            CHECK(t->port.kind == QStringLiteral("recomp"));
            CHECK(t->port.authorNotes == QStringLiteral("Use USA Redump image (Rev 1)"));
            // THE TIER. Every entry in the published catalogue names an engine, so every one of them is the
            // self-compiled tier, and the row's Install has to say so rather than start a download.
            CHECK(t->port.buildEngine == QStringLiteral("psxrecomp"));
            CHECK(recomps::tierOf(*t) == recomps::Tier::SelfCompiled);
            // THE LICENCE, filled from the engine table because a published manifest has no licence field —
            // the terms that govern a self-compiled port are the recompiler's.
            CHECK(t->port.license == QStringLiteral("PolyForm Noncommercial 1.0.0"));
            // The four digest kinds, verbatim, because the gate compares against exactly these strings.
            CHECK(t->port.crc32 == QStringList{ QStringLiteral("020e8ae1") });
            CHECK(t->port.md5 == QStringList{ QStringLiteral("0e5d822f108fcef31057645721d3d710") });
            CHECK(t->port.sha1 == QStringList{ QStringLiteral("64e226413b27ea12639589b3ca2806976c795c8b") });
            CHECK(t->port.sha256
                  == QStringList{ QStringLiteral(
                         "1be99e1d98c5b5e2d7572f3b46aa63c43ddd6f19f1b5cf179ffd853261a139f1") });
            CHECK(t->port.sizes.size() == 1 && t->port.sizes.first() == 384615504LL);
            CHECK(t->port.romExtensions == (QStringList{ QStringLiteral(".cue"), QStringLiteral(".bin") }));
            CHECK(t->port.filenames.size() == 1);
            CHECK(t->port.installDirName == QStringLiteral("TwistedMetal4Recomp"));
            CHECK(t->port.allowPrerelease);
            // ...and the projection onto EB's standalone tier that section 11 asserts for the in-tree
            // catalogue, asserted again here because the feed is the SAME reader.
            CHECK(t->displayName == QStringLiteral("TechnicallyComputers"));
            CHECK(t->homepage
                  == QStringLiteral("https://github.com/TechnicallyComputers/TwistedMetal4Recomp"));
            CHECK(t->updateJsonUrl.contains(QStringLiteral("TwistedMetal4Recomp/releases/latest")));
            CHECK(t->winArtifact == QStringLiteral("windows"));
            CHECK(t->winBinaries.contains(QStringLiteral("TwistedMetal4_Recompiled.exe")));
            CHECK(t->extensions == (QStringList{ QStringLiteral("cue"), QStringLiteral("bin") }));
            // A port never names a system, on this feed either: that is what would make it a selectable PSX
            // emulator for every PSX game (section 15's tripwire, asked of the feed).
            CHECK(t->systems.isEmpty());
        }
        // The second entry publishes ONE digest kind and a disc serial. Both matter below: an author may
        // publish only the algorithm their own gate uses, and a serial is NOT a digest this build can check.
        const ExternalEmulator* tb = find(feed.titles, QStringLiteral("tomba-psx"));
        CHECK(tb != nullptr);
        if (tb)
        {
            CHECK(tb->port.crc32.isEmpty() && tb->port.md5.isEmpty() && tb->port.sha256.isEmpty());
            CHECK(tb->port.sha1.size() == 1);
            CHECK(tb->port.discSerials == QStringList{ QStringLiteral("SCUS-94236") });
            CHECK(recomps::publishesHash(*tb));
        }
    }

    // ---- 20. a document the reader cannot parse is an ERROR, never an empty list (#174) -------------------
    {
        auto shapeErrorOf = [](const QByteArray& zip) { return RecompFeed::parseCatalogZip(zip).shapeError; };

        // Not a zip at all — a captive portal's HTML, or a truncated download.
        CHECK(!shapeErrorOf(QByteArrayLiteral("<html>Sign in to continue</html>")).isEmpty());
        CHECK(!shapeErrorOf(QByteArray()).isEmpty());
        // A readable zip with no index.
        QHash<QString, QByteArray> m = goodMembers;
        m.remove(QStringLiteral("index.json"));
        CHECK(!shapeErrorOf(makeZip(m)).isEmpty());
        // An index that is not JSON, and one that is JSON of the wrong shape.
        m = goodMembers; m.insert(QStringLiteral("index.json"), QByteArrayLiteral("{not json"));
        CHECK(!shapeErrorOf(makeZip(m)).isEmpty());
        m = goodMembers; m.insert(QStringLiteral("index.json"), QByteArrayLiteral("[\"tomba-psx\"]"));
        CHECK(!shapeErrorOf(makeZip(m)).isEmpty());
        // An index with no `titles` key: a document this reader does not understand, not an empty catalogue.
        m = goodMembers; m.insert(QStringLiteral("index.json"), QByteArrayLiteral("{\"schema_version\":1}"));
        CHECK(!shapeErrorOf(makeZip(m)).isEmpty());

        // ...and the case that is NOT an error, which is the other half of the same rule: an index that
        // understood itself and listed nothing. Inventing an error for that would put a red row on screen
        // because somebody published an empty list.
        m = goodMembers;
        m.insert(QStringLiteral("index.json"), QByteArrayLiteral("{\"schema_version\":1,\"titles\":[]}"));
        const RecompFeed::Feed emptyFeed = RecompFeed::parseCatalogZip(makeZip(m));
        CHECK(emptyFeed.ok());
        CHECK(emptyFeed.titles.isEmpty());

        // A listed title whose file did not land: skipped, and the ones that did land survive. A catalogue
        // mid-publish does this routinely and it is not a broken catalogue.
        m = goodMembers; m.remove(QStringLiteral("titles/tomba-psx.json"));
        const RecompFeed::Feed partial = RecompFeed::parseCatalogZip(makeZip(m));
        CHECK(partial.ok());
        CHECK(partial.titles.size() == 1);
        CHECK(find(partial.titles, QStringLiteral("twisted-metal4-psx")) != nullptr);

        // ...but when NONE of them can be read, the document describes nothing and that IS the error case.
        m.clear();
        m.insert(QStringLiteral("index.json"), goodIndex);
        const RecompFeed::Feed nothing = RecompFeed::parseCatalogZip(makeZip(m));
        CHECK(!nothing.ok());
        CHECK(nothing.titles.isEmpty());

        // An id that is a path, not a slug. Nothing here writes a file, so this cannot traverse anything —
        // it is refused because a document with one in it is not a document this reader understands.
        m = goodMembers;
        m.insert(QStringLiteral("index.json"),
                 QByteArrayLiteral("{\"titles\":[\"../../etc/passwd\",\"twisted-metal4-psx\"]}"));
        const RecompFeed::Feed traversal = RecompFeed::parseCatalogZip(makeZip(m));
        CHECK(traversal.ok());
        CHECK(traversal.titles.size() == 1);

        // The INDEX is authoritative about the id: a manifest whose own `id` disagrees with its filename is a
        // packaging accident, and every later lookup uses the id the index published.
        m = goodMembers;
        QByteArray renamed = tm4;
        renamed.replace("\"id\":\"twisted-metal4-psx\"", "\"id\":\"something-else\"");
        m.insert(QStringLiteral("titles/twisted-metal4-psx.json"), renamed);
        const RecompFeed::Feed renamedFeed = RecompFeed::parseCatalogZip(makeZip(m));
        CHECK(find(renamedFeed.titles, QStringLiteral("twisted-metal4-psx")) != nullptr);
        CHECK(find(renamedFeed.titles, QStringLiteral("something-else")) == nullptr);
    }

    // ---- 21. the merge: in-tree wins, by TITLE identity rather than by id ---------------------------------
    // Two catalogues describing one game agree on the game and never on the slug, so the id is not the key.
    // The in-tree entry wins because it carries what the published one structurally cannot: `rom_delivery`
    // (our extension — the schema has no field for how a port takes the game file) and a licence we checked.
    {
        const QList<ExternalEmulator> inTree = shippedPorts();
        CHECK(inTree.size() >= 1);

        // A feed that offers the SAME game on the SAME console under a different id and a different spelling.
        QJsonObject clash;
        clash.insert(QStringLiteral("id"), QStringLiteral("majoras-mask-n64"));
        clash.insert(QStringLiteral("name"), QStringLiteral("The Legend of Zelda: Majora's Mask"));
        clash.insert(QStringLiteral("platform"), QStringLiteral("n64"));
        QJsonObject clashRel; clashRel.insert(QStringLiteral("github"), QStringLiteral("someone/MMRecomp"));
        clash.insert(QStringLiteral("release"), clashRel);
        const ExternalEmulator clashPort = titleFromJson(clash);

        // ...and one that offers a game nothing in-tree covers.
        const RecompFeed::Feed feed = RecompFeed::parseCatalogZip(goodZip);
        QList<ExternalEmulator> feedTitles = feed.titles;
        feedTitles << clashPort;

        const QList<ExternalEmulator> merged = RecompFeed::mergeByTitleIdentity(inTree, feedTitles);
        // The clash is DROPPED and the in-tree entry is the one that survived — asserted on the field only the
        // in-tree entry has, because "an entry with this id is present" would pass either way.
        CHECK(find(merged, QStringLiteral("majoras-mask-n64")) == nullptr);
        const ExternalEmulator* z = find(merged, QStringLiteral("zelda64recomp"));
        CHECK(z != nullptr);
        if (z) CHECK(z->port.romDelivery == QStringLiteral("in_app_menu"));
        if (z) CHECK(z->port.license == QStringLiteral("GPL-3.0"));
        // The feed-only titles are all there.
        CHECK(find(merged, QStringLiteral("twisted-metal4-psx")) != nullptr);
        CHECK(find(merged, QStringLiteral("tomba-psx")) != nullptr);
        CHECK(merged.size() == inTree.size() + 2);

        // An id collision shadows too, whatever the titles say.
        QJsonObject sameId;
        sameId.insert(QStringLiteral("id"), QStringLiteral("zelda64recomp"));
        sameId.insert(QStringLiteral("name"), QStringLiteral("Something Else Entirely"));
        sameId.insert(QStringLiteral("platform"), QStringLiteral("n64"));
        const QList<ExternalEmulator> byId =
            RecompFeed::mergeByTitleIdentity(inTree, { titleFromJson(sameId) });
        CHECK(byId.size() == inTree.size());

        // The SAME title on ANOTHER console is a different game and both rows stand. This is the near miss the
        // whole identity rule exists for: a PSX Majora's Mask row must not be eaten by the N64 entry.
        QJsonObject otherConsole = clash;
        otherConsole.insert(QStringLiteral("id"), QStringLiteral("mm-psx"));
        otherConsole.insert(QStringLiteral("platform"), QStringLiteral("psx"));
        const QList<ExternalEmulator> across =
            RecompFeed::mergeByTitleIdentity(inTree, { titleFromJson(otherConsole) });
        CHECK(across.size() == inTree.size() + 1);
    }

    // ---- 22. the ROM-identity gate, one section per digest kind -------------------------------------------
    // A recomp is compiled against ONE exact dump. The gate is what stands between a person and a build made
    // against the PAL disc, a bad rip or a hack — so it is asserted per kind (an author publishes only the
    // algorithm their own check uses) and, above all, on the near miss.
    {
        using recomps::DumpMatch;
        const QString crc    = QStringLiteral("020e8ae1");
        const QString md5    = QStringLiteral("0e5d822f108fcef31057645721d3d710");
        const QString sha1   = QStringLiteral("64e226413b27ea12639589b3ca2806976c795c8b");
        const QString sha256 = QStringLiteral("1be99e1d98c5b5e2d7572f3b46aa63c43ddd6f19f1b5cf179ffd853261a139f1");

        // One entry per kind, each publishing exactly one digest and nothing else, so a match can only have
        // come from that kind.
        auto entryWith = [](const char* key, const QString& value) {
            QJsonObject o;
            o.insert(QStringLiteral("id"), QStringLiteral("k"));
            o.insert(QStringLiteral("name"), QStringLiteral("Twisted Metal 4"));
            o.insert(QStringLiteral("platform"), QStringLiteral("psx"));
            o.insert(QStringLiteral("rom_extensions"), QJsonArray{ QStringLiteral(".bin") });
            QJsonObject ri;
            ri.insert(QLatin1String(key), QJsonArray{ value });
            o.insert(QStringLiteral("rom_identity"), ri);
            return titleFromJson(o);
        };
        auto romWith = [](const recomps::CachedHashes& h) {
            recomps::LibraryRom r;
            r.systemId = QStringLiteral("psx");
            r.title    = QStringLiteral("Twisted Metal 4");
            r.path     = QStringLiteral("C:/roms/psx/Twisted Metal 4 (USA) (Rev 1) (Track 01).bin");
            r.hashes   = h;
            return QVector<recomps::LibraryRom>{ r };
        };

        CHECK(recomps::dumpMatch(entryWith("crc32", crc), romWith({ crc, {}, {}, {} }))
              == DumpMatch::Hashed);
        CHECK(recomps::dumpMatch(entryWith("md5", md5), romWith({ {}, md5, {}, {} }))
              == DumpMatch::Hashed);
        CHECK(recomps::dumpMatch(entryWith("sha1", sha1), romWith({ {}, {}, sha1, {} }))
              == DumpMatch::Hashed);
        CHECK(recomps::dumpMatch(entryWith("sha256", sha256), romWith({ {}, {}, {}, sha256 }))
              == DumpMatch::Hashed);
        // ANY published kind matching is a match — the schema says so in as many words, and requiring
        // agreement across kinds would refuse a dump over a digest nobody computed.
        CHECK(recomps::dumpMatch(entryWith("sha1", sha1), romWith({ crc, md5, sha1, sha256 }))
              == DumpMatch::Hashed);
        // Hex case is not identity.
        CHECK(recomps::dumpMatch(entryWith("sha1", sha1.toUpper()), romWith({ {}, {}, sha1, {} }))
              == DumpMatch::Hashed);
        // A digest of the same length that is not this one.
        const QString wrong1 = QStringLiteral("0000000000000000000000000000000000000000");
        CHECK(recomps::dumpMatch(entryWith("sha1", sha1), romWith({ {}, {}, wrong1, {} }))
              == DumpMatch::None);

        // ---- THE BEHAVIOUR CHANGE, stated against increment (a)'s own function ------------------------
        // A file whose NAME is this game and whose BYTES are not. Increment (a) called that a match, because
        // the title was all it compared. It is not one any more, and both answers are asserted here so the
        // change is visible in the probe rather than only in a commit message.
        {
            const ExternalEmulator e = entryWith("sha1", sha1);
            const QVector<recomps::LibraryRom> lib = romWith({ {}, {}, wrong1, {} });
            CHECK(recomps::libraryMatches(e, lib));                      // increment (a): a match
            CHECK(recomps::dumpMatch(e, lib) == DumpMatch::None);        // increment (b): not one
            // ...and nothing more is scheduled to be hashed about it. The digest is known and it is wrong.
            CHECK(recomps::pathsNeedingHash(e, lib).isEmpty());
        }

        // ---- NO PUBLISHED DIGEST: the title match stands, and the row says it is unverified ------------
        {
            QJsonObject o;
            o.insert(QStringLiteral("id"), QStringLiteral("nohash"));
            o.insert(QStringLiteral("name"), QStringLiteral("Twisted Metal 4"));
            o.insert(QStringLiteral("platform"), QStringLiteral("psx"));
            const ExternalEmulator e = titleFromJson(o);
            CHECK(!recomps::publishesHash(e));
            const QVector<recomps::LibraryRom> lib = romWith({});
            CHECK(recomps::dumpMatch(e, lib) == DumpMatch::TitleOnly);
            // A serial is NOT counted as a digest: this build has no reader for a disc image's serial, and an
            // entry that looked gated by one would be gated by nothing at all.
            QJsonObject withSerial = o;
            QJsonObject ri;
            ri.insert(QStringLiteral("disc_serials"), QJsonArray{ QStringLiteral("SLUS-00562") });
            withSerial.insert(QStringLiteral("rom_identity"), ri);
            CHECK(!recomps::publishesHash(titleFromJson(withSerial)));
            // ...and a library with nothing of that name is still no match.
            CHECK(recomps::dumpMatch(e, {}) == DumpMatch::None);
        }

        // ---- THE NARROWING, which is what stops this hashing the whole disk -----------------------------
        {
            QJsonObject o;
            o.insert(QStringLiteral("id"), QStringLiteral("n"));
            o.insert(QStringLiteral("name"), QStringLiteral("Twisted Metal 4"));
            o.insert(QStringLiteral("platform"), QStringLiteral("psx"));
            o.insert(QStringLiteral("rom_extensions"), QJsonArray{ QStringLiteral(".bin") });
            QJsonObject ri;
            ri.insert(QStringLiteral("sha1"), QJsonArray{ sha1 });
            ri.insert(QStringLiteral("sizes"), QJsonArray{ 384615504LL });
            o.insert(QStringLiteral("rom_identity"), ri);
            const ExternalEmulator e = titleFromJson(o);

            recomps::LibraryRom r;
            r.systemId = QStringLiteral("psx");
            r.title    = QStringLiteral("Twisted Metal 4");
            r.path     = QStringLiteral("C:/roms/psx/tm4 (Track 01).bin");
            r.size     = 384615504LL;
            CHECK(recomps::worthHashing(e, r));

            // Another console's file is never a candidate, however it is named.
            { auto x = r; x.systemId = QStringLiteral("n64"); CHECK(!recomps::worthHashing(e, x)); }
            // The .cue beside it: the entry does not list that extension, so it is not hashed. This is what
            // keeps a multi-file disc dump from being hashed once per track.
            { auto x = r; x.path = QStringLiteral("C:/roms/psx/tm4.cue"); CHECK(!recomps::worthHashing(e, x)); }
            // A published size that this file is not.
            { auto x = r; x.size = 700 * 1024 * 1024LL; CHECK(!recomps::worthHashing(e, x)); }
            // An UNKNOWN size is not a wrong size. -1 means the caller could not stat it, and refusing on that
            // would silently drop every candidate on a slow or disconnected volume.
            { auto x = r; x.size = -1; CHECK(recomps::worthHashing(e, x)); }
            // An ARCHIVE is exempt from BOTH: a .7z of the dump has its own extension and its own size, and
            // the digests the cache holds for it are the digests of what is INSIDE it.
            { auto x = r; x.archive = true; x.size = 12345; x.path = QStringLiteral("C:/roms/psx/tm4.7z");
              CHECK(recomps::worthHashing(e, x)); }
            // ...and an archive that is on another console still is not.
            { auto x = r; x.archive = true; x.systemId = QStringLiteral("snes");
              CHECK(!recomps::worthHashing(e, x)); }
            // A row with no path at all (a catalogue row for a game that is not downloaded) is not a file.
            { auto x = r; x.path.clear(); CHECK(!recomps::worthHashing(e, x)); }
        }
    }

    // ---- 23. hashing is NEVER done at browse time --------------------------------------------------------
    // The mechanism, not the convention: no function in RecompRows.h opens a file. What is asserted here is
    // the consequence — a cold cache reports `checking` and asks for exactly one file; a WARM cache asks for
    // nothing at all, which is what stops an open section re-hashing a 660 MB disc image on every redraw.
    {
        using recomps::DumpMatch;
        const QString sha1 = QStringLiteral("64e226413b27ea12639589b3ca2806976c795c8b");
        QJsonObject o;
        o.insert(QStringLiteral("id"), QStringLiteral("c"));
        o.insert(QStringLiteral("name"), QStringLiteral("Twisted Metal 4"));
        o.insert(QStringLiteral("platform"), QStringLiteral("psx"));
        QJsonObject ri; ri.insert(QStringLiteral("sha1"), QJsonArray{ sha1 });
        o.insert(QStringLiteral("rom_identity"), ri);
        const ExternalEmulator e = titleFromJson(o);

        recomps::LibraryRom cold;
        cold.systemId = QStringLiteral("psx");
        cold.title    = QStringLiteral("Twisted Metal 4");
        cold.path     = QStringLiteral("C:/roms/psx/tm4.bin");
        CHECK(cold.hashes.isEmpty());

        // COLD: not "you do not own it" — "we have not looked yet".
        CHECK(recomps::dumpMatch(e, { cold }) == DumpMatch::Checking);
        CHECK(recomps::pathsNeedingHash(e, { cold }) == QStringList{ cold.path });
        {
            recomps::Facts f;
            f.checkingDumps = true;
            CHECK(recomps::deriveState(f) == recomps::State::CheckingDumps);
            // ...and it is only ever read when nothing matched. A match is not made provisional by a second
            // candidate nobody has hashed, and an installed port is `installed` regardless.
            f.libraryMatch = true;
            CHECK(recomps::deriveState(f) == recomps::State::NotInstalled);
            f.installed = true;
            CHECK(recomps::deriveState(f) == recomps::State::Installed);
        }

        // WARM AND MATCHING: no work.
        auto warm = cold; warm.hashes.sha1 = sha1;
        CHECK(recomps::dumpMatch(e, { warm }) == DumpMatch::Hashed);
        CHECK(recomps::pathsNeedingHash(e, { warm }).isEmpty());

        // WARM AND NOT MATCHING: also no work. The digest is known; hashing it again would produce the same
        // answer, and this is the case that would otherwise re-hash every unrelated PSX dump on every redraw.
        auto warmWrong = cold; warmWrong.hashes.sha1 = QStringLiteral("1111111111111111111111111111111111111111");
        CHECK(recomps::dumpMatch(e, { warmWrong }) == DumpMatch::None);
        CHECK(recomps::pathsNeedingHash(e, { warmWrong }).isEmpty());

        // A record from an OLDER build carries only the kinds that build computed. Against an entry that
        // publishes a kind the record does not have, that is a cache MISS and not a refusal.
        QJsonObject o256 = o;
        QJsonObject ri256;
        ri256.insert(QStringLiteral("sha256"),
                     QJsonArray{ QStringLiteral(
                         "1be99e1d98c5b5e2d7572f3b46aa63c43ddd6f19f1b5cf179ffd853261a139f1") });
        o256.insert(QStringLiteral("rom_identity"), ri256);
        const ExternalEmulator e256 = titleFromJson(o256);
        auto legacy = cold; legacy.hashes.sha1 = sha1;   // sha1 only, as a pre-#248 stamp holds
        CHECK(recomps::dumpMatch(e256, { legacy }) == DumpMatch::Checking);
        CHECK(recomps::pathsNeedingHash(e256, { legacy }) == QStringList{ legacy.path });

        // A MATCH ALREADY FOUND ends the question: the other candidate is not hashed just because it is there.
        auto other = cold; other.path = QStringLiteral("C:/roms/psx/some other dump.bin");
        CHECK(recomps::pathsNeedingHash(e, { warm, other }).isEmpty());
        CHECK(recomps::pathsNeedingHash(e, { other, warm }).isEmpty());
        // An entry that publishes NOTHING never asks for a hash either — there would be nothing to compare.
        QJsonObject bare;
        bare.insert(QStringLiteral("id"), QStringLiteral("b"));
        bare.insert(QStringLiteral("name"), QStringLiteral("Twisted Metal 4"));
        bare.insert(QStringLiteral("platform"), QStringLiteral("psx"));
        CHECK(recomps::pathsNeedingHash(titleFromJson(bare), { cold }).isEmpty());
    }

    // ---- 24. the last good copy, and what a broken publish may not do -------------------------------------
    // A feed that cannot be reached keeps what it had; a publisher who ships one broken build must not thereby
    // delete the working catalogue on every machine that fetches it.
    {
        QDir(RecompFeed::cacheDir()).removeRecursively();
        // Nothing downloaded yet: an error, and an empty list — the caller shows the in-tree rows and says so.
        CHECK(!RecompFeed::cached().ok());
        CHECK(RecompFeed::cached().titles.isEmpty());
        // ...and `catalogue()` still returns the in-tree catalogue whole, with the reason as an out-param.
        {
            QString err;
            const QList<ExternalEmulator> only = RecompFeed::catalogue(&err);
            CHECK(!err.isEmpty());
            CHECK(only.size() == all().size());
        }

        const RecompFeed::Feed stored = RecompFeed::storeIfParses(goodZip);
        CHECK(stored.ok());
        CHECK(QFile::exists(RecompFeed::cachedCatalogPath()));
        CHECK(RecompFeed::cached().ok());
        CHECK(RecompFeed::cached().titles.size() == 2);

        // The bytes on disk, before a broken publish is offered.
        QByteArray onDisk;
        { QFile f(RecompFeed::cachedCatalogPath()); CHECK(f.open(QIODevice::ReadOnly)); onDisk = f.readAll(); }
        CHECK(!onDisk.isEmpty());

        const RecompFeed::Feed broken = RecompFeed::storeIfParses(QByteArrayLiteral("<html>502</html>"));
        CHECK(!broken.ok());
        // BYTE-IDENTICAL. Not "still parses" — identical, because a rewrite that happened to parse would hide
        // exactly the defect this asserts against.
        { QFile f(RecompFeed::cachedCatalogPath()); CHECK(f.open(QIODevice::ReadOnly));
          CHECK(f.readAll() == onDisk); }
        CHECK(RecompFeed::cached().titles.size() == 2);
        // ...and no half-written temp file survives to be read as the catalogue next launch.
        CHECK(!QFile::exists(RecompFeed::cachedCatalogPath() + QStringLiteral(".part")));

        // With a good copy on disk, the section browses BOTH feeds and the error is empty.
        {
            QString err;
            const QList<ExternalEmulator> both = RecompFeed::catalogue(&err);
            CHECK(err.isEmpty());
            CHECK(both.size() == all().size() + 2);
            CHECK(find(both, QStringLiteral("twisted-metal4-psx")) != nullptr);
            CHECK(find(both, QStringLiteral("zelda64recomp")) != nullptr);
        }

        // Row activation resolves an id from EITHER source — a feed-only row that could not be resolved would
        // be a row that does nothing when pressed.
        ExternalEmulator got;
        CHECK(RecompFeed::findById(QStringLiteral("zelda64recomp"), &got));
        CHECK(got.port.romDelivery == QStringLiteral("in_app_menu"));
        CHECK(RecompFeed::findById(QStringLiteral("twisted-metal4-psx"), &got));
        CHECK(got.port.buildEngine == QStringLiteral("psxrecomp"));
        CHECK(recomps::tierOf(got) == recomps::Tier::SelfCompiled);
        CHECK(!RecompFeed::findById(QStringLiteral("no-such-title"), &got));
        CHECK(!RecompFeed::findById(QString(), &got));

        // THE SCHEDULE. Once a day, and a stamp in the FUTURE (a clock that moved backwards, or an ini synced
        // from another machine) must not freeze the feed until the clock catches up.
        RecompFeed::forgetRefreshStamp();
        CHECK(RecompFeed::dueForRefresh());
        RecompFeed::markRefreshed();
        CHECK(!RecompFeed::dueForRefresh());
        RecompFeed::forgetRefreshStamp();
        CHECK(RecompFeed::dueForRefresh());

        // ---- the rows the section actually draws, over the merged catalogue --------------------------
        // The whole increment, end to end: a feed-only PSX row is self-compiled, needs a ROM, and carries the
        // engine and the engine's licence; the in-tree N64 row is untouched by any of it.
        {
            const QList<ExternalEmulator> both = RecompFeed::catalogue();
            const QVector<recomps::Row> rows = recomps::buildRows(
                both, [](const ExternalEmulator&) { return recomps::Facts{}; });
            const recomps::Row* tm = nullptr;
            const recomps::Row* zz = nullptr;
            for (const recomps::Row& r : rows)
            {
                if (r.portId == QStringLiteral("twisted-metal4-psx")) tm = &r;
                if (r.portId == QStringLiteral("zelda64recomp"))      zz = &r;
            }
            CHECK(tm != nullptr);
            CHECK(zz != nullptr);
            if (tm)
            {
                CHECK(tm->tier == recomps::Tier::SelfCompiled);
                CHECK(tm->engine == QStringLiteral("psxrecomp"));
                CHECK(tm->license == QStringLiteral("PolyForm Noncommercial 1.0.0"));
                CHECK(tm->creditedName == QStringLiteral("TechnicallyComputers"));
                CHECK(tm->state == recomps::State::NeedsRom);
                CHECK(tm->systemId == QStringLiteral("psx"));
            }
            if (zz)
            {
                CHECK(zz->tier == recomps::Tier::PreBuilt);
                CHECK(zz->engine.isEmpty());
                CHECK(zz->license == QStringLiteral("GPL-3.0"));
                CHECK(zz->systemId == QStringLiteral("n64"));
            }
            // Grouped by system id in ascending order, so the PSX header precedes the N64 rows... which is
            // exactly what an id sort gives and a console-name sort would not. Asserted because the ORDER is
            // what makes the list stable across runs.
            int firstHeader = -1, psxHeader = -1;
            for (int i = 0; i < rows.size(); ++i)
                if (rows[i].kind == recomps::Row::Kind::SystemHeader)
                {
                    if (firstHeader < 0) firstHeader = i;
                    if (rows[i].systemId == QStringLiteral("psx")) psxHeader = i;
                }
            CHECK(firstHeader >= 0);
            CHECK(psxHeader >= 0);
            CHECK(rows[firstHeader].systemId == QStringLiteral("n64"));   // "n64" < "psx"
        }

        // ...and `dumpUnverified` reaches the row only where it is the row's actual basis.
        {
            recomps::Facts f;
            f.libraryMatch = true; f.dumpUnverified = true;
            const QList<ExternalEmulator> in = shippedPorts();
            const QVector<recomps::Row> rows =
                recomps::buildRows(in, [&f](const ExternalEmulator&) { return f; });
            bool sawPort = false;
            for (const recomps::Row& r : rows)
                if (r.kind == recomps::Row::Kind::Port) { sawPort = true; CHECK(r.dumpUnverified); }
            CHECK(sawPort);
            // An INSTALLED row is not "unverified" — the question is not being asked there at all.
            f.installed = true;
            const QVector<recomps::Row> installedRows =
                recomps::buildRows(in, [&f](const ExternalEmulator&) { return f; });
            for (const recomps::Row& r : installedRows)
                if (r.kind == recomps::Row::Kind::Port) CHECK(!r.dumpUnverified);
        }

        QDir(RecompFeed::cacheDir()).removeRecursively();
    }

    if (failures == 0) std::printf("PORTS-OK\n");
    else               std::fprintf(stderr, "PORTS had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
