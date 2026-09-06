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

#include <QCoreApplication>
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

    if (failures == 0) std::printf("DOSCONF-OK\n");
    else               std::fprintf(stderr, "DOSCONF had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
