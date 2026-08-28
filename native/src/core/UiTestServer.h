// Local UI-automation channel, so the UI can be TESTED without bringing the window to the front or giving
// it OS focus: an agent/script drives navigation and captures what the user would see while another app
// keeps the foreground. Line-based protocol over a QLocalSocket (a named pipe on Windows, a unix socket
// elsewhere), served on the GUI thread:
//
//   status                                          -> "ok ready" once the window exists, "ok starting" before
//   key <up|down|left|right|enter|back|escape|Nx>   inject a nav key through sendNavKey (Nx = raw Qt::Key int)
//   state                                           -> "ok {json}": page, focus widget, overlays, geometry
//   shot <absolute-path.png>                        render the whole window (works occluded/backgrounded)
//   click X Y                                       synthesize a real LEFT CLICK at window coords (X, Y).
//                                                   Distinct from a touch tap on purpose: Qt routes a click to
//                                                   the child widget under the cursor, so the two reach
//                                                   different objects and must both be testable.
//   touch tap X Y                                   synthesize a real touch tap at window coords (X, Y)
//   touch flick X1 Y1 X2 Y2 [MS]                    a real drag/flick from (X1,Y1) to (X2,Y2) over MS (default 150)
//   touch pinch CX CY SCALE [MS]                    two fingers around (CX,CY) diverging by SCALE over MS
//
// Injected keys use the app's own routing (overlays -> rings -> back actions), not OS input, so they need
// no focus; before each one the window is given Qt-INTERNAL activation (no OS foreground change) so focus
// styling and the watchdog behave exactly as they would live. See native/tools/uitest.py for the client.
//
// OFF by default. Enabled only by EB_UITEST=1 in the environment or a --uitest command-line argument.
//
// LISTENING IS SPLIT FROM THE HOOKS, and that split is the whole point of issue #172. The hooks need a built
// MainWindow, but the CHANNEL does not: main() calls ensureListening() before the startup work (asset
// bootstrap, brand migration, the cloud pull) and MainWindow's ctor later calls setHooks() on the same
// object. Before that moment every command answers `err not-ready: ...`.
//
// The failure this replaces: the server used to be constructed ~400 lines into the MainWindow ctor, so
// ANY stall before that point (a blocking startup step, a slow network round-trip, a modal nobody can see
// because the window is not shown yet) left NO pipe at all — the client's connect just failed, which looks
// exactly like "the app isn't running with EB_UITEST" and reads, to a harness, like nothing at all. A
// channel that is silently absent is indistinguishable from a test that passed. So now:
//   * the pipe exists from the first moments of startup, and says `not-ready` while the window is missing;
//   * a listen() that FAILS (the usual cause: a second instance already owns the name) is announced on
//     stderr and through qCritical, never swallowed.
#pragma once
#include <QObject>
#include <QString>
#include <functional>

class QLocalServer;

class UiTestServer : public QObject
{
    Q_OBJECT
public:
    struct Hooks
    {
        std::function<void(int)> sendKey;                 // deliver a synthetic nav key (Qt::Key_*)
        std::function<QString()> state;                   // compact JSON snapshot of the UI state
        std::function<bool(const QString&)> screenshot;   // render the window to a PNG path
        std::function<bool(const QString&)> openDoc;      // open a document/book by path (reader tests)
        // Synthesize a REAL touch sequence on the top-level window (QWindowSystemInterface::handleTouchEvent —
        // real hit-testing, not a shortcut into the graph). arg is the raw sub-line: "tap X Y",
        // "flick X1 Y1 X2 Y2 [MS]", or "pinch CX CY SCALE [MS]". Non-blocking (a QTimer state machine).
        // Returns false if a sequence is already in flight (the caller gets `err busy` and should retry) —
        // overlapping sequences share a touch id and would interleave press/update/release into corrupt Qt
        // touch state, so a second command is REJECTED rather than queued (keeps the harness deterministic).
        std::function<bool(const QString&)> touch;
        std::function<bool(const QString&)> click;   // a real left click at window coords
        // "pad" / "pointer" / "brand <name>": drives InputMode, the authority behind every help-bar chip.
        // Returns the reply line (an "ok …"/"err …" the harness reads). A HOOK rather than a direct call so
        // this file keeps its lean link — probe_uitest builds it with QtCore+QtNetwork and nothing else.
        //
        // There is no software path from a harness to a real controller press: the poll reads SDL, and the
        // key channel injects Qt events, which Gamepad never sees. Without this, the entire controller-aware
        // help bar is undriveable from a test and only a human with a pad in hand can see it at all.
        std::function<QString(const QString&)> inputMode;
    };

