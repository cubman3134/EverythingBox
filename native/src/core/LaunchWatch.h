// Play-time for games a STORE launches (issue #61). A steam:// or com.epicgames.launcher:// hand-off gives us
// no process handle — the store client starts the game, maybe after an update or a login prompt, maybe
// through a wrapper that restarts it — so the directly-launched-exe timing (MainWindow::launchPcExe's
// QWinEventNotifier) cannot apply. Instead the game is SAMPLED: every few seconds, "is it running now?".
//
// This header is the pure half, QtCore-only and clock-free, so probe_launchwatch drives it with injected
// samples and injected time on any OS:
//   * LaunchWatch::Machine — one launch's state machine: pending -> running -> exited, or pending -> timed out;
//   * the rules a sampler is built from (a process image under an install dir; Steam's RunningAppID);
//   * which launch URLs get a watch at all (a steam://install/ hand-off does not);
//   * LaunchWatcher — the set of live watches plus the ONE timer that samples them, which runs only while at
//     least one watch is pending or running.
// The platform samplers (the registry read, the process enumeration) live in LaunchSamplers.{h,cpp}. Off
// Windows there are none: the launch stays fire-and-forget, exactly as before #61.
#pragma once
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <functional>
#include <memory>
#include <vector>

namespace LaunchWatch
{
    // A pending launch that never shows a running game within this long records nothing: the store may be
    // updating the game, asking for a login, or the user cancelled in the store's own UI.
    constexpr qint64 kPendingTimeoutSecs = 180;
    // Running -> exited needs this many CONSECUTIVE not-running samples, so a launcher that restarts the game
    // (a brief gap between the wrapper's first and second process) is not counted as an exit.
    constexpr int    kExitMissedSamples  = 2;
    // A session shorter than this records nothing — a crash at start is not play.
    constexpr qint64 kMinSessionSecs     = 30;
    // How often the watcher samples. Only runs while a watch is pending or running.
    constexpr int    kSampleIntervalMs   = 5000;

    enum class State { Pending, Running, Exited, TimedOut };

    // One launch. Feed it samples in time order; it never reads a clock or the OS itself.
    class Machine
    {
    public:
        explicit Machine(qint64 launchedAt) : launchedAt_(launchedAt) {}

        void sample(qint64 now, bool running);

        State  state() const  { return state_; }
        bool   active() const { return state_ == State::Pending || state_ == State::Running; }
        // First running sample to last running sample (0 until the game has been seen running).
        qint64 sessionSeconds() const { return seenRunning_ ? lastRunning_ - firstRunning_ : 0; }
        // What to add to PlayStats: the session once it has EXITED and only if it reached kMinSessionSecs.
        // A timed-out launch, an active one, and a too-short one all record 0.
        qint64 secondsToRecord() const;

    private:
        qint64 launchedAt_;
        State  state_ = State::Pending;
        bool   seenRunning_ = false;
        qint64 firstRunning_ = 0;
        qint64 lastRunning_ = 0;
        int    missed_ = 0;   // consecutive not-running samples since the last running one
    };

    // Is `imagePath` (a process's full image path) inside `dir`? Case-insensitive (Windows paths), either
    // separator, and the directory must be followed by a separator, so C:\Games\Foo does NOT claim
    // C:\Games\FooBar\game.exe. An empty dir, a bare drive or a filesystem root claims nothing — a watch on
    // "every process on the drive" would count the whole session the store client itself is open.
    bool imageUnderDir(const QString& imagePath, const QString& dir);
    bool anyImageUnderDir(const QStringList& imagePaths, const QString& dir);

    // The Steam "is it running" rule. `runningAppId` is HKCU\Software\Valve\Steam\RunningAppID as read (an
    // invalid QVariant when the value is absent): present -> running iff it equals `appid`; absent -> fall back
    // to `installDirRunning` (the install-dir process check), or false when there is no fallback.
    bool steamRunning(const QVariant& runningAppId, const QString& appid,
                      const std::function<bool()>& installDirRunning);

    // Which store launch a URL is, and so whether it gets a watch. steam://rungameid/<id> and steam://run/<id>
    // -> Steam; com.epicgames.launcher://apps/<AppName>[?action=launch...] -> Epic. Everything else — notably
    // steam://install/<id>, which opens Steam's install dialog and plays nothing — is None.
    struct Target
    {
        enum class Kind { None, Steam, Epic } kind = Kind::None;
        QString storeId; // the Steam appid / the Epic AppName
    };
    Target targetForLaunchUrl(const QString& url);
}

// The live watches and the one sampling timer. Owned by MainWindow; probes drive tick() directly with an
// injected clock. Sampling is synchronous and cheap (a registry read / one process snapshot per tick), so
// there is nothing to wait for at quit: stopAll() drops every watch and records nothing.
class LaunchWatcher
{
public:
    using Sampler = std::function<bool()>;
    using Clock   = std::function<qint64()>;
    // Called once per watch when it ends. `seconds` is what to add to PlayStats — 0 for a timed-out launch or a
    // session under kMinSessionSecs, in which case the caller records nothing.
    using OnEnd   = std::function<void(const QString& gameKey, const QString& playId, LaunchWatch::State end,
                                       qint64 seconds)>;

    explicit LaunchWatcher(OnEnd onEnd, Clock clock = Clock());
    // Optional: called once when a watch first sees its game running (pending -> running). Not again when a
    // launcher wrapper restarts the game within the same session.
    void setOnRunning(std::function<void(const QString& gameKey)> onRunning) { onRunning_ = std::move(onRunning); }
    ~LaunchWatcher();
    LaunchWatcher(const LaunchWatcher&) = delete;
    LaunchWatcher& operator=(const LaunchWatcher&) = delete;

    // Start watching a launch of `gameKey`. Returns false and changes nothing when that game is already being
    // watched (a second launch while it is pending or running is ignored) or when there is no sampler (no
    // watch on this platform / no install dir to look under).
    bool watch(const QString& gameKey, const QString& playId, Sampler sampler);

    void tick();      // one sampling pass over every watch — the timer's slot
    void stopAll();   // quit: drop every watch without recording anything, and stop the timer

    int  count() const { return int(watches_.size()); }
    bool isWatching(const QString& gameKey) const;
    bool timerActive() const { return timer_.isActive(); }

private:
    struct Watch
    {
        QString gameKey, playId;
        Sampler sampler;
        LaunchWatch::Machine machine;
    };
    qint64 now() const;

    OnEnd  onEnd_;
    std::function<void(const QString&)> onRunning_;
    Clock  clock_;
    QTimer timer_;
    std::vector<std::unique_ptr<Watch>> watches_;
};
