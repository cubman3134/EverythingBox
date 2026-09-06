#include "StoreBackend.h"
#include "LegendaryBackend.h"
#include "AppPaths.h"
#include "AppBrand.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QThreadPool>
#include <atomic>
#include <memory>

// The shared ini, same accessor shape every store in src/core uses.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

static QString gamesKey(const QString& backendId)
{
    return QStringLiteral("storebackend/") + backendId + QStringLiteral("/games");
}
static QString gamesAtKey(const QString& backendId)
{
    return QStringLiteral("storebackend/") + backendId + QStringLiteral("/gamesAt");
}
static QString signedInKey(const QString& backendId)
{
    return QStringLiteral("storebackend/") + backendId + QStringLiteral("/signedIn");
}

// ---- tool discovery ------------------------------------------------------------------------------------

QStringList storeback::toolFileNames(const QString& base)
{
    if (base.isEmpty()) return {};
#ifdef Q_OS_WIN
    // Windows needs the extensions spelled out because a bare name is not executable there. The BARE name is
    // still last, so every platform shares one final candidate and a Windows-only reading of this list can
    // never be the only thing that works. legendary ships as a .exe; the .cmd/.bat forms are what pipx and a
    // few package managers leave behind, and they are cheap to try.
    return { base + QStringLiteral(".exe"), base + QStringLiteral(".cmd"),
             base + QStringLiteral(".bat"), base };
#else
    return { base };
#endif
}

QString storeback::pickTool(const QString& base, const QStringList& dirs,
                            const std::function<bool(const QString&)>& isExecutable)
{
    if (base.isEmpty() || !isExecutable) return {};
    const QStringList names = toolFileNames(base);
    for (const QString& rawDir : dirs)
    {
        QString dir = QDir::fromNativeSeparators(rawDir).trimmed();
        if (dir.isEmpty()) continue;
        while (dir.endsWith(QLatin1Char('/')) && dir.size() > 1) dir.chop(1);
        for (const QString& n : names)
        {
            const QString cand = dir + QLatin1Char('/') + n;
            if (isExecutable(cand)) return cand;
        }
    }
    return {};
}

QStringList storeback::toolSearchDirs()
{
    QStringList dirs;
    // <app>/tools — where a managed download would land (a LATER increment; nothing writes here yet) and
    // where an appliance image can drop a known-good copy.
    dirs << AppPaths::dataDir() + QStringLiteral("/tools");
    // Then PATH. QDir::listSeparator() is ';' on Windows and ':' everywhere else — the separator is never
    // spelled out here, because the one place this file could quietly become Windows-only is a literal ';'.
    const QString path = QProcessEnvironment::systemEnvironment().value(QStringLiteral("PATH"));
    for (const QString& p : path.split(QDir::listSeparator(), Qt::SkipEmptyParts)) dirs << p;
    return dirs;
}

QString storeback::findTool(const QString& base)
{
    return pickTool(base, toolSearchDirs(), [](const QString& p) {
        const QFileInfo fi(p);
        return fi.isFile() && fi.isExecutable();
    });
}

// ---- the readable reason -------------------------------------------------------------------------------

QString storeback::reasonFor(StoreStatus status, const QString& toolName, const QString& storeName)
{
    switch (status)
    {
    case StoreStatus::Ok:
        return {};
    case StoreStatus::ToolMissing:
        return QCoreApplication::translate("StoreBackend",
            "%1 isn't installed on this machine, so your %2 library can't be listed.").arg(toolName, storeName);
    case StoreStatus::NotSignedIn:
        return QCoreApplication::translate("StoreBackend",
            "%1 is installed but isn't signed in to %2 yet.").arg(toolName, storeName);
    case StoreStatus::Timeout:
        return QCoreApplication::translate("StoreBackend",
            "%1 didn't answer in time, so your %2 library isn't showing.").arg(toolName, storeName);
    case StoreStatus::Failed:
        return QCoreApplication::translate("StoreBackend",
            "%1 couldn't list your %2 library.").arg(toolName, storeName);
    case StoreStatus::Malformed:
        return QCoreApplication::translate("StoreBackend",
            "%1 answered with something this version can't read, so your %2 library isn't showing.")
            .arg(toolName, storeName);
    }
    return {};
}

