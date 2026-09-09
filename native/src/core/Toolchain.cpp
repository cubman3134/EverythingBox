// The half of the toolchain detector that looks at this machine (issue #248, increment c).
//
// SPLIT DELIBERATELY. Everything in Toolchain.h is pure and drives every combination in a probe; everything
// here starts a process or reads a file, and none of it can be asserted on a runner whose answer is whatever
// that runner happens to have installed. The one thing this file must never become is a second place where
// the DECISION is made — it only ever fills in a `Found` and hands it to `decide()`.
//
// IT ONLY READS. There is no branch here that downloads, installs, unpacks or writes anything except the
// cache of its own answer. That is the constraint #248 states in as many words, and it is enforced by the
// shape of the file rather than by a comment: the only network header this translation unit includes is none.
#include "Toolchain.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

#include "AppPaths.h"

namespace toolchain {
namespace {

// Run a program purely to read its banner. Merged channels because gcc and clang print theirs to stdout and
// some wrappers to stderr, and a short deadline because a program that does not answer `--version` in four
// seconds is not one we are going to build with — the point of the deadline is that a hung `cl.exe` on a
// broken install cannot hang the section that asked.
QString banner(const QString& program, const QStringList& args, int timeoutMs = 4000)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    // No inherited stdin. A tool that decides to prompt (an expired licence, a first-run notice) would
    // otherwise sit there until the deadline with nobody able to answer it.
    p.setStandardInputFile(QProcess::nullDevice());
    p.start(program, args);
    if (!p.waitForStarted(timeoutMs)) { p.kill(); p.waitForFinished(500); return QString(); }
    if (!p.waitForFinished(timeoutMs))
    {
        p.kill();
        p.waitForFinished(500);
        return QString();
    }
    return QString::fromLocal8Bit(p.readAll()).trimmed();
}

// `which`, plus the places a Windows installer puts a program when the user did not tick "add to PATH" —
// which is the default, and is therefore the common case rather than the exotic one. Returns the absolute
// path or "".
QString findProgram(const QString& name, const QStringList& extraDirs = {})
{
    const QString onPath = QStandardPaths::findExecutable(name);
    if (!onPath.isEmpty()) return onPath;
    const QString exe =
#if defined(Q_OS_WIN)
        name + QStringLiteral(".exe");
#else
        name;
#endif
    for (const QString& d : extraDirs)
    {
        const QString cand = QDir(d).filePath(exe);
        if (QFileInfo(cand).isExecutable() || QFileInfo::exists(cand)) return cand;
    }
    return QString();
}

#if defined(Q_OS_WIN)
// vswhere is the ONLY supported way to find a Visual Studio installation — Microsoft ships it with the
// installer itself, at a fixed path, precisely so that nobody has to guess at registry keys again. It is
// present whenever the VS Installer is, including for Build Tools with no IDE.
QString vswherePath()
{
    const QStringList roots{ qEnvironmentVariable("ProgramFiles(x86)"), qEnvironmentVariable("ProgramFiles") };
    for (const QString& r : roots)
    {
        if (r.isEmpty()) continue;
        const QString cand = r + QStringLiteral("/Microsoft Visual Studio/Installer/vswhere.exe");
        if (QFileInfo::exists(cand)) return cand;
    }
    return QString();
}
#endif

QString isoNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODate); }

}  // namespace

