// STORE BACKENDS (issue #118, increment 1) — owning a game on a storefront WITHOUT that storefront's client.
//
// Our four PC importers (SteamLibrary / EpicLibrary / GogLibrary / BattleNetLibrary) all read a store
// CLIENT's local state and hand launches back to it. That model has a floor: on Linux there is no Epic
// client at all, so the Epic importer imports nothing there and there is nothing to hand a launch to.
// A store BACKEND is the other half — a small standalone executable (legendary for Epic, later gogdl for
// GOG and nile for Amazon) that authenticates with the user's own account and talks to the store directly.
//
// THE ONE RULE THIS UNIT EXISTS TO ENFORCE: we SHELL OUT, we never reimplement a storefront protocol.
// Everything below is process plumbing and JSON parsing around somebody else's CLI. If a change here starts
// to look like an implementation of Epic's API, it is in the wrong repository.
//
// WHAT INCREMENT 1 COVERS: detection, sign-in surfacing, and the OWNED-LIBRARY listing. Install, update,
// verify/repair, the download queue, store cloud saves, gogdl and nile are later increments — a backend-listed
// game therefore contributes a NOT-READY source that says it is not installed, and nothing here downloads or
// installs anything, including the backend tool itself.
//
// DETECTION, NOT INSTALLATION. pickTool() looks in `<app>/tools/` and then on PATH. When the tool is absent
// the settings row says so and links the project's own releases page; a managed download of a third-party
// binary is its own decision with its own security argument, and it is not made here.
//
// NOTHING HERE MAY ASSUME WINDOWS. The whole motivation is Linux. Paths are built with '/', the executable
// name is `toolFileNames()`'s LAST candidate on every platform (the bare, extension-less name), and no
// comparison anywhere spells ".exe". The CI runner is Linux and is the thing that actually checks this.
//
// THREADING. Every call that spawns a process (auth(), ownedGames(), signIn()) BLOCKS, so none of them may be
// called on the GUI thread. ownedGamesFetch() / signInAsync() are the supported entry points: they run the
// blocking call on the global thread pool and deliver the result back on the caller's thread, guarded so a
// result that lands after the caller is destroyed is delivered to nobody. run() always bounds the child with
// a timeout and kills it on expiry — a backend that hangs degrades to "no games from this source" with a
// readable reason, exactly like one that is absent.
//
// CREDENTIALS. We hold none. legendary keeps its own config (its refresh token lives in legendary's config
// directory, which is legendary's business); the only credential that ever passes through this code is the
// authorization code the user pastes during sign-in, which is handed to the child on its argv — legendary's
// only non-interactive interface — and then dropped. It is never written to the ini, never logged, and never
// put in a reason string. The only thing we persist is the LISTING CACHE and a signed-in boolean, both under
// the `storebackend/` prefix, which CloudSync::isDeviceLocalKey carves out of the synced bundle (probe_cloudmerge
// pins it): a listing is this machine's view of an account linked on this machine, and it churns.
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

class QObject;

// One game as a store BACKEND reports it. Deliberately not EpicLibrary's EpicGame: that struct is a
// launcher MANIFEST (it carries an install location because the launcher installed it), and a backend
// listing is an entitlement — the user owns it, and whether anything is installed is a separate question.
struct StoreGame
{
    QString appName;            // the backend's own launch id (Epic AppName)
    QString title;              // display name
    bool    installed = false;  // the backend reports a local install it manages (unused until a later increment)
};

// Why a backend answered the way it did. Every failure is one of these, and each maps to ONE sentence a
// person can act on (storeback::reasonFor). There is no generic "error": a row that says "something went
// wrong" is a row that teaches the user to ignore it.
enum class StoreStatus
{
    Ok,            // the tool ran, exited 0, and its output parsed
    ToolMissing,   // no such executable in <app>/tools or on PATH (or it would not start)
    NotSignedIn,   // the tool is there but no account is linked
    Timeout,       // the tool was started, did not answer inside the bound, and was killed
    Failed,        // the tool ran and exited non-zero
    Malformed,     // the tool exited 0 and printed something this build cannot read
};

struct StoreListing
{
    StoreStatus        status = StoreStatus::ToolMissing;
    QVector<StoreGame> games;   // empty for every status but Ok (and legitimately empty FOR Ok)
};

struct StoreAuth
{
    StoreStatus status   = StoreStatus::ToolMissing;
    bool        signedIn = false;
};

// The seam. One implementation today (legendary); the rest of the app asks a StoreBackend and never learns
// which CLI answered.
class StoreBackend
{
public:
    virtual ~StoreBackend() = default;

    virtual QString id()          const = 0;  // the cache/settings key: "legendary"
    virtual QString toolName()    const = 0;  // what the executable is called, with no extension: "legendary"
    virtual QString storeName()   const = 0;  // the STORE a person knows: "Epic Games"
    virtual QString launcherId()  const = 0;  // the pcgame::PcGameSource::launcher this backend feeds: "epic"
    virtual QString releasesUrl() const = 0;  // the project's own releases page (what the settings row links)
    virtual QString signInUrl()   const = 0;  // the page the user authenticates on, in their browser

    // The resolved executable, or empty when it is not installed. A filesystem lookup, no process spawn —
    // safe on the GUI thread.
    virtual QString toolPath() const = 0;

