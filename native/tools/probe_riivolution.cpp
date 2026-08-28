// Headless test for RiivolutionPatch (pure) and DiscOverlay (against a real, temporary directory tree).
//
// The XML fixtures here are cut down from the real document shipped by the mod this work targets, so the
// element shapes, attribute names and the locale ALIASING (seven disc folders sourced from one external
// folder) are the measured article rather than an invention.
//
// The extracted-disc shapes cases 7-9 build are measured too: DolphinTool writes a Wii disc as
// <root>/DATA/files and a GameCube disc as <root>/files, both observed on real discs. What is NOT covered
// here is composing an image back: no disc is read or written and DolphinTool is never run, so a green run
// says the overlay landed correctly in a directory, not that a composed disc boots.
//
// Mutation targets: drop the <memory> refusal and case 3 passes when it must fail; drop the multi-choice
// refusal and case 4 passes; drop the containment check in DiscOverlay and case 7 fails on BOTH of its
// assertions (measured: two failures with the check forced true, one without the escape path fixed);
// move DiscCompose's parse after the extract and case 11 fails.
//
// Prints RIIVOLUTION-OK on success; RIIVOLUTION-FAIL (nonzero exit) on any miss.
#include "../src/core/DiscCompose.h"
#include "../src/core/DiscOverlay.h"
#include "../src/core/RiivolutionPatch.h"
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

static QByteArray realShapeXml()
{
    return QByteArray(
        "<wiidisc version=\"1\" root=\"\">\n"
        "  <id game=\"SB4\"><region type=\"P\"/></id>\n"
        "  <options><section name=\"Super Mario Gravity\">\n"
        "    <option name=\"Demo\"><choice name=\"Enabled\"><patch id=\"smgra\"/></choice></option>\n"
        "  </section></options>\n"
        "  <patch id=\"smgra\" root=\"/SuperMarioGravity_Demo\">\n"
        "    <savegame external=\"SaveGame/{$__gameid}\" clone=\"false\"/>\n"
        "    <folder disc=\"/StageData\" external=\"StageData\" create=\"true\"/>\n"
        "    <folder disc=\"/LocalizeData/EuFrench\" external=\"LocalizeData/EuEnglish\" create=\"true\"/>\n"
        "  </patch>\n"
        "</wiidisc>\n");
}

