#pragma once
#include "AppBrand.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <cstdio>
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
#include <QStandardPaths>
#endif
#ifdef EB_ISOLATED_DATA_DIR
#include <QRandomGenerator>
#endif

// The app's writable base directory. On Windows and macOS this is the executable's own folder - the app is
// portable, so everythingbox.ini, cores/, saves/, states/, downloads/, addons/, ... all live next to
// EverythingBox. On Android and iOS the executable directory isn't writable, so use the app's private data
// location instead. Everything that builds a path off the app dir goes through here, so a platform only
// changes this function.
//
// LINUX (issue #341) is the third answer, and the reason it exists is worth stating: the released Linux
// artefact is an AppImage, whose payload is mounted READ-ONLY. applicationDirPath() inside one is a path
// within that mount, so a portable data dir there is not merely unconventional - it cannot be written at
// all, and the app remembers nothing between runs: no settings, no resume points, no save states, no
// installed add-ons. Linux therefore separates the two roles the app dir used to play. Where the app's
// BUNDLED files live (themes2/, addons/, gamecontrollerdb.txt - shipped beside the binary by #339, read
// only) stays applicationDirPath(); where the USER's data lives becomes the XDG data directory. That split
// is correct for any Linux install, packaged or not - writing a user's library into the application
// directory is wrong whether or not the directory happens to be writable - and the AppImage is only its
// sharpest instance. Windows and macOS are deliberately untouched: this is one branch of one function.
//
// PROBES (issue #42). "The executable's own folder" is also build/Release, where every probe binary is built
// next to the GUI exe - so one everythingbox.ini, one addons/, one metadata/ was shared between the app, the
// forty-odd probes, and anything a developer happened to drop in that folder. Three separate debugging
// sessions in a single day went into suite failures that turned out to be that collision rather than the
// branch under test, and a gate that cries wolf teaches people to discount it.
//
// So every probe_* target is compiled with EB_ISOLATED_DATA_DIR, which redirects dataDir() at a scratch
// directory created per PROCESS and removed when that process ends. Two probes in one suite run therefore
// cannot fight over one ini, and nothing sitting in build/Release can change any probe's result. The define
// is applied by NAME over every probe target at the bottom of native/CMakeLists.txt rather than at each
// add_executable, so a probe written next month is isolated without its author knowing this comment exists.
// The app target never gets it: production behaviour here is byte-for-byte what it was. probe_isolation
// asserts the result, because a redirect that silently stopped applying looks exactly like one that works.
namespace AppPaths
{
#ifdef EB_ISOLATED_DATA_DIR
namespace Isolated
{
    // Created on the first dataDir() call and removed when the process's statics are torn down. The ordering
    // is in our favour and not by luck: every store's `static QSettings s(AppPaths::dataDir() + ...)` has to
    // CALL dataDir() to compute its constructor argument, so this object finishes construction first and is
    // therefore destroyed last - the settings files are flushed and closed before the directory goes.
    //
    // Three escape hatches, all opt-in and all named for what they are:
    //   EB_PROBE_DATA_DIR      - use this exact directory instead of making one. It belongs to whoever set
    //                            the variable, so it is never removed here. For handing a child process the
    //                            parent's dir, or pointing a probe at a directory you want to keep an eye on.
    //   EB_PROBE_DATA_DIR_KEEP - keep the scratch directory after the run, to see what a failing probe
    //                            actually wrote.
    //   EB_PROBE_SCRATCH_ROOT  - the parent to create the per-process directory under. run-headless-probes.sh
    //                            sets it to one directory per suite run and deletes that directory afterwards,
    //                            so even a probe that dies before its destructor runs leaves nothing behind.
    //
    // The first two are for running a probe BY HAND. run-headless-probes.sh unsets both before it starts, on
    // purpose: EB_PROBE_DATA_DIR left in a shell profile would hand every probe in every suite run the same
    // never-cleaned directory, which is issue #42 restored in full and invisible to every gate here.