    // BLOCKING (spawns a process). Never call these on the GUI thread; use the async wrappers below.
    virtual StoreAuth    authState()  const = 0;
    virtual StoreListing ownedGames() const = 0;
    // The tool's own sign-in, completed with the authorization code the user pasted. The code is a
    // CREDENTIAL: it goes to the child's argv and nowhere else.
    virtual StoreStatus  signIn(const QString& authorizationCode) const = 0;
};

namespace storeback
{
    // ---- tool discovery (pure) ------------------------------------------------------------------------

    // The file names `base` can have on THIS platform, most specific first, and ALWAYS ending with the bare
    // extension-less name. The order matters on Windows only; the invariant that matters everywhere is the
    // last element, because that is the only name the tool has on Linux and macOS.
    QStringList toolFileNames(const QString& base);

    // Pure: the first `dir`/`name` pair `isExecutable` accepts, joined with '/', or empty. DIRECTORY ORDER
    // DOMINATES name order — a tool the user dropped in <app>/tools wins over one on PATH, so an appliance
    // install can ship a known-good copy without fighting whatever the distro packaged.
    QString pickTool(const QString& base, const QStringList& dirs,
                     const std::function<bool(const QString&)>& isExecutable);

    // <app>/tools first, then every PATH entry (split on QDir::listSeparator(), so ';' on Windows and ':'
    // elsewhere — never a hardcoded separator).
    QStringList toolSearchDirs();

    // The real lookup: pickTool over toolSearchDirs() with an is-an-executable-file test.
    QString findTool(const QString& base);

    // ---- the readable reason --------------------------------------------------------------------------

    // ONE sentence per status, naming the tool and the store. Ok returns empty (there is nothing to say).
    // It never interpolates anything the tool printed: a child's stderr can contain a URL with a token in it.
    QString reasonFor(StoreStatus status, const QString& toolName, const QString& storeName);

    // ---- the child process ----------------------------------------------------------------------------

    struct ToolRun
    {
        bool       started  = false;  // the executable was found and launched
        bool       timedOut = false;  // it was started, blew the bound, and was killed
        int        exitCode = -1;     // meaningful only when started && !timedOut
        QByteArray out;               // stdout (the JSON)
        QByteArray err;               // stderr (legendary logs here) — NEVER surfaced to the user verbatim
    };

    // BLOCKING, bounded. Kills the child on timeout and reports it. Never throws, never asserts: an absent
    // executable comes back as `started == false`, which is a normal answer here and not an error path.
    ToolRun run(const QString& exe, const QStringList& args, int timeoutMs);

    // ---- the listing cache (device-local, `storebackend/` prefix) --------------------------------------

    // Pure TTL predicate — false for a zero or FUTURE timestamp (a clock that jumped forward would otherwise
    // freeze a stale list in place for ever). Mirrors SteamLibrary::ownedCacheFresh deliberately: the owned
    // library has one caching rule in this app, not one per store.
    bool cacheFresh(qint64 cachedTs, qint64 now, int ttlSecs);

    // Round-trip of a listing, pure. The blob carries appName/title/installed and NOTHING else — no account,
    // no token, no path.
    QByteArray         encodeGames(const QVector<StoreGame>& games);
    QVector<StoreGame> decodeGames(const QByteArray& blob);

    // Instant and process-free: the TTL-fresh cached listing for this backend, or {} when it was never
    // fetched or has gone stale. Safe to call on every folder refresh.
    QVector<StoreGame> cachedOwned(const QString& backendId, int ttlSecs);
    void               storeOwned(const QString& backendId, const QVector<StoreGame>& games);
    qint64             cachedOwnedAt(const QString& backendId);

    // The signed-in flag as of the last time we actually asked. A BOOLEAN and not the account name: the
    // account is an email address, and a settings row does not need to write one into an ini to say
    // "you are signed in".
    bool cachedSignedIn(const QString& backendId);
    void storeSignedIn(const QString& backendId, bool signedIn);

    // Pure state machine: what should a refresh do? Mirrors SteamLibrary::ownedFetchDecision.
    enum class Refresh { NotAvailable, CacheHit, Fetch };
    Refresh refreshDecision(bool toolPresent, qint64 cachedTs, qint64 now, int ttlSecs);

    // ---- async wrappers (the only GUI-thread-safe way in) ---------------------------------------------

    // 30 minutes, the same bound the Steam owned library uses. Declared BEFORE the function that defaults to
    // it: a default argument is looked up where it is written, so a constant declared below would not compile
    // on GCC even though MSVC accepts plenty of things it should not.
    inline constexpr int kOwnedTtlSecs = 30 * 60;

    // Runs backend->ownedGames() on the global thread pool and delivers the result on `context`'s thread.
    // No-ops (never calls back) when the tool is absent or the cache is still TTL-fresh — which is also what
    // stops a re-present loop, since the callback re-populates the folder. On success the listing is cached
    // and the signed-in flag updated BEFORE onReady runs, so the callback's re-read sees it.
    void ownedGamesFetch(StoreBackend* backend, QObject* context,
                         std::function<void(const StoreListing&)> onReady, int ttlSecs = kOwnedTtlSecs);

    // Runs backend->signIn(code) off the GUI thread. `code` is a credential: it is moved into the worker,
    // handed to the child's argv, and never stored.
    void signInAsync(StoreBackend* backend, const QString& authorizationCode, QObject* context,
                     std::function<void(StoreStatus)> onDone);

    // ---- the registry ---------------------------------------------------------------------------------

    // The one backend today. Process-wide and stateless (it re-resolves its tool on every call), so there is
    // nothing to invalidate when the user installs legendary while the app is running.
    StoreBackend* legendary();
    QVector<StoreBackend*> all();
}
