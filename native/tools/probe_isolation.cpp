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
    CHECK(!QFileInfo(iniPath).canonicalPath().startsWith(canonExe),
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

    if (failures == 0) { std::puts("ISOLATION-OK"); return 0; }
    std::fprintf(stderr, "ISOLATION: %d check(s) failed\n", failures);
    return 1;
}