    // mkpath() returning false is not hypothetical - a full or unwritable temp volume does it - and the
    // failure is otherwise invisible here: dataDir() hands back a path that does not exist, and every store
    // layered on top then fails in its own obscure way ("cannot open settings", an empty cache, a save that
    // never lands), one confusing symptom per probe. Say it once, at the source, on stderr: the runner
    // captures each probe's stderr into the suite log, so the real cause is in front of whoever reads it.
    // stderr rather than qFatal() deliberately - aborting the process on Windows can raise a crash dialog
    // that would hang an unattended suite run, which is a worse failure mode than a loud line.
    inline bool mkpathOrComplain(const QString &p)
    {
        if (QDir().mkpath(p))
            return true;
        std::fprintf(stderr,
                     "EB_ISOLATED_DATA_DIR: could not create the probe data directory '%s'. Every store this "
                     "probe opens will now fail; check free space and permissions on the temp volume.\n",
                     p.toLocal8Bit().constData());
        return false;
    }

    struct Scratch
    {
        QString path;
        bool    owned = false;   // false => the caller handed us the path; deleting it is not ours to do

        Scratch()
        {
            const QByteArray pinned = qgetenv("EB_PROBE_DATA_DIR");
            if (!pinned.isEmpty())
            {
                path = QDir::fromNativeSeparators(QString::fromLocal8Bit(pinned));
                mkpathOrComplain(path);
                return;
            }
            const QByteArray rootEnv = qgetenv("EB_PROBE_SCRATCH_ROOT");
            const QString    root    = rootEnv.isEmpty()
                                     ? QDir::tempPath() + QStringLiteral("/everythingbox-probe")
                                     : QDir::fromNativeSeparators(QString::fromLocal8Bit(rootEnv));
            // The pid alone is not enough: process ids get recycled, and a directory leaked by a crashed run
            // would then be adopted by a later probe as its "clean" data dir.
            path = root + QStringLiteral("/p%1-%2")
                              .arg(QCoreApplication::applicationPid())
                              .arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
            mkpathOrComplain(path);
            owned = true;
        }

        ~Scratch()
        {
            if (owned && !qEnvironmentVariableIsSet("EB_PROBE_DATA_DIR_KEEP"))
                QDir(path).removeRecursively();
        }
    };
}
#endif

    // ---- The user's own data directory (issue #341) ---------------------------------------------------
    //
    // Compiled on EVERY platform on purpose, though only the Linux branch of dataDir() calls it: these are
    // pure functions of their arguments - no environment, no platform API - so the Windows/MSVC gate, which
    // is the only gate that runs before CI, exercises the same code the Linux build depends on. A
    // Linux-only helper would have shipped with its first execution happening on a machine nobody here can
    // run, which is precisely how this issue got as far as a release.
    namespace UserData
    {
        // $XDG_DATA_HOME/<appFolder>, falling back to <homeDir>/.local/share/<appFolder>.
        //
        // The spec's words are exact and are honoured literally: "If $XDG_DATA_HOME is either not set or
        // EMPTY, a default equal to $HOME/.local/share should be used." An empty value therefore means
        // UNSET. Treating it as a path would resolve to "/EverythingBox" - the filesystem root, unwritable
        // for a normal user - which is this very issue in a new location, and which would look exactly like
        // a working fix on any machine where the variable is never empty.
        //
        // A RELATIVE value is invalid per the same spec (these variables "must be absolute") and is ignored
        // rather than honoured: resolving it would anchor the user's library to whatever the process's
        // working directory happened to be at startup, so the same install would have a different library
        // depending on how it was launched. Qt's own XDG handling makes the same choice.
        inline QString xdgDataDirFor(const QString& xdgDataHome, const QString& homeDir,
                                     const QString& appFolder)
        {
            QString base = xdgDataHome;
            if (base.isEmpty() || !base.startsWith(QLatin1Char('/')))
                base = homeDir + QStringLiteral("/.local/share");
            // "$XDG_DATA_HOME/" and "$XDG_DATA_HOME" name ONE directory; without this they name two, and an
            // environment that gained a trailing slash between runs would send the app looking for its data
            // in a path it has never written.
            while (base.size() > 1 && base.endsWith(QLatin1Char('/')))
                base.chop(1);
            // The separator is added only if the base does not already end in one, which it does in exactly
            // one case: XDG_DATA_HOME="/". Appending unconditionally would answer "//EverythingBox", and a
            // leading double slash is implementation-defined in POSIX rather than the root directory.
            return (base.endsWith(QLatin1Char('/')) ? base : base + QLatin1Char('/')) + appFolder;
        }

