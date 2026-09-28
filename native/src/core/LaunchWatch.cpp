#include "LaunchWatch.h"

#include <QDateTime>
#include <QDir>
#include <QUrl>
#include <QUrlQuery>

// ---- the machine ---------------------------------------------------------------------------------------------

void LaunchWatch::Machine::sample(qint64 now, bool running)
{
    switch (state_)
    {
        case State::Pending:
            if (running)
            {
                state_ = State::Running;
                seenRunning_ = true;
                firstRunning_ = lastRunning_ = now;
                missed_ = 0;
            }
            else if (now - launchedAt_ >= kPendingTimeoutSecs)
            {
                state_ = State::TimedOut;   // never ran: the store updated, asked for a login, or was cancelled
            }
            return;
        case State::Running:
            if (running)
            {
                lastRunning_ = now;
                missed_ = 0;                // a single miss (a wrapper restarting the game) is forgiven
            }
            else if (++missed_ >= kExitMissedSamples)
            {
                state_ = State::Exited;     // the session is first -> last RUNNING sample, the misses excluded
            }
            return;
        case State::Exited:
        case State::TimedOut:
            return;                         // terminal: a later start is a new launch, not this one
    }
}

qint64 LaunchWatch::Machine::secondsToRecord() const
{
    if (state_ != State::Exited) return 0;
    const qint64 secs = sessionSeconds();
    return secs >= kMinSessionSecs ? secs : 0;
}

// ---- the rules a sampler is built from -----------------------------------------------------------------------

// Forward slashes, dot segments resolved, no trailing separator. Windows paths either way round.
static QString normalisedDir(const QString& p)
{
    QString s = p;
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));
    s = QDir::cleanPath(s);
    while (s.endsWith(QLatin1Char('/'))) s.chop(1);
    return s;
}

bool LaunchWatch::imageUnderDir(const QString& imagePath, const QString& dir)
{
    if (imagePath.isEmpty()) return false;
    const QString d = normalisedDir(dir);
    // Nothing that would claim a whole drive or filesystem: "", "/" (chopped to ""), and a bare "C:".
    if (d.isEmpty() || (d.size() == 2 && d.at(1) == QLatin1Char(':'))) return false;
    QString img = imagePath;
    img.replace(QLatin1Char('\\'), QLatin1Char('/'));
    img = QDir::cleanPath(img);
    // The trailing separator is the whole Foo-vs-FooBar rule: C:/Games/Foo/ is not a prefix of C:/Games/FooBar/.
    return img.startsWith(d + QLatin1Char('/'), Qt::CaseInsensitive);
}

bool LaunchWatch::anyImageUnderDir(const QStringList& imagePaths, const QString& dir)
{
    for (const QString& p : imagePaths)
        if (imageUnderDir(p, dir)) return true;
    return false;
}

bool LaunchWatch::steamRunning(const QVariant& runningAppId, const QString& appid,
                               const std::function<bool()>& installDirRunning)
{
    if (runningAppId.isValid() && !runningAppId.isNull())
        return !appid.isEmpty() && runningAppId.toString().trimmed() == appid;
    return installDirRunning ? installDirRunning() : false;
}

LaunchWatch::Target LaunchWatch::targetForLaunchUrl(const QString& url)
{
    Target t;
    // Parsed by hand, the way MainWindow parses these URLs for the Recent key (section on '/' and '?'), so the
    // store id here is byte-for-byte the one in "steam:<appid>" / "epic:<AppName>".
    static const QString steamRun  = QStringLiteral("steam://rungameid/");
    static const QString steamRun2 = QStringLiteral("steam://run/");
    static const QString epicApps  = QStringLiteral("com.epicgames.launcher://apps/");
    if (url.startsWith(steamRun, Qt::CaseInsensitive) || url.startsWith(steamRun2, Qt::CaseInsensitive))
    {
        const QString id = url.section(QLatin1Char('/'), -1).section(QLatin1Char('?'), 0, 0);
        if (id.isEmpty()) return t;
        t.kind = Target::Kind::Steam;
        t.storeId = id;
        return t;
    }
    if (url.startsWith(epicApps, Qt::CaseInsensitive))
    {
        const QString appName = url.section(QLatin1Char('/'), -1).section(QLatin1Char('?'), 0, 0);
        if (appName.isEmpty()) return t;
        const QString query = url.section(QLatin1Char('?'), 1);
        const QString action = QUrlQuery(query).queryItemValue(QStringLiteral("action"));
        if (!action.isEmpty() && action.compare(QStringLiteral("launch"), Qt::CaseInsensitive) != 0) return t;
        t.kind = Target::Kind::Epic;
        t.storeId = appName;
        return t;
    }
    return t;   // steam://install/<id>, steam://open/..., anything else: no watch
}

// ---- the watcher ---------------------------------------------------------------------------------------------

LaunchWatcher::LaunchWatcher(OnEnd onEnd, Clock clock)
    : onEnd_(std::move(onEnd)), clock_(std::move(clock))
{
    timer_.setInterval(LaunchWatch::kSampleIntervalMs);
    QObject::connect(&timer_, &QTimer::timeout, [this] { tick(); });
}

LaunchWatcher::~LaunchWatcher() = default;

qint64 LaunchWatcher::now() const
{
    return clock_ ? clock_() : QDateTime::currentSecsSinceEpoch();
}

bool LaunchWatcher::isWatching(const QString& gameKey) const
{
    for (const auto& w : watches_)
        if (w->gameKey == gameKey) return true;
    return false;
}

bool LaunchWatcher::watch(const QString& gameKey, const QString& playId, Sampler sampler)
{
    if (!sampler || gameKey.isEmpty() || isWatching(gameKey)) return false;
    watches_.push_back(std::unique_ptr<Watch>(new Watch{ gameKey, playId, std::move(sampler),
                                                         LaunchWatch::Machine(now()) }));
    if (!timer_.isActive()) timer_.start();
    return true;
}

void LaunchWatcher::tick()
{
    if (watches_.empty()) { timer_.stop(); return; }
    const qint64 t = now();
    // Sample every watch first, then report the ended ones: onEnd may start a new watch (it must not see a
    // half-walked list).
    std::vector<std::unique_ptr<Watch>> ended;
    QStringList started;
    for (auto it = watches_.begin(); it != watches_.end();)
    {
        const bool wasPending = (*it)->machine.state() == LaunchWatch::State::Pending;
        (*it)->machine.sample(t, (*it)->sampler());
        if (wasPending && (*it)->machine.state() == LaunchWatch::State::Running) started << (*it)->gameKey;
        if ((*it)->machine.active()) { ++it; continue; }
        ended.push_back(std::move(*it));
        it = watches_.erase(it);
    }
    if (watches_.empty()) timer_.stop();   // only alive while a watch is pending or running
    if (onRunning_)
        for (const QString& k : started) onRunning_(k);
    for (const auto& w : ended)
        if (onEnd_) onEnd_(w->gameKey, w->playId, w->machine.state(), w->machine.secondsToRecord());
}

void LaunchWatcher::stopAll()
{
    watches_.clear();   // nothing is recorded: a live session has no end to measure
    timer_.stop();
}
