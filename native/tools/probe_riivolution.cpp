// Headless test for RiivolutionPatch (pure) and DiscOverlay (against a real, temporary directory tree).
//
// The XML fixtures here are cut down from the real document shipped by the mod this work targets, so the
// element shapes, attribute names and the locale ALIASING (seven disc folders sourced from one external
// folder) are the measured article rather than an invention.
//
// The extracted-disc shapes cases 7-9 build are measured too: DolphinTool writes a Wii disc as
// <root>/DATA/files and a GameCube disc as <root>/files, both observed on real discs. What is NOT covered
// here is composing a REAL image: no disc image is read or written and DolphinTool itself is never run, so
// a green run says the overlay landed correctly in a directory, not that a composed disc boots.
//
// Cases 16 and 17 DO drive composePatchedDisc past its parse, which nothing did before them. They do it by
// re-invoking THIS BINARY as the disc tool (see stubTool below): a real child process, started and killed
// by the real runTool code, rather than a stub that answers for it. That covers the tool-wait loop, the
// kill-on-cancel, and the cleanup guarantees -- not the disc format, which the stub knows nothing about.
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
//   The cancellation mutants, each run rather than reasoned about:
//     force DiscCompose's aborted() false     -> 6 assertions fail: case 16's "stops instead of running to
//                                             its ceiling", "acted on, not waited out", "does not report
//                                             success", "says it was cancelled", "leaves the
//                                             already-installed image untouched", and case 17's "the disc
//                                             tool was never started at all". Note which do NOT fail: the
//                                             two cleanup assertions stay green, because the build then
//                                             SUCCEEDS and a success removes the staging tree and renames
//                                             the ".part" away. Cleanup assertions alone cannot catch a
//                                             missing interruption check
//     skip the staging removal when !ok       -> ONLY "16: a cancelled build removes its staging tree"
//     skip the ".part" removal when !ok       -> ONLY "16: a cancelled build removes its partial image"
//                                             (the two cleanups are pinned independently, by one
//                                             assertion each)
//
//   The no-op-overlay and promotion mutants, run rather than reasoned about:
//     delete DiscCompose's filesWritten == 0 guard -> case 18 fails on 4: "an overlay that matched nothing is
//                                             refused", "the refusal says the mod's own files were not
//                                             found", "no disc was installed under the hack's name" and
//                                             "the convert was never started". The build SUCCEEDS instead,
//                                             composing the base disc unmodified -- which is why the tool
//                                             log is asserted and not just the outcome
//     make modRootForXml return payloadDir    -> case 19 fails on the two wrapper assertions and on "the
//                                             anchored wrapper distribution composes" + "really reached the
//                                             convert". The flat and degenerate assertions stay GREEN, which
//                                             is the point of keeping them: they say the fix did not move
//                                             the layouts that already worked
//     skip the promotion rename               -> case 20 fails on THREE: "the composed image is at the output
//                                             path", "no .part is left behind" and "REPLACED the one already
//                                             installed". Note which does NOT fail: "a completed compose
//                                             reports success" stays GREEN, because a skipped rename is not
//                                             an error -- the outcome says ok while the ".part" sits there
//                                             and the destination is the hole the pre-remove left. An
//                                             outcome-only case could not have caught this
//     skip the promotion pre-remove           -> case 20 fails on a DIFFERENT three: "a completed compose
//                                             reports success", "the outcome names the installed image" and
//                                             "REPLACED the one already installed". The rename refuses onto
//                                             the existing file, so this one IS reported -- and the ".part"
//                                             assertion stays green, since the failure path removes it. The
//                                             two sets overlap in one assertion only, which is what says the
//                                             halves are pinned separately rather than jointly covered
//
// Prints RIIVOLUTION-OK on success; RIIVOLUTION-FAIL (nonzero exit) on any miss.
#include "../src/core/DiscCompose.h"
#include "../src/core/DiscOverlay.h"
#include "../src/core/RiivolutionPatch.h"
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <cstring>