// ---- the child process ---------------------------------------------------------------------------------

storeback::ToolRun storeback::run(const QString& exe, const QStringList& args, int timeoutMs)
{
    ToolRun r;
    if (exe.isEmpty()) return r;                   // started == false: "no such tool", a normal answer

    QProcess p;
    p.setProgram(exe);
    p.setArguments(args);
    p.setProcessChannelMode(QProcess::SeparateChannels);   // stderr is the tool's log; stdout is the JSON
    // legendary is a Python program, and on a console whose code page is not UTF-8 it will happily mangle
    // (or die on) a game title outside Latin-1. Asking Python for UTF-8 explicitly costs nothing and is the
    // difference between listing a library and failing to.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    p.setProcessEnvironment(env);

    p.start();
    // A start that never happens is the ABSENT-TOOL case, not an error: come back with started == false and
    // let the caller turn that into the readable reason.
    if (!p.waitForStarted(qMin(timeoutMs, 5000))) return r;
    r.started = true;

    if (!p.waitForFinished(timeoutMs))
    {
        // THE HANG CASE, and the reason every call here is bounded. Kill it and say so; a backend that never
        // answers must cost the folder one timeout, not the session.
        r.timedOut = true;
        p.kill();
        p.waitForFinished(2000);
        return r;
    }
    r.out = p.readAllStandardOutput();
    r.err = p.readAllStandardError();
    // A crash is reported as a non-zero exit rather than a status of its own: the user's answer is the same
    // sentence either way, and a fifth status nobody can act on differently is noise.
    r.exitCode = (p.exitStatus() == QProcess::NormalExit) ? p.exitCode() : -1;
    return r;
}

// ---- the listing cache ---------------------------------------------------------------------------------

bool storeback::cacheFresh(qint64 cachedTs, qint64 now, int ttlSecs)
{
    if (cachedTs <= 0 || ttlSecs <= 0) return false;
    if (cachedTs > now) return false;             // a clock that jumped forward must not freeze a stale list
    return (now - cachedTs) < ttlSecs;
}

QByteArray storeback::encodeGames(const QVector<StoreGame>& games)
{
    QJsonArray a;
    for (const StoreGame& g : games)
    {
        QJsonObject o;
        o.insert(QStringLiteral("app"), g.appName);
        o.insert(QStringLiteral("title"), g.title);
        o.insert(QStringLiteral("installed"), g.installed);
        a.push_back(o);
    }
    return QJsonDocument(a).toJson(QJsonDocument::Compact);
}

QVector<StoreGame> storeback::decodeGames(const QByteArray& blob)
{
    QVector<StoreGame> out;
    const QJsonDocument doc = QJsonDocument::fromJson(blob);
    if (!doc.isArray()) return out;
    for (const QJsonValue& v : doc.array())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        StoreGame g;
        g.appName   = o.value(QStringLiteral("app")).toString();
        g.title     = o.value(QStringLiteral("title")).toString();
        g.installed = o.value(QStringLiteral("installed")).toBool();
        if (g.appName.isEmpty()) continue;
        if (g.title.isEmpty()) g.title = g.appName;
        out.push_back(g);
    }
    return out;
}

qint64 storeback::cachedOwnedAt(const QString& backendId)
{
    return store().value(gamesAtKey(backendId)).toLongLong();
}

QVector<StoreGame> storeback::cachedOwned(const QString& backendId, int ttlSecs)
{
    if (!cacheFresh(cachedOwnedAt(backendId), QDateTime::currentSecsSinceEpoch(), ttlSecs)) return {};
    return decodeGames(store().value(gamesKey(backendId)).toByteArray());
}

void storeback::storeOwned(const QString& backendId, const QVector<StoreGame>& games)
{
    store().setValue(gamesKey(backendId), encodeGames(games));
    store().setValue(gamesAtKey(backendId), QDateTime::currentSecsSinceEpoch());
    store().sync();
}

bool storeback::cachedSignedIn(const QString& backendId)
{
    return store().value(signedInKey(backendId), false).toBool();
}

void storeback::storeSignedIn(const QString& backendId, bool signedIn)
{
    store().setValue(signedInKey(backendId), signedIn);
    store().sync();
}