        // The same answer, read from this process's environment. Creates nothing.
        //
        // kDisplayName rather than kName so that this directory and the per-user crash log main.cpp already
        // writes (GenericDataLocation + kDisplayName, i.e. ~/.local/share/EverythingBox) are the SAME
        // directory by construction instead of by two spellings that happen to match today. main.cpp
        // collapses its two crash-log paths when they name one file, so the two must not drift apart.
        inline QString xdgDataDir()
        {
            return xdgDataDirFor(qEnvironmentVariable("XDG_DATA_HOME"), QDir::homePath(),
                                 QString::fromLatin1(AppBrand::kDisplayName));
        }

        // What a migration moves out of an old portable install - and, just as deliberately, what it does
        // not.
        //
        // Named entries rather than "everything beside the executable": the app dir also holds the binary,
        // Qt's libraries and plugins and, inside an AppImage, the entire read-only payload. Copying it
        // wholesale would duplicate hundreds of megabytes of runtime into the user's home on first launch.
        //
        // The list is the small, irreplaceable state: the settings file, the per-library index files, and
        // the stores whose loss a user would actually feel (emulator saves, save states, screenshots,
        // cheats). BULK CONTENT is left where it is on purpose - roms/, cores/, downloads/, music/, books/,
        // metadata/ and friends run to tens of gigabytes, this copy happens synchronously on the first
        // dataDir() call (i.e. during startup, before a window exists), and each of them is either still
        // readable where it is or re-fetched on demand. themes2/ and addons/ are absent for a different
        // reason: on Linux AssetBootstrap::run() now seeds those from the app dir with its own
        // copy-if-absent + version-stamp semantics, which is strictly better than a one-shot copy because a
        // later release's stock themes still reach an install that already has them.
        inline QStringList migratableEntries()
        {
            return { QString::fromLatin1(AppBrand::kIniFile),
                     QStringLiteral("saves-meta.json"),
                     QStringLiteral("subtitles.json"),
                     QStringLiteral("musicindex.json"),
                     QStringLiteral("bookindex.json"),
                     QStringLiteral("audiobookindex.json"),
                     QStringLiteral("localresolve.json"),
                     QStringLiteral("ps3-updates.json"),
                     QStringLiteral("saves"),
                     QStringLiteral("states"),
                     QStringLiteral("screenshots"),
                     QStringLiteral("cheats") };
        }

        // One file, copied only when nothing is at the destination. Returns true iff it wrote one.
        inline bool copyFileIfAbsent(const QString& src, const QString& dst)
        {
            if (QFileInfo::exists(dst) || !QFileInfo(src).isFile())
                return false;
            QDir().mkpath(QFileInfo(dst).absolutePath());
            if (!QFile::copy(src, dst))
                return false;
            // QFile::copy carries the SOURCE's permissions across, and the source here is very often
            // read-only: an AppImage's payload is a read-only squashfs, so a file copied out of one arrives
            // with no write bit set. The user's own settings file would then be unwritable in its new home -
            // a fix that moves the failure instead of removing it. OR-ed onto what the copy already has, so
            // an executable or group-read bit is preserved rather than quietly stripped.
            QFile::setPermissions(dst, QFile::permissions(dst) | QFile::ReadOwner | QFile::WriteOwner);
            return true;
        }

        // Recursive copy-if-absent, decided per FILE. Returns how many files it wrote.
        inline int copyTreeIfAbsent(const QString& srcDir, const QString& dstDir)
        {
            if (!QDir(srcDir).exists())
                return 0;
            int wrote = 0;
            QDirIterator it(srcDir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);
            while (it.hasNext())
            {
                const QString srcFile = it.next();
                const QString rel     = QDir(srcDir).relativeFilePath(srcFile);
                if (copyFileIfAbsent(srcFile, dstDir + QLatin1Char('/') + rel))
                    ++wrote;
            }
            return wrote;
        }

        // The record that the one-time migration has run. Its presence, not the state of the two
        // directories, is what makes it one-time.
        inline QString migrationStampPath(const QString& to)
        {
            return to + QStringLiteral("/.migrated-from-appdir");
        }