Found probe()
{
    Found f;
    f.probedAt = isoNow();

#if defined(Q_OS_WIN)
    // MSVC. `-requires …VC.Tools.x86.x64` is the component that actually contains cl.exe: a Visual Studio
    // with only the .NET workload installed is a real and common machine, and reporting it as "a C++
    // compiler is present" would send a build straight into a configure failure. `-products *` includes
    // Build Tools, which has no `Community`/`Professional` product id and is otherwise invisible.
    const QString vsw = vswherePath();
    if (!vsw.isEmpty())
    {
        const QStringList common{ QStringLiteral("-latest"), QStringLiteral("-products"), QStringLiteral("*"),
                                  QStringLiteral("-requires"),
                                  QStringLiteral("Microsoft.VisualStudio.Component.VC.Tools.x86.x64") };
        const QString path = banner(vsw, common + QStringList{ QStringLiteral("-property"),
                                                               QStringLiteral("installationPath") });
        if (!path.isEmpty())
        {
            f.msvcPath = path.section(QLatin1Char('\n'), 0, 0).trimmed();
            f.msvcVersion = firstVersionToken(
                banner(vsw, common + QStringList{ QStringLiteral("-property"),
                                                  QStringLiteral("installationVersion") }));
        }
    }
    // A developer command prompt, or a hand-placed toolset, puts cl.exe on PATH with no VS installer record
    // behind it. Present is present.
    if (!f.haveMsvc())
    {
        const QString cl = findProgram(QStringLiteral("cl"));
        // cl.exe has no --version: its banner is what it prints when handed nothing, and it exits non-zero
        // doing it. The banner is read for the version and its ABSENCE is not read as anything.
        if (!cl.isEmpty()) { f.msvcPath = cl; f.msvcVersion = firstVersionToken(banner(cl, {})); }
    }
#endif

    const QString clang = findProgram(QStringLiteral("clang++")).isEmpty()
                              ? findProgram(QStringLiteral("clang"))
                              : findProgram(QStringLiteral("clang++"));
    if (!clang.isEmpty())
    {
        f.clangPath = clang;
        f.clangVersion = firstVersionToken(banner(clang, { QStringLiteral("--version") }));
    }

    const QString gcc = findProgram(QStringLiteral("g++")).isEmpty() ? findProgram(QStringLiteral("gcc"))
                                                                    : findProgram(QStringLiteral("g++"));
    if (!gcc.isEmpty())
    {
        f.gccPath = gcc;
        f.gccVersion = firstVersionToken(banner(gcc, { QStringLiteral("--version") }));
    }

    QStringList cmakeDirs;
#if defined(Q_OS_WIN)
    for (const QString& r : { qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramFiles(x86)") })
        if (!r.isEmpty()) cmakeDirs << r + QStringLiteral("/CMake/bin");
#elif defined(Q_OS_MACOS)
    cmakeDirs << QStringLiteral("/Applications/CMake.app/Contents/bin")
              << QStringLiteral("/opt/homebrew/bin") << QStringLiteral("/usr/local/bin");
#endif
    const QString cmake = findProgram(QStringLiteral("cmake"), cmakeDirs);
    if (!cmake.isEmpty())
    {
        f.cmakePath = cmake;
        f.cmakeVersion = firstVersionToken(banner(cmake, { QStringLiteral("--version") }));
    }
    return f;
}

// ---- the cache -----------------------------------------------------------------------------------------
QString cachePath() { return AppPaths::dataDir() + QStringLiteral("/recomps/toolchain.json"); }

QString toJson(const Found& f)
{
    QJsonObject o;
    auto put = [&o](const char* k, const QString& v) { if (!v.isEmpty()) o.insert(QLatin1String(k), v); };
    put("msvc_version", f.msvcVersion);   put("msvc_path", f.msvcPath);
    put("clang_version", f.clangVersion); put("clang_path", f.clangPath);
    put("gcc_version", f.gccVersion);     put("gcc_path", f.gccPath);
    put("cmake_version", f.cmakeVersion); put("cmake_path", f.cmakePath);
    put("probed_at", f.probedAt);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

Found fromJson(const QString& json)
{
    Found f;
    const QJsonDocument d = QJsonDocument::fromJson(json.toUtf8());
    // A cache file that is not an object yields an EMPTY Found with no stamp, which `detect()` reads as "no
    // usable cache" and re-probes. A corrupt cache must never be able to assert that this machine has no
    // compiler — that is the difference between a stale answer and a wrong one.
    if (!d.isObject()) return f;
    const QJsonObject o = d.object();
    auto get = [&o](const char* k) { return o.value(QLatin1String(k)).toString().trimmed(); };
    f.msvcVersion = get("msvc_version");   f.msvcPath  = get("msvc_path");
    f.clangVersion = get("clang_version"); f.clangPath = get("clang_path");
    f.gccVersion = get("gcc_version");     f.gccPath   = get("gcc_path");
    f.cmakeVersion = get("cmake_version"); f.cmakePath = get("cmake_path");
    f.probedAt = get("probed_at");
    return f;
}

namespace {
// The process-lifetime copy. Two reasons it exists on top of the file: the section re-derives its rows on
// every draw, and a re-probe is four process launches.
Found  g_cached;
bool   g_haveCached = false;
}  // namespace

Found detect()
{
    if (g_haveCached) return g_cached;

    QFile f(cachePath());
    if (f.open(QIODevice::ReadOnly))
    {
        const Found onDisk = fromJson(QString::fromUtf8(f.readAll()));
        f.close();
        const QDateTime when = QDateTime::fromString(onDisk.probedAt, Qt::ISODate);
        // A stamp we cannot read is a cache we do not trust — and, importantly, a FUTURE stamp is too: a
        // clock that jumped forward once would otherwise pin a wrong answer for a week.
        if (when.isValid())
        {
            const qint64 age = when.secsTo(QDateTime::currentDateTimeUtc());
            if (age >= 0 && age < qint64(kCacheDays) * 24 * 3600)
            {
                g_cached = onDisk;
                g_haveCached = true;
                return g_cached;
            }
        }
    }

    g_cached = probe();
    g_haveCached = true;
    QDir().mkpath(QFileInfo(cachePath()).absolutePath());
    QFile out(cachePath());
    // A cache that cannot be written is not an error worth telling anybody about: the answer is already in
    // memory and the only cost is a re-probe next launch.
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write(toJson(g_cached).toUtf8());
    return g_cached;
}

void recheck()
{
    g_haveCached = false;
    g_cached = Found{};
    QFile::remove(cachePath());
}

}  // namespace toolchain