storeback::Refresh storeback::refreshDecision(bool toolPresent, qint64 cachedTs, qint64 now, int ttlSecs)
{
    if (!toolPresent) return Refresh::NotAvailable;
    if (cacheFresh(cachedTs, now, ttlSecs)) return Refresh::CacheHit;
    return Refresh::Fetch;
}

// ---- async wrappers ------------------------------------------------------------------------------------
namespace {

// The delivery hop. A worker thread cannot touch `context` — it may be destroyed at any moment — so:
//   * `alive` is an atomic flag flipped by context's own destroyed() signal, read on the GUI thread;
//   * `relay` is a plain QObject that NOTHING but its own queued lambda ever deletes, so the worker's
//     invokeMethod target cannot dangle. (A relay parented to `context` would dangle exactly when the guard
//     is needed, which is the bug this shape avoids rather than the one it looks like.)
// No Q_OBJECT, no moc: QMetaObject::invokeMethod with a functor needs neither, and a probe target that
// compiled this file without AUTOMOC would otherwise fail to link for reasons unrelated to the feature.
struct AliveFlag { std::atomic<bool> alive{true}; };

} // namespace

void storeback::ownedGamesFetch(StoreBackend* backend, QObject* context,
                                std::function<void(const StoreListing&)> onReady, int ttlSecs)
{
    if (!backend || !context || !onReady) return;
    const QString bid = backend->id();
    const Refresh what = refreshDecision(!backend->toolPath().isEmpty(), cachedOwnedAt(bid),
                                         QDateTime::currentSecsSinceEpoch(), ttlSecs);
    // NotAvailable / CacheHit both mean "no process, no callback". The silent CacheHit is also what stops the
    // re-present loop: the callback repopulates the folder, which asks again, which hits the fresh cache.
    if (what != Refresh::Fetch) return;

    auto guard = std::make_shared<AliveFlag>();
    QObject::connect(context, &QObject::destroyed, [guard] { guard->alive.store(false); });

    QObject* relay = new QObject;   // lives on the calling (GUI) thread; deleted by its own queued lambda
    QThreadPool::globalInstance()->start([backend, relay, guard, onReady, bid] {
        const StoreListing listing = backend->ownedGames();   // BLOCKING, off the GUI thread by construction
        QMetaObject::invokeMethod(relay, [relay, guard, onReady, bid, listing] {
            if (guard->alive.load())
            {
                // Cache BEFORE the callback: the callback repopulates the folder, and the folder reads the
                // cache. Writing after it would present the old list and then sit on the new one.
                if (listing.status == StoreStatus::Ok) { storeOwned(bid, listing.games); storeSignedIn(bid, true); }
                else if (listing.status == StoreStatus::NotSignedIn) storeSignedIn(bid, false);
                onReady(listing);
            }
            relay->deleteLater();
        }, Qt::QueuedConnection);
    });
}

void storeback::signInAsync(StoreBackend* backend, const QString& authorizationCode, QObject* context,
                            std::function<void(StoreStatus)> onDone)
{
    if (!backend || !context || !onDone) return;
    const QString bid = backend->id();

    auto guard = std::make_shared<AliveFlag>();
    QObject::connect(context, &QObject::destroyed, [guard] { guard->alive.store(false); });

    QObject* relay = new QObject;
    // `authorizationCode` is captured by value into the worker and dies with the lambda. It is not logged,
    // not stored, and not put in any status text.
    QThreadPool::globalInstance()->start([backend, relay, guard, onDone, bid, authorizationCode] {
        const StoreStatus st = backend->signIn(authorizationCode);
        QMetaObject::invokeMethod(relay, [relay, guard, onDone, bid, st] {
            if (guard->alive.load())
            {
                if (st == StoreStatus::Ok) storeSignedIn(bid, true);
                onDone(st);
            }
            relay->deleteLater();
        }, Qt::QueuedConnection);
    });
}

// ---- the registry --------------------------------------------------------------------------------------

StoreBackend* storeback::legendary()
{
    static LegendaryBackend b;
    return &b;
}

QVector<StoreBackend*> storeback::all()
{
    // gogdl (GOG) and nile (Amazon) join this list in later increments; the seam exists so that is an
    // addition here and nowhere else.
    return { legendary() };
}
