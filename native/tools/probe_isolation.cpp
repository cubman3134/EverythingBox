// Headless verification that a probe's data directory is ISOLATED from the app's (issue #42).
//
// On desktop AppPaths::dataDir() is the executable's own folder — the app is portable. Every probe binary
// lives in build/Release next to the GUI exe, so before this existed a probe run and a GUI run (or any
// throwaway app someone dropped in that folder) shared one everythingbox.ini, one addons/, one metadata/.
// Three separate debugging sessions in a single day were spent on suite failures that turned out to be that
// collision and not the branch under test.
//
// Every probe_* target is now compiled with EB_ISOLATED_DATA_DIR (native/CMakeLists.txt applies it to every
// target whose name starts with probe_, so a probe written next month gets it without its author knowing this
// file exists), which redirects AppPaths::dataDir() at a per-process scratch dir. That redirect is invisible
// when it works, which is exactly why it needs a test: if it silently stopped applying, every probe would go
// back to sharing build/Release and nothing would go red.
//
// Sections 7-10 (issue #341) are the other half of the same question: WHERE the platform puts the user's
// data when nothing is redirecting it. Linux gained a real answer there - the XDG data dir, because an
// AppImage's own folder is a read-only mount - so this probe now pins each platform's answer, the XDG rules
// including the empty-value case, the one-time migration out of an old portable install, and, most
// importantly, that EB_ISOLATED_DATA_DIR still beats all of it.
//
// Prints ISOLATION-OK; ISOLATION-FAIL <what> + non-zero on failure.
#include "AppBrand.h"
#include "AppPaths.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTextStream>
#include <cstdio>

static int failures = 0;
#define CHECK(c, w) do { if (!(c)) { std::fprintf(stderr, "ISOLATION-FAIL %s (line %d)\n", w, __LINE__); ++failures; } } while (0)

// The runner seeds an everythingbox.ini carrying this key, and an addons/<kJunkAddon> folder, into the exe's
// folder before running this probe — the "someone dropped a throwaway app in build/Release" case, made
// permanent. Both must be invisible from here. See run-headless-probes.sh.
static const QString kSentinelKey = QStringLiteral("probeIsolation/sentinel");
static const QString kJunkAddon   = QStringLiteral("probeisolationjunk");

