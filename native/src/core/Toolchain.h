// WHAT THIS MACHINE CAN BUILD WITH (issue #248, increment c) — the toolchain detector behind the
// self-compiled recomp tier.
//
// WHY THERE IS A DETECTOR AT ALL. A self-compiled recomp is not a download: the port is produced here, by a
// recompiler plus a C/C++ compiler, out of a dump the user already owns. That means the build can fail before
// it starts, for a reason that has nothing to do with the game — this computer has no compiler on it. The one
// thing a person must never get is a Build button that spins for ninety seconds and then says "failed".
//
// THE DECISION IS MADE, AND IT IS NOT THE ONE RETCOMM MADE. RetComM downloads a toolchain pack
// (`build.toolchain`, `TechnicallyComputers/retcomm-toolchains`) so that a machine with nothing on it can
// still build. This app deliberately does NOT:
//   * we DETECT what is present and, when something is missing, say exactly what to install and link the
//     vendor's own installer;
//   * we do NOT download or install a compiler, an SDK or a toolchain pack;
//   * there is NO hidden bundled toolchain to fall back to, and there is no code path here that could acquire
//     one — `decide()` cannot install anything because it is a pure function over what was found, and the
//     detector below only ever READS.
// A compiler is a large, long-lived, system-wide thing that a person is entitled to choose, place and update
// themselves; silently planting one inside a media app's data directory is not a favour.
//
// PURE, AND QtCore ONLY, for the part that matters. `decide()` is (what was found + which OS) -> (a verdict +
// the sentences a person reads). It touches no process, no file and no clock, so probe_recompbuild drives
// every combination — including the ones this machine cannot be put into, like "no compiler at all" — and
// pins the sentences themselves. The half that actually looks at the machine is declared here and lives in
// Toolchain.cpp.
//
// THE SENTENCES ARE HERE, NOT IN THE UI. `recomps::emptyCatalogueMessage()` set the precedent: a user-visible
// string whose exact wording is the thing under test belongs beside the decision that produces it, so a probe
// and the screen cannot disagree about what a person was told. These are untranslated by consequence, which
// is the same trade that header already makes.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

namespace toolchain
{
    // Which OS the decision is being made for. An explicit parameter rather than #ifdef, so one probe run
    // drives all three answers — the Windows advice is wrong on Linux and nobody would notice on a Windows
    // runner.
    enum class Os { Windows, Linux, Macos };

    inline Os hostOs()
    {
#if defined(Q_OS_WIN)
        return Os::Windows;
#elif defined(Q_OS_MACOS)
        return Os::Macos;
#else
        return Os::Linux;
#endif
    }

    // WHAT WAS FOUND. Every field is "this is present, and here is what it said about itself" — never a
    // judgement. An empty version with a non-empty path is a real and expected state (the program is there and
    // did not answer `--version` in a shape we parse), and it counts as present: refusing a compiler because
    // its banner changed would be inventing a problem.
    struct Found
    {
        QString msvcVersion,  msvcPath;    // vswhere's installationVersion / installationPath, or cl.exe's dir
        QString clangVersion, clangPath;
        QString gccVersion,   gccPath;
        QString cmakeVersion, cmakePath;
        // When this answer was produced (ISO-8601, UTC). Carried so a cached result can be shown with its age
        // rather than presented as if it were live.
        QString probedAt;

        bool haveMsvc()  const { return !msvcPath.isEmpty(); }
        bool haveClang() const { return !clangPath.isEmpty(); }
        bool haveGcc()   const { return !gccPath.isEmpty(); }
        bool haveCMake() const { return !cmakePath.isEmpty(); }
        bool haveCompiler() const { return haveMsvc() || haveClang() || haveGcc(); }
    };

    enum class Verdict
    {
        Ready,        // a compiler and CMake are both here; a build may be offered
        NoCMake,      // a compiler is here and CMake is not
        NoCompiler,   // CMake is here and no compiler is
        NoToolchain,  // neither
    };

    // One thing the user has to go and get, with the vendor's OWN page. Not a package manager command dressed
    // up as a link: what is offered is where the thing comes from.
    struct Requirement
    {
        QString what;   // "CMake 3.20 or newer"
        QString url;    // "https://cmake.org/download/"
    };