    // WHERE IN STARTUP THE CALLER IS, and specifically whether the ini may be read yet. Mandatory on both
    // entry points below, because getting it wrong is not a UI bug — it decides the whole session's settings.
    //
    // main() brings the channel up BEFORE the brand migration copies the ini into place, and
    // Settings::store() holds a function-local static QSettings that snapshots the file on its FIRST read,
    // so a settings read at that point runs the rest of the session off a pre-migration (usually empty)
    // file. That call therefore passes NotSettled and the Settings half of the answer is skipped.
    //
    // This used to be spelled `wantedFromEnvOrArgs() || Settings::uiTestChannel()` with an unguarded early
    // call site: correct ONLY because the left operand short-circuited the right one away on the path that
    // mattered. That made the operand ORDER of an `||` in one file a startup-correctness invariant of
    // another, enforceable by nothing and invertible by any tidy-up — issue #177. The phase is now stated by
    // the caller and the Settings read sits behind an explicit early return, so there is no order to invert
    // and no call site that can forget the guard: omitting the argument does not compile.
    enum class IniPhase
    {
        NotSettled,   // pre-brand-migration: env/args only, the ini must NOT be touched
        Settled       // the ini is in place: the Settings ▸ Debug toggle counts too
    };

    static bool wanted(IniPhase phase);                   // EB_UITEST=1, --uitest, or (when Settled) the toggle
    // The half of wanted() that reads NO settings, exposed for tests and for anyone who needs the answer
    // without the phase question. wanted(NotSettled) is exactly this.
    static bool wantedFromEnvOrArgs();
    explicit UiTestServer(const Hooks& hooks = {}, QObject* parent = nullptr);
    ~UiTestServer() override;

    // The process-wide channel. ensureListening() creates + listens on first call and is a no-op after that
    // (it returns the existing object, reparenting it to `parent` when one is given, so the window can take
    // ownership of a channel main() started). Returns nullptr only when the channel is not wanted at all.
    // A listen FAILURE still returns the object — it has already been announced, and the caller's hooks are
    // harmless on a server nobody can reach; isListening() is how you ask.
    //
    // `phase` comes FIRST and has no default on purpose: it is the one argument a caller must not omit
    // (see IniPhase). The optional `parent` follows it.
    static UiTestServer* ensureListening(IniPhase phase, QObject* parent = nullptr);
    static UiTestServer* instance();
    bool isListening() const { return listening_; }

    // Bind (or, with a default-constructed Hooks, unbind) the app-side hooks. Until this is called the
    // channel answers, and says it is not ready — see handle().
    void setHooks(const Hooks& hooks) { hooks_ = hooks; }

    // EB_UITEST_PIPE overrides the channel name so a test build can be driven alongside a normally-running
    // instance (which already owns the default pipe). uitest.py honours the same variable.
    static QString serverName()
    {
        const QByteArray n = qgetenv("EB_UITEST_PIPE");
        return n.isEmpty() ? QStringLiteral("EverythingBox-uitest") : QString::fromUtf8(n);
    }

private:
    QString handle(const QString& line);                  // one command -> one response line
    Hooks         hooks_;
    QLocalServer* server_    = nullptr;
    bool          listening_ = false;
};