        // Move a COPY of an old portable install's data into the user's data directory, once, without ever
        // deleting anything at the source. Returns the number of files copied.
        //
        // NEVER DELETES, for two reasons that each stand alone: a migration that deletes is a migration that
        // can lose (a copy interrupted by a full disk would take the only copy with it), and the source is
        // frequently READ-ONLY anyway - inside an AppImage a copy is the only operation available at all.
        // The original stays exactly where the user left it.
        //
        // ONCE, via a stamp file rather than "is the destination empty?": copy-if-absent on its own would
        // resurrect, on the next launch, a file the user had deliberately deleted here. Once the stamp
        // exists this is a no-op whatever the two directories now contain.
        inline int migrateFromPortableDir(const QString& from, const QString& to)
        {
            if (from.isEmpty() || to.isEmpty())
                return 0;
            // The same directory spelled two ways is not two directories. cleanPath, not canonicalFilePath:
            // the destination may not exist yet, and canonicalFilePath() answers EMPTY for a path that is
            // not on disk - which would compare two different directories equal and skip a real migration.
            if (QDir::cleanPath(from) == QDir::cleanPath(to))
                return 0;
            if (QFileInfo::exists(migrationStampPath(to)))
                return 0;
            if (!QDir(from).exists())
                return 0;   // nothing to migrate FROM: leave the stamp unwritten, ask again next launch

            int copied = 0;
            for (const QString& name : migratableEntries())
            {
                const QString   src = from + QLatin1Char('/') + name;
                const QString   dst = to + QLatin1Char('/') + name;
                const QFileInfo si(src);
                if (si.isDir())
                    copied += copyTreeIfAbsent(src, dst);
                else if (si.isFile() && copyFileIfAbsent(src, dst))
                    ++copied;
            }

            QFile stamp(migrationStampPath(to));
            if (stamp.open(QIODevice::WriteOnly | QIODevice::Truncate))
                stamp.write(QDir::cleanPath(from).toUtf8());
            return copied;
        }

        // First use of the user data directory: create it, then migrate an old portable install into it
        // once. Returns the directory either way - a failed mkpath is reported, not swallowed and not
        // fatal, exactly as the isolated scratch directory reports its own: every store layered on top
        // would otherwise fail in its own obscure way, one confusing symptom at a time, and an app that
        // cannot save its settings is still more useful than one that refuses to start.
        inline QString prepare(const QString& target, const QString& portableSource)
        {
            if (!QDir().mkpath(target))
                std::fprintf(stderr,
                             "AppPaths: could not create the user data directory '%s'. Settings, saves and "
                             "add-ons will not persist; check permissions and free space.\n",
                             target.toLocal8Bit().constData());
            migrateFromPortableDir(portableSource, target);
            return target;
        }
    }

    // Where this platform keeps the user's data - the answer alone, ignoring EB_ISOLATED_DATA_DIR and
    // creating nothing. dataDir() is this plus isolation plus first-use preparation, and it CALLS this
    // rather than repeating the branch, so the two cannot drift. Split out so probe_isolation can assert
    // each platform's answer from inside an isolated build, where dataDir() itself is redirected: without
    // it, "Windows and macOS are unchanged" would be a claim no test could make.
    inline QString platformDataDir()
    {
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#elif defined(Q_OS_LINUX)
        return UserData::xdgDataDir();
#else
        return QCoreApplication::applicationDirPath();
#endif
    }

    inline QString dataDir()
    {
#ifdef EB_ISOLATED_DATA_DIR
        // Ahead of the platform branch on purpose: isolation is a property of the BUILD, not of the OS, so it
        // holds identically if probes are ever built for a platform whose real data dir is somewhere else.
        // (Today they are not - every probe target sits inside `if(NOT ANDROID AND NOT IOS)`.)
        //
        // It also has to stay ahead of the Linux branch below for a second reason: that branch CREATES a
        // directory in the user's home and migrates files into it. A probe that reached it would write into
        // a real user directory - issue #42 restored, with a privacy problem on top. probe_isolation pins
        // this ordering explicitly.
        static const Isolated::Scratch scratch;
        return scratch.path;
#elif defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
        const QString d = platformDataDir();
        QDir().mkpath(d);
        return d;
#elif defined(Q_OS_LINUX)
        // ORDER IS LOAD-BEARING: Android defines Q_OS_LINUX too, so this branch must stay BELOW the
        // Android/iOS one. Above it, every Android install's data directory moves - ini, saves, states,
        // addons - with no migration, i.e. a silent wipe (the same trap AppBrand::Legacy::kDisplayName
        // documents for applicationName).
        //
        // static: the directory is created, and the one-time migration from a portable install is run, on
        // the FIRST call and never again. dataDir() is called from ~260 places, some per frame; and
        // probe_isolation requires the answer to be stable within a process.
        static const QString d = UserData::prepare(platformDataDir(), QCoreApplication::applicationDirPath());
        return d;
#else
        return platformDataDir();
#endif
    }
}