    struct Decision
    {
        Verdict verdict = Verdict::NoToolchain;
        // The compiler a build would actually use, spelled for a person ("MSVC 17.9.34728.123"). Empty when
        // there is none. This is a REPORT of the preference below, not a second place it is decided.
        QString compiler;
        QString headline;                 // one sentence: where this machine stands
        QString detail;                   // what to do about it, naming exactly what to install
        QVector<Requirement> missing;     // empty iff verdict == Ready
        bool canBuild() const { return verdict == Verdict::Ready; }
    };

    // THE PREFERENCE, per OS, and it is a property of where the C runtime and the system headers come from
    // rather than of anybody's taste:
    //   * Windows — MSVC first. It is the only one of the three that arrives with the Windows SDK, and a
    //     clang on PATH is very often a clang with no MSVC headers behind it, which fails at the first
    //     `#include <windows.h>` rather than at configure time;
    //   * macOS — clang first: it IS the system compiler, and a Homebrew gcc there is the unusual case;
    //   * Linux — gcc first, for the same reason in reverse.
    // Returns "" when nothing was found.
    inline QString preferredCompiler(const Found& f, Os os)
    {
        auto spelled = [](const QString& name, const QString& version) {
            return version.isEmpty() ? name : name + QStringLiteral(" ") + version;
        };
        if (os == Os::Windows)
        {
            if (f.haveMsvc())  return spelled(QStringLiteral("MSVC"),  f.msvcVersion);
            if (f.haveClang()) return spelled(QStringLiteral("clang"), f.clangVersion);
            if (f.haveGcc())   return spelled(QStringLiteral("gcc"),   f.gccVersion);
            return QString();
        }
        if (os == Os::Macos)
        {
            if (f.haveClang()) return spelled(QStringLiteral("clang"), f.clangVersion);
            if (f.haveGcc())   return spelled(QStringLiteral("gcc"),   f.gccVersion);
            if (f.haveMsvc())  return spelled(QStringLiteral("MSVC"),  f.msvcVersion);
            return QString();
        }
        if (f.haveGcc())   return spelled(QStringLiteral("gcc"),   f.gccVersion);
        if (f.haveClang()) return spelled(QStringLiteral("clang"), f.clangVersion);
        if (f.haveMsvc())  return spelled(QStringLiteral("MSVC"),  f.msvcVersion);
        return QString();
    }

    // The compiler requirement for this OS, by the name the vendor uses for it. `cmakeRequirement` is one
    // line because CMake is one program everywhere; the compiler is three different acquisitions.
    inline Requirement compilerRequirement(Os os)
    {
        if (os == Os::Windows)
            return { QStringLiteral("Visual Studio Build Tools 2022, with the \"Desktop development with "
                                    "C++\" workload ticked"),
                     QStringLiteral("https://visualstudio.microsoft.com/visual-cpp-build-tools/") };
        if (os == Os::Macos)
            return { QStringLiteral("the Xcode command line tools (run: xcode-select --install)"),
                     QStringLiteral("https://developer.apple.com/xcode/resources/") };
        return { QStringLiteral("a C++ compiler — the build-essential package on Debian/Ubuntu, gcc-c++ on "
                                "Fedora"),
                 QStringLiteral("https://gcc.gnu.org/install/") };
    }

    inline Requirement cmakeRequirement()
    {
        return { QStringLiteral("CMake 3.20 or newer — tick \"Add CMake to the system PATH\" while installing"),
                 QStringLiteral("https://cmake.org/download/") };
    }

