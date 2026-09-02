// Headless check of decoration (bezel) pack install / remove — issue #187.
//
// #187 adds a THIRD install mechanism to the registry browser: a zip that unpacks into
// <data>/bezels/<system>/<packId>/…, next to the add-on file-list and the themes2 folder-tree of #91. The
// three things that can silently go wrong with it are the three this probe exists for:
//
//   1. THE DIGEST. A pack is an opaque binary from a public registry over a URL the index chose. `sha256` is
//      required and verified before anything is written; a mismatch is a REFUSED install with a visible
//      reason, never a silent skip and never a "well, install it anyway".
//   2. ZIP-SLIP. A member named "../../evil.png" must not write outside the destination. The guard is
//      ArchiveSafePath::join — the same one ArchiveRom::extractAll gates every ROM archive on — reached
//      from DecorationPack::planInstall, so this probe pins the DECORATION path's use of it rather than
//      assuming another probe's coverage carries over.
//   3. THE WRAPPER-FOLDER STRIP. Every archiver on every platform produces "MyPack/snes/…" by default, and a
//      single-system pack's root is "snes/…" — one top-level folder in both cases. Strip the wrong one and
//      the pack either installs a system called "MyPack" or installs nothing at all. The rule is "strip a
//      single top-level folder ONLY when it is not itself a known system id", and both halves are pinned.
//
// Also pinned: a two-system pack lands in BOTH bezels/<sys>/<packId>/ folders; a top-level name that is not
// a known system is ignored (with the ignore reported) rather than refusing the pack; remove deletes exactly
// the pack's folders and nothing beside them; and the installed pack is visible to #106's selection through
// DecorationPack::packsForSystem + BezelSelect::candidates with NO restart — the same call RetroView makes
// per session.
//
// Fixture zips are BUILT HERE with miniz's writer rather than committed as binaries: a checked-in zip cannot
// be varied per case (the digest cases need two archives differing by one byte) and nobody can review it.
//
// Prints DECOPACK-OK on success; any failure prints DECOPACK-FAIL <cond> (line) and exits non-zero.
#include "DecorationPack.h"
#include "DecorationInstall.h"
#include "BezelSelect.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "miniz.h"
}

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "DECOPACK-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Build a zip at `path` from (member name -> contents). Uncompressed (level 0) so the bytes are stable and
// the archive's digest is a function of the fixture alone.
static bool makeZip(const QString& path, const QVector<QPair<QString, QByteArray>>& members)
{
    QFile::remove(path);
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, path.toUtf8().constData(), 0)) return false;
    for (const auto& m : members)
    {
        const QByteArray name = m.first.toUtf8();
        if (!mz_zip_writer_add_mem(&zip, name.constData(), m.second.constData(), size_t(m.second.size()),
                                   MZ_NO_COMPRESSION))
        { mz_zip_writer_end(&zip); return false; }
    }
    const bool ok = mz_zip_writer_finalize_archive(&zip) != MZ_FALSE;
    mz_zip_writer_end(&zip);
    return ok;
}

static QString sha256Of(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return QString::fromLatin1(h.result().toHex());
}

static QByteArray readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