static int failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// ---- The stub disc tool -------------------------------------------------------------------------------
//
// Cases 16 and 17 hand DiscCompose this binary's own path as `toolPath`, so the compose really does start a
// child process and really does have to kill it. That is the point: the alternative -- a hook that reports
// what the cancel "would" have done -- is the exact shape of harness this repo has been burned by, where a
// green result covers a path nothing executed.
//
// Dispatch is on argv[1] because DiscCompose chooses the argument list, not the probe, and its first
// argument is always the subcommand. The suite runs this binary with NO arguments, so the normal cases are
// unreachable from here and vice versa.
//
// The stub knows nothing about disc formats. It produces the SHAPE the next stage needs -- an extracted
// tree with a DATA/files root, then an output file -- which is all DiscOverlay and the cleanup care about.
static const int kStubConvertHoldMs = 60000;

static QString argAfter(int argc, char** argv, const char* flag)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return QString::fromLocal8Bit(argv[i + 1]);
    return QString();
}

static bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(bytes);
    f.close();
    return true;
}

// Where the stub records that it was invoked, and with which subcommand. Read from the environment
// because DiscCompose owns the tool's argument list and the probe cannot add to it; a child process
// inherits the parent's environment, so qputenv on this side is enough.
//
// This exists because a staging-directory assertion CANNOT prove the tool never ran: staging is removed
// on every exit path, so an extract that did happen leaves the parent just as empty as one that did not.
// Measured, not reasoned: case 17's first version asserted exactly that, and mutant A (DiscCompose's
// interruption check forced false) left it GREEN while the extract really had run -- the cancel merely
// happened later, in DiscOverlay's own poll. A record that outlives staging is the only thing that
// discriminates the early check.
static const char* kToolLogEnv = "EB_PROBE_TOOL_LOG";

// Set: the stub's convert FINISHES instead of holding, so a compose can be driven all the way to its success
// path. Case 16 needs the hold (a cancel must land mid-convert); cases 18-20 need the opposite, so the two
// behaviours cannot both be the default and something has to choose between them.
//
// An ENVIRONMENT VARIABLE rather than a dispatch on the output name, deliberately. DiscCompose owns the
// argument list and derives the output name from the caller's destination -- a name-based convention would
// put test-only meaning into a path the app builds from a hack's TITLE, where a real hack called the wrong
// thing would trip it. The environment is already how the tool log reaches the child (kToolLogEnv), by the
// same inheritance, so this adds a channel that exists rather than a second mechanism.
static const char* kStubNoHoldEnv = "EB_PROBE_STUB_NO_HOLD";

static void recordToolRun(const QString& sub)
{
    const QString log = qEnvironmentVariable(kToolLogEnv);
    if (log.isEmpty()) return;
    QFile f(log);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    f.write(sub.toLocal8Bit() + "\n");
    f.close();
}

static int stubTool(int argc, char** argv)
{
    const QString sub = QString::fromLocal8Bit(argv[1]);
    recordToolRun(sub);
    const QString out = argAfter(argc, argv, "-o");
    if (out.isEmpty()) return 2;

    if (sub == QLatin1String("extract"))
    {
        // Instant, so the cancel in case 16 lands in the CONVERT and not here -- the convert is the stage
        // that has a partial output file to clean up, which is the guarantee under test.
        if (!writeFile(out + QStringLiteral("/DATA/files/StageData/stock.arc"), QByteArray("STOCK"))) return 3;
        return 0;
    }

    if (sub == QLatin1String("convert"))
    {
        // Write the partial FIRST and only then hold. Case 16 waits for this file to appear before it
        // cancels, so its existence is what proves the build was genuinely mid-convert at the moment of
        // cancellation rather than already finished -- without it, a case that passed because the compose
        // completed early would look identical to one that passed because the cancel worked.
        if (!writeFile(out, QByteArray("PARTIAL-IMAGE-BYTES"))) return 3;

        // The finishing mode. Overwrites the partial with DIFFERENT bytes before exiting 0, so a case that
        // asserts on the installed file's contents is reading something only a completed convert produces --
        // "the partial happened to be left lying at the destination" is then not an available explanation.
        if (qEnvironmentVariableIsSet(kStubNoHoldEnv))
        {
            if (!writeFile(out, QByteArray("COMPOSED-IMAGE-BYTES"))) return 3;
            return 0;
        }

        // Far longer than the test can take, so "it finished on its own" is not an available explanation
        // for a green run. Sliced only so a stray un-killed stub is not immortal.
        for (int slept = 0; slept < kStubConvertHoldMs; slept += 100)
            QThread::msleep(100);
        return 0;
    }

    return 2;
}