    // THE DECISION TABLE. Four inputs collapse to four verdicts, and each verdict carries the two sentences a
    // person reads. Written as one function with no early returns skipping the sentence, because the failure
    // this shape exists to prevent is a state that has a verdict and no explanation — which is what a
    // disabled button with no tooltip is.
    inline Decision decide(const Found& f, Os os)
    {
        Decision d;
        d.compiler = preferredCompiler(f, os);
        const bool haveC = f.haveCompiler();
        const bool haveK = f.haveCMake();

        if (haveC && haveK)
        {
            d.verdict = Verdict::Ready;
            d.headline = QStringLiteral("This computer can build it.");
            d.detail = QStringLiteral("Found %1 and CMake %2. The build runs here, on this machine, and "
                                      "nothing about your game leaves it.")
                           .arg(d.compiler,
                                f.cmakeVersion.isEmpty() ? QStringLiteral("(version not reported)")
                                                         : f.cmakeVersion);
            return d;
        }
        if (haveC && !haveK)
        {
            d.verdict = Verdict::NoCMake;
            d.missing << cmakeRequirement();
            d.headline = QStringLiteral("CMake is missing.");
            d.detail = QStringLiteral("%1 is here, but a recomp is put together with CMake and this computer "
                                      "does not have it. Install %2. EverythingBox will not install it for "
                                      "you — a compiler and its tools are yours to choose and update.")
                           .arg(d.compiler, cmakeRequirement().what);
            return d;
        }
        if (!haveC && haveK)
        {
            d.verdict = Verdict::NoCompiler;
            d.missing << compilerRequirement(os);
            d.headline = QStringLiteral("No C++ compiler was found.");
            d.detail = QStringLiteral("CMake is here, but there is nothing for it to compile with. Install %1. "
                                      "EverythingBox will not install one for you, and it has no compiler of "
                                      "its own hidden away to fall back on.")
                           .arg(compilerRequirement(os).what);
            return d;
        }
        d.verdict = Verdict::NoToolchain;
        d.missing << compilerRequirement(os) << cmakeRequirement();
        d.headline = QStringLiteral("This computer has no C++ compiler and no CMake.");
        d.detail = QStringLiteral("Building a recomp here needs both. Install %1, and %2. EverythingBox will "
                                  "not install either for you — they are large system-wide tools that are "
                                  "yours to choose, place and update.")
                       .arg(compilerRequirement(os).what, cmakeRequirement().what);
        return d;
    }

    // The one-line form for a row or a card heading. Separate from `headline` because a row has room for the
    // verdict and not for the paragraph.
    inline QString shortStatus(const Decision& d)
    {
        switch (d.verdict)
        {
            case Verdict::Ready:       return QStringLiteral("toolchain ready");
            case Verdict::NoCMake:     return QStringLiteral("CMake missing");
            case Verdict::NoCompiler:  return QStringLiteral("no C++ compiler");
            case Verdict::NoToolchain: return QStringLiteral("no build tools");
        }
        return QString();
    }

    // ---- version-banner parsing (pure) -----------------------------------------------------------------
    // The first dotted-numeric token of a `--version` banner. Every one of the four programs answers in a
    // different shape ("cmake version 3.29.2", "gcc (Ubuntu 13.2.0-…) 13.2.0", "clang version 18.1.3",
    // vswhere's bare "17.9.34728.123") and none of them is worth a per-program parser: what the card shows is
    // a version number, and the first one on the line is it.
    //
    // At least one dot is required, so "gcc (Ubuntu 13" cannot yield "13" out of the middle of a package
    // string, and a leading 'v' is dropped because some banners carry one.
    inline QString firstVersionToken(const QString& banner)
    {
        const QString line = banner.section(QLatin1Char('\n'), 0, 0);
        int i = 0;
        while (i < line.size())
        {
            if (!line.at(i).isDigit()) { ++i; continue; }
            int j = i;
            bool dot = false;
            while (j < line.size() && (line.at(j).isDigit() || line.at(j) == QLatin1Char('.')))
            {
                if (line.at(j) == QLatin1Char('.')) dot = true;
                ++j;
            }
            QString tok = line.mid(i, j - i);
            while (tok.endsWith(QLatin1Char('.'))) tok.chop(1);
            if (dot && tok.contains(QLatin1Char('.'))) return tok;
            i = j + 1;
        }
        return QString();
    }

    // ---- the half that looks at this machine (Toolchain.cpp) -------------------------------------------
    // BLOCKING: it runs `--version` on up to four programs, and on Windows it runs vswhere. Call it from a
    // worker thread; on the GUI thread it is a stall of a few hundred milliseconds every time somebody opens
    // the section, which is exactly why the answer is cached.
    Found probe();

    // The cached answer, re-probing only when there is none or it has aged out. This is what the UI calls.
    Found detect();

    // Throw the cache away, so the next detect() looks again. The "Check again" verb — a person who has just
    // installed the Build Tools in the other window must not have to restart the app to be believed.
    void recheck();

    // The cached answer's on-disk form. Split out and declared so probe_recompbuild can drive the round trip
    // (and a truncated / hand-mangled cache file) without running a single program.
    QString toJson(const Found& f);
    Found   fromJson(const QString& json);

    // Where the cached answer is kept, and how long it is trusted for. A week: installing a compiler is a
    // deliberate act, and the person who does it presses "Check again"; the expiry is for the machine that
    // was reimaged, not for the one that just changed.
    QString cachePath();
    inline constexpr int kCacheDays = 7;
}
