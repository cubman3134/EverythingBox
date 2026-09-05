// Headless check of GAME UPDATES AND DLC (issue #189) — the per-emulator install recipes carried as DATA in
// the emulator registry, the `updates/` + `dlc/` sidecar convention, the hash-tracked install record that
// makes a launch idempotent, and the two RESTRAINT verdicts that stop this feature from ever overwriting
// content the user installed themselves.
//
// QtCore-only (ContentRecipe.h is a header-only schema; ContentInstall.cpp is QDir/QFile/QCryptographicHash
// over the probe's own isolated data dir), so it runs under the offscreen QPA in CI, and it drives the SHIPPED
// recipes out of EmulatorRegistry rather than a fixture copy of them. It pins:
//
//   * SCHEMA — every shipped emulator's recipe parses to the kind it claims; every kind is one of
//     ContentRecipe::knownKinds(); the whole spec round-trips through JSON unchanged, and so does the
//     ExternalEmulator carrying it; a MALFORMED recipe (unknown kind, a known kind missing its one required
//     field, a non-object where a recipe belongs) parses without a crash, reads as invalid, and produces
//     EXACTLY ONE slot-wide "ignored" decision rather than one per file.
//   * SIDECARS — <game folder>/updates/ and /dlc/, their DIRECT children only, sorted; a missing folder is
//     empty, not an error. Title-id derivation: an explicit titleid.txt wins, then the game file's name (16
//     hex digits, or a Sony serial), and NO GUESS when neither says anything.
//   * RYUJINX (jsonRegistry) end to end against a fixture data dir — updates.json and dlc.json are created,
//     an EXISTING USER ENTRY survives byte-for-byte, the user's own "selected" pin is LEFT ALONE and reported,
//     and a "selected" this app itself wrote IS advanced when a newer package arrives.
//   * CEMU (copyTree) end to end against a fixture mlc01 — the Wii U update/DLC title paths are right, an
//     identical file already there is skipped, and a DIFFERENT file already there is left alone with its own
//     bytes intact and reported.
//   * IDEMPOTENCE — a second launch with the same packages installs nothing and rewrites nothing.
//   * THE SNAPSHOT — the emulator's content index is snapshotted BEFORE the first write, and the snapshot is
//     never retaken (it describes the world before this app existed, not the world before the last launch).
//   * THE PER-GAME OVERRIDE (#51's store) — a version pin selects one package, "none" installs no update at
//     all, DLC "off" installs no DLC, and both levers round-trip through the override store as full levers.
//   * RPCS3 — the recipe DESCRIBES what the PS3 code path already does. The argv the recipe produces is
//     asserted against the literal argument list EmulatorManager.cpp really spawns, read out of the source, so
//     the description cannot rot into a lie.
//   * #97's HASH — ContentInstall::fileSha1's streaming digest is byte-identical to HashVerify::hashBytes over
//     the same bytes, so "verified by the recorded hash" means the app's one hasher.
//   * THE LIBRARY SCAN — a package inside `updates/` or `dlc/` is NOT a game. Without that rule (found on
//     the first live drive) every update beside a game became a second, playable-looking library tile
//     named after the patch.
//
// Prints CONTENTINSTALL-OK on success; any failure prints CONTENTINSTALL-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so the install record,
// the fixture ROMs folder and the fixture emulator data dirs all live in a directory that starts empty and is
// removed at exit.
#include "ContentInstall.h"
#include "ContentRecipe.h"
#include "EmulatorRegistry.h"
#include "HashVerify.h"
#include "LaunchOptionsStore.h"
#include "RomRouting.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "CONTENTINSTALL-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using ContentRecipe::Recipe;
using ContentRecipe::Spec;
using ContentRecipe::Verdict;

// ---- fixture helpers ---------------------------------------------------------------------------------------
static QString root() { return AppPaths::dataDir() + QStringLiteral("/fx"); }

static bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(bytes) == bytes.size());
    f.close();
    return ok;
}

static QByteArray readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    const QByteArray b = f.readAll();
    f.close();
    return b;
}

static QJsonDocument readJson(const QString& path) { return QJsonDocument::fromJson(readFile(path)); }