int main()
{
    // 1. The measured shape parses, and the patch root is carried through.
    {
        const auto p = RiivolutionPatch::parse(realShapeXml());
        check(p.ok, "1: the real document shape parses");
        check(p.refusal.isEmpty(), "1: a parse that succeeded states no refusal");
        check(p.root == QStringLiteral("/SuperMarioGravity_Demo"), "1: patch root is carried through");
        check(p.ops.size() == 2, "1: both folder mappings survive");
    }

    // 2. Locale ALIASING: two different disc paths may share one external path. A map keyed by external
    //    path would silently collapse these, and six of the mod's twelve mappings would vanish.
    {
        const auto p = RiivolutionPatch::parse(realShapeXml());
        check(p.ops.size() == 2, "2: aliased mappings are not collapsed");
        check(p.ops[1].discPath == QStringLiteral("/LocalizeData/EuFrench"), "2: alias disc path kept");
        check(p.ops[1].externalPath == QStringLiteral("LocalizeData/EuEnglish"), "2: alias source kept");
    }

    // 3. <memory> is REFUSED, not ignored. A RAM patch cannot exist in a composed disc.
    {
        const auto p = RiivolutionPatch::parse(QByteArray(
            "<wiidisc version=\"1\"><options><section name=\"s\"><option name=\"o\">"
            "<choice name=\"c\"><patch id=\"p\"/></choice></option></section></options>"
            "<patch id=\"p\" root=\"/m\"><memory offset=\"0x80000000\" value=\"60000000\"/></patch></wiidisc>"));
        check(!p.ok, "3: a document with <memory> is refused");
        check(p.refusal.contains(QStringLiteral("memory")), "3: the refusal names the element");
    }

    // 4. A document that offers a real choice is refused: there is no UI to choose with, and choosing
    //    silently installs a different mod from the one the user thinks they picked.
    {
        const auto p = RiivolutionPatch::parse(QByteArray(
            "<wiidisc version=\"1\"><options><section name=\"s\"><option name=\"o\">"
            "<choice name=\"a\"><patch id=\"p\"/></choice>"
            "<choice name=\"b\"><patch id=\"q\"/></choice></option></section></options>"
            "<patch id=\"p\" root=\"/m\"/><patch id=\"q\" root=\"/n\"/></wiidisc>"));
        check(!p.ok, "4: a multi-choice document is refused");
        check(p.refusal.contains(QStringLiteral("choice")), "4: the refusal says a choice was needed");
    }

    // 5. <savegame> is ignored, and the caller is TOLD it was ignored.
    {
        const auto p = RiivolutionPatch::parse(realShapeXml());
        check(p.ok, "5: <savegame> does not fail the parse");
        check(p.savegameIgnored, "5: the caller is told <savegame> was dropped");
    }

    // 6. Malformed input is a refusal, never a crash and never a silent empty success.
    {
        const auto p = RiivolutionPatch::parse(QByteArray("<wiidisc><patch"));
        check(!p.ok, "6: malformed XML is refused");
        check(!p.refusal.isEmpty(), "6: a refusal always states a reason");
    }

    // 7. CONTAINMENT: a disc path that climbs out of the tree is refused, and nothing is written outside it.
    {
        QTemporaryDir tmp;
        const QString discRoot = tmp.filePath(QStringLiteral("disc"));
        const QString modRoot  = tmp.filePath(QStringLiteral("mod"));
        QDir().mkpath(discRoot + QStringLiteral("/DATA/files"));
        QDir().mkpath(modRoot + QStringLiteral("/m/Evil"));
        QFile f(modRoot + QStringLiteral("/m/Evil/x.bin"));
        f.open(QIODevice::WriteOnly); f.write("x"); f.close();

        RiivolutionPatch::Parsed p;
        p.ok = true;
        p.root = QStringLiteral("/m");
        RiivolutionPatch::Op op;
        op.kind = RiivolutionPatch::Op::Folder;
        op.discPath = QStringLiteral("/../../escaped");
        op.externalPath = QStringLiteral("Evil");
        op.create = true;
        p.ops.append(op);

        const auto r = DiscOverlay::apply(discRoot, modRoot, p);
        check(!r.ok, "7: a disc path escaping the tree is refused");
        check(!QFile::exists(discRoot + QStringLiteral("/escaped/x.bin")), "7: nothing was written outside");
    }

    // 8. The Wii layout is chosen by what the extraction actually produced, not assumed.
    {
        QTemporaryDir tmp;
        QDir().mkpath(tmp.filePath(QStringLiteral("wii/DATA/files")));
        QDir().mkpath(tmp.filePath(QStringLiteral("gc/files")));
        check(DiscOverlay::discFilesRoot(tmp.filePath(QStringLiteral("wii")))
                  .endsWith(QStringLiteral("DATA/files")), "8: a Wii tree resolves to DATA/files");
        check(DiscOverlay::discFilesRoot(tmp.filePath(QStringLiteral("gc")))
                  .endsWith(QStringLiteral("files")), "8: a GameCube tree resolves to files");
    }

    // 9. A real overlay lands where the disc path says, and REPLACES an existing file -- the mod's whole
    //    purpose. A copy that refused to overwrite would leave the stock game with extra files beside it.
    {
        QTemporaryDir tmp;
        const QString discRoot = tmp.filePath(QStringLiteral("disc"));
        const QString modRoot  = tmp.filePath(QStringLiteral("mod"));
        QDir().mkpath(discRoot + QStringLiteral("/DATA/files/LayoutData"));
        QDir().mkpath(modRoot + QStringLiteral("/m/LayoutData"));
        QFile stock(discRoot + QStringLiteral("/DATA/files/LayoutData/TitleLogo.arc"));
        stock.open(QIODevice::WriteOnly); stock.write("STOCK"); stock.close();
        QFile mod(modRoot + QStringLiteral("/m/LayoutData/TitleLogo.arc"));
        mod.open(QIODevice::WriteOnly); mod.write("MODDED"); mod.close();

        RiivolutionPatch::Parsed p;
        p.ok = true;
        p.root = QStringLiteral("/m");
        RiivolutionPatch::Op op;
        op.kind = RiivolutionPatch::Op::Folder;
        op.discPath = QStringLiteral("/LayoutData");
        op.externalPath = QStringLiteral("LayoutData");
        op.create = true;
        p.ops.append(op);

        const auto r = DiscOverlay::apply(discRoot, modRoot, p);
        check(r.ok, "9: a well-formed overlay applies");
        check(r.filesWritten == 1, "9: one file was written");
        QFile back(discRoot + QStringLiteral("/DATA/files/LayoutData/TitleLogo.arc"));
        back.open(QIODevice::ReadOnly);
        check(back.readAll() == QByteArray("MODDED"), "9: the mod's file REPLACED the stock one");
    }

    // 10. The space estimate scales with the disc, and always demands more than the disc itself -- an
    //     estimate that did not could pass and then run out mid-compose, which is the failure this exists
    //     to prevent.
    {
        const qint64 fourGiB = 4LL * 1024 * 1024 * 1024;
        check(DiscCompose::requiredFreeBytes(fourGiB) > fourGiB, "10: the estimate exceeds the disc itself");
        check(DiscCompose::requiredFreeBytes(fourGiB) > DiscCompose::requiredFreeBytes(fourGiB / 4),
              "10: the estimate scales with disc size");
    }

    // 11. A refused document composes NOTHING, and says why in the words the parser used. A tool that ran
    //     anyway would produce a disc missing the very patches that caused the refusal.
    {
        QTemporaryDir tmp;
        const QByteArray memoryXml(
            "<wiidisc version=\"1\"><options><section name=\"s\"><option name=\"o\">"
            "<choice name=\"c\"><patch id=\"p\"/></choice></option></section></options>"
            "<patch id=\"p\" root=\"/m\"><memory offset=\"0\" value=\"0\"/></patch></wiidisc>");

        const auto o = DiscCompose::composePatchedDisc(
            QStringLiteral("no-such-tool.exe"), QStringLiteral("no-such.iso"), tmp.path(), memoryXml,
            tmp.filePath(QStringLiteral("out.rvz")), tmp.path());
        check(!o.ok, "11: a refused document does not compose");
        check(o.error.contains(QStringLiteral("memory")), "11: the refusal reaches the caller intact");
        check(!QFile::exists(tmp.filePath(QStringLiteral("out.rvz"))), "11: no output file was left behind");

        // The three checks above prove the error TEXT is the parser's. They do NOT prove the disc was
        // never extracted, which is the property the ordering exists to protect, and the difference is
        // measured rather than hypothetical: moving the parse after the extract while still returning
        // its refusal unconditionally leaves all three passing, and the whole suite green, while the
        // disc really is extracted first.
        //
        // So this second call discriminates on WHICH failure comes back. The staging parent is a path
        // inside a regular FILE: no volume can be measured for it and no working folder can be made in
        // it, so any code that reaches the space check or the mkpath must report one of those instead.
        // Correct code never gets that far -- it refuses at the parse, before both -- so the message
        // still names <memory>. A path inside a file is used rather than an absent drive letter
        // because it fails the same way on every platform instead of depending on which letters this
        // machine happens to have mounted.
        QFile blocker(tmp.filePath(QStringLiteral("blocker")));
        blocker.open(QIODevice::WriteOnly); blocker.write("x"); blocker.close();
        const auto o2 = DiscCompose::composePatchedDisc(
            QStringLiteral("no-such-tool.exe"), QStringLiteral("no-such.iso"), tmp.path(), memoryXml,
            tmp.filePath(QStringLiteral("out2.rvz")),
            tmp.filePath(QStringLiteral("blocker/staging")));
        check(!o2.ok, "11: an unusable staging parent composes nothing either");
        check(o2.error.contains(QStringLiteral("memory")),
              "11: the parse refuses BEFORE any staging work is attempted");
    }

    if (failures == 0) { std::printf("RIIVOLUTION-OK\n"); return 0; }
    std::fprintf(stderr, "RIIVOLUTION-FAIL (%d)\n", failures);
    return 1;
}