static QByteArray fileBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Fixture writer for the migration checks (issue #341): makes the parent, then the file.
static void writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(bytes);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Child mode: print this process's data dir and exit. The parent uses it to prove that two probe
    // PROCESSES never share a directory (per-probe isolation, not just per-suite) and that the directory is
    // gone once the process is.
    if (app.arguments().contains(QStringLiteral("--print-data-dir")))
    {
        QTextStream out(stdout);
        out << AppPaths::dataDir() << "\n";
        return 0;
    }

    const QString exeDir = QCoreApplication::applicationDirPath();
    const QString data   = AppPaths::dataDir();

    // ---- 1. The dir is real, and it is NOT the exe's folder -------------------------------------------
    CHECK(!data.isEmpty(), "dataDir() is empty");
    CHECK(QFileInfo(data).isDir(), "dataDir() does not exist as a directory");

    const QString canonData = QFileInfo(data).canonicalFilePath();
    const QString canonExe  = QFileInfo(exeDir).canonicalFilePath();
    CHECK(!canonData.isEmpty() && !canonExe.isEmpty(), "canonical paths did not resolve");
    // The assertion this probe exists for. Failure mode is silent: everything still "works", it just works
    // in the folder the GUI and every other probe are using.
    CHECK(canonData != canonExe, "dataDir() IS applicationDirPath() — the probe is not isolated");
    CHECK(!canonData.startsWith(canonExe + QLatin1Char('/')),
          "dataDir() lives INSIDE applicationDirPath() — a GUI run still shares this tree");

    // ---- 2. Stable within the process ----------------------------------------------------------------
    // Every store caches its QSettings on first use; a dataDir() that changed between calls would give one
    // process two data dirs and make a probe's own writes unreadable to it.
    CHECK(AppPaths::dataDir() == data, "dataDir() is not stable across calls");

    // ---- 3. Writable ---------------------------------------------------------------------------------
    {
        const QString probeFile = data + QStringLiteral("/isolation-write-test");
        QFile f(probeFile);
        CHECK(f.open(QIODevice::WriteOnly), "dataDir() is not writable");
        f.write("x"); f.close();
        CHECK(fileBytes(probeFile) == QByteArray("x"), "write to dataDir() did not read back");
        QFile::remove(probeFile);
    }

    // ---- 4. Blind to state sitting in the exe's folder ------------------------------------------------
    // This is the contamination itself. The store line below is the one EVERY core unit uses verbatim
    // (ItemMarks.cpp, CloudMerge.cpp, ThemeChoice.cpp, ...): dataDir() + "/" + kIniFile. If the runner's
    // seeded sentinel is readable through it, the probe is reading the exe folder's ini.
    const QString iniPath = data + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);
    // absolutePath(), not canonicalPath(): the ini does not exist yet on a first run, and canonicalPath()
    // answers empty for a path that is not on disk — which would quietly make this check pass for free.
    CHECK(!QFileInfo(iniPath).absolutePath().startsWith(canonExe),
          "the store's ini resolves into applicationDirPath()");
    {
        QSettings s(iniPath, QSettings::IniFormat);
        s.sync();
        CHECK(!s.contains(kSentinelKey),
              "a key seeded in the EXE FOLDER's everythingbox.ini is visible through this probe's store");
    }
    // ...and the same for the addons/ half: the throwaway app's add-ons must not appear in this probe's
    // add-on root (AddonManager resolves it as dataDir() + "/addons").
    CHECK(!QDir(data + QStringLiteral("/addons")).exists(kJunkAddon),
          "an add-on seeded in the EXE FOLDER's addons/ is visible through this probe's data dir");

    // ---- 5. Writing through the store does not touch the exe's folder --------------------------------
    // The other direction of the same bug: a probe's writes must not land in, or modify, the folder the GUI
    // reads. Compare the exe folder's ini byte-for-byte across a real write+sync.
    {
        const QString exeIni    = exeDir + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);
        const bool    hadExeIni = QFileInfo::exists(exeIni);
        const QByteArray before = fileBytes(exeIni);

        {
            QSettings s(iniPath, QSettings::IniFormat);
            s.setValue(QStringLiteral("probeIsolation/wrote"), QStringLiteral("yes"));
            s.sync();
        }
        CHECK(QFileInfo::exists(iniPath), "the probe's own ini was not created under dataDir()");
        CHECK(QFileInfo::exists(exeIni) == hadExeIni,
              "writing through the probe's store created an ini in applicationDirPath()");
        CHECK(fileBytes(exeIni) == before,
              "writing through the probe's store modified the ini in applicationDirPath()");
    }

    // ---- 6. Two probe PROCESSES get two different dirs, and the dir dies with the process --------------
    // Per-probe isolation, not just per-suite: two probes in one suite run must not fight over one ini. The
    // child is this same binary, so this holds for every probe by construction.
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("EB_PROBE_DATA_DIR"));       // an inherited pin would make them share...
        env.remove(QStringLiteral("EB_PROBE_DATA_DIR_KEEP"));  // ...and an inherited keep would skip cleanup

        QProcess child;
        child.setProcessEnvironment(env);
        child.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--print-data-dir") });
        CHECK(child.waitForStarted(10000), "could not start the child probe process");
        CHECK(child.waitForFinished(30000), "the child probe process did not finish");
        CHECK(child.exitCode() == 0, "the child probe process exited non-zero");

        const QString childDir = QString::fromLocal8Bit(child.readAllStandardOutput()).trimmed();
        CHECK(!childDir.isEmpty(), "the child probe printed no data dir");
        CHECK(childDir != data, "two probe processes were handed the SAME data dir");
        CHECK(QFileInfo(childDir).canonicalFilePath() != canonExe,
              "the child probe's data dir IS applicationDirPath()");
        // Cleanup: the scratch dir is removed when the process's statics are torn down, so by the time
        // waitForFinished() returned it must be gone. Without this the suite would silently accrete a
        // directory per probe per run.
        CHECK(!QFileInfo::exists(childDir), "the child probe's data dir survived the process");
    }

    // ---- 7. Each platform's own answer, from inside an isolated build --------------------------------
    // AppPaths::platformDataDir() is the platform branch alone - no isolation, no directory creation - and
    // dataDir() calls it rather than repeating it, so these assertions are about the real thing. Without
    // this split "Windows and macOS did not change" would be a claim nothing here could test, because
    // dataDir() in a probe is always the scratch dir.
    {
#if defined(Q_OS_LINUX)
        // Issue #341. The Linux answer is the XDG data dir, NOT the executable's folder: inside an AppImage
        // that folder is a read-only mount and nothing the app owns can be written at all.
        const QString platform = AppPaths::platformDataDir();
        CHECK(platform == AppPaths::UserData::xdgDataDir(),
              "platformDataDir() on Linux is not the XDG user data dir");
        CHECK(platform.endsWith(QLatin1Char('/') + QString::fromLatin1(AppBrand::kDisplayName)),
              "the Linux user data dir is not named after the app");
        CHECK(QDir::cleanPath(platform) != canonExe && !QDir::cleanPath(platform).startsWith(canonExe + QLatin1Char('/')),
              "the Linux user data dir resolves into applicationDirPath() - an AppImage cannot write there");
#else
        // The pin the brief asks for: this branch is the one that must be byte-identical. If it ever stops
        // being applicationDirPath(), every Windows and macOS install's portable data directory has moved
        // and nothing else would say so.
        CHECK(AppPaths::platformDataDir() == QCoreApplication::applicationDirPath(),
              "platformDataDir() is no longer applicationDirPath() - the portable desktop data dir moved");
#endif
    }

    // ---- 8. The XDG rules, as a table -----------------------------------------------------------------
    // xdgDataDirFor() is a pure function and is compiled on every platform on purpose, so the Windows gate
    // tests the same code the Linux build runs. The empty case is the one worth spelling out: the spec says
    // an EMPTY XDG_DATA_HOME means unset, and honouring it as a path would answer "/EverythingBox" - the
    // filesystem root, which a normal user cannot write, i.e. issue #341 again in a new place.
    {
        const QString appFolder = QString::fromLatin1(AppBrand::kDisplayName);
        const QString home      = QStringLiteral("/home/eb");
        const QString fallback  = home + QStringLiteral("/.local/share/") + appFolder;

        CHECK(AppPaths::UserData::xdgDataDirFor(QStringLiteral("/xdg/data"), home, appFolder)
                  == QStringLiteral("/xdg/data/") + appFolder,
              "a set XDG_DATA_HOME was not honoured");
        CHECK(AppPaths::UserData::xdgDataDirFor(QString(), home, appFolder) == fallback,
              "an unset XDG_DATA_HOME did not fall back to ~/.local/share");
        CHECK(AppPaths::UserData::xdgDataDirFor(QStringLiteral(""), home, appFolder) == fallback,
              "an EMPTY XDG_DATA_HOME was treated as a path - the spec says empty means unset");
        CHECK(AppPaths::UserData::xdgDataDirFor(QStringLiteral("/xdg/data/"), home, appFolder)
                  == QStringLiteral("/xdg/data/") + appFolder,
              "a trailing slash in XDG_DATA_HOME produced a second, different directory");
        CHECK(AppPaths::UserData::xdgDataDirFor(QStringLiteral("/xdg/data//"), home, appFolder)
                  == QStringLiteral("/xdg/data/") + appFolder,
              "repeated trailing slashes in XDG_DATA_HOME produced a doubled separator");
        CHECK(AppPaths::UserData::xdgDataDirFor(QStringLiteral("relative/dir"), home, appFolder) == fallback,
              "a RELATIVE XDG_DATA_HOME was honoured - the library would follow the working directory");
        CHECK(AppPaths::UserData::xdgDataDirFor(QStringLiteral("/"), home, appFolder)
                  == QStringLiteral("/") + appFolder,
              "XDG_DATA_HOME=/ produced a doubled leading slash");

        // ...and reading the environment answers WHERE, without creating anything. A lookup with a side
        // effect would mean any tool that merely asks leaves a directory in the user's home.
        const bool       hadXdg   = qEnvironmentVariableIsSet("XDG_DATA_HOME");
        const QByteArray savedXdg = qgetenv("XDG_DATA_HOME");
        const QByteArray probeXdg = "/eb341-xdg-that-does-not-exist";
        qputenv("XDG_DATA_HOME", probeXdg);
        CHECK(AppPaths::UserData::xdgDataDir()
                  == QString::fromLatin1(probeXdg) + QLatin1Char('/') + appFolder,
              "xdgDataDir() did not read XDG_DATA_HOME from the environment");
        CHECK(!QFileInfo::exists(QString::fromLatin1(probeXdg)),
              "asking where the user data dir is CREATED it");
        if (hadXdg) qputenv("XDG_DATA_HOME", savedXdg); else qunsetenv("XDG_DATA_HOME");
    }

    // ---- 9. The one-time migration out of an old portable install -------------------------------------
    // Everything here runs on every platform (the fixture is two directories under this probe's own scratch
    // dir), because the migration is where the data is at risk and the Windows gate is the only gate that
    // runs before CI.
    {
        const QString root = data + QStringLiteral("/migration");
        const QString from = root + QStringLiteral("/portable");
        const QString to   = root + QStringLiteral("/userdata");
        const QString ini  = QString::fromLatin1(AppBrand::kIniFile);

        QDir().mkpath(from + QStringLiteral("/saves/nes"));
        QDir().mkpath(from + QStringLiteral("/roms"));
        writeFile(from + QLatin1Char('/') + ini, QByteArrayLiteral("[probe]\nmigrated=yes\n"));
        writeFile(from + QStringLiteral("/saves/nes/game.srm"), QByteArrayLiteral("SAVE"));
        writeFile(from + QStringLiteral("/roms/big.rom"), QByteArrayLiteral("ROM"));
        writeFile(from + QStringLiteral("/unrelated.bin"), QByteArrayLiteral("NOPE"));

        // The AppImage case, staged: the source files are READ-ONLY, exactly as they are on a squashfs
        // payload. QFile::copy carries the source's permissions to the destination, so without the explicit
        // re-permission in copyFileIfAbsent the migrated ini would land unwritable and the fix would have
        // moved the failure rather than removed it.
        QFile::setPermissions(from + QLatin1Char('/') + ini, QFile::ReadOwner);
        QFile::setPermissions(from + QStringLiteral("/saves/nes/game.srm"), QFile::ReadOwner);

        const QString prepared = AppPaths::UserData::prepare(to, from);
        CHECK(prepared == to, "prepare() did not return the directory it was asked for");
        CHECK(QFileInfo(to).isDir(), "prepare() did not create the user data directory");
        {
            QFile f(to + QStringLiteral("/prepare-write-test"));
            CHECK(f.open(QIODevice::WriteOnly), "the prepared user data directory is not writable");
            f.write("x"); f.close();
            QFile::remove(f.fileName());
        }

        CHECK(fileBytes(to + QLatin1Char('/') + ini) == QByteArrayLiteral("[probe]\nmigrated=yes\n"),
              "the settings file did not migrate out of the portable directory");
        CHECK(fileBytes(to + QStringLiteral("/saves/nes/game.srm")) == QByteArrayLiteral("SAVE"),
              "a nested save did not migrate (the copy is not recursive)");
        // Bulk content is deliberately NOT copied: it can be tens of gigabytes and this runs synchronously
        // during startup. An entry outside the list is not copied either.
        CHECK(!QFileInfo::exists(to + QStringLiteral("/roms/big.rom")),
              "bulk content was copied - first launch would stall duplicating the user's library");
        CHECK(!QFileInfo::exists(to + QStringLiteral("/unrelated.bin")),
              "an entry outside the migration list was copied");

        // A copy out of a read-only source must be writable where it lands.
        {
            QFile f(to + QLatin1Char('/') + ini);
            CHECK(f.open(QIODevice::Append),
                  "the migrated settings file is READ-ONLY at its destination - a copy out of an AppImage's "
                  "read-only payload kept the source's permissions");
            f.close();
        }

        // NEVER DELETES. The source is often the only copy, and inside an AppImage it cannot be touched
        // anyway; a migration that removes is a migration that can lose.
        CHECK(fileBytes(from + QLatin1Char('/') + ini) == QByteArrayLiteral("[probe]\nmigrated=yes\n"),
              "the migration modified or removed the SOURCE settings file");
        CHECK(QFileInfo::exists(from + QStringLiteral("/saves/nes/game.srm")),
              "the migration removed the source save");

        // ONCE, and by the stamp rather than by "is it empty?" - otherwise the next launch resurrects a file
        // the user deliberately deleted.
        CHECK(QFileInfo::exists(AppPaths::UserData::migrationStampPath(to)),
              "no migration stamp was written - the migration would run again on every launch");
        QFile::remove(to + QLatin1Char('/') + ini);
        CHECK(AppPaths::UserData::migrateFromPortableDir(from, to) == 0,
              "the migration ran a SECOND time");
        CHECK(!QFileInfo::exists(to + QLatin1Char('/') + ini),
              "the second migration resurrected a file the user had deleted");

        // The same directory spelled twice is not two directories, and must not be stamped as migrated.
        {
            const QString same = root + QStringLiteral("/same");
            QDir().mkpath(same);
            CHECK(AppPaths::UserData::migrateFromPortableDir(same, same + QStringLiteral("/.")) == 0,
                  "a directory was migrated into itself");
            CHECK(!QFileInfo::exists(AppPaths::UserData::migrationStampPath(same)),
                  "a self-migration wrote a stamp claiming a migration had happened");
        }

        // No source at all (a fresh install, or an app dir this process cannot read) still has to yield a
        // working writable data directory - the whole point of the change.
        {
            const QString to2 = root + QStringLiteral("/nosource");
            AppPaths::UserData::prepare(to2, root + QStringLiteral("/there-is-no-such-dir"));
            CHECK(QFileInfo(to2).isDir(), "prepare() with an absent source did not create the data dir");
            QFile f(to2 + QStringLiteral("/write-test"));
            CHECK(f.open(QIODevice::WriteOnly), "prepare() with an absent source produced an unwritable dir");
            f.write("x"); f.close();
        }

        // Leave nothing read-only behind: on Windows a read-only file survives removeRecursively(), and the
        // scratch directory this probe was handed would then outlive the process it belongs to.
        QFile::setPermissions(from + QLatin1Char('/') + ini, QFile::ReadOwner | QFile::WriteOwner);
        QFile::setPermissions(from + QStringLiteral("/saves/nes/game.srm"),
                              QFile::ReadOwner | QFile::WriteOwner);
    }

    // ---- 10. Isolation still beats the platform answer -------------------------------------------------
    // The single most dangerous regression available in this file. dataDir()'s EB_ISOLATED_DATA_DIR branch
    // sits ahead of the platform branch; if it ever stopped doing so, every probe in the suite would write
    // into the REAL user data directory - on Linux that branch also creates directories in the user's home
    // and migrates files into them. Check 1 above says the probe's dir is not the exe folder, which was the
    // whole answer while the exe folder WAS the platform answer; on Linux it no longer is, so the precedence
    // needs its own assertion or it would go unchecked on exactly the platform that just gained a new path.
    {
        const QString platform = AppPaths::platformDataDir();
        CHECK(data != platform,
              "dataDir() returned the PLATFORM data dir - EB_ISOLATED_DATA_DIR has stopped winning");
        CHECK(!QDir::cleanPath(data).startsWith(QDir::cleanPath(platform) + QLatin1Char('/')),
              "the probe's data dir lives INSIDE the real user data dir");
        CHECK(!QFileInfo::exists(platform + QStringLiteral("/isolation-write-test")),
              "this probe's write landed in the real user data dir");
    }

    if (failures == 0) { std::puts("ISOLATION-OK"); return 0; }
    std::fprintf(stderr, "ISOLATION: %d check(s) failed\n", failures);
    return 1;
}