// A pack entry that would install, except for whatever the individual case changes.
static DecorationPack::Entry entryFor(const QString& id, const QStringList& systems,
                                      const QString& zipUrl, const QString& digest)
{
    DecorationPack::Entry e;
    e.id      = id;
    e.name    = QStringLiteral("Test Pack");
    e.systems = systems;
    e.author  = QStringLiteral("nobody");
    e.license = QStringLiteral("CC0-1.0");
    e.version = QStringLiteral("1.0.0");
    e.zip     = zipUrl;
    e.sha256  = digest;
    return e;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QStringList known = { QStringLiteral("snes"), QStringLiteral("nes"), QStringLiteral("gba") };

    QTemporaryDir tmp;
    if (!tmp.isValid()) { std::fprintf(stderr, "DECOPACK-FAIL no temp dir\n"); return 1; }
    const QString dataDir = tmp.path() + QStringLiteral("/data");
    const QString root = DecorationPack::bezelsRoot(dataDir);
    CHECK(root == dataDir + QStringLiteral("/bezels"));
    CHECK(DecorationPack::bezelsRoot(QString()).isEmpty());   // empty in, empty out — never "/bezels"
    QDir().mkpath(root);

    const QByteArray png = QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(600, 'A');
    const QByteArray cfg = QByteArray("custom_viewport_width = 1494\n"
                                      "custom_viewport_height = 1120\n"
                                      "custom_viewport_x = 213\n"
                                      "custom_viewport_y = 24\n");

    // ---- 1. planInstall: the two accepted layouts ----------------------------------------------------
    // Layout A — system folders at the zip root. NOTHING is stripped: "snes" IS a known system, so treating
    // it as a wrapper would leave every file systemless and the pack refused as empty.
    {
        const DecorationPack::Plan p = DecorationPack::planInstall(
            { QStringLiteral("snes/default.png"), QStringLiteral("snes/default.cfg") }, known);
        CHECK(p.ok());
        CHECK(!p.stripTop);
        CHECK(p.systems == QStringList{ QStringLiteral("snes") });
        CHECK(p.items.size() == 2);
        CHECK(p.items[0].system == QStringLiteral("snes"));
        CHECK(p.items[0].rel == QStringLiteral("default.png"));
    }
    // Layout B — one wrapper folder, which is what every archiver produces by default. Stripped, because
    // "Arcade Shells" is not a system id.
    {
        const DecorationPack::Plan p = DecorationPack::planInstall(
            { QStringLiteral("Arcade Shells/snes/default.png"),
              QStringLiteral("Arcade Shells/nes/default.png") }, known);
        CHECK(p.ok());
        CHECK(p.stripTop);
        CHECK(p.topFolder == QStringLiteral("Arcade Shells"));
        CHECK(p.systems == (QStringList{ QStringLiteral("nes"), QStringLiteral("snes") }));   // sorted
        CHECK(p.items.size() == 2);
    }
    // A wrapper is stripped ONLY when everything is under it. A README beside the wrapper means the archive
    // has two top-level names, so nothing is stripped and the wrapper becomes an ignored non-system name.
    {
        const DecorationPack::Plan p = DecorationPack::planInstall(
            { QStringLiteral("Arcade Shells/snes/default.png"), QStringLiteral("README.txt") }, known);
        CHECK(!p.stripTop);
        CHECK(!p.ok());   // nothing is under a known system id, so there is nothing to install
    }
    // Junk an archiver adds does not defeat the strip: "__MACOSX" would otherwise be a second top-level name.
    {
        const DecorationPack::Plan p = DecorationPack::planInstall(
            { QStringLiteral("Arcade Shells/snes/default.png"),
              QStringLiteral("__MACOSX/Arcade Shells/._snes") }, known);
        CHECK(p.ok());
        CHECK(p.stripTop);
        CHECK(p.items.size() == 1);
    }
    // Zip-slip, at the plan level: refused OUTRIGHT, whole pack, not "skip that member".
    for (const QString& evil : { QStringLiteral("../evil.png"),
                                 QStringLiteral("snes/../../evil.png"),
                                 QStringLiteral("/etc/passwd"),
                                 QStringLiteral("C:/Windows/system32/evil.png"),
                                 QStringLiteral("\\\\host\\share\\evil.png") })
    {
        const DecorationPack::Plan p = DecorationPack::planInstall(
            { QStringLiteral("snes/default.png"), evil }, known);
        CHECK(!p.ok());
        CHECK(p.error.contains(QStringLiteral("unsafe")));
    }
    // A system nobody has heard of is IGNORED, not a refusal: a pack that also carries art for a console
    // this build does not know is still a good pack for the ones it does.
    {
        const DecorationPack::Plan p = DecorationPack::planInstall(
            { QStringLiteral("snes/default.png"), QStringLiteral("dreamcast/default.png"),
              QStringLiteral("LICENSE") }, known);
        CHECK(p.ok());
        CHECK(p.systems == QStringList{ QStringLiteral("snes") });
        CHECK(p.ignored.contains(QStringLiteral("dreamcast")));
        CHECK(p.ignored.contains(QStringLiteral("LICENSE")));
    }
    // …but a pack with NOTHING for a known system is refused rather than installed empty.
    {
        const DecorationPack::Plan p = DecorationPack::planInstall(
            { QStringLiteral("dreamcast/default.png") }, known);
        CHECK(!p.ok());
    }

    // ---- 2. install: the digest ----------------------------------------------------------------------
    const QString zipA = tmp.path() + QStringLiteral("/packA.zip");
    CHECK(makeZip(zipA, { { QStringLiteral("snes/default.png"), png },
                          { QStringLiteral("snes/default.cfg"), cfg } }));
    const QString digestA = sha256Of(zipA);
    CHECK(DecorationPack::isSha256Hex(digestA));

    {   // WRONG digest -> refused, with a reason naming the checksum, and NOTHING written.
        DecorationPack::Entry e = entryFor(QStringLiteral("shells"), { QStringLiteral("snes") },
                                           QStringLiteral("https://example.invalid/a.zip"),
                                           QString(64, QLatin1Char('0')));
        DecorationInstall::Result res;
        QString err;
        CHECK(!DecorationInstall::installZip(zipA, root, e, known, &res, &err));
        CHECK(err.contains(QStringLiteral("checksum")));
        CHECK(!QFileInfo::exists(root + QStringLiteral("/snes/shells")));
        CHECK(DecorationPack::packsForSystem(root, QStringLiteral("snes")).isEmpty());
    }
    {   // The RIGHT digest for a DIFFERENT archive is still wrong: the check is of these bytes, not of any
        // bytes that happen to hash to something in the index.
        const QString zipOther = tmp.path() + QStringLiteral("/other.zip");
        CHECK(makeZip(zipOther, { { QStringLiteral("nes/default.png"), png } }));
        DecorationPack::Entry e = entryFor(QStringLiteral("shells"), { QStringLiteral("snes") },
                                           QStringLiteral("https://example.invalid/a.zip"), sha256Of(zipOther));
        QString err;
        CHECK(!DecorationInstall::installZip(zipA, root, e, known, nullptr, &err));
        CHECK(err.contains(QStringLiteral("checksum")));
    }

    // ---- 3. install: the happy path, and #106 picking it up ------------------------------------------
    {
        DecorationPack::Entry e = entryFor(QStringLiteral("shells"), { QStringLiteral("snes") },
                                           QStringLiteral("https://example.invalid/a.zip"), digestA);
        DecorationInstall::Result res;
        QString err;
        CHECK(DecorationInstall::installZip(zipA, root, e, known, &res, &err));
        CHECK(err.isEmpty());
        CHECK(res.systems == QStringList{ QStringLiteral("snes") });
        CHECK(res.files == 2);
        // The layout, spelled out here rather than asked of packDir(): a probe that computes its oracle
        // with the function under test pins nothing.
        CHECK(QFileInfo::exists(root + QStringLiteral("/snes/shells/default.png")));
        CHECK(QFileInfo::exists(root + QStringLiteral("/snes/shells/default.cfg")));
        CHECK(readAll(root + QStringLiteral("/snes/shells/default.png")) == png);
        // The staging directory is gone, and it never was a "system".
        CHECK(!QFileInfo::exists(root + QStringLiteral("/.eb-decorations-installing")));
        CHECK(DecorationPack::packsForSystem(root, QStringLiteral("snes"))
              == QStringList{ QStringLiteral("shells") });
        const QVector<DecorationPack::Installed> inst = DecorationPack::installedPacks(root);
        CHECK(inst.size() == 1);
        CHECK(inst[0].id == QStringLiteral("shells"));
        CHECK(inst[0].name == QStringLiteral("Test Pack"));
        CHECK(inst[0].version == QStringLiteral("1.0.0"));
        CHECK(inst[0].systems == QStringList{ QStringLiteral("snes") });
    }
    {
        // #106's selection sees it with no restart: this is the exact list RetroView builds per session.
        std::vector<std::string> packs;
        for (const QString& p : DecorationPack::packsForSystem(root, QStringLiteral("snes")))
            packs.push_back(p.toStdString());
        const std::vector<std::string> c = BezelSelect::candidates("snes", "Chrono Trigger", "snes9x", packs);
        // Bounds-checked rather than indexed raw. A failing assertion here must print DECOPACK-FAIL and exit
        // 1; a probe that instead reads past the end exits 0xC0000005, and this repo has already spent a
        // week on one crashing probe being mistaken for another (#180/#211). A wrong ANSWER and a wrong
        // LENGTH have to look the same from outside: like a failed check.
        auto at = [&c](size_t i) { return i < c.size() ? c[i] : std::string("<missing>"); };
        CHECK(c.size() == 6);
        CHECK(at(0) == "snes/Chrono Trigger.png");        // a loose game-specific file still wins
        CHECK(at(1) == "snes/shells/Chrono Trigger.png"); // then the pack's game-specific art
        CHECK(at(2) == "snes/default.png");               // then the loose per-system file
        CHECK(at(3) == "snes/shells/default.png");        // then the pack's per-system art <- what installed
        CHECK(at(4) == "snes9x.png");                     // the two legacy global tiers, unchanged
        CHECK(at(5) == "default.png");
        // …and the file the fourth tier names is the one on disk.
        CHECK(QFileInfo::exists(root + QStringLiteral("/") + QString::fromStdString(at(3))));
    }

    // ---- 4. zip-slip, through the real installer -----------------------------------------------------
    {
        const QString evilZip = tmp.path() + QStringLiteral("/evil.zip");
        CHECK(makeZip(evilZip, { { QStringLiteral("snes/default.png"), png },
                                 { QStringLiteral("../../pwned.png"), png } }));
        DecorationPack::Entry e = entryFor(QStringLiteral("evilpack"), { QStringLiteral("snes") },
                                           QStringLiteral("https://example.invalid/e.zip"), sha256Of(evilZip));
        QString err;
        CHECK(!DecorationInstall::installZip(evilZip, root, e, known, nullptr, &err));
        CHECK(err.contains(QStringLiteral("unsafe")));
        // Nothing escaped, and the pack that WAS installed is untouched.
        CHECK(!QFileInfo::exists(QFileInfo(root).absolutePath() + QStringLiteral("/pwned.png")));
        CHECK(!QFileInfo::exists(dataDir + QStringLiteral("/pwned.png")));
        CHECK(!QFileInfo::exists(root + QStringLiteral("/snes/evilpack")));
        CHECK(QFileInfo::exists(root + QStringLiteral("/snes/shells/default.png")));
    }

    // ---- 5. a two-system pack lands in BOTH folders, from the wrapper layout -------------------------
    {
        const QString zipB = tmp.path() + QStringLiteral("/packB.zip");
        CHECK(makeZip(zipB, { { QStringLiteral("Retro Shells/snes/default.png"), png },
                              { QStringLiteral("Retro Shells/nes/default.png"), png },
                              { QStringLiteral("Retro Shells/nes/Metroid.png"), png },
                              { QStringLiteral("Retro Shells/dreamcast/default.png"), png },
                              { QStringLiteral("Retro Shells/README.txt"), QByteArray("hi") } }));
        DecorationPack::Entry e = entryFor(QStringLiteral("retro"),
                                           { QStringLiteral("snes"), QStringLiteral("nes") },
                                           QStringLiteral("https://example.invalid/b.zip"), sha256Of(zipB));
        DecorationInstall::Result res;
        QString err;
        CHECK(DecorationInstall::installZip(zipB, root, e, known, &res, &err));
        CHECK(res.systems == (QStringList{ QStringLiteral("nes"), QStringLiteral("snes") }));
        CHECK(res.files == 3);
        CHECK(res.ignored.contains(QStringLiteral("dreamcast")));   // reported, not refused
        CHECK(res.ignored.contains(QStringLiteral("README.txt")));
        // "dreamcast" is ignored here because the app does not know it. The OTHER half of the allowed set
        // is the ENTRY's own declaration: a zip that carries art for a system the index entry did NOT name
        // must not install it, because the card the user pressed did not offer it. Live-drive finding — the
        // fixture pack's zip carried a fourth console, the entry did not list it, and it landed anyway.
        {
            DecorationPack::Entry narrow = e;
            narrow.id = QStringLiteral("retro-snes-only");
            narrow.systems = QStringList{ QStringLiteral("snes") };   // the zip still carries nes
            DecorationInstall::Result r2;
            QString e2;
            CHECK(DecorationInstall::installZip(zipB, root, narrow, known, &r2, &e2));
            CHECK(r2.systems == QStringList{ QStringLiteral("snes") });
            CHECK(r2.ignored.contains(QStringLiteral("nes")));
            CHECK(QFileInfo::exists(root + QStringLiteral("/snes/retro-snes-only/default.png")));
            CHECK(!QFileInfo::exists(root + QStringLiteral("/nes/retro-snes-only")));
            CHECK(DecorationPack::removePack(root, QStringLiteral("retro-snes-only"), &e2));

            // …and an entry declaring ONLY systems this build does not emulate is refused outright, with a
            // reason naming them, rather than installing an empty pack.
            DecorationPack::Entry alien = e;
            alien.id = QStringLiteral("alien");
            alien.systems = QStringList{ QStringLiteral("dreamcast") };
            CHECK(!DecorationInstall::installZip(zipB, root, alien, known, nullptr, &e2));
            CHECK(e2.contains(QStringLiteral("dreamcast")));
        }
        CHECK(QFileInfo::exists(root + QStringLiteral("/snes/retro/default.png")));
        CHECK(QFileInfo::exists(root + QStringLiteral("/nes/retro/default.png")));
        CHECK(QFileInfo::exists(root + QStringLiteral("/nes/retro/Metroid.png")));
        // The wrapper folder is NOT a system, and the ignored system was not created.
        CHECK(!QFileInfo::exists(root + QStringLiteral("/Retro Shells")));
        CHECK(!QFileInfo::exists(root + QStringLiteral("/dreamcast")));
        // Two packs now compete for snes, in a stable sorted order — never the filesystem's order.
        CHECK(DecorationPack::packsForSystem(root, QStringLiteral("snes"))
              == (QStringList{ QStringLiteral("retro"), QStringLiteral("shells") }));
        const QVector<DecorationPack::Installed> inst = DecorationPack::installedPacks(root);
        CHECK(inst.size() == 2);
        CHECK(inst[0].id == QStringLiteral("retro"));
        CHECK(inst[0].systems == (QStringList{ QStringLiteral("nes"), QStringLiteral("snes") }));
        CHECK(inst[1].id == QStringLiteral("shells"));
    }

    // ---- 6. remove deletes EXACTLY this pack's folders ------------------------------------------------
    {
        QString err;
        CHECK(DecorationPack::removePack(root, QStringLiteral("retro"), &err));
        CHECK(err.isEmpty());
        CHECK(!QFileInfo::exists(root + QStringLiteral("/snes/retro")));
        CHECK(!QFileInfo::exists(root + QStringLiteral("/nes/retro")));
        // The neighbour pack, and the system folders themselves, are untouched.
        CHECK(QFileInfo::exists(root + QStringLiteral("/snes/shells/default.png")));
        CHECK(QFileInfo::exists(root + QStringLiteral("/nes")));
        CHECK(DecorationPack::packsForSystem(root, QStringLiteral("snes"))
              == QStringList{ QStringLiteral("shells") });
        // Removing it again is a refusal with a reason, not a silent success.
        CHECK(!DecorationPack::removePack(root, QStringLiteral("retro"), &err));
        CHECK(!err.isEmpty());
        // A traversal-shaped id never reaches the filesystem.
        CHECK(!DecorationPack::removePack(root, QStringLiteral("../shells"), &err));
        CHECK(QFileInfo::exists(root + QStringLiteral("/snes/shells/default.png")));
    }

    // ---- 7. reinstalling replaces, rather than merging ------------------------------------------------
    {
        const QString zipC = tmp.path() + QStringLiteral("/packC.zip");
        CHECK(makeZip(zipC, { { QStringLiteral("snes/default.png"), QByteArray("NEWER") } }));
        DecorationPack::Entry e = entryFor(QStringLiteral("shells"), { QStringLiteral("snes") },
                                           QStringLiteral("https://example.invalid/c.zip"), sha256Of(zipC));
        e.version = QStringLiteral("2.0.0");
        QString err;
        CHECK(DecorationInstall::installZip(zipC, root, e, known, nullptr, &err));
        CHECK(readAll(root + QStringLiteral("/snes/shells/default.png")) == QByteArray("NEWER"));
        // The previous install's second file is GONE — a replace, not a merge, or an old .cfg would keep
        // placing the game into the previous shell's cutout.
        CHECK(!QFileInfo::exists(root + QStringLiteral("/snes/shells/default.cfg")));
        const QVector<DecorationPack::Installed> inst = DecorationPack::installedPacks(root);
        CHECK(inst.size() == 1 && inst[0].version == QStringLiteral("2.0.0"));
    }

    // ---- 8. installBytes: the entry point BOTH download surfaces actually call -----------------------
    // The UI never has a file, it has a QByteArray, and the temp-file dance in between is where a whole
    // install was silently lost: a QTemporaryFile's OS handle is still open after close() on Windows, so the
    // zip reader that opens the path next reads a ZERO-length file and refuses a perfectly good pack whose
    // digest had just matched. Found by driving the live UI, not by this probe — so what the probe can do
    // is keep the surfaces' actual entry point exercised, and agreeing with installZip.
    {
        const QString zipD = tmp.path() + QStringLiteral("/packD.zip");
        CHECK(makeZip(zipD, { { QStringLiteral("Wrapped/gba/default.png"), png } }));
        DecorationPack::Entry e = entryFor(QStringLiteral("frombytes"), { QStringLiteral("gba") },
                                           QStringLiteral("https://example.invalid/d.zip"), sha256Of(zipD));
        DecorationInstall::Result res;
        QString err;
        CHECK(DecorationInstall::installBytes(readAll(zipD), root, e, known, &res, &err));
        CHECK(err.isEmpty());
        CHECK(res.systems == QStringList{ QStringLiteral("gba") });
        CHECK(QFileInfo::exists(root + QStringLiteral("/gba/frombytes/default.png")));
        CHECK(readAll(root + QStringLiteral("/gba/frombytes/default.png")) == png);

        // The same refusals reach through: a wrong digest is refused from bytes exactly as from a file, and
        // empty bytes are refused before a temp folder is even made.
        DecorationPack::Entry bad = e;
        bad.id = QStringLiteral("frombytes2");
        bad.sha256 = QString(64, QLatin1Char('0'));
        CHECK(!DecorationInstall::installBytes(readAll(zipD), root, bad, known, nullptr, &err));
        CHECK(err.contains(QStringLiteral("checksum")));
        CHECK(!QFileInfo::exists(root + QStringLiteral("/gba/frombytes2")));
        CHECK(!DecorationInstall::installBytes(QByteArray(), root, e, known, nullptr, &err));
        CHECK(!err.isEmpty());
    }

    if (failures == 0) std::printf("DECOPACK-OK\n");
    return failures == 0 ? 0 : 1;
}
