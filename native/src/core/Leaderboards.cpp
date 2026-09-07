#include "Leaderboards.h"

#include <QCoreApplication>
#include <QStringList>

namespace ra
{

EventKind kindForRcEvent(unsigned rcEventType)
{
    switch (rcEventType)
    {
    case kRcLeaderboardStarted:   return EventKind::AttemptStarted;
    case kRcLeaderboardFailed:    return EventKind::AttemptFailed;
    case kRcLeaderboardSubmitted: return EventKind::AttemptSubmitted;
    case kRcScoreboard:           return EventKind::SubmitResult;
    case kRcTrackerShow:          return EventKind::TrackerShow;
    case kRcTrackerUpdate:        return EventKind::TrackerUpdate;
    case kRcTrackerHide:          return EventKind::TrackerHide;
    default:                      return EventKind::None;
    }
}

// ---- TrackerModel ----------------------------------------------------------------------------------------

void TrackerModel::show(unsigned trackerId, const QString& display)
{
    for (Entry& e : entries_)
        if (e.id == trackerId) { e.display = display; return; } // re-show of a live id is a value change
    entries_.push_back(Entry{ trackerId, display });
}

void TrackerModel::update(unsigned trackerId, const QString& display)
{
    // An update for an id that is not showing is dropped rather than promoted to a show: rc_client only
    // updates a tracker it has already asked us to show, and inventing one here would leave an overlay on
    // screen that no hide will ever match.
    for (Entry& e : entries_)
        if (e.id == trackerId) { e.display = display; return; }
}

void TrackerModel::hide(unsigned trackerId)
{
    for (int i = 0; i < entries_.size(); ++i)
        if (entries_[i].id == trackerId) { entries_.remove(i); return; }
}

void TrackerModel::clear() { entries_.clear(); }

bool TrackerModel::has(unsigned trackerId) const
{
    for (const Entry& e : entries_)
        if (e.id == trackerId) return true;
    return false;
}

QString TrackerModel::display() const
{
    QStringList lines;
    lines.reserve(entries_.size());
    for (const Entry& e : entries_) lines << e.display;
    return lines.join(QLatin1Char('\n'));
}

QString TrackerModel::displayFor(unsigned trackerId) const
{
    for (const Entry& e : entries_)
        if (e.id == trackerId) return e.display;
    return QString();
}

// ---- the honesty rules -----------------------------------------------------------------------------------

bool willSubmit(bool hardcoreActive, bool loggedIn)
{
    // Both halves matter and neither is redundant: a signed-out player's softcore session cannot submit, and
    // neither can a signed-in player's softcore session. The site accepts leaderboard entries from hardcore
    // sessions only.
    return hardcoreActive && loggedIn;
}

QString submissionNote(bool hardcoreActive, bool loggedIn)
{
    if (!loggedIn)
        return QCoreApplication::translate("ra", "Not submitting — sign in to RetroAchievements to compete");
    if (!hardcoreActive)
        return QCoreApplication::translate("ra", "Not submitting — leaderboards only accept hardcore runs");
    return QCoreApplication::translate("ra", "Submitting — hardcore run");
}

QString attemptNotice(EventKind kind, const Leaderboard& lb, bool willSubmitNow)
{
    switch (kind)
    {
    case EventKind::AttemptStarted:
        return willSubmitNow ? QCoreApplication::translate("ra", "Attempt started")
                             : QCoreApplication::translate("ra", "Attempt started — not submitting");
    case EventKind::AttemptFailed:
        return QCoreApplication::translate("ra", "Attempt failed");
    case EventKind::AttemptSubmitted:
        if (!willSubmitNow)
            return lb.trackerValue.isEmpty()
                ? QCoreApplication::translate("ra", "Not submitted (hardcore only)")
                : QCoreApplication::translate("ra", "%1 — not submitted (hardcore only)").arg(lb.trackerValue);
        return lb.trackerValue.isEmpty()
            ? QCoreApplication::translate("ra", "Submitted")
            : QCoreApplication::translate("ra", "Submitted %1").arg(lb.trackerValue);
    default:
        return QString();
    }
}

QString scoreboardNotice(const Scoreboard& sb)
{
    if (sb.numEntries == 0)
        return QCoreApplication::translate("ra", "Submitted %1").arg(sb.submitted);
    if (sb.best != sb.submitted && !sb.best.isEmpty())
        return QCoreApplication::translate("ra", "Submitted %1 — rank %2 of %3 (best %4)")
                   .arg(sb.submitted).arg(sb.newRank).arg(sb.numEntries).arg(sb.best);
    return QCoreApplication::translate("ra", "Submitted %1 — rank %2 of %3")
               .arg(sb.submitted).arg(sb.newRank).arg(sb.numEntries);
}

// ---- dispatch --------------------------------------------------------------------------------------------

void dispatch(const Event& e, TrackerModel& tracker, Sink& sink)
{
    switch (e.kind)
    {
    case EventKind::AttemptStarted:   sink.attemptStarted(e.leaderboard);   return;
    case EventKind::AttemptFailed:    sink.attemptFailed(e.leaderboard);    return;
    case EventKind::AttemptSubmitted: sink.attemptSubmitted(e.leaderboard); return;
    case EventKind::SubmitResult:     sink.submitResult(e.scoreboard);      return;

    case EventKind::TrackerShow:
        tracker.show(e.trackerId, e.trackerDisplay);
        sink.trackerChanged(tracker.visible(), tracker.display());
        return;

    case EventKind::TrackerUpdate:
    {
        // Only report a change the model actually took. rc_client updates trackers every frame an attempt's
        // value moves, and repainting the overlay for an id that is not showing would be pure churn on the
        // GUI thread while somebody is mid-run.
        if (!tracker.has(e.trackerId)) return;             // not showing: nothing to update
        if (tracker.displayFor(e.trackerId) == e.trackerDisplay) return; // the overlay already reads this
        tracker.update(e.trackerId, e.trackerDisplay);
        sink.trackerChanged(tracker.visible(), tracker.display());
        return;
    }

    case EventKind::TrackerHide:
    {
        const int before = tracker.count();
        tracker.hide(e.trackerId);
        // A hide for an id that was never shown must not repaint (nor claim the overlay went away while a
        // second, still-running attempt is being tracked).
        if (tracker.count() != before) sink.trackerChanged(tracker.visible(), tracker.display());
        return;
    }

    case EventKind::None:
    default:
        return;
    }
}

} // namespace ra