// A minimal document that PARSES and yields one folder op, so cases 16 and 17 get past the parse and into
// the tool. Deliberately not one of the fixtures above: those exist to pin parser behaviour, and reusing
// one would couple these cases to refusals they are not about.
static QByteArray composableXml()
{
    return QByteArray(
        "<wiidisc version=\"1\" root=\"\">\n"
        "  <patch id=\"p\" root=\"/m\">\n"
        "    <folder disc=\"/StageData\" external=\"StageData\" create=\"true\"/>\n"
        "  </patch>\n"
        "</wiidisc>\n");
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

int main(int argc, char** argv)
{
    // Re-invoked as DiscCompose's disc tool by cases 16 and 17. Handled before the QCoreApplication so a
    // child costs as little as possible; nothing the stub does needs one.
    if (argc > 1) return stubTool(argc, argv);

    // Needed for applicationFilePath(), which is how those cases find this binary to hand to DiscCompose.
    QCoreApplication app(argc, argv);

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

    // 16. CANCELLING A BUILD IN FLIGHT. composePatchedDisc is run on a worker, exactly as MainWindow runs
    //     it, and interrupted while the convert is running. This is the first case in this file that gets
    //     past the parse: cases 11's two calls both refuse there, so until now the tool wait, the kill, the
    //     staging cleanup and the ".part" cleanup had NO coverage at all, which DiscCompose.cpp said of
    //     itself in a comment.
    //
    //     The cancel is not fired on a timer. The stub writes its ".part" and then holds for a minute, and
    //     this case waits for that file to APPEAR before interrupting -- so a green run cannot be explained
    //     by "the build happened to finish first", which is the way a cancellation test usually lies. The
    //     elapsed-time assertion says the same thing from the other side.
    {
        QTemporaryDir tmp;
        const QString stagingParent = tmp.filePath(QStringLiteral("staging"));
        const QString romsDir       = tmp.filePath(QStringLiteral("roms"));
        const QString modRoot       = tmp.filePath(QStringLiteral("mod"));
        QDir().mkpath(stagingParent);
        QDir().mkpath(romsDir);
        QDir().mkpath(modRoot + QStringLiteral("/m/StageData"));

        QFile mod(modRoot + QStringLiteral("/m/StageData/stock.arc"));
        mod.open(QIODevice::WriteOnly); mod.write("MODDED"); mod.close();

        // Stands in for the base disc. Only its SIZE is read (for the free-space estimate), never its
        // contents -- the stub does not open it.
        const QString discPath = tmp.filePath(QStringLiteral("base.iso"));
        QFile disc(discPath); disc.open(QIODevice::WriteOnly); disc.write("ISO"); disc.close();

        // An ALREADY-INSTALLED image at the destination. A cancelled REBUILD must not destroy it: that is
        // the reason DiscCompose stopped deleting outputPath on failure, and a cancel is a failure path.
        const QByteArray installedBytes("PREVIOUSLY-INSTALLED-IMAGE");
        const QString dest = romsDir + QStringLiteral("/hack.rvz");
        QFile prev(dest); prev.open(QIODevice::WriteOnly); prev.write(installedBytes); prev.close();
        const QString partPath = dest + QStringLiteral(".part");

        const QString toolPath = QCoreApplication::applicationFilePath();
        const QByteArray xml = composableXml();

        const QString toolLog = tmp.filePath(QStringLiteral("tool.log"));
        qputenv(kToolLogEnv, toolLog.toLocal8Bit());

        DiscCompose::Outcome outcome;
        QThread* worker = QThread::create([&] {
            outcome = DiscCompose::composePatchedDisc(toolPath, discPath, modRoot, xml, dest, stagingParent);
        });

        QElapsedTimer clock;
        clock.start();
        worker->start();

        // Wait for the convert to be genuinely under way. Bounded so a broken build fails the case rather
        // than hanging the suite.
        bool sawPart = false, sawStaging = false;
        while (clock.elapsed() < 30000)
        {
            if (!QDir(stagingParent).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty())
                sawStaging = true;
            if (QFile::exists(partPath)) { sawPart = true; break; }
            QThread::msleep(25);
        }
        check(sawStaging, "16: the build really made a staging tree while it was running");
        check(sawPart, "16: the convert really started -- a partial image existed mid-build");

        worker->requestInterruption();
        const bool joined = worker->wait(30000);
        const qint64 elapsed = clock.elapsed();
        if (!joined) worker->wait(2 * kStubConvertHoldMs);   // never leave a live thread behind
        delete worker;

        check(joined, "16: an interrupted build stops instead of running to its ceiling");
        // The stub holds for kStubConvertHoldMs and the tool ceiling is 45 minutes. Finishing well inside
        // the hold is what distinguishes a cancel that was ACTED ON from one that was merely survived.
        check(elapsed < kStubConvertHoldMs / 2,
              "16: the cancel was acted on, not waited out");
        check(!outcome.ok, "16: a cancelled build does not report success");
        check(outcome.error == DiscCompose::cancelledMessage(),
              "16: a cancelled build says it was cancelled, not that the tool failed");
        check(QDir(stagingParent).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(),
              "16: a cancelled build removes its staging tree");
        check(!QFile::exists(partPath), "16: a cancelled build removes its partial image");
        // The one thing a failure path must NOT clean up.
        QFile back(dest);
        back.open(QIODevice::ReadOnly);
        check(back.readAll() == installedBytes,
              "16: a cancelled rebuild leaves the already-installed image untouched");

        // Which stages actually ran. Says the cancel landed in the CONVERT -- the only stage with a
        // partial image to clean up -- rather than somewhere cheaper that would leave these guarantees
        // untested.
        QFile ran(toolLog);
        ran.open(QIODevice::ReadOnly);
        const QByteArray stages = ran.readAll();
        check(stages.contains("extract") && stages.contains("convert"),
              "16: both tool stages really ran, so the cancel landed in the convert");
        qunsetenv(kToolLogEnv);
    }

    // 17. CANCELLED BEFORE IT BEGAN. The race MainWindow's app-quit teardown can lose: the worker is
    //     started, quit arrives and requests interruption, and only then does the compose body run. The
    //     flag is therefore already set when composePatchedDisc is entered, and it must refuse at its own
    //     early check -- BEFORE making a staging directory, and without ever starting the tool -- rather
    //     than doing a disc's worth of work nothing is waiting for.
    //
     //     What discriminates this from a cancel that merely arrived LATER is the tool log, not the
    //     staging assertion. Staging is removed on every exit path, so an extract that did run leaves the
    //     parent exactly as empty as one that did not -- measured: with DiscCompose's interruption check
    //     forced false, this case stayed GREEN on staging alone while the extract really had run and the
    //     cancel had come from DiscOverlay's poll instead. Asserting the tool was never STARTED is the
    //     assertion that can actually fail.
    //
    //     The semaphore is not decoration, and this was MEASURED rather than assumed: the case first
    //     called requestInterruption() BEFORE start(), and Qt silently ignores that -- QThread returns
    //     early for a thread that is not yet running -- so the flag was never set, the whole stub convert
    //     ran to its 60-second hold, and all four assertions failed. The gate makes the ordering real:
    //     start the thread (so the request is honoured), request, and only then let the body proceed.
    {
        QTemporaryDir tmp;
        const QString stagingParent = tmp.filePath(QStringLiteral("staging"));
        const QString romsDir       = tmp.filePath(QStringLiteral("roms"));
        const QString modRoot       = tmp.filePath(QStringLiteral("mod"));
        QDir().mkpath(stagingParent);
        QDir().mkpath(romsDir);
        QDir().mkpath(modRoot + QStringLiteral("/m/StageData"));

        const QString discPath = tmp.filePath(QStringLiteral("base.iso"));
        QFile disc(discPath); disc.open(QIODevice::WriteOnly); disc.write("ISO"); disc.close();

        const QString dest = romsDir + QStringLiteral("/hack.rvz");
        const QString toolPath = QCoreApplication::applicationFilePath();
        const QByteArray xml = composableXml();

        const QString toolLog = tmp.filePath(QStringLiteral("tool.log"));
        qputenv(kToolLogEnv, toolLog.toLocal8Bit());

        DiscCompose::Outcome outcome;
        QSemaphore gate;
        QThread* worker = QThread::create([&] {
            gate.acquire();   // released once the interruption has been requested, never before
            outcome = DiscCompose::composePatchedDisc(toolPath, discPath, modRoot, xml, dest, stagingParent);
        });
        worker->start();
        worker->requestInterruption();
        gate.release();
        const bool joined = worker->wait(30000);
        if (!joined) worker->wait(2 * kStubConvertHoldMs);
        delete worker;

        check(joined, "17: a build cancelled before it began returns promptly");
        check(!outcome.ok, "17: a build cancelled before it began does not report success");
        check(outcome.error == DiscCompose::cancelledMessage(),
              "17: it says it was cancelled");
        check(!QFile::exists(toolLog),
              "17: the disc tool was never started at all");
        check(QDir(stagingParent).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(),
              "17: no staging tree is left behind");
        check(!QFile::exists(dest) && !QFile::exists(dest + QStringLiteral(".part")),
              "17: nothing was written at the destination");
        qunsetenv(kToolLogEnv);
    }

    // 18. A WRAPPER-FOLDER ARCHIVE MUST NOT COMPOSE A VANILLA DISC. The archive holds one top-level folder
    //     and everything inside it, which is the commonest layout deviation there is. MainWindow finds the
    //     document recursively, so it is found; anchoring the mod root at the unpacked directory then points
    //     one level ABOVE the tree, every op's source is missing, and DiscOverlay::apply skips each one as
    //     "a folder this distribution does not ship" -- returning ok with nothing written. Composing from
    //     there produces the base game, unmodified, installed under the hack's name.
    //
    //     This case hands compose the UN-ANCHORED root on purpose: the modRootForXml fix (case 19) is not
    //     what is under test here. The guard is, because the anchor cannot know about a layout nobody has
    //     seen yet, and this is the failure that must not be survivable however the root was picked.
    //
    //     What discriminates it from "some other refusal" is the tool log: the extract runs (the guard sits
    //     after it), the CONVERT must never start. Measured, not reasoned: with the filesWritten == 0 guard
    //     deleted the compose runs to completion, the log gains a "convert" line and an image lands at the
    //     destination -- so the log and the destination assertions are the ones that can actually fail.
    {
        QTemporaryDir tmp;
        const QString stagingParent = tmp.filePath(QStringLiteral("staging"));
        const QString romsDir       = tmp.filePath(QStringLiteral("roms"));
        const QString payloadDir    = tmp.filePath(QStringLiteral("payload"));
        QDir().mkpath(stagingParent);
        QDir().mkpath(romsDir);

        // The wrapper layout: document AND tree both one level down, exactly as the archive unpacks.
        QDir().mkpath(payloadDir + QStringLiteral("/Wrapper/riivolution"));
        QDir().mkpath(payloadDir + QStringLiteral("/Wrapper/m/StageData"));
        check(writeFile(payloadDir + QStringLiteral("/Wrapper/riivolution/hack.xml"), composableXml()),
              "18: the wrapper fixture wrote its document");
        check(writeFile(payloadDir + QStringLiteral("/Wrapper/m/StageData/stock.arc"), QByteArray("MODDED")),
              "18: the wrapper fixture wrote its replacement file");

        const QString discPath = tmp.filePath(QStringLiteral("base.iso"));
        QFile disc(discPath); disc.open(QIODevice::WriteOnly); disc.write("ISO"); disc.close();

        const QString dest = romsDir + QStringLiteral("/hack.rvz");
        const QString toolLog = tmp.filePath(QStringLiteral("tool.log"));
        qputenv(kToolLogEnv, toolLog.toLocal8Bit());
        // Finishing, not holding: a mutant with the guard removed must be able to COMPLETE, or it would look
        // like a hang rather than the wrong disc being built.
        qputenv(kStubNoHoldEnv, "1");

        const auto o = DiscCompose::composePatchedDisc(QCoreApplication::applicationFilePath(), discPath,
                                                       payloadDir, composableXml(), dest, stagingParent);

        check(!o.ok, "18: an overlay that matched nothing is refused, not composed");
        check(o.error.contains(QStringLiteral("none of this mod's files")),
              "18: the refusal says the mod's own files were not found");
        check(!QFile::exists(dest) && !QFile::exists(dest + QStringLiteral(".part")),
              "18: no disc was installed under the hack's name");

        QFile ran(toolLog);
        ran.open(QIODevice::ReadOnly);
        const QByteArray stages = ran.readAll();
        check(stages.contains("extract"), "18: the refusal came after the extract, where the overlay runs");
        check(!stages.contains("convert"),
              "18: the convert was never started, so no vanilla disc was composed");
        check(QDir(stagingParent).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(),
              "18: a refused build removes its staging tree");
        qunsetenv(kToolLogEnv);
        qunsetenv(kStubNoHoldEnv);
    }

    // 19. THE ANCHOR ITSELF. Riivolution's own convention puts the document at <sd-root>/riivolution/<x>.xml,
    //     so the GRANDPARENT of the document is the root `<patch root=>` is relative to -- which handles a
    //     wrapper at any depth rather than at one. The flat case must keep behaving exactly as it did, and a
    //     document sitting directly in the unpacked directory must NOT anchor above it.
    //
    //     The last block composes the wrapper payload from case 18 with the anchor applied, so this does not
    //     rest on string equality alone: the same distribution that case 18 refuses must build here.
    {
        QTemporaryDir tmp;
        const QString payload = tmp.filePath(QStringLiteral("payload"));
        QDir().mkpath(payload + QStringLiteral("/riivolution"));
        QDir().mkpath(payload + QStringLiteral("/Wrapper/riivolution"));
        QDir().mkpath(payload + QStringLiteral("/A/B/riivolution"));
        const QString cleanPayload = QDir::cleanPath(QDir(payload).absolutePath());

        check(DiscOverlay::modRootForXml(payload, payload + QStringLiteral("/riivolution/x.xml"))
                  == cleanPayload,
              "19: a flat archive still anchors at the unpacked directory");
        check(DiscOverlay::modRootForXml(payload, payload + QStringLiteral("/Wrapper/riivolution/x.xml"))
                  == cleanPayload + QStringLiteral("/Wrapper"),
              "19: a wrapper folder anchors at the wrapper, not at the unpacked directory");
        check(DiscOverlay::modRootForXml(payload, payload + QStringLiteral("/A/B/riivolution/x.xml"))
                  == cleanPayload + QStringLiteral("/A/B"),
              "19: a wrapper nested deeper anchors at ITS grandparent, not at a fixed depth");
        // The degenerate layout. Its grandparent is the unpacked directory's PARENT -- the machine's temp
        // folder -- and anchoring there would hand apply() a root holding every other unpacked payload.
        check(DiscOverlay::modRootForXml(payload, payload + QStringLiteral("/x.xml")) == cleanPayload,
              "19: a document at the top level does not anchor OUTSIDE the unpacked directory");
        check(DiscOverlay::modRootForXml(payload, QString()) == cleanPayload,
              "19: no document at all anchors at the unpacked directory");

        // End to end: case 18's exact distribution, anchored, composes.
        const QString stagingParent = tmp.filePath(QStringLiteral("staging"));
        const QString romsDir       = tmp.filePath(QStringLiteral("roms"));
        QDir().mkpath(stagingParent);
        QDir().mkpath(romsDir);
        QDir().mkpath(payload + QStringLiteral("/Wrapper/m/StageData"));
        const QString xmlPath = payload + QStringLiteral("/Wrapper/riivolution/hack.xml");
        check(writeFile(xmlPath, composableXml()), "19: the wrapper fixture wrote its document");
        check(writeFile(payload + QStringLiteral("/Wrapper/m/StageData/stock.arc"), QByteArray("MODDED")),
              "19: the wrapper fixture wrote its replacement file");

        const QString discPath = tmp.filePath(QStringLiteral("base.iso"));
        QFile disc(discPath); disc.open(QIODevice::WriteOnly); disc.write("ISO"); disc.close();

        const QString dest = romsDir + QStringLiteral("/hack.rvz");
        const QString toolLog = tmp.filePath(QStringLiteral("tool.log"));
        qputenv(kToolLogEnv, toolLog.toLocal8Bit());
        qputenv(kStubNoHoldEnv, "1");

        const auto o = DiscCompose::composePatchedDisc(QCoreApplication::applicationFilePath(), discPath,
                                                       DiscOverlay::modRootForXml(payload, xmlPath),
                                                       composableXml(), dest, stagingParent);
        check(o.ok, "19: the anchored wrapper distribution composes, where the un-anchored one was refused");

        QFile ran(toolLog);
        ran.open(QIODevice::ReadOnly);
        check(ran.readAll().contains("convert"),
              "19: the anchored build really reached the convert");
        qunsetenv(kToolLogEnv);
        qunsetenv(kStubNoHoldEnv);
    }

    // 20. THE SUCCESS PATH, which nothing had ever executed. Every earlier call into composePatchedDisc
    //     refuses (cases 11, 18) or is cancelled (16, 17), so the promotion below -- remove any existing
    //     image, then rename the ".part" into place -- had never run anywhere, in this suite or by hand.
    //     It is also the one branch in this file that DELETES a file the user already has installed.
    //
    //     The stub finishes rather than holding (kStubNoHoldEnv), writing bytes distinct from the partial it
    //     wrote first, so reading COMPOSED-IMAGE-BYTES back at the destination says the file there is the one
    //     the tool completed and not a leftover.
    //
    //     Both halves of the promotion are pinned separately, measured by running each mutant:
    //       skip the rename      -> "the composed image is at the output path", "no .part is left behind" and
    //                               "REPLACED the one already installed" go red. "reports success" does NOT:
    //                               a skipped rename raises no error, so the outcome still says ok while the
    //                               image is missing -- which is why this case reads the filesystem and not
    //                               only the Outcome
    //       skip the pre-remove  -> the rename refuses onto the existing file: "reports success", "the
    //                               outcome names the installed image" and "REPLACED the one already
    //                               installed" go red (the ".part" assertion stays green, since the failure
    //                               path removes it). One assertion in common with the mutant above, which is
    //                               what says the two halves are pinned separately
    {
        QTemporaryDir tmp;
        const QString stagingParent = tmp.filePath(QStringLiteral("staging"));
        const QString romsDir       = tmp.filePath(QStringLiteral("roms"));
        const QString modRoot       = tmp.filePath(QStringLiteral("mod"));
        QDir().mkpath(stagingParent);
        QDir().mkpath(romsDir);
        QDir().mkpath(modRoot + QStringLiteral("/m/StageData"));
        check(writeFile(modRoot + QStringLiteral("/m/StageData/stock.arc"), QByteArray("MODDED")),
              "20: the fixture wrote its replacement file");

        const QString discPath = tmp.filePath(QStringLiteral("base.iso"));
        QFile disc(discPath); disc.open(QIODevice::WriteOnly); disc.write("ISO"); disc.close();

        // An image already installed at the destination -- a REBUILD, which is the case the pre-remove
        // exists for and the only case in which this function deletes something of the user's.
        const QByteArray installedBytes("PREVIOUSLY-INSTALLED-IMAGE");
        const QString dest = romsDir + QStringLiteral("/hack.rvz");
        QFile prev(dest); prev.open(QIODevice::WriteOnly); prev.write(installedBytes); prev.close();

        const QString toolLog = tmp.filePath(QStringLiteral("tool.log"));
        qputenv(kToolLogEnv, toolLog.toLocal8Bit());
        qputenv(kStubNoHoldEnv, "1");

        const auto o = DiscCompose::composePatchedDisc(QCoreApplication::applicationFilePath(), discPath,
                                                       modRoot, composableXml(), dest, stagingParent);

        check(o.ok, "20: a completed compose reports success");
        check(o.outputPath == dest, "20: the outcome names the installed image");
        check(QFile::exists(dest), "20: the composed image is at the output path");
        check(!QFile::exists(dest + QStringLiteral(".part")),
              "20: no .part is left behind once the image is promoted");

        QFile back(dest);
        back.open(QIODevice::ReadOnly);
        check(back.readAll() == QByteArray("COMPOSED-IMAGE-BYTES"),
              "20: the composed image REPLACED the one already installed");

        check(QDir(stagingParent).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(),
              "20: a successful build removes its staging tree too");

        QFile ran(toolLog);
        ran.open(QIODevice::ReadOnly);
        const QByteArray stages = ran.readAll();
        check(stages.contains("extract") && stages.contains("convert"),
              "20: both tool stages really ran");
        qunsetenv(kToolLogEnv);
        qunsetenv(kStubNoHoldEnv);
    }

    if (failures == 0) { std::printf("RIIVOLUTION-OK\n"); return 0; }
    std::fprintf(stderr, "RIIVOLUTION-FAIL (%d)\n", failures);
    return 1;
}
