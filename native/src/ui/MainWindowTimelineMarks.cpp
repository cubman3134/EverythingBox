// The marked-up timeline (issue #85) — MainWindow's half.
//
// The seek bar has always drawn two facts: how long the file is and where you are in it. This adds the two
// the app already knew and was not showing — where the chapters begin, and which stretches are the intro and
// the end credits — and it adds NOTHING ELSE. No fetch, no job, no setting, no cache. Every number below is
// already in this window because some other feature put it there:
//
//   * currentChapters() is the list the chapter buttons and the sleep timer already navigate (mpv's own, or
//     an Audiobookshelf book's rebased onto the part that is open);
//   * segArmed_ is exactly what gatherSegments() resolved and armed the skip tracker with — the same ranges
//     the skip chip offers, so the bar and the chip can never disagree about where the intro is.
//
// SILENT WHEN THERE IS NOTHING. An unchaptered file with no segments gets an empty model, which is a bar
// that paints exactly what it painted before this file existed. That is the same rule the seek PREVIEW
// half of #85 states at the top of MainWindowTrickplay.cpp, and for the same reason: markup nobody can act
// on is noise, and there is nothing a viewer could do about a file that has no chapters.
//
// BOTH LAYOUTS, ONE MODEL. TimelineMarks::build() is called once here; the classic transport is handed the
// model and lays it out in pixels, the themed now-playing bar is handed the same marks as fractions of the
// bar it is drawing. Neither computes a position the other does not. Video plays on the classic player page
// under BOTH homes (see openVideoPath), so a film's ticks and bands reach a themed user through the classic
// bar; the themed audio page is the other surface, and it is where a chaptered audiobook's ticks land.
#include "MainWindow.h"

#include "SeekSlider.h"
#include "TimelineMarks.h"

#include "../media/PlaybackSession.h"
#include "../video/MpvWidget.h"
#ifdef EB_HAVE_QML
#include "../theme2/ThemeEngine.h"
#include <QQuickItem>
#endif

#include <QStringList>
#include <QVariantMap>

// The one place the model is built. Cheap enough to run on any of its three triggers: a handful of chapters
// and at most a few ranges, no allocation that matters, no I/O.
TimelineMarks::Marks MainWindow::timelineMarkModel() const
{
    // durGen_ != nextEpGen_ is "this length belongs to the PREVIOUS file" — the same epoch test gatherSegments
    // and the marks menu apply. Marking a new file up with the old one's runtime would put every tick at a
    // plausible-looking wrong place, which is worse than drawing none.
    if (duration_ <= 0.0 || durGen_ != nextEpGen_) return {};
    return TimelineMarks::build(currentChapters(), segArmed_, duration_);
}

void MainWindow::refreshTimelineMarks()
{
    const TimelineMarks::Marks m = timelineMarkModel();
    if (seek_) seek_->setMarks(m);
#ifdef EB_HAVE_QML
    pushThemedTimelineMarks();
#endif
}

QVariantList MainWindow::timelineTickFractions() const
{
    const TimelineMarks::Marks m = timelineMarkModel();
    // Inside a multi-file book the themed bar is the whole book's timeline (#218), so this part's chapters
    // sit bookPartStart() seconds along a bookTimeline_.total()-second bar. Everywhere else the bar is this
    // file and the two terms are 0 and its duration.
    const bool   book   = bookScale();
    const double offset = book ? bookPartStart() : 0.0;
    const double total  = book ? bookTimeline_.total() : m.duration;
    QVariantList out;
    for (double f : TimelineMarks::tickFractions(m.ticks, offset, total)) out << f;
    return out;
}

QVariantList MainWindow::timelineBandSpans() const
{
    const TimelineMarks::Marks m = timelineMarkModel();
    const bool   book   = bookScale();
    const double offset = book ? bookPartStart() : 0.0;
    const double total  = book ? bookTimeline_.total() : m.duration;
    QVariantList out;
    if (total <= 0.0) return out;
    for (const TimelineMarks::Band& b : m.bands)
    {
        // Clamped rather than dropped, exactly as rectForRange does it for the classic bar: the band is a
        // range and a range that hangs off the end is trimmed, not discarded.
        const double s = qBound(0.0, (b.start + offset) / total, 1.0);
        const double e = qBound(0.0, (b.end   + offset) / total, 1.0);
        if (e <= s) continue;
        QVariantMap one;
        one.insert(QStringLiteral("start"), s);
        one.insert(QStringLiteral("end"),   e);
        one.insert(QStringLiteral("kind"), b.kind == TimelineMarks::BandKind::Intro
                                               ? QStringLiteral("intro") : QStringLiteral("credits"));
        out << one;
    }
    return out;
}

// A cheap identity for what the themed bar is currently showing. The push is gated on it because a QML
// binding handed the SAME value again does not re-evaluate, and because the 1 Hz progress tick would
// otherwise rewrite two list properties every second for a file whose marks never change.
QString MainWindow::timelineMarkSignature() const
{
    const QVariantList ticks = timelineTickFractions();
    const QVariantList bands = timelineBandSpans();
    if (ticks.isEmpty() && bands.isEmpty()) return QString();
    QStringList parts;
    parts.reserve(ticks.size() + bands.size());
    for (const QVariant& t : ticks) parts << QString::number(t.toDouble(), 'f', 6);
    for (const QVariant& b : bands)
    {
        const QVariantMap m = b.toMap();
        parts << QStringLiteral("%1:%2-%3").arg(m.value(QStringLiteral("kind")).toString(),
                                                QString::number(m.value(QStringLiteral("start")).toDouble(), 'f', 6),
                                                QString::number(m.value(QStringLiteral("end")).toDouble(), 'f', 6));
    }
    return parts.join(QLatin1Char('|'));
}

#ifdef EB_HAVE_QML
// Push the marks into the themed now-playing page, if that is what is on screen. Returns quietly otherwise —
// the model is rebuilt from live state whenever the page next opens, so there is nothing to queue.
void MainWindow::pushThemedTimelineMarks()
{
    QWidget* cur = themedAudioHost();
    if (!cur) return;
    QQuickItem* r = ThemeEngine::rootItem(cur);
    if (!r) return;
    const QString sig = timelineMarkSignature();
    if (sig == themedMarkSig_ && r == themedMarkRoot_) return;
    themedMarkSig_  = sig;
    themedMarkRoot_ = r;
    r->setProperty("audioMarkTicks", timelineTickFractions());
    r->setProperty("audioMarkBands", timelineBandSpans());
}
#endif
