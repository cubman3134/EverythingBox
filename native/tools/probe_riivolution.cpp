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
// Mutation targets, each measured by running the mutant rather than reasoned about:
//   drop the <memory> refusal              -> case 3 fails
//   drop the multi-choice refusal          -> case 4 fails
//   force the DISC-side containment true   -> cases 7 and 15 fail on all of their assertions
//   force the EXTERNAL-side containment    -> case 13 fails on both (case 7 stays GREEN: separate halves)
//
//   The disc side has TWO sites, and which case pins which was measured one at a time rather than assumed:
//     delete apply()'s OUTER `|| !contained(filesRoot, dst)`   -> only case 15 fails. Case 7 stays GREEN,
//                                             because its op is a FOLDER and the per-file check inside the
//                                             folder loop catches the escape instead -- which is exactly
//                                             what masked this deletion until case 15 existed
//     delete the INNER per-file `contained(filesRoot, target)` -> nothing fails. Unreachable WHILE the
//                                             outer term stands, so it is not dead code: it is the mask
//     delete BOTH                                              -> cases 7 AND 15 fail (5 assertions). Case 7
//                                             only reacts once its mask is gone too, which is the whole
//                                             reason deleting the outer term alone looked harmless
//   hardcode discFilesRoot to DATA/files   -> case 8 fails. It did NOT before this case was rewritten: the
//                                             old "ends with files" assertion ran GREEN against this exact
//                                             mutant, since ".../gc/DATA/files" ends with "files" as well
//   delete apply()'s `if (!parsed.ok)`     -> case 12 fails on all three
//   gut the Op::File branch to `continue;` -> case 14 fails on TWO of its three; "a single-file op applies"
//                                             still passes, because skipping the copy still returns ok --
//                                             which is why 14 also counts the write and reads the bytes back
//   move DiscCompose's parse after extract -> case 11 fails
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

        // The GameCube half asserts the ABSENCE of DATA rather than "ends with files", because
        // ".../gc/DATA/files" ends with "files" too -- measured, the weaker form left discFilesRoot
        // hardcoded to the Wii path GREEN. The code is shape-driven; only this form establishes it.
        // Only the part BELOW the disc root is inspected: the enclosing temporary path is the machine's,
        // not ours, and a %TEMP% that happened to contain "DATA" would otherwise fail this for no reason.
        const QString gcRoot = tmp.filePath(QStringLiteral("gc"));
        check(!DiscOverlay::discFilesRoot(gcRoot).mid(gcRoot.size()).contains(QStringLiteral("DATA")),
              "8: a GameCube tree resolves to files, NOT to the Wii DATA path");
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

    // 12. A document the PARSER refused is refused by the overlay too, and copies NOTHING. The op below is
    //     perfectly valid and its source file really exists, so this discriminates: measured, deleting the
    //     `if (!parsed.ok)` guard applies the op, the file lands, and all three assertions here go red.
    //     Without this case the overlay happily applied a document the parser had already declined, which
    //     is exactly the "builds, boots, and is subtly wrong" disc RiivolutionPatch.h refuses to make.
    {
        QTemporaryDir tmp;
        const QString discRoot = tmp.filePath(QStringLiteral("disc"));
        const QString modRoot  = tmp.filePath(QStringLiteral("mod"));
        QDir().mkpath(discRoot + QStringLiteral("/DATA/files"));
        QDir().mkpath(modRoot + QStringLiteral("/m/StageData"));
        QFile good(modRoot + QStringLiteral("/m/StageData/course.arc"));
        good.open(QIODevice::WriteOnly); good.write("MODDED"); good.close();

        RiivolutionPatch::Parsed p;
        p.ok = false;
        p.refusal = QStringLiteral("this mod needs a <memory> patch, which a composed disc cannot hold");
        p.root = QStringLiteral("/m");
        RiivolutionPatch::Op op;
        op.kind = RiivolutionPatch::Op::Folder;
        op.discPath = QStringLiteral("/StageData");
        op.externalPath = QStringLiteral("StageData");
        op.create = true;
        p.ops.append(op);

        const auto r = DiscOverlay::apply(discRoot, modRoot, p);
        check(!r.ok, "12: a document the parser refused is not applied");
        check(r.error == p.refusal, "12: the parser's own refusal reaches the caller unchanged");
        check(!QFile::exists(discRoot + QStringLiteral("/DATA/files/StageData/course.arc")),
              "12: a refused document copies NOTHING, though its op was applicable");
    }

    // 13. CONTAINMENT, SOURCE end. Case 7 escapes on the disc path; this escapes on the external path, and
    //     they are separate halves of one condition. Measured: deleting `!contained(modRoot, src) ||` leaves
    //     case 7 and the rest of the suite green while secret.bin really is copied in from outside the mod
    //     tree -- so before this case the comment's claim that "both ends are checked" was untested on one
    //     of the two ends.
    {
        QTemporaryDir tmp;
        const QString discRoot = tmp.filePath(QStringLiteral("disc"));
        const QString modRoot  = tmp.filePath(QStringLiteral("mod"));
        QDir().mkpath(discRoot + QStringLiteral("/DATA/files"));
        QDir().mkpath(modRoot + QStringLiteral("/m"));
        QDir().mkpath(tmp.filePath(QStringLiteral("outside")));
        QFile secret(tmp.filePath(QStringLiteral("outside/secret.bin")));
        secret.open(QIODevice::WriteOnly); secret.write("SECRET"); secret.close();

        RiivolutionPatch::Parsed p;
        p.ok = true;
        p.root = QStringLiteral("/m");
        RiivolutionPatch::Op op;
        op.kind = RiivolutionPatch::Op::Folder;
        // The DISC path is innocent, so only the source end can refuse this: <mod>/m/../../outside is
        // tmp/outside, a sibling of the mod tree entirely.
        op.discPath = QStringLiteral("/Stolen");
        op.externalPath = QStringLiteral("../../outside");
        op.create = true;
        p.ops.append(op);

        const auto r = DiscOverlay::apply(discRoot, modRoot, p);
        check(!r.ok, "13: a source path climbing out of the mod tree is refused");
        check(!QFile::exists(discRoot + QStringLiteral("/DATA/files/Stolen/secret.bin")),
              "13: nothing was pulled in from outside the mod tree");
    }

    // 14. A single-FILE op. Every other overlay case here maps a FOLDER; measured, gutting the file branch
    //     to a bare `continue;` left the whole suite green, so one of the two op kinds was uncovered.
    {
        QTemporaryDir tmp;
        const QString discRoot = tmp.filePath(QStringLiteral("disc"));
        const QString modRoot  = tmp.filePath(QStringLiteral("mod"));
        QDir().mkpath(discRoot + QStringLiteral("/DATA/files/LayoutData"));
        QDir().mkpath(modRoot + QStringLiteral("/m"));
        QFile one(modRoot + QStringLiteral("/m/Common.arc"));
        one.open(QIODevice::WriteOnly); one.write("MODDED-FILE"); one.close();

        RiivolutionPatch::Parsed p;
        p.ok = true;
        p.root = QStringLiteral("/m");
        RiivolutionPatch::Op op;
        op.kind = RiivolutionPatch::Op::File;
        op.discPath = QStringLiteral("/LayoutData/Common.arc");
        op.externalPath = QStringLiteral("Common.arc");
        p.ops.append(op);

        const auto r = DiscOverlay::apply(discRoot, modRoot, p);
        check(r.ok, "14: a single-file op applies");
        check(r.filesWritten == 1, "14: exactly one file was written");
        QFile back(discRoot + QStringLiteral("/DATA/files/LayoutData/Common.arc"));
        back.open(QIODevice::ReadOnly);
        check(back.readAll() == QByteArray("MODDED-FILE"),
              "14: the file landed at the mapped disc path");
    }

    // 15. CONTAINMENT, disc end, for a FILE op. Case 7 already escapes on the disc path, but it is a FOLDER
    //     op, and the per-file `contained(filesRoot, target)` check INSIDE the folder loop catches that
    //     escape on its own -- so case 7 stays green with the outer `|| !contained(filesRoot, dst)` term
    //     deleted, and pinned only the inner site. A File op has no such loop, so for it the outer term is
    //     the only thing between an escaping discPath and the filesystem, and case 14's File op uses an
    //     innocent disc path. Measured: with the outer term deleted and this case absent, the whole suite
    //     ran green while the payload really was written to <discRoot>/escaped.arc -- one directory above
    //     DATA, outside the disc's files entirely. The path asserted below is that measured landing site,
    //     not a guess: filesRoot is <discRoot>/DATA/files, so "/../../escaped.arc" cleans to <discRoot>.
    //
    //     The source end cannot be what refuses this: externalPath is an ordinary name inside the mod tree.
    {
        QTemporaryDir tmp;
        const QString discRoot = tmp.filePath(QStringLiteral("disc"));
        const QString modRoot  = tmp.filePath(QStringLiteral("mod"));
        QDir().mkpath(discRoot + QStringLiteral("/DATA/files"));
        QDir().mkpath(modRoot + QStringLiteral("/m"));
        QFile payload(modRoot + QStringLiteral("/m/Payload.arc"));
        payload.open(QIODevice::WriteOnly); payload.write("PAYLOAD"); payload.close();

        RiivolutionPatch::Parsed p;
        p.ok = true;
        p.root = QStringLiteral("/m");
        RiivolutionPatch::Op op;
        op.kind = RiivolutionPatch::Op::File;
        op.discPath = QStringLiteral("/../../escaped.arc");
        op.externalPath = QStringLiteral("Payload.arc");
        p.ops.append(op);

        const auto r = DiscOverlay::apply(discRoot, modRoot, p);
        check(!r.ok, "15: a FILE op whose disc path escapes the tree is refused");
        check(r.filesWritten == 0, "15: a refused file op writes nothing at all");
        check(!QFile::exists(discRoot + QStringLiteral("/escaped.arc")),
              "15: the payload did not land above the disc's files root");
    }

    if (failures == 0) { std::printf("RIIVOLUTION-OK\n"); return 0; }
    std::fprintf(stderr, "RIIVOLUTION-FAIL (%d)\n", failures);
    return 1;
}