// A spec whose {data} is pinned at a real fixture directory, so a recipe under test never has to guess where
// the emulator keeps its files. Takes the SHIPPED spec and swaps only the dataDirs.
static Spec pinnedAt(const Spec& shipped, const QString& dir)
{
    Spec s = shipped;
    s.dataDirs = QStringList{ dir };
    return s;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QDir().mkpath(root());

    // ==== 1. The shipped recipes parse, and say what they claim ==============================================
    {
        const ExternalEmulator* ryu = EmulatorRegistry::byId(QStringLiteral("ryujinx"));
        const ExternalEmulator* cem = EmulatorRegistry::byId(QStringLiteral("cemu"));
        const ExternalEmulator* rp3 = EmulatorRegistry::byId(QStringLiteral("rpcs3"));
        const ExternalEmulator* vit = EmulatorRegistry::byId(QStringLiteral("vita3k"));
        const ExternalEmulator* aza = EmulatorRegistry::byId(QStringLiteral("azahar"));
        CHECK(ryu && cem && rp3 && vit && aza);
        if (ryu && cem && rp3 && vit && aza)
        {
            CHECK(ryu->contentInstall.updates.kind == QStringLiteral("jsonRegistry"));
            CHECK(ryu->contentInstall.dlc.kind     == QStringLiteral("jsonRegistry"));
            CHECK(ryu->contentInstall.updates.container == QStringLiteral("object"));
            CHECK(ryu->contentInstall.dlc.container     == QStringLiteral("array"));
            CHECK(ryu->contentInstall.updates.path.contains(QStringLiteral("{titleId}")));
            CHECK(ryu->contentInstall.updates.isValid() && ryu->contentInstall.dlc.isValid());

            CHECK(cem->contentInstall.updates.kind == QStringLiteral("copyTree"));
            CHECK(cem->contentInstall.dlc.kind     == QStringLiteral("copyTree"));
            // The Wii U high half is a CONSTANT per slot: 0005000E update, 0005000C DLC (base is 00050000).
            CHECK(cem->contentInstall.updates.dest.contains(QStringLiteral("0005000E")));
            CHECK(cem->contentInstall.dlc.dest.contains(QStringLiteral("0005000C")));
            CHECK(cem->contentInstall.updates.isValid() && cem->contentInstall.dlc.isValid());

            CHECK(rp3->contentInstall.updates.kind == QStringLiteral("cli"));
            CHECK(rp3->contentInstall.updates.isDescribedOnly());

            CHECK(vit->contentInstall.updates.kind == QStringLiteral("copyTree"));
            CHECK(vit->contentInstall.updates.dest.contains(QStringLiteral("ux0/patch")));
            CHECK(vit->contentInstall.dlc.dest.contains(QStringLiteral("ux0/addcont")));

            CHECK(aza->contentInstall.updates.kind == QStringLiteral("emulatorUpdater"));
            CHECK(aza->contentInstall.updates.isDelegated());
            CHECK(!aza->contentInstall.updates.note.isEmpty());   // a delegated recipe MUST say why
        }

        // Every kind that ships is a kind this build knows, and every shipped emulator round-trips WITH its
        // recipe through the registry's own JSON schema.
        int withRecipes = 0;
        for (const ExternalEmulator& e : EmulatorRegistry::builtinEmulators())
        {
            for (const Recipe* r : { &e.contentInstall.updates, &e.contentInstall.dlc })
                if (!r->isEmpty()) CHECK(ContentRecipe::knownKinds().contains(r->kind));
            if (!e.contentInstall.isEmpty()) ++withRecipes;
            CHECK(ContentRecipe::specFromJson(ContentRecipe::toJson(e.contentInstall)) == e.contentInstall);
            CHECK(EmulatorRegistry::fromJson(EmulatorRegistry::toJson(e)).contentInstall == e.contentInstall);
        }
        CHECK(withRecipes == 5);   // ryujinx, cemu, rpcs3, vita3k, azahar — the table this increment writes
    }

    // ==== 2. Malformed / unknown recipes: ignored, never fatal ==============================================
    {
        // An unknown kind PARSES and ROUND-TRIPS (a newer registry file must not lose data here) but is not
        // valid, so nothing acts on it.
        const QJsonObject bad = QJsonDocument::fromJson(
            R"({"kind":"wobble","dest":"somewhere"})").object();
        const Recipe r = ContentRecipe::recipeFromJson(bad);
        CHECK(r.kind == QStringLiteral("wobble"));
        CHECK(!r.isValid());
        CHECK(ContentRecipe::recipeFromJson(ContentRecipe::toJson(r)) == r);

        // A KNOWN kind missing its one required field is equally inert.
        CHECK(!ContentRecipe::recipeFromJson(QJsonDocument::fromJson(R"({"kind":"jsonRegistry"})").object()).isValid());
        CHECK(!ContentRecipe::recipeFromJson(QJsonDocument::fromJson(R"({"kind":"copyTree"})").object()).isValid());
        CHECK(!ContentRecipe::recipeFromJson(QJsonDocument::fromJson(R"({"kind":"cli"})").object()).isValid());

        // A non-object where a recipe belongs, and a non-object where a whole spec belongs: empty, no crash.
        CHECK(ContentRecipe::recipeFromJson(QJsonValue(42)).isEmpty());
        CHECK(ContentRecipe::recipeFromJson(QJsonValue(QStringLiteral("cli"))).isEmpty());
        CHECK(ContentRecipe::specFromJson(QJsonValue(QJsonArray{})).isEmpty());
        CHECK(ContentRecipe::specFromJson(QJsonValue()).isEmpty());

        // ONE logged decision for the whole slot, not one per file — the "ignored with one line" contract.
        QVector<ContentInstall::Candidate> three;
        for (int i = 0; i < 3; ++i)
        {
            ContentInstall::Candidate c;
            c.path = root() + QStringLiteral("/x%1.bin").arg(i);
            c.name = QStringLiteral("x%1.bin").arg(i);
            c.size = 10; c.mtime = 100;
            three.push_back(c);
        }
        const QVector<ContentInstall::Planned> p = ContentInstall::planSlot(
            r, ContentRecipe::slotUpdates(), three, ContentInstall::TitleRecord{}, QString(), QStringLiteral("0100000000010000"));
        CHECK(p.size() == 1);
        CHECK(p.size() == 1 && p[0].decision == ContentInstall::Decision::UnknownKind);

        // A registry FILE carrying a malformed recipe merges without dropping the built-in table (#52's rule).
        const QString dataDir = root() + QStringLiteral("/emujson");
        QDir().mkpath(dataDir);
        CHECK(writeFile(dataDir + QStringLiteral("/x.json"),
                        QByteArray(R"([{"id":"cemu","contentInstall":{"updates":{"kind":"wobble"}}}])")));
        QStringList warns;
        const QList<ExternalEmulator> merged = EmulatorRegistry::loadDataDir(
            dataDir, EmulatorRegistry::builtinEmulators(), [&](const QString& w) { warns << w; });
        CHECK(merged.size() == EmulatorRegistry::builtinEmulators().size());
        const ExternalEmulator* mc = nullptr;
        for (const ExternalEmulator& e : merged) if (e.id == QStringLiteral("cemu")) mc = &e;
        CHECK(mc && mc->contentInstall.updates.kind == QStringLiteral("wobble"));
        CHECK(mc && !mc->contentInstall.updates.isValid());
    }

    // ==== 3. Sidecar discovery + title-id derivation =========================================================
    {
        const QString gameDir = root() + QStringLiteral("/roms/wiiu");
        const QString game    = gameDir + QStringLiteral("/Some Game [00050000101C9300].wux");
        CHECK(writeFile(game, QByteArray("base")));
        CHECK(ContentInstall::discover(game, ContentRecipe::slotUpdates()).isEmpty());   // no folder: empty, not an error
        CHECK(writeFile(gameDir + QStringLiteral("/updates/b.pack"), QByteArray("bbb")));
        CHECK(writeFile(gameDir + QStringLiteral("/updates/a.pack"), QByteArray("aaa")));
        CHECK(writeFile(gameDir + QStringLiteral("/updates/sub/deep.bin"), QByteArray("deep")));
        const QStringList found = ContentInstall::discover(game, ContentRecipe::slotUpdates());
        CHECK(found.size() == 3);                                        // a.pack, b.pack, sub/  (direct children)
        CHECK(found.size() == 3 && QFileInfo(found[0]).fileName() == QStringLiteral("a.pack"));   // sorted
        CHECK(found.size() == 3 && QFileInfo(found[1]).fileName() == QStringLiteral("b.pack"));
        CHECK(found.size() == 3 && QFileInfo(found[2]).fileName() == QStringLiteral("sub"));      // a FOLDER is one package
        for (const QString& f : found) CHECK(!f.contains(QStringLiteral("deep.bin")));            // never recursed into

        CHECK(ContentInstall::sidecarDir(game, ContentRecipe::slotDlc())
              == gameDir + QStringLiteral("/dlc"));

        // Title-id derivation.
        CHECK(ContentRecipe::titleIdFromName(QStringLiteral("Some Game [00050000101C9300].wux"))
              == QStringLiteral("00050000101C9300"));
        CHECK(ContentRecipe::titleIdFromName(QStringLiteral("game (BCUS98148).iso")) == QStringLiteral("BCUS98148"));
        CHECK(ContentRecipe::titleIdFromName(QStringLiteral("Ordinary Game.nsp")).isEmpty());
        CHECK(ContentRecipe::titleIdFromName(QStringLiteral("012345678901234")).isEmpty());   // 15 digits: NOT a guess
        CHECK(ContentInstall::resolveTitleId(game) == QStringLiteral("00050000101C9300"));
        CHECK(ContentRecipe::titleIdHigh(QStringLiteral("00050000101C9300")) == QStringLiteral("00050000"));
        CHECK(ContentRecipe::titleIdLow(QStringLiteral("00050000101C9300"))  == QStringLiteral("101C9300"));
        CHECK(ContentRecipe::titleIdHigh(QStringLiteral("BCUS98148")).isEmpty());

        // titleid.txt is the owner's explicit statement and OUTRANKS anything derived from a name.
        const QString gd2 = root() + QStringLiteral("/roms/nameless");
        CHECK(writeFile(gd2 + QStringLiteral("/Ordinary Game.nsp"), QByteArray("base")));
        CHECK(ContentInstall::resolveTitleId(gd2 + QStringLiteral("/Ordinary Game.nsp")).isEmpty());
        CHECK(writeFile(gd2 + QStringLiteral("/titleid.txt"), QByteArray(" 0100000000010000 \n")));
        CHECK(ContentInstall::resolveTitleId(gd2 + QStringLiteral("/Ordinary Game.nsp"))
              == QStringLiteral("0100000000010000"));
    }

    // ==== 4. The two restraint verdicts (pure) ==============================================================
    {
        CHECK(ContentRecipe::verdictForFile(false, QString(), QStringLiteral("aa"), false) == Verdict::Write);
        CHECK(ContentRecipe::verdictForFile(true, QStringLiteral("AA"), QStringLiteral("aa"), false) == Verdict::SkipIdentical);
        // THE CLOBBER GUARD: different bytes, not ours -> left alone. This one line is the whole discipline.
        CHECK(ContentRecipe::verdictForFile(true, QStringLiteral("bb"), QStringLiteral("aa"), false) == Verdict::LeaveAlone);
        // Different bytes but the record says we put the current file there: replacing our own is an upgrade.
        CHECK(ContentRecipe::verdictForFile(true, QStringLiteral("bb"), QStringLiteral("aa"), true) == Verdict::Write);

        const QStringList ours{ QStringLiteral("C:/emu/pkgs/ours.nsp") };
        CHECK(ContentRecipe::verdictForScalar(false, QString(), QStringLiteral("x"), ours) == Verdict::Write);
        CHECK(ContentRecipe::verdictForScalar(true, QStringLiteral("  "), QStringLiteral("x"), ours) == Verdict::Write);
        CHECK(ContentRecipe::verdictForScalar(true, QStringLiteral("x"), QStringLiteral("x"), ours) == Verdict::SkipIdentical);
        CHECK(ContentRecipe::verdictForScalar(true, QStringLiteral("D:/mine/user.nsp"), QStringLiteral("x"), ours) == Verdict::LeaveAlone);
        #ifdef Q_OS_WIN
        // A BACKSLASH SPELLING IS THE SAME PATH — on Windows. The comparison normalises with
        // QDir::fromNativeSeparators, which converts backslashes only where the backslash IS the separator.
        CHECK(ContentRecipe::verdictForScalar(true, QStringLiteral("C:\\emu\\pkgs\\ours.nsp"), QStringLiteral("x"), ours) == Verdict::Write);
        #else
        // On Unix it is not the same path, so the value is simply somebody else's and is left alone.
        const QString oursBackslash = QStringLiteral("C:\emu\pkgs\ours.nsp");
        CHECK(ContentRecipe::verdictForScalar(true, oursBackslash, QStringLiteral("x"), ours) == Verdict::LeaveAlone);
        #endif

        // The override levers.
        CHECK(ContentRecipe::pinAccepts(QString(), QStringLiteral("upd v65536.nsp")));
        CHECK(!ContentRecipe::pinAccepts(QStringLiteral("none"), QStringLiteral("upd v65536.nsp")));
        CHECK(ContentRecipe::pinAccepts(QStringLiteral("v65536"), QStringLiteral("upd v65536.nsp")));
        CHECK(!ContentRecipe::pinAccepts(QStringLiteral("v131072"), QStringLiteral("upd v65536.nsp")));
        CHECK(ContentRecipe::dlcEnabled(QString()) && ContentRecipe::dlcEnabled(QStringLiteral("on")));
        CHECK(!ContentRecipe::dlcEnabled(QStringLiteral("off")) && !ContentRecipe::dlcEnabled(QStringLiteral("OFF")));
    }

    // ==== 5. The #51 override store carries both content levers ==============================================
    {
        const QString key = QStringLiteral("tt1234567");
        LaunchOpts::Override ov;
        ov.contentUpdate = QStringLiteral("  v65536  ");
        ov.contentDlc    = QStringLiteral("OFF");
        CHECK(!ov.isEmpty());                                  // a content-only override is a REAL record
        LaunchOpts::set(key, ov);
        const LaunchOpts::Override back = LaunchOpts::get(key);
        CHECK(back.contentUpdate == QStringLiteral("v65536")); // trimmed
        CHECK(back.contentDlc    == QStringLiteral("off"));    // one spelling
        CHECK(LaunchOpts::has(key));
        LaunchOpts::reset(key);
        CHECK(LaunchOpts::get(key).contentUpdate.isEmpty() && LaunchOpts::get(key).contentDlc.isEmpty());
    }

    // ==== 6. Ryujinx (jsonRegistry) end to end ===============================================================
    const QString ryuData = root() + QStringLiteral("/ryujinx-data");
    const QString swDir   = root() + QStringLiteral("/roms/switch");
    const QString swGame  = swDir + QStringLiteral("/Great Game [0100000000010000].nsp");
    const QString titleSw = QStringLiteral("0100000000010000");
    const QString updJson = ryuData + QStringLiteral("/games/") + titleSw + QStringLiteral("/updates.json");
    const QString dlcJson = ryuData + QStringLiteral("/games/") + titleSw + QStringLiteral("/dlc.json");
    {
        const ExternalEmulator* ryu = EmulatorRegistry::byId(QStringLiteral("ryujinx"));
        CHECK(ryu);
        if (!ryu) { std::fprintf(stderr, "CONTENTINSTALL: no ryujinx entry\n"); return 1; }
        const Spec spec = pinnedAt(ryu->contentInstall, ryuData);

        CHECK(writeFile(swGame, QByteArray("base rom")));
        const QString updA = swDir + QStringLiteral("/updates/Great Game [0100000000010800][v65536].nsp");
        CHECK(writeFile(updA, QByteArray("update A bytes")));
        const QString dlc1 = swDir + QStringLiteral("/dlc/Great Game DLC 1.nsp");
        CHECK(writeFile(dlc1, QByteArray("dlc one")));

        // A user entry that was already in the index. It must survive, and its "selected" pin must NOT move.
        CHECK(writeFile(updJson, QByteArray(
            "{\n  \"paths\": [\"D:/mine/user-made.nsp\"],\n  \"selected\": \"D:/mine/user-made.nsp\"\n}\n")));

        const ContentInstall::Result r1 = ContentInstall::installForLaunch(
            QStringLiteral("ryujinx"), spec, swGame, root() + QStringLiteral("/ryujinx-bin"), QString(), QString());
        CHECK(r1.installed == 2);        // one update entry, one DLC entry
        CHECK(r1.failed == 0);

        const QJsonObject uo = readJson(updJson).object();
        const QJsonArray paths = uo.value(QStringLiteral("paths")).toArray();
        CHECK(paths.size() == 2);
        // BYTE-FOR-BYTE: the user's own entry is still there, still first, still exactly what they wrote.
        CHECK(paths.size() == 2 && paths[0].toString() == QStringLiteral("D:/mine/user-made.nsp"));
        CHECK(paths.size() == 2 && paths[1].toString() == QDir::toNativeSeparators(updA));
        // THE CLOBBER GUARD on a scalar: their pin is theirs.
        CHECK(uo.value(QStringLiteral("selected")).toString() == QStringLiteral("D:/mine/user-made.nsp"));
        bool reported = false;
        for (const QString& l : r1.log) if (l.contains(QStringLiteral("selected"))) reported = true;
        CHECK(reported);   // left alone AND said so

        // dlc.json is the ARRAY shape.
        const QJsonArray da = readJson(dlcJson).array();
        CHECK(da.size() == 1);
        CHECK(da.size() == 1 && da[0].toObject().value(QStringLiteral("path")).toString() == QDir::toNativeSeparators(dlc1));

        // The snapshot was taken BEFORE the first write: updates.json existed (the user's file), dlc.json did not.
        const ContentInstall::Record rec = ContentInstall::loadRecord(QStringLiteral("ryujinx"));
        CHECK(rec.titles.contains(titleSw));
        const ContentInstall::TitleRecord tr = rec.titles.value(titleSw);
        CHECK(tr.snapshots.contains(ContentRecipe::slotUpdates()));
        CHECK(tr.snapshots.contains(ContentRecipe::slotDlc()));
        const QJsonObject snapU = tr.snapshots.value(ContentRecipe::slotUpdates()).toObject();
        CHECK(snapU.value(QStringLiteral("present")).toBool() == true);
        CHECK(QByteArray::fromBase64(snapU.value(QStringLiteral("bytes")).toString().toLatin1())
                  .contains("user-made.nsp"));
        CHECK(tr.snapshots.value(ContentRecipe::slotDlc()).toObject().value(QStringLiteral("present")).toBool() == false);
        CHECK(tr.items.size() == 2);

        // ---- IDEMPOTENCE: a second launch installs nothing and rewrites nothing.
        const QByteArray beforeU = readFile(updJson), beforeD = readFile(dlcJson);
        const qint64 snapAt = snapU.value(QStringLiteral("at")).toDouble();
        const ContentInstall::Result r2 = ContentInstall::installForLaunch(
            QStringLiteral("ryujinx"), spec, swGame, root() + QStringLiteral("/ryujinx-bin"), QString(), QString());
        CHECK(r2.installed == 0);
        CHECK(readFile(updJson) == beforeU);
        CHECK(readFile(dlcJson) == beforeD);
        // AND THE SNAPSHOT WAS NOT RETAKEN. It describes the world before this app ever wrote here, not the
        // world before the last launch — so it must still hold ONLY the user's own entry, with no trace of
        // what we installed in between. (A stamp comparison alone would not catch this: two launches a
        // fraction of a second apart carry the same second.)
        const ContentInstall::TitleRecord tr2 = ContentInstall::loadRecord(QStringLiteral("ryujinx")).titles.value(titleSw);
        const QJsonObject snapU2 = tr2.snapshots.value(ContentRecipe::slotUpdates()).toObject();
        CHECK(qint64(snapU2.value(QStringLiteral("at")).toDouble()) == snapAt);
        const QByteArray snapBytes2 = QByteArray::fromBase64(snapU2.value(QStringLiteral("bytes")).toString().toLatin1());
        CHECK(snapBytes2.contains("user-made.nsp"));
        CHECK(!snapBytes2.contains("v65536"));           // nothing we installed leaked into the snapshot
        CHECK(tr2.snapshots.value(ContentRecipe::slotDlc()).toObject().value(QStringLiteral("present")).toBool() == false);
        CHECK(tr2.items.size() == 2);

        // ---- THE RECORD IS THE AUTHORITY, not the destination. Remove the index the app wrote and the next
        // launch leaves it removed: "it is gone" is usually the user having removed it in Ryujinx's own UI,
        // and a frontend that puts it back every launch is the exact failure this feature must not have.
        CHECK(QFile::remove(updJson));
        const ContentInstall::Result rGone = ContentInstall::installForLaunch(
            QStringLiteral("ryujinx"), spec, swGame, root() + QStringLiteral("/ryujinx-bin"), QString(), QString());
        CHECK(rGone.installed == 0);
        CHECK(!QFileInfo::exists(updJson));
        CHECK(writeFile(updJson, beforeU));   // put it back for the assertions below

        // ---- A NEWER package arrives. The "selected" the USER pinned is still not ours to move...
        const QString updB = swDir + QStringLiteral("/updates/Great Game [0100000000010800][v131072].nsp");
        CHECK(writeFile(updB, QByteArray("update B bytes")));
        const ContentInstall::Result r3 = ContentInstall::installForLaunch(
            QStringLiteral("ryujinx"), spec, swGame, root() + QStringLiteral("/ryujinx-bin"), QString(), QString());
        CHECK(r3.installed == 1);
        const QJsonObject uo3 = readJson(updJson).object();
        CHECK(uo3.value(QStringLiteral("paths")).toArray().size() == 3);
        CHECK(uo3.value(QStringLiteral("selected")).toString() == QStringLiteral("D:/mine/user-made.nsp"));
        // A launch that DID install something still must not retake the snapshot — that is the launch where a
        // retake would actually be reachable, and where it would silently replace "the world before this app"
        // with "the world including everything this app already did".
        const QJsonObject snapU3 = ContentInstall::loadRecord(QStringLiteral("ryujinx")).titles.value(titleSw)
                                       .snapshots.value(ContentRecipe::slotUpdates()).toObject();
        const QByteArray snapBytes3 = QByteArray::fromBase64(snapU3.value(QStringLiteral("bytes")).toString().toLatin1());
        CHECK(snapBytes3.contains("user-made.nsp"));
        CHECK(!snapBytes3.contains("v65536"));
        CHECK(!snapBytes3.contains("v131072"));

        // ---- ...but a "selected" THIS APP wrote is ours to advance. Fresh title, no user pin.
        {
            const QString sw2 = root() + QStringLiteral("/roms/switch2");
            const QString g2  = sw2 + QStringLiteral("/Other Game [0100000000020000].nsp");
            CHECK(writeFile(g2, QByteArray("base 2")));
            const QString u1 = sw2 + QStringLiteral("/updates/Other v1.nsp");
            CHECK(writeFile(u1, QByteArray("u one")));
            const ContentInstall::Result a = ContentInstall::installForLaunch(
                QStringLiteral("ryujinx"), spec, g2, root() + QStringLiteral("/ryujinx-bin"), QString(), QString());
            CHECK(a.installed == 1);
            const QString j2 = ryuData + QStringLiteral("/games/0100000000020000/updates.json");
            CHECK(readJson(j2).object().value(QStringLiteral("selected")).toString() == QDir::toNativeSeparators(u1));
            const QString u2 = sw2 + QStringLiteral("/updates/Other v2.nsp");
            CHECK(writeFile(u2, QByteArray("u two")));
            const ContentInstall::Result b = ContentInstall::installForLaunch(
                QStringLiteral("ryujinx"), spec, g2, root() + QStringLiteral("/ryujinx-bin"), QString(), QString());
            CHECK(b.installed == 1);
            CHECK(readJson(j2).object().value(QStringLiteral("selected")).toString() == QDir::toNativeSeparators(u2));
            CHECK(readJson(j2).object().value(QStringLiteral("paths")).toArray().size() == 2);
        }

        // ---- An index we cannot parse is not ours to rewrite.
        {
            const QString sw3 = root() + QStringLiteral("/roms/switch3");
            const QString g3  = sw3 + QStringLiteral("/Third [0100000000030000].nsp");
            CHECK(writeFile(g3, QByteArray("base 3")));
            CHECK(writeFile(sw3 + QStringLiteral("/updates/Third v1.nsp"), QByteArray("u")));
            const QString j3 = ryuData + QStringLiteral("/games/0100000000030000/updates.json");
            CHECK(writeFile(j3, QByteArray("this is not json {{{")));
            const ContentInstall::Result c = ContentInstall::installForLaunch(
                QStringLiteral("ryujinx"), spec, g3, root() + QStringLiteral("/ryujinx-bin"), QString(), QString());
            CHECK(c.installed == 0);
            CHECK(c.leftAlone == 1);
            CHECK(readFile(j3) == QByteArray("this is not json {{{"));
        }
    }

    // ==== 7. Cemu (copyTree) end to end ======================================================================
    const QString cemuData = root() + QStringLiteral("/cemu-data");
    {
        const ExternalEmulator* cem = EmulatorRegistry::byId(QStringLiteral("cemu"));
        CHECK(cem);
        if (!cem) { std::fprintf(stderr, "CONTENTINSTALL: no cemu entry\n"); return 1; }
        const Spec spec = pinnedAt(cem->contentInstall, cemuData);

        const QString wuDir = root() + QStringLiteral("/roms/wiiu2");
        const QString game  = wuDir + QStringLiteral("/Big Game [00050000101C9300].wux");
        CHECK(writeFile(game, QByteArray("base")));
        // A Wii U update is a FOLDER of code/content/meta — one direct child, one package.
        CHECK(writeFile(wuDir + QStringLiteral("/updates/Big Game v16/code/app.rpx"), QByteArray("rpx bytes")));
        CHECK(writeFile(wuDir + QStringLiteral("/updates/Big Game v16/meta/meta.xml"), QByteArray("<meta/>")));
        CHECK(writeFile(wuDir + QStringLiteral("/dlc/Big Game DLC/content/dlc.dat"), QByteArray("dlc bytes")));

        // Something the user installed themselves already sits at one of the update destinations.
        const QString updDest = cemuData + QStringLiteral("/mlc01/usr/title/0005000E/101C9300");
        CHECK(writeFile(updDest + QStringLiteral("/meta/meta.xml"), QByteArray("MY OWN META")));

        const ContentInstall::Result r = ContentInstall::installForLaunch(
            QStringLiteral("cemu"), spec, game, root() + QStringLiteral("/cemu-bin"), QString(), QString());
        CHECK(r.failed == 0);
        // The update landed at the right Wii U path...
        CHECK(readFile(updDest + QStringLiteral("/code/app.rpx")) == QByteArray("rpx bytes"));
        // ...and the file the user put there is BYTE-FOR-BYTE untouched, and was reported.
        CHECK(readFile(updDest + QStringLiteral("/meta/meta.xml")) == QByteArray("MY OWN META"));
        bool said = false;
        for (const QString& l : r.log) if (l.contains(QStringLiteral("left alone"))) said = true;
        CHECK(said);
        // DLC went to the DLC title path, not the update one.
        CHECK(readFile(cemuData + QStringLiteral("/mlc01/usr/title/0005000C/101C9300/content/dlc.dat"))
              == QByteArray("dlc bytes"));

        // The snapshot recorded the destination tree AS IT WAS (the user's meta.xml, and nothing of ours).
        const ContentInstall::TitleRecord tr =
            ContentInstall::loadRecord(QStringLiteral("cemu")).titles.value(QStringLiteral("00050000101C9300"));
        const QJsonObject snap = tr.snapshots.value(ContentRecipe::slotUpdates()).toObject();
        CHECK(snap.value(QStringLiteral("present")).toBool() == true);
        CHECK(snap.value(QStringLiteral("files")).toArray().size() == 1);
        CHECK(snap.value(QStringLiteral("files")).toArray().size() == 1
              && snap.value(QStringLiteral("files")).toArray()[0].toObject().value(QStringLiteral("p")).toString()
                     == QStringLiteral("meta/meta.xml"));

        // OWNERSHIP IS PER-FILE, NOT PER-FOLDER. The record claims the file we laid down and NOT the one the
        // user had there, which is the only thing that stops the NEXT package clobbering theirs.
        CHECK(tr.items.size() == 2);
        for (const ContentInstall::Item& i : tr.items)
            if (i.slot == ContentRecipe::slotUpdates())
            {
                CHECK(i.files.contains(QStringLiteral("code/app.rpx")));
                CHECK(!i.files.contains(QStringLiteral("meta/meta.xml")));
            }

        // IDEMPOTENCE: nothing is copied a second time.
        const ContentInstall::Result r2 = ContentInstall::installForLaunch(
            QStringLiteral("cemu"), spec, game, root() + QStringLiteral("/cemu-bin"), QString(), QString());
        CHECK(r2.installed == 0);
        CHECK(readFile(updDest + QStringLiteral("/meta/meta.xml")) == QByteArray("MY OWN META"));
        // The snapshot still lists ONE file — the user's. A retaken snapshot would also list what we copied.
        const QJsonObject snap2 =
            ContentInstall::loadRecord(QStringLiteral("cemu")).titles.value(QStringLiteral("00050000101C9300"))
                .snapshots.value(ContentRecipe::slotUpdates()).toObject();
        CHECK(snap2.value(QStringLiteral("files")).toArray().size() == 1);

        // The record is the authority here too: delete what we copied and it stays deleted.
        CHECK(QFile::remove(updDest + QStringLiteral("/code/app.rpx")));
        const ContentInstall::Result rGone = ContentInstall::installForLaunch(
            QStringLiteral("cemu"), spec, game, root() + QStringLiteral("/cemu-bin"), QString(), QString());
        CHECK(rGone.installed == 0);
        CHECK(!QFileInfo::exists(updDest + QStringLiteral("/code/app.rpx")));
        CHECK(writeFile(updDest + QStringLiteral("/code/app.rpx"), QByteArray("rpx bytes")));  // put ours back

        // A SECOND, NEWER update package. Our own app.rpx is ours to replace; their meta.xml is still theirs.
        CHECK(writeFile(wuDir + QStringLiteral("/updates/Big Game v32/code/app.rpx"), QByteArray("rpx v32")));
        CHECK(writeFile(wuDir + QStringLiteral("/updates/Big Game v32/meta/meta.xml"), QByteArray("VENDOR META v32")));
        const ContentInstall::Result r3 = ContentInstall::installForLaunch(
            QStringLiteral("cemu"), spec, game, root() + QStringLiteral("/cemu-bin"), QString(), QString());
        CHECK(r3.installed == 1);
        CHECK(readFile(updDest + QStringLiteral("/code/app.rpx")) == QByteArray("rpx v32"));
        CHECK(readFile(updDest + QStringLiteral("/meta/meta.xml")) == QByteArray("MY OWN META"));
        // Same rule on the copyTree side: the snapshot still describes only what was there before us.
        const QJsonObject snap3 =
            ContentInstall::loadRecord(QStringLiteral("cemu")).titles.value(QStringLiteral("00050000101C9300"))
                .snapshots.value(ContentRecipe::slotUpdates()).toObject();
        CHECK(snap3.value(QStringLiteral("files")).toArray().size() == 1);
    }

    // ==== 8. The per-game override decides what installs =====================================================
    {
        const ExternalEmulator* ryu = EmulatorRegistry::byId(QStringLiteral("ryujinx"));
        const Spec spec = pinnedAt(ryu->contentInstall, root() + QStringLiteral("/ryu-ov"));
        const QString dir = root() + QStringLiteral("/roms/ovgame");
        const QString game = dir + QStringLiteral("/Pinned [0100000000040000].nsp");
        CHECK(writeFile(game, QByteArray("base")));
        CHECK(writeFile(dir + QStringLiteral("/updates/Pinned v65536.nsp"), QByteArray("u1")));
        CHECK(writeFile(dir + QStringLiteral("/updates/Pinned v131072.nsp"), QByteArray("u2")));
        CHECK(writeFile(dir + QStringLiteral("/dlc/Pinned DLC.nsp"), QByteArray("d1")));

        // "none" + DLC off: NOTHING is installed, and no index file is even created.
        const ContentInstall::Result none = ContentInstall::installForLaunch(
            QStringLiteral("ryujinx-ov"), spec, game, root() + QStringLiteral("/bin"),
            ContentRecipe::pinNone(), ContentRecipe::dlcOff());
        CHECK(none.installed == 0);
        CHECK(!QFileInfo::exists(root() + QStringLiteral("/ryu-ov/games/0100000000040000/updates.json")));
        CHECK(!QFileInfo::exists(root() + QStringLiteral("/ryu-ov/games/0100000000040000/dlc.json")));

        // A version PIN takes exactly the package that matches, and leaves the other alone.
        const ContentInstall::Result pinned = ContentInstall::installForLaunch(
            QStringLiteral("ryujinx-ov"), spec, game, root() + QStringLiteral("/bin"),
            QStringLiteral("v131072"), ContentRecipe::dlcOff());
        CHECK(pinned.installed == 1);
        const QJsonArray pp = readJson(root() + QStringLiteral("/ryu-ov/games/0100000000040000/updates.json"))
                                  .object().value(QStringLiteral("paths")).toArray();
        CHECK(pp.size() == 1);
        CHECK(pp.size() == 1 && pp[0].toString().contains(QStringLiteral("v131072")));
        CHECK(!QFileInfo::exists(root() + QStringLiteral("/ryu-ov/games/0100000000040000/dlc.json")));  // DLC still off

        // DLC back on installs it, without disturbing the pin.
        const ContentInstall::Result dlcOn = ContentInstall::installForLaunch(
            QStringLiteral("ryujinx-ov"), spec, game, root() + QStringLiteral("/bin"),
            QStringLiteral("v131072"), QString());
        CHECK(dlcOn.installed == 1);
        CHECK(readJson(root() + QStringLiteral("/ryu-ov/games/0100000000040000/dlc.json")).array().size() == 1);
    }

    // ==== 9. A game whose title id cannot be derived is REPORTED, never guessed ===============================
    {
        const ExternalEmulator* ryu = EmulatorRegistry::byId(QStringLiteral("ryujinx"));
        const Spec spec = pinnedAt(ryu->contentInstall, root() + QStringLiteral("/ryu-noid"));
        const QString dir = root() + QStringLiteral("/roms/noid");
        const QString game = dir + QStringLiteral("/Anonymous.nsp");
        CHECK(writeFile(game, QByteArray("base")));
        CHECK(writeFile(dir + QStringLiteral("/updates/u.nsp"), QByteArray("u")));
        const ContentInstall::Result r = ContentInstall::installForLaunch(
            QStringLiteral("ryujinx-noid"), spec, game, root() + QStringLiteral("/bin"), QString(), QString());
        CHECK(r.installed == 0);
        CHECK(r.failed == 0);                       // not a failure — a skip with a reason
        bool said = false;
        for (const QString& l : r.log) if (l.contains(QStringLiteral("title id"))) said = true;
        CHECK(said);
        // Nothing was written into a folder literally named {titleId}.
        CHECK(!QDir(root() + QStringLiteral("/ryu-noid/games/{titleId}")).exists());
    }

    // ==== 10. RPCS3: the recipe DESCRIBES the code that already does this ====================================
    {
        const ExternalEmulator* rp3 = EmulatorRegistry::byId(QStringLiteral("rpcs3"));
        CHECK(rp3);
        if (rp3)
        {
            const QStringList argv = ContentRecipe::cliArgv(rp3->contentInstall.updates,
                                                            QStringLiteral("C:\\pkgs\\My Game Update.pkg"));
            CHECK(argv.size() == 3);
            CHECK(argv.size() == 3 && argv[0] == QStringLiteral("--headless"));
            CHECK(argv.size() == 3 && argv[1] == QStringLiteral("--installpkg"));
            // {file} substituted AFTER the shell-style cut (#237), so a spaced path is ONE argument and
            // carries no quote characters.
            CHECK(argv.size() == 3 && argv[2] == QStringLiteral("C:\\pkgs\\My Game Update.pkg"));

            // And the code really does spawn that. Read the source, so the description cannot rot into a lie.
            QFile em(QStringLiteral(EB_CONTENTINSTALL_SRC_DIR) + QStringLiteral("/core/EmulatorManager.cpp"));
            CHECK(em.open(QIODevice::ReadOnly));
            if (em.isOpen())
            {
                const QByteArray src = em.readAll();
                em.close();
                CHECK(src.contains("QStringLiteral(\"--headless\"), QStringLiteral(\"--installpkg\")"));
            }
        }
    }

    // ==== 11. The recorded hash is #97's hash ================================================================
    {
        const QString f = root() + QStringLiteral("/hash/sample.bin");
        QByteArray bytes;
        for (int i = 0; i < 4096; ++i) bytes.append(char(i * 7 + 3));
        CHECK(writeFile(f, bytes));
        CHECK(ContentInstall::fileSha1(f) == HashVerify::hashBytes(bytes).sha1);
        CHECK(!ContentInstall::fileSha1(f).isEmpty());
        CHECK(ContentInstall::fileSha1(root() + QStringLiteral("/hash/missing.bin")).isEmpty());
        // A folder package gets a stable identity too, and it changes when its contents do.
        const QString d = root() + QStringLiteral("/hash/tree");
        CHECK(writeFile(d + QStringLiteral("/a/one.bin"), QByteArray("one")));
        CHECK(writeFile(d + QStringLiteral("/b/two.bin"), QByteArray("two")));
        const QString h1 = ContentInstall::treeSha1(d);
        CHECK(!h1.isEmpty());
        CHECK(ContentInstall::treeSha1(d) == h1);                    // deterministic
        CHECK(writeFile(d + QStringLiteral("/b/two.bin"), QByteArray("TWO")));
        CHECK(ContentInstall::treeSha1(d) != h1);
    }

    // ==== 12. The record's own JSON round-trip and its two pure queries ======================================
    {
        ContentInstall::Record rec;
        ContentInstall::TitleRecord t;
        ContentInstall::Item it;
        it.slot = ContentRecipe::slotUpdates(); it.name = QStringLiteral("u.nsp");
        it.sha1 = QStringLiteral("abc123"); it.dest = QStringLiteral("C:/emu/pkgs/u.nsp");
        it.size = 42; it.mtime = 1700000000; it.at = 1700000001;
        t.items.push_back(it);
        t.snapshots.insert(ContentRecipe::slotUpdates(), QJsonObject{ { QStringLiteral("present"), false } });
        rec.titles.insert(QStringLiteral("0100000000010000"), t);
        const ContentInstall::Record back = ContentInstall::fromJson(ContentInstall::toJson(rec));
        CHECK(back.titles.size() == 1);
        const ContentInstall::TitleRecord bt = back.titles.value(QStringLiteral("0100000000010000"));
        CHECK(bt.items.size() == 1 && bt.items[0].sha1 == QStringLiteral("abc123") && bt.items[0].size == 42);
        CHECK(bt.snapshots.contains(ContentRecipe::slotUpdates()));

        // THE IDEMPOTENCE GUARD, as a pure decision: the stamp is sufficient, the hash is the authority, and a
        // package that is neither is new.
        CHECK(ContentInstall::alreadyInstalled(t, ContentRecipe::slotUpdates(), QStringLiteral("u.nsp"), 42, 1700000000, QString()));
        CHECK(ContentInstall::alreadyInstalled(t, ContentRecipe::slotUpdates(), QStringLiteral("renamed.nsp"), 99, 1, QStringLiteral("ABC123")));
        CHECK(!ContentInstall::alreadyInstalled(t, ContentRecipe::slotUpdates(), QStringLiteral("u.nsp"), 43, 1700000000, QStringLiteral("ddd")));
        CHECK(!ContentInstall::alreadyInstalled(t, ContentRecipe::slotDlc(), QStringLiteral("u.nsp"), 42, 1700000000, QString()));
        #ifdef Q_OS_WIN
        // Same rule asked of the install record: the backslash twin of a path we installed is ours.
        CHECK(ContentInstall::weInstalled(t, QStringLiteral("C:\\emu\\pkgs\\u.nsp")));
        #else
        // On Unix that string names a different file, so the record does not claim it.
        const QString uBackslash = QStringLiteral("C:\emu\pkgs\u.nsp");
        CHECK(!ContentInstall::weInstalled(t, uBackslash));
        #endif
        CHECK(!ContentInstall::weInstalled(t, QStringLiteral("D:/mine/u.nsp")));
        CHECK(ContentInstall::ourPaths(t, ContentRecipe::slotUpdates()).size() == 1);
    }

    // ==== 13. mergeRegistry, as a pure function =============================================================
    {
        const QJsonObject entry = QJsonDocument::fromJson(
            R"({"paths":["C:/new.nsp"],"selected":"C:/new.nsp"})").object();
        // Absent file -> created.
        const ContentInstall::MergeResult a = ContentInstall::mergeRegistry(QJsonDocument(), QStringLiteral("object"), entry, {});
        CHECK(a.changed);
        CHECK(a.doc.object().value(QStringLiteral("paths")).toArray().size() == 1);
        // Second merge of the same entry -> no change (the index already names it).
        const ContentInstall::MergeResult b = ContentInstall::mergeRegistry(a.doc, QStringLiteral("object"), entry, {});
        CHECK(!b.changed);
        // An unrelated key the emulator wrote is preserved.
        QJsonObject withExtra = a.doc.object();
        withExtra.insert(QStringLiteral("someEmulatorField"), 7);
        const ContentInstall::MergeResult c = ContentInstall::mergeRegistry(QJsonDocument(withExtra), QStringLiteral("object"), entry, {});
        CHECK(c.doc.object().value(QStringLiteral("someEmulatorField")).toInt() == 7);
        // Array container: duplicate suppression by the path the entry names.
        const QJsonObject dentry = QJsonDocument::fromJson(R"({"path":"C:/d.nsp","dlc_nca_list":[]})").object();
        const ContentInstall::MergeResult d1 = ContentInstall::mergeRegistry(QJsonDocument(), QStringLiteral("array"), dentry, {});
        CHECK(d1.changed && d1.doc.array().size() == 1);
        const ContentInstall::MergeResult d2 = ContentInstall::mergeRegistry(d1.doc, QStringLiteral("array"), dentry, {});
        CHECK(!d2.changed && d2.doc.array().size() == 1);
        // jsonNamesPath finds a path at any depth, and normalises separators.
        #ifdef Q_OS_WIN
        // ...and of the registry search: on Windows the backslash spelling is the same name.
        CHECK(ContentInstall::jsonNamesPath(dentry, QStringLiteral("C:\\d.nsp")));
        #else
        // On Unix it is a DIFFERENT name — a backslash is a legal character in a file name there — so it
        // is not found, and that is the answer we want rather than two files comparing equal.
        const QString dBackslash = QStringLiteral("C:\d.nsp");
        CHECK(!ContentInstall::jsonNamesPath(dentry, dBackslash));
        #endif
        CHECK(!ContentInstall::jsonNamesPath(dentry, QStringLiteral("C:/other.nsp")));
    }

    // ==== 14. An emulator that declares NOTHING for a slot says so once, and never fails a launch ============
    {
        Spec halfSpec;
        halfSpec.dataDirs = QStringList{ root() + QStringLiteral("/half") };
        halfSpec.updates = ContentRecipe::recipeFromJson(QJsonDocument::fromJson(
            R"({"kind":"copyTree","dest":"{data}/out"})").object());
        // .dlc left empty on purpose.
        const QString dir = root() + QStringLiteral("/roms/half");
        const QString game = dir + QStringLiteral("/Half [0100000000050000].nsp");
        CHECK(writeFile(game, QByteArray("base")));
        CHECK(writeFile(dir + QStringLiteral("/updates/u.bin"), QByteArray("u")));
        CHECK(writeFile(dir + QStringLiteral("/dlc/d1.bin"), QByteArray("d1")));
        CHECK(writeFile(dir + QStringLiteral("/dlc/d2.bin"), QByteArray("d2")));
        const ContentInstall::Result r = ContentInstall::installForLaunch(
            QStringLiteral("half"), halfSpec, game, root() + QStringLiteral("/bin"), QString(), QString());
        CHECK(r.installed == 1);
        CHECK(readFile(root() + QStringLiteral("/half/out/u.bin")) == QByteArray("u"));
        int noRecipeLines = 0;
        for (const QString& l : r.log) if (l.contains(QStringLiteral("no \"dlc\" recipe"))) ++noRecipeLines;
        CHECK(noRecipeLines == 1);   // TWO dlc files, ONE line

        // An empty spec, an empty game path and an emulator id nobody registered are all inert, not crashes.
        CHECK(ContentInstall::installForLaunch(QStringLiteral("x"), Spec{}, game, root(), QString(), QString()).items.isEmpty());
        CHECK(ContentInstall::installForLaunch(QString(), halfSpec, game, root(), QString(), QString()).items.isEmpty());
        CHECK(ContentInstall::installForLaunch(QStringLiteral("x"), halfSpec, QString(), root(), QString(), QString()).items.isEmpty());
    }

    // ==== 15. The library scan does not turn a sidecar package into a game ===================================
    {
        // A DIRECTORY segment named updates/dlc, at any depth, means "this is content for the game beside it".
        CHECK(RomRouting::underContentSidecar(QStringLiteral("updates/Great Game v65536.nsp")));
        CHECK(RomRouting::underContentSidecar(QStringLiteral("dlc/Great Game DLC 1.nsp")));
        CHECK(RomRouting::underContentSidecar(QStringLiteral("Wii U/updates/Big Game v16/code/app.rpx")));
        CHECK(RomRouting::underContentSidecar(QStringLiteral("DLC/thing.nsp")));       // case-insensitive
        CHECK(RomRouting::underContentSidecar(QStringLiteral("Updates/thing.nsp")));
        // ...and a whole SEGMENT only, so a real game is never hidden by a name that merely contains the word.
        CHECK(!RomRouting::underContentSidecar(QStringLiteral("Updates.nsp")));
        CHECK(!RomRouting::underContentSidecar(QStringLiteral("dlcpack/Some Game.nsp")));
        CHECK(!RomRouting::underContentSidecar(QStringLiteral("My DLC Collection/Some Game.nsp")));
        CHECK(!RomRouting::underContentSidecar(QStringLiteral("Great Game.nsp")));
        CHECK(!RomRouting::underContentSidecar(QString()));
        // The extension filter still decides everything else, unchanged.
        CHECK(RomRouting::acceptUnderSystemFolder(QStringLiteral("nsp")));
        CHECK(!RomRouting::acceptUnderSystemFolder(QStringLiteral("srm")));
    }

    if (failures == 0) std::printf("CONTENTINSTALL-OK\n");
    else               std::fprintf(stderr, "CONTENTINSTALL: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
