#include "TrickplayIdleWalk.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "Settings.h"
#include "Trickplay.h"
#include "TrickplayGen.h"

#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace
{
// Where the walk left off, under the same "previews/" prefix as the cache bound — which CloudSync already
// classifies device-local (#301). That is the right shape twice over: a position in THIS machine's library
// means nothing on another one, and a cursor is churn, which is exactly what the bundle must not carry.
const QLatin1String kCursorKey("previews/idleCursor");

QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}
} // namespace

TrickplayIdleWalk::TrickplayIdleWalk(TrickplayGen* gen, QObject* parent)
    : QObject(parent), gen_(gen)
{
    timer_ = new QTimer(this);
    timer_->setInterval(kPollMs);
    connect(timer_, &QTimer::timeout, this, &TrickplayIdleWalk::tick);
    if (gen_)
    {
        // The generator finishing OUR item is the cue to consider the next one straight away rather than at
        // the next poll — an idle machine should get through a library, not one film per five seconds. The
        // conditions are still re-asked inside tick(), so "straight away" never means "unconditionally".
        connect(gen_, &TrickplayGen::idleItemFinished, this, [this](const QString&) {
            working_ = false;
            tick();
        });
    }
}

void TrickplayIdleWalk::setConditionsProvider(std::function<TrickplayIdle::Conditions()> fn)
{ conditions_ = std::move(fn); }

void TrickplayIdleWalk::setLibraryProvider(std::function<QStringList()> fn)
{ library_ = std::move(fn); }

void TrickplayIdleWalk::start()
{
    if (timer_ && !timer_->isActive()) timer_->start();
}

QString TrickplayIdleWalk::cursor() const
{
    return store().value(kCursorKey).toString();
}

void TrickplayIdleWalk::setCursor(const QString& path)
{
    store().setValue(kCursorKey, path);
    store().sync();
}

// Conditions stopped saying Go. Drop whatever of ours is queued or running, and leave the cursor exactly
// where it is — that IS the resume point, and an interrupted item keeps its finished grids on disk, so the
// next sweep restarts neither the library nor the film.
void TrickplayIdleWalk::stopWork(TrickplayIdle::Verdict why)
{
    last_ = why;
    if (!gen_) return;
    gen_->cancelIdle();
    working_ = false;
}

void TrickplayIdleWalk::tick()
{
    if (!gen_ || !conditions_) return;

    TrickplayIdle::Conditions c = conditions_();
    // The cache size is the WORKER's last measurement, and it is advisory here: the authoritative check is
    // taken on the worker thread with a live number immediately before a decoder is opened.
    //
    // It is also deliberately forgotten every kFullRetryTicks. A cache that filled up is not full for ever —
    // the user watching films runs #85's LRU sweep, which frees space this side never hears about — and a
    // latched "full" would leave the walk stopped until the app was restarted. Zeroing the belief lets one
    // item be considered, which makes the worker re-measure and publish the truth. That is the whole retry,
    // and it costs one directory measurement every few minutes while the cache is at its bound.
    if (++sinceMeasure_ >= kFullRetryTicks) { sinceMeasure_ = 0; c.cacheBytes = 0; }
    else                                     c.cacheBytes = gen_->knownCacheBytes();

    const TrickplayIdle::Verdict v = TrickplayIdle::evaluate(c);
    last_ = v;
    if (v != TrickplayIdle::Verdict::Go)
    {
        // EVERY tick, not only the ones that start something. This is what makes pressing play stop a walk
        // that is halfway through a two-hour film instead of at the end of it.
        stopWork(v);
        return;
    }
    if (working_ || gen_->busy()) return;    // one file at a time — #85's manner, unchanged

    if (!library_) return;
    if (sorted_.isEmpty())
    {
        sorted_ = library_();
        // Only files this machine owns outright reach the walk at all: the stream guard is a VALUE and it is
        // applied here as well as inside the generator, because a library entry can perfectly well be a path
        // on a share the user mounted.
        QStringList keep;
        keep.reserve(sorted_.size());
        for (const QString& p : std::as_const(sorted_))
            if (Trickplay::eligible(p)) keep << p;
        sorted_ = keep;
        std::sort(sorted_.begin(), sorted_.end());
        if (sorted_.isEmpty()) return;
    }

    // A BOUNDED look. At most kProbePerTick candidates are examined before the GUI thread is given back, and
    // the cursor moves over the ones already complete, so the next tick continues rather than restarting.
    QString cur = cursor();
    for (int budget = 0; budget < kProbePerTick; ++budget)
    {
        const int i = TrickplayIdle::nextAfter(sorted_, cur);
        if (i < 0)
        {
            // The end of the sweep. The cursor clears so the next idle period starts at the top and picks up
            // whatever has been added since; the snapshot is dropped so it is re-read then. On an already
            // swept library that costs one sidecar read per file and never a decoder.
            setCursor(QString());
            sorted_.clear();
            return;
        }
        const QString path = sorted_.at(i);
        if (TrickplayIdle::needsGeneration(TrickplayGen::itemState(path)))
        {
            working_ = true;
            gen_->requestIdle(path);
            // The cursor advances NOW rather than on completion. A file that cannot be generated — a codec
            // libmpv will not open, a disk that went away — would otherwise be retried on every tick for
            // ever, and the walk would never reach the rest of the library. The item keeps whatever whole
            // grids it managed, and the next sweep will find it partial and finish it.
            setCursor(TrickplayIdle::advanceCursor(sorted_, i));
            return;
        }
        cur = TrickplayIdle::advanceCursor(sorted_, i);
        setCursor(cur);
        if (cur.isEmpty()) { sorted_.clear(); return; }   // that was the last file, and it was already done
    }
}
