#pragma once
#include <QString>
#include <QCoreApplication>
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
#include <QStandardPaths>
#include <QDir>
#endif
#ifdef EB_ISOLATED_DATA_DIR
#include <QByteArray>
#include <QDir>
#include <QRandomGenerator>
#endif

// The app's writable base directory. On desktop this is the executable's own folder - the app is portable,
// so everythingbox.ini, cores/, saves/, states/, downloads/, addons/, ... all live next to EverythingBox.
// On Android and iOS the executable directory isn't writable, so use the app's private data location instead.
// Everything that builds a path off the app dir goes through here, so a platform only changes this function.
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
                QDir().mkpath(path);
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
            QDir().mkpath(path);
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

    inline QString dataDir()
    {
#ifdef EB_ISOLATED_DATA_DIR
        // Ahead of the platform branch on purpose: isolation is a property of the BUILD, not of the OS, so it
        // holds identically if probes are ever built for a platform whose real data dir is somewhere else.
        // (Today they are not - every probe target sits inside `if(NOT ANDROID AND NOT IOS)`.)
        static const Isolated::Scratch scratch;
        return scratch.path;
#elif defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
        const QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(d);
        return d;
#else
        return QCoreApplication::applicationDirPath();
#endif
    }
}
