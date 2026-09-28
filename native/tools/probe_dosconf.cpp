// Headless check of the DOS POWER-USER TIER (issue #191): a dosbox.conf beside the game translated onto the
// core's options with an honest applied/ignored report, the two standalone DOSBox registry entries and their
// `-conf` hand-off, and the MIDI assets.
//
// FIVE THINGS HERE CAN FAIL SILENTLY, and each one is a rail below.
//
//  1. A CONF IS HALF-APPLIED AND REPORTED AS APPLIED. This is the failure the whole feature is built to
//     avoid. A conf has ~90 possible keys and a libretro core has a few dozen options; the sets do not match,
//     so a partial mapping is the only honest outcome and SAYING SO is the feature. The rail is an
//     accounting one: every entry of the conf must land in exactly one of `applied` or `ignored`, the counts
//     in the report must equal the file's, and both lists must name their keys. A report that said "conf
//     applied" while dropping fourteen settings would pass every other test.
//
//  2. A BROKEN CONF IS HALF-READ. A conf with a syntax error must apply NOTHING and say why, with a line
//     number. A parser that stopped at the bad line and applied the good half would leave the user debugging
//     a game configured out of the first six lines of their file.
//
//  3. A VALUE IS TRANSLATED WRONG BUT PLAUSIBLY. `machine=svga_s3` -> `svga`, `cycles=fixed 5000` -> `5000`,
//     `cycles=max 80%` -> `max`. None of these is a crash; each is a game running on the wrong hardware. Each
//     mapping and each value shape is pinned against a hand-written oracle.
//
//  4. THE -conf ARGUMENT LOSES A SPACED PATH. The conf hand-off is the LOSSLESS half of #191, and it is one
//     `-conf <file>` argument pair away from working. `{conf}` is substituted BEFORE the shell-style cut
//     (unlike `{rom}`, which is substituted after), so a path with a space in it survives only because the
//     template quotes it — issue #237's quoting, load-bearing here. And an emulator with NO conf must produce
//     byte-for-byte the command line it produced before #191, or every non-DOS launch changed.
//
//  5. THE MIDI MESSAGE DOESN'T NAME THE FILE. An MT-32 needs BOTH of its ROMs; naming one would send the
//     user back for a second trip. Nothing is ever downloaded, so the message is the entire feature.
//
// Expected values are hand-authored oracles, never read back out of the code under test. Prints DOSCONF-OK on
// success; on failure prints DOSCONF-FAIL <cond> per failure and exits non-zero.
#include "DosConf.h"
#include "LaunchRecipe.h"
#include "EmulatorRegistry.h"
#include "LaunchOptionsStore.h"
#include "dosbox_pure_declared.h"   // #288: dosbox-pure's declared options, verified against its source

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "DOSCONF-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// The GOG shape: CRLF, a leading comment block, sections out of order, a [midi] section, an [autoexec] that
// mounts and runs the game. Written as a list of lines so the CRLF is explicit rather than accidental.
static QByteArray gogConf()
{
    const char* lines[] = {
        "# DOSBox configuration written by the installer.",
        "% an alternative comment marker DOSBox also accepts",
        "",
        "[sdl]",
        "fullscreen=false",
        "output=opengl",
        "",
        "[dosbox]",
        "machine=svga_s3",
        "memsize=16",
        "",
        "[cpu]",
        "core=dynamic",
        "cputype=auto",
        "cycles=fixed 20000",
        "",
        "[sblaster]",
        "sbtype=sb16",
        "oplmode=opl3",
        "",
        "[midi]",
        "mididevice=mt32",
        "",
        "[autoexec]",
        "mount c .",
        "c:",
        "GAME.EXE",
    };
    QByteArray out;
    for (const char* l : lines) { out += l; out += "\r\n"; }
    return out;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. the parser, over the real GOG shape ---------------------------------------------------------
    {
        DosConf::File f;
        CHECK(DosConf::parse(gogConf(), &f));
        CHECK(f.ok);
        CHECK(f.error.isEmpty());
        // Comments (# and %) and blank lines contribute nothing; [autoexec] is NOT a settings section.
        CHECK(f.entries.size() == 10);
        CHECK(f.autoexec.size() == 3);
        CHECK(f.autoexec.value(0) == QLatin1String("mount c ."));
        CHECK(f.autoexec.value(2) == QLatin1String("GAME.EXE"));
        // Sections and keys are lowercased; values are NOT (a value can be a file name).
        CHECK(DosConf::value(f, QStringLiteral("dosbox"), QStringLiteral("machine")) == QLatin1String("svga_s3"));
        CHECK(DosConf::value(f, QStringLiteral("CPU"), QStringLiteral("Cycles")) == QLatin1String("fixed 20000"));
        CHECK(DosConf::value(f, QStringLiteral("cpu"), QStringLiteral("nosuchkey")).isEmpty());
        // The qualified spelling is what the mapping table and every message use.
        CHECK(DosConf::qualified(f.entries.at(0)) == QLatin1String("sdl.fullscreen"));
        // The same key in two sections is two different settings.
        CHECK(DosConf::value(f, QStringLiteral("cpu"), QStringLiteral("core")) == QLatin1String("dynamic"));
        // A LF-only conf reads identically to the CRLF one.
        QByteArray lf = gogConf();
        lf.replace("\r\n", "\n");
        DosConf::File g;
        CHECK(DosConf::parse(lf, &g));
        CHECK(g.entries.size() == f.entries.size());
        CHECK(g.autoexec == f.autoexec);
    }

    // ---- 2. a conf we cannot read applies NOTHING -------------------------------------------------------
    {
        // An unclosed section header: everything after it belongs to a section we cannot name.
        DosConf::File f;
        CHECK(!DosConf::parse(QByteArray("[cpu\ncycles=max\n"), &f));
        CHECK(!f.ok);
        CHECK(f.error.startsWith(QLatin1String("line 1:")));
        CHECK(f.entries.isEmpty());

        // A settings line with no '=' inside a real section.
        DosConf::File g;
        CHECK(!DosConf::parse(QByteArray("[cpu]\ncore=dynamic\ncycles\n"), &g));
        CHECK(g.error.startsWith(QLatin1String("line 3:")));

        // A key before any section — the same key means different things in different sections.
        DosConf::File h;
        CHECK(!DosConf::parse(QByteArray("cycles=max\n[cpu]\n"), &h));
        CHECK(h.error.startsWith(QLatin1String("line 1:")));

        // An empty header.
        DosConf::File i;
        CHECK(!DosConf::parse(QByteArray("[]\nx=1\n"), &i));
        CHECK(i.error.startsWith(QLatin1String("line 1:")));

        // …and a plan built from an unreadable conf carries NO options and says so, with the line number.
        DosConf::Spec spec;
        DosConf::Mapping m; m.from = QStringLiteral("cpu.cycles"); m.to = QStringLiteral("k");
        spec.map.push_back(m);
        const DosConf::Plan p = DosConf::translate(g, spec);
        CHECK(!p.ok);
        CHECK(p.options.isEmpty());
        CHECK(p.applied.isEmpty());
        const QString msg = DosConf::report(QStringLiteral("Doom"), QStringLiteral("DOSBOX.CONF"), p);
        CHECK(msg.contains(QLatin1String("could not be read")));
        CHECK(msg.contains(QLatin1String("line 3:")));
        CHECK(msg.contains(QLatin1String("none of it was applied")));
        CHECK(!msg.contains(QLatin1String("applied (")));
    }

    // ---- 3. the cycles grammar --------------------------------------------------------------------------
    {
        CHECK(DosConf::transformCycles(QStringLiteral("auto")) == QLatin1String("auto"));
        CHECK(DosConf::transformCycles(QStringLiteral("AUTO")) == QLatin1String("auto"));
        CHECK(DosConf::transformCycles(QStringLiteral("max")) == QLatin1String("max"));
        CHECK(DosConf::transformCycles(QStringLiteral("max 80%")) == QLatin1String("max"));
        CHECK(DosConf::transformCycles(QStringLiteral("max limit 20000")) == QLatin1String("max"));
        CHECK(DosConf::transformCycles(QStringLiteral("fixed 5000")) == QLatin1String("5000"));
        CHECK(DosConf::transformCycles(QStringLiteral("  fixed   5000 ")) == QLatin1String("5000"));
        CHECK(DosConf::transformCycles(QStringLiteral("20000")) == QLatin1String("20000"));
        // Shapes with no option form yield "" — which is what puts the key in the IGNORED list with a reason,
        // rather than guessing a number.
        CHECK(DosConf::transformCycles(QStringLiteral("fixed")).isEmpty());
        CHECK(DosConf::transformCycles(QStringLiteral("nonsense")).isEmpty());
        CHECK(DosConf::transformCycles(QStringLiteral("0")).isEmpty());
        CHECK(DosConf::transformCycles(QString()).isEmpty());
    }

    // ---- 4. the SHIPPED mapping, against the real msdos recipe ------------------------------------------
    {
        const LaunchRecipe dos = LaunchRecipes::load(QStringLiteral("msdos"), QString());
        CHECK(!dos.isNull());
        const RecipeCore* pure = LaunchRecipes::coreFor(dos, QStringLiteral("dosbox_pure"));
        CHECK(pure != nullptr);
        if (pure)
        {
            CHECK(!pure->conf.isNull());
            CHECK(!pure->midi.isNull());

            DosConf::File f;
            CHECK(DosConf::parse(gogConf(), &f));
            const DosConf::Plan p = DosConf::translate(f, pure->conf);
            CHECK(p.ok);

            // (a) EVERY entry is accounted for. 9 settings + the [autoexec] block = 10 reported items, and
            //     nothing is silently dropped. This is the accounting rail.
            CHECK(p.applied.size() + p.ignored.size() == f.entries.size() + 1);

            // (b) each mapped key, and its VALUE translation, against a hand-written oracle.
            CHECK(p.options.value(QStringLiteral("dosbox_pure_machine")) == QLatin1String("svga"));
            CHECK(p.options.value(QStringLiteral("dosbox_pure_memory_size")) == QLatin1String("16"));
            CHECK(p.options.value(QStringLiteral("dosbox_pure_cycles")) == QLatin1String("20000"));
            CHECK(p.options.value(QStringLiteral("dosbox_pure_cpu_type")) == QLatin1String("auto"));
            CHECK(p.options.value(QStringLiteral("dosbox_pure_cpu_core")) == QLatin1String("dynamic"));
            CHECK(p.options.value(QStringLiteral("dosbox_pure_sblaster_type")) == QLatin1String("sb16"));
            CHECK(p.options.value(QStringLiteral("dosbox_pure_sblaster_adlib_mode")) == QLatin1String("opl3"));
            CHECK(p.options.size() == 7);

            // (c) the UNMAPPED keys land in `ignored`, by name, and are NOT in the options.
            QSet<QString> ignoredKeys;
            for (const DosConf::Ignored& i : p.ignored) ignoredKeys.insert(i.from);
            CHECK(ignoredKeys.contains(QStringLiteral("sdl.fullscreen")));
            CHECK(ignoredKeys.contains(QStringLiteral("sdl.output")));
            CHECK(ignoredKeys.contains(QStringLiteral("autoexec")));
            // midi.mididevice is a KNOWN key the recipe deliberately declines to translate: it must be
            // ignored WITH THE POINTER at the setting, not with the generic "no matching core option".
            CHECK(ignoredKeys.contains(QStringLiteral("midi.mididevice")));
            CHECK(!p.options.contains(QStringLiteral("dosbox_pure_midi")));
            for (const DosConf::Ignored& i : p.ignored)
                if (i.from == QLatin1String("midi.mididevice"))
                    CHECK(i.reason.contains(QLatin1String("Settings")));
            // …and every ignored entry gives a reason. A silent drop is the bug this file exists for.
            for (const DosConf::Ignored& i : p.ignored) CHECK(!i.reason.isEmpty());

            // (d) a mapped key whose VALUE the core has no setting for is ignored with a reason, not passed
            //     through. `machine=svga_et9999` is not a machine dosbox-pure knows.
            DosConf::File odd;
            CHECK(DosConf::parse(QByteArray("[dosbox]\nmachine=svga_et9999\nmemsize=3\n"), &odd));
            const DosConf::Plan op = DosConf::translate(odd, pure->conf);
            CHECK(op.ok);
            CHECK(op.options.isEmpty());
            CHECK(op.applied.isEmpty());
            CHECK(op.ignored.size() == 2);

            // (e) THE REPORT. Counts, both lists, and the exact translation for each applied key.
            const QString r = DosConf::report(QStringLiteral("Doom"), QStringLiteral("DOSBOX.CONF"), p);
            CHECK(r.contains(QStringLiteral("DOSBOX.CONF")));
            CHECK(r.contains(QStringLiteral("Doom")));
            CHECK(r.contains(QStringLiteral("7 of 11 settings applied")));
            CHECK(r.contains(QStringLiteral("dosbox.machine → dosbox_pure_machine=svga")));
            CHECK(r.contains(QStringLiteral("cpu.cycles → dosbox_pure_cycles=20000")));
            CHECK(r.contains(QStringLiteral("Ignored:")));
            CHECK(r.contains(QStringLiteral("sdl.fullscreen")));
            CHECK(r.contains(QStringLiteral("autoexec")));

            // (f) the log lines carry the REASON for every ignored key (the report stays short by naming them
            //     only), and one line per accounted-for item — the same accounting as (a).
            const QStringList lines = DosConf::logLines(p);
            CHECK(lines.size() == p.applied.size() + p.ignored.size());
        }

        // dosbox_core has its OWN mapping onto its OWN option names — a conf mapping is a property of the
        // core, never of the system, exactly as #190's option seeds are.
        const RecipeCore* dcore = LaunchRecipes::coreFor(dos, QStringLiteral("dosbox_core"));
        CHECK(dcore != nullptr);
        if (dcore)
        {
            CHECK(!dcore->conf.isNull());
            DosConf::File f;
            CHECK(DosConf::parse(QByteArray("[dosbox]\nmachine=svga_s3\n"), &f));
            const DosConf::Plan p = DosConf::translate(f, dcore->conf);
            CHECK(p.options.value(QStringLiteral("dosbox_core_machine")) == QLatin1String("svga_s3"));
            CHECK(!p.options.contains(QStringLiteral("dosbox_pure_machine")));
            // dosbox_core has no MIDI-asset option, and saying nothing is how a recipe says so.
            CHECK(dcore->midi.isNull());
        }

        // A system with no conf block at all (every non-DOS recipe) translates nothing — the pre-#191
        // behaviour, so silence never changes a launch.
        const LaunchRecipe c64 = LaunchRecipes::load(QStringLiteral("c64"), QString());
        const RecipeCore* vice = c64.isNull() ? nullptr : LaunchRecipes::coreFor(c64, QStringLiteral("vice_x64"));
        if (vice) { CHECK(vice->conf.isNull()); CHECK(vice->midi.isNull()); }
    }

    // ---- 5. which file beside the game is THE conf ------------------------------------------------------
    {
        // dosbox.conf wins outright, whatever its case and whatever else is there.
        const QStringList a = { QStringLiteral("GAME.EXE"), QStringLiteral("dosbox_game.conf"),
                                QStringLiteral("DOSBOX.CONF") };
        CHECK(DosConf::chooseConf(a) == QLatin1String("DOSBOX.CONF"));
        // The GOG pair: the SETTINGS file, not the _single one that also runs the game and exits.
        const QStringList b = { QStringLiteral("dosbox_doom.conf"), QStringLiteral("dosbox_doom_single.conf") };
        CHECK(DosConf::chooseConf(b) == QLatin1String("dosbox_doom.conf"));
        // Exactly one .conf of any name is unambiguous.
        const QStringList c = { QStringLiteral("GAME.EXE"), QStringLiteral("mysettings.conf") };
        CHECK(DosConf::chooseConf(c) == QLatin1String("mysettings.conf"));
        // Two unrelated confs is a genuine ambiguity; picking one would be a coin flip that changed the game.
        const QStringList d = { QStringLiteral("alpha.conf"), QStringLiteral("beta.conf") };
        CHECK(DosConf::chooseConf(d).isEmpty());
        // A conf inside a sub-folder is not "beside the game".
        const QStringList e = { QStringLiteral("SETUP/dosbox.conf"), QStringLiteral("GAME.EXE") };
        CHECK(DosConf::chooseConf(e).isEmpty());
        CHECK(DosConf::chooseConf(QStringList{ QStringLiteral("GAME.EXE") }).isEmpty());
        CHECK(DosConf::chooseConf(QStringList()).isEmpty());
    }

    // ---- 6. the standalone entries, and the -conf hand-off ----------------------------------------------
    {
        const QList<ExternalEmulator>& table = EmulatorRegistry::builtinEmulators();
        const ExternalEmulator* staging = nullptr;
        const ExternalEmulator* dosboxx = nullptr;
        for (const ExternalEmulator& e : table)
        {
            if (e.id == QLatin1String("dosbox-staging")) staging = &e;
            if (e.id == QLatin1String("dosbox-x"))       dosboxx = &e;
        }
        CHECK(staging != nullptr);
        CHECK(dosboxx != nullptr);

        const QString expectedConfArgs = QStringLiteral("-conf \"{confPath}\"");
        for (const ExternalEmulator* e : { staging, dosboxx })
        {
            if (!e) continue;
            CHECK(!e->displayName.isEmpty());
            // The LOAD-BEARING binding: without `systems` the picker never offers it for MS-DOS.
            CHECK(e->systems.contains(QStringLiteral("msdos")));
            // MS-DOS's DEFAULT stays the in-process core — these are a choice, not a takeover.
            CHECK(e->argsTemplate.contains(QStringLiteral("{conf}")));
            CHECK(e->argsTemplate.contains(QStringLiteral("{rom}")));
            CHECK(e->confArgs == expectedConfArgs);
            // The path is QUOTED in the template (issue #237) because it is substituted BEFORE the cut.
            CHECK(e->confArgs.count(QLatin1Char('"')) == 2);
            CHECK(!e->winBinaries.isEmpty());
            CHECK(!e->homepage.isEmpty());
            // Round-trip: confArgs survives the canonical JSON both ways, so a user's own <data>/emulators
            // entry can declare one and an override of ours can change it.
            const ExternalEmulator back = EmulatorRegistry::fromJson(EmulatorRegistry::toJson(*e));
            CHECK(back.confArgs == e->confArgs);
            CHECK(back == *e);
        }

        // No OTHER built-in emulator declares a conf hand-off, so nothing else can change shape.
        int withConf = 0;
        for (const ExternalEmulator& e : table) if (!e.confArgs.isEmpty()) ++withConf;
        CHECK(withConf == 2);

        // The substitution itself. A conf path WITH A SPACE stays one argument — the property #237 bought and
        // the reason the template quotes {confPath}.
        if (staging)
        {
            QString tmpl = staging->argsTemplate;
            tmpl.replace(QStringLiteral("{fs}"), staging->fullscreenArgs);
            const QString spaced = QStringLiteral("/games/My Games/Doom/dosbox.conf");
            const QString withPath = LaunchOpts::applyConfArg(tmpl, staging->confArgs, spaced);
            const QStringList args = LaunchOpts::buildArgs(withPath, QStringLiteral("/games/My Games/Doom"));
            CHECK(args.contains(QStringLiteral("-conf")));
            CHECK(args.contains(spaced));
            CHECK(args.contains(QStringLiteral("/games/My Games/Doom")));
            const int confAt = int(args.indexOf(QStringLiteral("-conf")));
            CHECK(confAt >= 0);
            CHECK(args.value(confAt + 1) == spaced);

            // NO conf: the placeholder collapses away entirely — no empty argument, no dangling -conf.
            const QString none = LaunchOpts::applyConfArg(tmpl, staging->confArgs, QString());
            const QStringList bare = LaunchOpts::buildArgs(none, QStringLiteral("/games/Doom"));
            CHECK(!bare.contains(QStringLiteral("-conf")));
            for (const QString& a : bare) CHECK(!a.isEmpty());
            CHECK(bare.contains(QStringLiteral("/games/Doom")));

            // A path containing a double quote would break out of the quoting, so it is declined rather than
            // escaped by a convention we would have to guess per platform.
            const QString evil = QStringLiteral("/games/we\"ird/dosbox.conf");
            const QString declined = LaunchOpts::applyConfArg(tmpl, staging->confArgs, evil);
            CHECK(!declined.contains(QStringLiteral("-conf")));
        }

        // EVERY emulator that predates #191 is byte-for-byte unchanged: no {conf} in the template means
        // applyConfArg returns the string it was given, whatever it is handed.
        for (const ExternalEmulator& e : table)
        {
            if (!e.confArgs.isEmpty()) continue;
            const QString same = LaunchOpts::applyConfArg(e.argsTemplate, QStringLiteral("-conf \"{confPath}\""),
                                                          QStringLiteral("/tmp/x.conf"));
            CHECK(same == e.argsTemplate);
        }
    }

    // ---- 7. the MIDI assets ------------------------------------------------------------------------------
    {
        const LaunchRecipe dos = LaunchRecipes::load(QStringLiteral("msdos"), QString());
        const RecipeCore* pure = dos.isNull() ? nullptr : LaunchRecipes::coreFor(dos, QStringLiteral("dosbox_pure"));
        CHECK(pure != nullptr);
        if (!pure) { std::fprintf(stderr, "DOSCONF had %d failure(s)\n", failures + 1); return 1; }

        const DosConf::MidiSpec& midi = pure->midi;
        CHECK(midi.option == QLatin1String("dosbox_pure_midi"));
        const DosConf::MidiDevice* mt32 = DosConf::midiDevice(midi, QStringLiteral("mt32"));
        const DosConf::MidiDevice* gm   = DosConf::midiDevice(midi, QStringLiteral("gm"));
        CHECK(mt32 != nullptr);
        CHECK(gm != nullptr);
        CHECK(DosConf::midiDevice(midi, QStringLiteral("nosuchdevice")) == nullptr);
        CHECK(DosConf::midiDevice(midi, QString()) == nullptr);

        const QString folder = QStringLiteral("/opt/EverythingBox/system");
        if (mt32)
        {
            // An MT-32 needs BOTH ROMs. With neither present, BOTH are named: reporting one would send the
            // user back for a second trip.
            const auto haveNothing = [](const QString&) { return false; };
            const QStringList missing = DosConf::missingMidiFiles(*mt32, haveNothing);
            CHECK(missing.size() == 2);
            CHECK(missing.contains(QStringLiteral("MT32_CONTROL.ROM")));
            CHECK(missing.contains(QStringLiteral("MT32_PCM.ROM")));
            CHECK(DosConf::midiOptions(midi, QStringLiteral("mt32"), haveNothing).isEmpty());

            const QString m = DosConf::midiMessage(*mt32, missing, folder, QStringLiteral("Doom"));
            CHECK(m.contains(QStringLiteral("MT32_CONTROL.ROM")));
            CHECK(m.contains(QStringLiteral("MT32_PCM.ROM")));
            CHECK(m.contains(folder));
            CHECK(m.contains(QStringLiteral("system folder")));
            CHECK(m.contains(QStringLiteral("can't download")));
            CHECK(m.contains(QStringLiteral("copyrighted")));
            CHECK(m.contains(QStringLiteral("default audio")));
            CHECK(m.contains(QStringLiteral("Doom")));

            // HALF present is still missing, and the message names ONLY the file that is actually absent.
            const auto haveControlOnly = [](const QString& n) {
                return n == QLatin1String("MT32_CONTROL.ROM");
            };
            const QStringList half = DosConf::missingMidiFiles(*mt32, haveControlOnly);
            CHECK(half == QStringList{ QStringLiteral("MT32_PCM.ROM") });
            CHECK(DosConf::midiOptions(midi, QStringLiteral("mt32"), haveControlOnly).isEmpty());
            const QString hm = DosConf::midiMessage(*mt32, half, folder, QString());
            CHECK(hm.contains(QStringLiteral("MT32_PCM.ROM")));
            CHECK(!hm.contains(QStringLiteral("MT32_CONTROL.ROM")));
            CHECK(hm.contains(QStringLiteral("a file")));   // singular

            // Both present: the option is seeded, and there is no message.
            const auto haveAll = [](const QString&) { return true; };
            CHECK(DosConf::missingMidiFiles(*mt32, haveAll).isEmpty());
            const QMap<QString, QString> seeded = DosConf::midiOptions(midi, QStringLiteral("mt32"), haveAll);
            CHECK(seeded.size() == 1);
            CHECK(seeded.value(QStringLiteral("dosbox_pure_midi")) == QLatin1String("MT32_CONTROL.ROM"));
            CHECK(DosConf::midiMessage(*mt32, QStringList(), folder, QStringLiteral("Doom")).isEmpty());
        }
        if (gm)
        {
            const auto haveNothing = [](const QString&) { return false; };
            const QStringList missing = DosConf::missingMidiFiles(*gm, haveNothing);
            CHECK(missing == QStringList{ QStringLiteral("DOSBOX.SF2") });
            const QString m = DosConf::midiMessage(*gm, missing, folder, QStringLiteral("Doom"));
            CHECK(m.contains(QStringLiteral("DOSBOX.SF2")));
            CHECK(m.contains(folder));
            // Nothing is downloaded, and the reason a soundfont is not provided is not "copyright" — it is
            // that EverythingBox does not bundle one. The message must say the right thing per asset.
            CHECK(m.contains(QStringLiteral("bundle")));
            CHECK(!m.contains(QStringLiteral("copyrighted")));
            const auto haveAll = [](const QString&) { return true; };
            CHECK(DosConf::midiOptions(midi, QStringLiteral("gm"), haveAll)
                      .value(QStringLiteral("dosbox_pure_midi")) == QLatin1String("DOSBOX.SF2"));
        }
        // "Default" (the unset setting) seeds nothing at all — the pre-#191 launch, untouched.
        const auto haveAll = [](const QString&) { return true; };
        CHECK(DosConf::midiOptions(midi, QString(), haveAll).isEmpty());
        CHECK(DosConf::midiOptions(midi, QStringLiteral("default"), haveAll).isEmpty());
    }

    // ---- 8. #288: nothing is reported applied unless the LOADED core declares it ----------------------------
    // LibretroCore::setOptionValue does not validate, so a key the core never declared (or a value outside its
    // list) would be stored, reported "applied" and never read. The plan is therefore held against the loaded
    // core's own options before it is reported or seeded. The fake core here is dosbox-pure's REAL declaration
    // (dosbox_pure_declared.h, verified against the upstream source), in the exact CoreOption shape
    // LibretroCore::options() hands the launch path.
    {
        const std::vector<CoreOption> fakeCore = dosboxPureDeclaredOptions();
        const DosConf::Declared declared = DosConf::declaredFrom(fakeCore);
        CHECK(declared.size() == 8);
        CHECK(declared.value(QStringLiteral("dosbox_pure_cycles")).contains(QStringLiteral("7800")));
        CHECK(declared.value(QStringLiteral("dosbox_pure_cpu_type")).contains(QStringLiteral("386_slow")));

        const LaunchRecipe dos = LaunchRecipes::load(QStringLiteral("msdos"), QString());
        const RecipeCore* pure = dos.isNull() ? nullptr : LaunchRecipes::coreFor(dos, QStringLiteral("dosbox_pure"));
        CHECK(pure != nullptr);
        if (!pure) { std::fprintf(stderr, "DOSCONF had %d failure(s)\n", failures + 1); return 1; }

        const auto planFor = [&](const QByteArray& confBytes, const DosConf::Declared& d, DosConf::Plan* before) {
            DosConf::File f;
            DosConf::parse(confBytes, &f);
            const DosConf::Plan pre = DosConf::translate(f, pure->conf);
            if (before) *before = pre;
            return DosConf::checkAgainstCore(pre, d);
        };
        const auto ignoredReason = [](const DosConf::Plan& p, const QString& from) {
            for (const DosConf::Ignored& i : p.ignored) if (i.from == from) return i.reason;
            return QString();
        };
        const auto isApplied = [](const DosConf::Plan& p, const QString& from) {
            for (const DosConf::Applied& a : p.applied) if (a.from == from) return true;
            return false;
        };

        // (a) each of the seven mapped keys, with a representative conf value, lands APPLIED with the exact
        //     value dosbox-pure declares. cputype=386_slow is dosbox-pure's own "386_slow", not "386".
        const QByteArray seven("[dosbox]\nmachine=svga_et4000\nmemsize=32\n"
                               "[cpu]\ncycles=fixed 7800\ncputype=386_slow\ncore=dynamic_x86\n"
                               "[sblaster]\nsbtype=sbpro2\noplmode=dualopl2\n");
        DosConf::Plan sevenPre;
        const DosConf::Plan all = planFor(seven, declared, &sevenPre);
        CHECK(all.ok);
        CHECK(sevenPre.coreCheck == DosConf::CoreCheck::NotChecked);
        CHECK(all.coreCheck == DosConf::CoreCheck::Checked);
        CHECK(all.applied.size() == 7);
        CHECK(all.ignored.isEmpty());
        CHECK(all.options.value(QStringLiteral("dosbox_pure_machine")) == QLatin1String("svga"));
        CHECK(all.options.value(QStringLiteral("dosbox_pure_memory_size")) == QLatin1String("32"));
        CHECK(all.options.value(QStringLiteral("dosbox_pure_cycles")) == QLatin1String("7800"));
        CHECK(all.options.value(QStringLiteral("dosbox_pure_cpu_type")) == QLatin1String("386_slow"));
        CHECK(all.options.value(QStringLiteral("dosbox_pure_cpu_core")) == QLatin1String("dynamic"));
        CHECK(all.options.value(QStringLiteral("dosbox_pure_sblaster_type")) == QLatin1String("sbpro2"));
        CHECK(all.options.value(QStringLiteral("dosbox_pure_sblaster_adlib_mode")) == QLatin1String("dualopl2"));
        for (const DosConf::Applied& a : all.applied)
            CHECK(declared.value(a.option).contains(a.optionValue));

        // (b) an UNDECLARED KEY: the same conf on a core that does not offer the adlib-mode option. The recipe
        //     translated it (applied before load); the loaded core moves it to IGNORED, with the reason, and it
        //     is neither seeded nor counted as applied.
        DosConf::Declared noAdlib = declared;
        noAdlib.remove(QStringLiteral("dosbox_pure_sblaster_adlib_mode"));
        DosConf::Plan keyPre;
        const DosConf::Plan keyPost = planFor(seven, noAdlib, &keyPre);
        CHECK(isApplied(keyPre, QStringLiteral("sblaster.oplmode")));             // applied -> …
        CHECK(!isApplied(keyPost, QStringLiteral("sblaster.oplmode")));           // … -> ignored after load
        CHECK(ignoredReason(keyPost, QStringLiteral("sblaster.oplmode"))
              == QLatin1String("the loaded core does not offer dosbox_pure_sblaster_adlib_mode"));
        CHECK(!keyPost.options.contains(QStringLiteral("dosbox_pure_sblaster_adlib_mode")));
        CHECK(keyPost.applied.size() == 6);
        CHECK(keyPost.applied.size() + keyPost.ignored.size() == keyPre.applied.size() + keyPre.ignored.size());
        const QString keyReport = DosConf::report(QStringLiteral("Doom"), QStringLiteral("DOSBOX.CONF"), keyPost);
        CHECK(keyReport.contains(QStringLiteral("6 of 7 settings applied")));
        CHECK(keyReport.contains(QStringLiteral("Ignored: sblaster.oplmode.")));
        CHECK(!keyReport.contains(QStringLiteral("dosbox_pure_sblaster_adlib_mode=")));
        CHECK(DosConf::logLines(keyPost).join(QLatin1Char('\n'))
                  .contains(QStringLiteral("does not offer dosbox_pure_sblaster_adlib_mode")));

        // (c) an UNDECLARED VALUE: a dosbox-pure build with no dynamic recompiler declares cpu_core as only
        //     normal/simple (core_options.h:924-932). core=dynamic is ignored, and the reason NAMES the value.
        DosConf::Declared interpOnly = declared;
        interpOnly.insert(QStringLiteral("dosbox_pure_cpu_core"), { QStringLiteral("normal"), QStringLiteral("simple") });
        const DosConf::Plan valPost = planFor(QByteArray("[cpu]\ncore=dynamic\n"), interpOnly, nullptr);
        CHECK(valPost.applied.isEmpty());
        CHECK(valPost.options.isEmpty());
        const QString valReason = ignoredReason(valPost, QStringLiteral("cpu.core"));
        CHECK(valReason.contains(QStringLiteral("dynamic")));
        CHECK(valReason.contains(QStringLiteral("dosbox_pure_cpu_core")));
        // A repeated key: the dropped value must not survive into the seed, and the accepted one must.
        const DosConf::Plan rep = planFor(QByteArray("[cpu]\ncore=simple\ncore=dynamic\n"), interpOnly, nullptr);
        CHECK(rep.options.value(QStringLiteral("dosbox_pure_cpu_core")) == QLatin1String("simple"));
        CHECK(rep.applied.size() == 1);
        CHECK(rep.ignored.size() == 1);

        // (d) an EMPTY declared set means UNKNOWN: today's behaviour, unchanged, and the log says so.
        DosConf::Plan unkPre;
        const DosConf::Plan unk = planFor(seven, DosConf::Declared(), &unkPre);
        CHECK(unk.coreCheck == DosConf::CoreCheck::Unknown);
        CHECK(unk.options == unkPre.options);
        CHECK(unk.applied.size() == unkPre.applied.size());
        CHECK(unk.ignored.size() == unkPre.ignored.size());
        const QStringList unkLines = DosConf::logLines(unk);
        CHECK(unkLines.size() == unk.applied.size() + unk.ignored.size() + 1);
        CHECK(unkLines.value(0).contains(QStringLiteral("unknown")));
        CHECK(!DosConf::logLines(unkPre).join(QLatin1Char('\n')).contains(QStringLiteral("unknown")));
        CHECK(!DosConf::logLines(all).join(QLatin1Char('\n')).contains(QStringLiteral("unknown")));

        // (e) a declared key with an OPEN value list takes any value.
        DosConf::Declared open = declared;
        open.insert(QStringLiteral("dosbox_pure_cycles"), QStringList());
        const DosConf::Plan openPlan = planFor(QByteArray("[cpu]\ncycles=fixed 3000\n"), open, nullptr);
        CHECK(openPlan.options.value(QStringLiteral("dosbox_pure_cycles")) == QLatin1String("3000"));

        // (f) cycles, per the REAL core: it DECLARES a fixed list of auto, max and eleven counts, but it READS the
        //     option with atoi (dosbox_pure_libretro.cpp:2405-2414), so the recipe marks cpu.cycles
        //     acceptsNumber and any positive decimal count is honoured. Shapes with no positive count are not.
        struct CyclesCase { const char* conf; const char* applied; const char* ignoredNaming; };
        const CyclesCase cases[] = {
            { "auto",                  "auto", nullptr },
            { "AUTO",                  "auto", nullptr },
            { "auto 7800 limit 50000", "auto", nullptr },   // DOSBox's auto with a start value and a limit
            { "auto 50%",              "auto", nullptr },
            { "max",                   "max",  nullptr },
            { "max 80%",               "max",  nullptr },
            { "max limit 20000",       "max",  nullptr },
            { "fixed 7800",            "7800", nullptr },
            { "7800",                  "7800", nullptr },
            { "fixed 1000000",         "1000000", nullptr },
            { "fixed 3000",            "3000", nullptr },    // not in the list, but a count the core reads
            { "20000",                 "20000", nullptr },
        };
        for (const CyclesCase& c : cases)
        {
            const DosConf::Plan cp = planFor(QByteArray("[cpu]\ncycles=") + c.conf + "\n", declared, nullptr);
            if (c.applied)
            {
                if (cp.options.value(QStringLiteral("dosbox_pure_cycles")) != QLatin1String(c.applied))
                    std::fprintf(stderr, "DOSCONF-FAIL cycles=%s -> '%s', want '%s'\n", c.conf,
                                 qPrintable(cp.options.value(QStringLiteral("dosbox_pure_cycles"))), c.applied);
                CHECK(cp.options.value(QStringLiteral("dosbox_pure_cycles")) == QLatin1String(c.applied));
                CHECK(cp.ignored.isEmpty());
            }
            else
            {
                if (!cp.options.isEmpty())
                    std::fprintf(stderr, "DOSCONF-FAIL cycles=%s was applied\n", c.conf);
                CHECK(cp.options.isEmpty());
                CHECK(ignoredReason(cp, QStringLiteral("cpu.cycles")).contains(QLatin1String(c.ignoredNaming)));
            }
        }
        // …and a shape with no positive count is ignored with the recipe's own note, flag or no flag.
        for (const char* bad : { "fixed", "fixed abc", "-5", "0", "fixed -5", "fixed 0" })
        {
            const DosConf::Plan bp = planFor(QByteArray("[cpu]\ncycles=") + bad + "\n", declared, nullptr);
            if (!bp.options.isEmpty()) std::fprintf(stderr, "DOSCONF-FAIL cycles=%s was applied\n", bad);
            CHECK(bp.options.isEmpty());
            CHECK(bp.applied.isEmpty());
            CHECK(!ignoredReason(bp, QStringLiteral("cpu.cycles")).isEmpty());
        }

        // (f2) the acceptsNumber flag is DATA and strict by default. The same count through a mapping WITHOUT
        //      the flag is held to the declared list and ignored, naming the value; with the flag, a value that
        //      is not a positive decimal integer is still ignored, naming the value. The KEY check still applies.
        {
            DosConf::Spec strict;
            DosConf::Mapping sm; sm.from = QStringLiteral("cpu.cycles"); sm.to = QStringLiteral("dosbox_pure_cycles");
            sm.transform = QStringLiteral("cycles");            // acceptsNumber left at its default
            strict.map.push_back(sm);
            DosConf::File f; DosConf::parse(QByteArray("[cpu]\ncycles=fixed 3000\n"), &f);
            const DosConf::Plan sp = DosConf::checkAgainstCore(DosConf::translate(f, strict), declared);
            CHECK(sp.options.isEmpty());
            CHECK(ignoredReason(sp, QStringLiteral("cpu.cycles")).contains(QStringLiteral("3000")));

            DosConf::Spec flagged;
            DosConf::Mapping pm; pm.from = QStringLiteral("cpu.cycles"); pm.to = QStringLiteral("dosbox_pure_cycles");
            pm.acceptsNumber = true;                             // pass-through: the value reaches the check verbatim
            flagged.map.push_back(pm);
            for (const char* v : { "abc", "-5", "0", "+5", "5x", "12 34", "99999999999" })
            {
                DosConf::File g; DosConf::parse(QByteArray("[cpu]\ncycles=") + v + "\n", &g);
                const DosConf::Plan fp = DosConf::checkAgainstCore(DosConf::translate(g, flagged), declared);
                if (!fp.options.isEmpty()) std::fprintf(stderr, "DOSCONF-FAIL acceptsNumber took '%s'\n", v);
                CHECK(fp.options.isEmpty());
                CHECK(ignoredReason(fp, QStringLiteral("cpu.cycles")).contains(QLatin1String(v)));
            }
            DosConf::File h; DosConf::parse(QByteArray("[cpu]\ncycles=3000\n"), &h);
            const DosConf::Plan ok = DosConf::checkAgainstCore(DosConf::translate(h, flagged), declared);
            CHECK(ok.options.value(QStringLiteral("dosbox_pure_cycles")) == QLatin1String("3000"));
            // The flag never excuses a key the core does not declare.
            DosConf::Declared noCycles = declared;
            noCycles.remove(QStringLiteral("dosbox_pure_cycles"));
            const DosConf::Plan nk = DosConf::checkAgainstCore(DosConf::translate(h, flagged), noCycles);
            CHECK(nk.options.isEmpty());
            CHECK(ignoredReason(nk, QStringLiteral("cpu.cycles"))
                  == QLatin1String("the loaded core does not offer dosbox_pure_cycles"));
        }

        // (g) an unreadable conf stays unreadable — the check never invents an applied entry.
        const DosConf::Plan broken = planFor(QByteArray("[cpu\ncycles=max\n"), declared, nullptr);
        CHECK(!broken.ok);
        CHECK(broken.options.isEmpty());
        CHECK(broken.applied.isEmpty());

        // (h) the GOG conf from section 4 on the real core: every one of its seven mapped settings — including
        //     cycles=fixed 20000, a count the core reads though it does not list it — is still applied after
        //     the loaded core's check. (The applied -> ignored transition itself is case (b).)
        DosConf::Plan gogPre;
        const DosConf::Plan gog = planFor(gogConf(), declared, &gogPre);
        CHECK(isApplied(gogPre, QStringLiteral("cpu.cycles")));
        CHECK(isApplied(gog, QStringLiteral("cpu.cycles")));
        CHECK(gog.options.value(QStringLiteral("dosbox_pure_cycles")) == QLatin1String("20000"));
        CHECK(gog.options == gogPre.options);
        CHECK(DosConf::report(QStringLiteral("Doom"), QStringLiteral("DOSBOX.CONF"), gog)
                  .contains(QStringLiteral("7 of 11 settings applied")));

        // (i) THE USER'S OWN SETTING WINS, AND THE REPORT SAYS SO. openGame never seeds a conf key the user has
        //     set per core or per game. Counting it as applied would be the same lie as (b), so it is its own
        //     outcome: kept — not applied, not ignored, not in "N of M", not in the seed.
        const QSet<QString> userKeys = { QStringLiteral("dosbox_pure_cycles"), QStringLiteral("dosbox_pure_nosuch") };
        const DosConf::Plan kept = DosConf::keepUserSettings(gog, userKeys);
        CHECK(!isApplied(kept, QStringLiteral("cpu.cycles")));
        CHECK(ignoredReason(kept, QStringLiteral("cpu.cycles")).isEmpty());
        CHECK(kept.kept.size() == 1);
        CHECK(kept.kept.value(0).from == QLatin1String("cpu.cycles"));
        CHECK(kept.kept.value(0).option == QLatin1String("dosbox_pure_cycles"));
        CHECK(!kept.options.contains(QStringLiteral("dosbox_pure_cycles")));
        CHECK(kept.options.size() == gog.options.size() - 1);
        CHECK(kept.applied.size() == gog.applied.size() - 1);
        CHECK(kept.ignored.size() == gog.ignored.size());
        CHECK(kept.applied.size() + kept.ignored.size() + kept.kept.size() == gog.applied.size() + gog.ignored.size());
        const QString keptReport = DosConf::report(QStringLiteral("Doom"), QStringLiteral("DOSBOX.CONF"), kept);
        CHECK(keptReport.contains(QStringLiteral("6 of 10 settings applied")));
        CHECK(keptReport.contains(QStringLiteral("Kept your own setting for cpu.cycles.")));
        CHECK(!keptReport.contains(QStringLiteral("dosbox_pure_cycles=20000")));
        const QString keptLog = DosConf::logLines(kept).join(QLatin1Char('\n'));
        CHECK(keptLog.contains(QStringLiteral("kept cpu.cycles=fixed 20000")));
        CHECK(DosConf::logLines(kept).size() == kept.applied.size() + kept.ignored.size() + kept.kept.size());
        // No user keys: nothing moves, and the report has no kept clause.
        const DosConf::Plan none = DosConf::keepUserSettings(gog, QSet<QString>());
        CHECK(none.options == gog.options);
        CHECK(none.kept.isEmpty());
        CHECK(!DosConf::report(QString(), QStringLiteral("DOSBOX.CONF"), none).contains(QStringLiteral("Kept")));
        // Everything kept: the report says nothing was applied rather than "0 of 0".
        DosConf::File onlyCycles; DosConf::parse(QByteArray("[cpu]\ncycles=max\n"), &onlyCycles);
        const DosConf::Plan allKept = DosConf::keepUserSettings(
            DosConf::checkAgainstCore(DosConf::translate(onlyCycles, pure->conf), declared), userKeys);
        const QString allKeptReport = DosConf::report(QStringLiteral("Doom"), QStringLiteral("DOSBOX.CONF"), allKept);
        CHECK(allKept.options.isEmpty());
        CHECK(!allKeptReport.contains(QStringLiteral("0 of 0")));
        CHECK(allKeptReport.contains(QStringLiteral("nothing was applied")));
        CHECK(allKeptReport.contains(QStringLiteral("Kept your own setting for cpu.cycles.")));
        // An unreadable plan is left as it is.
        CHECK(!DosConf::keepUserSettings(broken, userKeys).ok);
    }

    // ---- 9. #191, the per-game half: game override > MS-DOS setting > recipe default ---------------------------
    // The device a launch uses, where it came from, and — the part a regression would hide — that the file
    // check, the seeded option and the message all follow the RESOLVED device. Every expected string below is
    // written out by hand; none is read back from the code under test.
    {
        const LaunchRecipe dos = LaunchRecipes::load(QStringLiteral("msdos"), QString());
        const RecipeCore* pure = dos.isNull() ? nullptr : LaunchRecipes::coreFor(dos, QStringLiteral("dosbox_pure"));
        CHECK(pure != nullptr);
        if (!pure) { std::fprintf(stderr, "DOSCONF had %d failure(s)\n", failures + 1); return 1; }
        const DosConf::MidiSpec& spec = pure->midi;
        // The shipped recipe names no default of its own: with nothing chosen anywhere, the core decides.
        CHECK(spec.defaultDevice.isEmpty());

        using DosConf::MidiSource;
        const QString none;
        const QString mt32 = QStringLiteral("mt32"), gm = QStringLiteral("gm");

        // (a) The order. The game's own choice beats the MS-DOS setting.
        {
            const DosConf::MidiChoice c = DosConf::resolveMidi(spec, mt32, gm);
            CHECK(c.device && c.device->id == mt32);
            CHECK(c.source == MidiSource::Game);
        }
        {
            const DosConf::MidiChoice c = DosConf::resolveMidi(spec, gm, mt32);
            CHECK(c.device && c.device->id == gm);
            CHECK(c.source == MidiSource::Game);
        }
        // With no game choice, the MS-DOS setting decides.
        {
            const DosConf::MidiChoice c = DosConf::resolveMidi(spec, none, gm);
            CHECK(c.device && c.device->id == gm);
            CHECK(c.source == MidiSource::System);
        }
        // Neither: no device (the shipped recipe has no default), and nothing to report.
        {
            const DosConf::MidiChoice c = DosConf::resolveMidi(spec, none, none);
            CHECK(c.device == nullptr);
            CHECK(c.source == MidiSource::None);
            CHECK(DosConf::midiReportLine(c).isEmpty());
        }
        // The game's choice is matched the way the MS-DOS setting is: trimmed, any case.
        CHECK(DosConf::resolveMidi(spec, QStringLiteral(" MT32 "), gm).source == MidiSource::Game);
        // A stored device the recipe does not declare is NOT an override: the MS-DOS setting still stands.
        {
            const DosConf::MidiChoice c = DosConf::resolveMidi(spec, QStringLiteral("sc55"), gm);
            CHECK(c.device && c.device->id == gm);
            CHECK(c.source == MidiSource::System);
        }

        // (b) The recipe default is the LAST layer. A hand-built spec (the same JSON shape a user recipe in
        // <data>/systems/recipes would carry) that names one.
        {
            const QJsonObject o = QJsonDocument::fromJson(QByteArray(
                "{\"option\":\"dosbox_pure_midi\",\"default\":\" GM \",\"devices\":["
                "{\"id\":\"gm\",\"label\":\"General MIDI (SoundFont)\",\"short\":\"General MIDI\","
                "\"value\":\"DOSBOX.SF2\",\"files\":[\"DOSBOX.SF2\"]},"
                "{\"id\":\"mt32\",\"label\":\"Roland MT-32\",\"value\":\"MT32_CONTROL.ROM\","
                "\"files\":[\"MT32_CONTROL.ROM\",\"MT32_PCM.ROM\"]}]}")).object();
            const DosConf::MidiSpec withDef = DosConf::midiFromJson(o);
            CHECK(withDef.defaultDevice == QLatin1String("gm"));   // trimmed and lowercased, like a stored id
            const DosConf::MidiChoice r = DosConf::resolveMidi(withDef, none, none);
            CHECK(r.device && r.device->id == gm);
            CHECK(r.source == MidiSource::Recipe);
            CHECK(DosConf::midiReportLine(r) == QLatin1String("MIDI: General MIDI (recipe default)"));
            // The MS-DOS setting beats the recipe default, and the game beats both.
            const DosConf::MidiChoice s = DosConf::resolveMidi(withDef, none, mt32);
            CHECK(s.device && s.device->id == mt32);
            CHECK(s.source == MidiSource::System);
            const DosConf::MidiChoice g = DosConf::resolveMidi(withDef, mt32, none);
            CHECK(g.device && g.device->id == mt32);
            CHECK(g.source == MidiSource::Game);
            // No "short": the report falls back to the full label.
            CHECK(DosConf::midiReportLine(g) == QLatin1String("MIDI: Roland MT-32 (this game)"));
        }

        // (c) "Default (use MS-DOS setting)" is the EMPTY value, and the empty value is no override at all:
        // for every MS-DOS setting, the game's Default resolves exactly as if the game had no record.
        {
            const QList<QPair<QString, QString>> choices = DosConf::gameMidiChoices(spec);
            CHECK(choices.size() == 3);
            if (choices.size() == 3)
            {
                CHECK(choices.at(0).first == QLatin1String("Default (use MS-DOS setting)"));
                CHECK(choices.at(0).second.isEmpty());
                CHECK(choices.at(1).first == QLatin1String("General MIDI (SoundFont)"));
                CHECK(choices.at(1).second == gm);
                CHECK(choices.at(2).first == QLatin1String("Roland MT-32"));
                CHECK(choices.at(2).second == mt32);
            }
            for (const QString& sys : { none, gm, mt32 })
            {
                const DosConf::MidiChoice d = DosConf::resolveMidi(spec, none, sys);
                CHECK(d.source == (sys.isEmpty() ? MidiSource::None : MidiSource::System));
                CHECK((d.device ? d.device->id : QString()) == sys);
            }
        }

        // (d) The file check follows the RESOLVED device. The folder holds only the soundfont; the MS-DOS
        // setting is General MIDI; this game says MT-32. The MT-32 is what is checked, its two ROMs are what
        // is named, nothing is seeded, and the soundfont that IS present does not leak in as a fallback.
        const QString folder = QStringLiteral("/opt/EverythingBox/system");
        const auto sf2Only = [](const QString& n) { return n == QLatin1String("DOSBOX.SF2"); };
        const auto romsOnly = [](const QString& n) {
            return n == QLatin1String("MT32_CONTROL.ROM") || n == QLatin1String("MT32_PCM.ROM");
        };
        const auto haveNothing = [](const QString&) { return false; };
        {
            const DosConf::MidiLaunch L = DosConf::midiLaunch(spec, mt32, gm, sf2Only, folder, QStringLiteral("Doom"));
            CHECK(L.choice.device && L.choice.device->id == mt32);
            CHECK(L.missing == (QStringList{ QStringLiteral("MT32_CONTROL.ROM"), QStringLiteral("MT32_PCM.ROM") }));
            CHECK(L.options.isEmpty());
            // Outside CHECK: an escape inside a stringified macro argument is what GCC has refused before.
            const QString expected = QString::fromUtf8(
                "Roland MT-32 needs files you have to supply: put MT32_CONTROL.ROM and MT32_PCM.ROM in the system "
                "folder (/opt/EverythingBox/system). EverythingBox can't download them \xE2\x80\x94 these ROMs are "
                "copyrighted. \xE2\x80\x9C" "Doom\xE2\x80\x9D will play through its default audio instead.");
            CHECK(L.message == expected);
            CHECK(L.report == QLatin1String("MIDI: MT-32 (this game)"));
        }
        // The mirror image: this game says General MIDI over an MT-32 setting, and only the ROMs are present.
        {
            const DosConf::MidiLaunch L = DosConf::midiLaunch(spec, gm, mt32, romsOnly, folder, QStringLiteral("Doom"));
            CHECK(L.missing == QStringList{ QStringLiteral("DOSBOX.SF2") });
            CHECK(L.options.isEmpty());
            CHECK(L.message.contains(QStringLiteral("put DOSBOX.SF2 in the system folder (/opt/EverythingBox/system)")));
            CHECK(!L.message.contains(QStringLiteral("MT32")));
            CHECK(L.report == QLatin1String("MIDI: General MIDI (this game)"));
        }
        // The same device, missing the same files, says the same thing whichever layer chose it: the per-game
        // MT-32 message IS the MS-DOS setting's message.
        {
            const DosConf::MidiLaunch byGame = DosConf::midiLaunch(spec, mt32, none, haveNothing, folder, QStringLiteral("Doom"));
            const DosConf::MidiLaunch bySys  = DosConf::midiLaunch(spec, none, mt32, haveNothing, folder, QStringLiteral("Doom"));
            CHECK(!byGame.message.isEmpty());
            CHECK(byGame.message == bySys.message);
            CHECK(byGame.report == QLatin1String("MIDI: MT-32 (this game)"));
            CHECK(bySys.report  == QLatin1String("MIDI: MT-32 (MS-DOS setting)"));
        }
        // Files present: the resolved device's option is seeded and there is no message.
        {
            const DosConf::MidiLaunch L = DosConf::midiLaunch(spec, mt32, gm, romsOnly, folder, QStringLiteral("Doom"));
            CHECK(L.missing.isEmpty());
            CHECK(L.message.isEmpty());
            CHECK(L.options.size() == 1);
            CHECK(L.options.value(QStringLiteral("dosbox_pure_midi")) == QLatin1String("MT32_CONTROL.ROM"));
        }
        {
            const DosConf::MidiLaunch L = DosConf::midiLaunch(spec, none, gm, sf2Only, folder, QString());
            CHECK(L.options.value(QStringLiteral("dosbox_pure_midi")) == QLatin1String("DOSBOX.SF2"));
            CHECK(L.report == QLatin1String("MIDI: General MIDI (MS-DOS setting)"));
        }
        // Nothing chosen anywhere: no device, no check, no seed, no message, no report (the pre-#191 launch).
        {
            const DosConf::MidiLaunch L = DosConf::midiLaunch(spec, none, none, haveNothing, folder, QStringLiteral("Doom"));
            CHECK(L.choice.device == nullptr);
            CHECK(L.missing.isEmpty() && L.options.isEmpty() && L.message.isEmpty() && L.report.isEmpty());
        }

        // (e) The composition GameLauncher::dosMidiSeed runs: the game's record from the #51 store, read by the
        // launch key, fed in as the game layer. A store that dropped the field would land on the setting.
        {
            const QString key = QStringLiteral("romlib:C:/roms/dos/Monkey Island");
            LaunchOpts::Override ov; ov.midiDevice = QStringLiteral("MT32");
            LaunchOpts::set(key, ov);
            const DosConf::MidiLaunch L = DosConf::midiLaunch(spec, LaunchOpts::get(key).midiDevice, gm, sf2Only,
                                                              folder, QStringLiteral("Monkey Island"));
            CHECK(L.report == QLatin1String("MIDI: MT-32 (this game)"));
            CHECK(L.message.contains(QStringLiteral("MT32_CONTROL.ROM and MT32_PCM.ROM")));
            LaunchOpts::reset(key);   // back to Default: the setting decides again
            CHECK(DosConf::midiLaunch(spec, LaunchOpts::get(key).midiDevice, gm, sf2Only, folder, QString()).report
                  == QLatin1String("MIDI: General MIDI (MS-DOS setting)"));
        }

        // (f) A non-DOS game has no MIDI entry. The chooser's list is the `midi` block of the recipe entry for the
        // core the game resolves to (the entry the launch reads), so only an MS-DOS game on dosbox_pure gets one.
        // C64 has a recipe without a `midi` block; SNES has no recipe at all; an MS-DOS game moved onto
        // dosbox_core, or onto a standalone DOSBox (no libretro core: ""), would be offered a lever the launch
        // ignores, so it is offered none.
        CHECK(!LaunchRecipes::midiSpecFor(dos, QStringLiteral("dosbox_pure")).isNull());
        CHECK(DosConf::gameMidiChoices(LaunchRecipes::midiSpecFor(dos, QStringLiteral("dosbox_pure"))).size() == 3);
        CHECK(DosConf::gameMidiChoices(LaunchRecipes::midiSpecFor(dos, QStringLiteral("dosbox_core"))).isEmpty());
        CHECK(DosConf::gameMidiChoices(LaunchRecipes::midiSpecFor(dos, QString())).isEmpty());
        const LaunchRecipe c64 = LaunchRecipes::load(QStringLiteral("c64"), QString());
        CHECK(!c64.isNull());
        CHECK(LaunchRecipes::midiSpecFor(c64, QStringLiteral("vice_x64")).isNull());
        CHECK(DosConf::gameMidiChoices(LaunchRecipes::midiSpecFor(c64, QStringLiteral("vice_x64"))).isEmpty());
        const LaunchRecipe snes = LaunchRecipes::load(QStringLiteral("snes"), QString());
        CHECK(DosConf::gameMidiChoices(LaunchRecipes::midiSpecFor(snes, QStringLiteral("snes9x"))).isEmpty());
    }

    if (failures == 0) std::printf("DOSCONF-OK\n");
    else               std::fprintf(stderr, "DOSCONF had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
