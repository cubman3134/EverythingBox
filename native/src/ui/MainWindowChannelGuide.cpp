// PERSONAL TV CHANNELS: the guide, and the bumpers between programmes (issue #179, increment 2) — the
// MainWindow half, in its own translation unit for MainWindowPlayOn.cpp's reason (that file is the busiest
// merge surface in the repository; this feature costs it three short insertions instead of two hundred
// lines). These are ordinary member functions, declared in MainWindow.h beside the rest.
//
// THREE THINGS LIVE HERE, and all three exist to keep one promise: what the guide printed is what the tuner
// plays.
//
//   1. tuneChannelFromGuide — a pressed CELL. It tunes the channel and then compares what the clock reached
//      with what the cell claimed. Equal is the ordinary case and says nothing. NOT equal happens for one
//      honest reason (the cell is a future programme; nothing can play it now) and would happen for a
//      dishonest one (the guide and the tuner disagreeing), so it is REPORTED rather than swallowed: the
//      viewer is told when the thing they pressed starts, or when it finished. A guide that quietly plays
//      something else is the exact failure this increment was written to make impossible.
//
//   2. channelInterstitials — the bumper pool, from this channel's folder or the global Setting, through the
//      SAME duration gate a programme passes. Nothing is downloaded, nothing is generated, and a missing or
//      empty folder is not an error: it is the default state of the feature.
//
//   3. awaitChannelProgramme — dead air. A gap too short for the shortest bumper is a few seconds of nothing
//      between two programmes, and the schedule says so honestly. Being thrown out of the channel for it
//      would be worse than the silence, so the window waits for the next programme instead — bounded, so a
//      channel that is genuinely off for the rest of the day still says so.
#include "MainWindow.h"

#include <QDateTime>
#include <QTimer>

#include "../browse/ChannelGuide.h"
#include "../core/ChannelLineup.h"
#include "../core/ChannelStore.h"
#include "../core/Settings.h"
#include "FeedbackPolicy.h"

// The longest dead air the window will sit through before calling a channel off air. Generous enough to cover
// any gap a break grid can leave (half an hour is the widest grid offered) and bounded so a channel whose day
// has ended does not leave a timer standing for hours.
static constexpr int kChannelGapWaitMax = 35 * 60;

QVector<channels::LineupItem> MainWindow::channelInterstitials(const channels::Channel& ch)
{
    QStringList skipped;
    QString     why;
    const QVector<channels::LineupItem> pool =
        ChannelLineup::interstitials(ch, Settings::interstitialFolder(), &skipped, &why);
    // THE ONE SENTENCE A BROKEN FOLDER IS OWED. A path that cannot be enumerated is a typo, and a typo that
    // fails silently is a feature the user believes is on. A folder that is merely EMPTY says nothing: that is
    // the default and the quiet case.
    if (!why.isEmpty()) notify(why, kFeedbackLong);
    if (!skipped.isEmpty())
        qInfo("[channels] bumpers: %d file(s) skipped - no known duration (%s)",
              int(skipped.size()), qPrintable(skipped.mid(0, 5).join(QStringLiteral(", "))));
    return pool;
}

void MainWindow::tuneChannelFromGuide(const QString& channelId, qint64 cellStartUtc)
{
    // The cell's own claim, read from the SAME cut the guide drew, before anything is tuned. Read here rather
    // than after the tune because tuning moves the clock on by however long the file took to open, and the
    // question being asked is about the second the viewer pressed.
    channels::Channel ch;
    if (!ChannelStore::get(channelId, ch))
    { notify(tr("That channel no longer exists."), kFeedbackShort); return; }

    const qint64 nowUtc = QDateTime::currentSecsSinceEpoch();
    const int    tzOff  = QDateTime::currentDateTime().offsetFromUtc();
    const channels::Schedule day =
        channelSchedules_.dayFor(ch, nowUtc, tzOff, ChannelLineup::build(ch));
    channels::Slot cell;
    const bool cellKnown = channels::programmeStartingAt(day, cellStartUtc, cell);

    tuneChannel(channelId);

    // …and now the comparison. `whatsOn` over the same frozen day is what tuneChannel just resolved from, so
    // "the cell is the programme that is on" is decided by the schedule and not by guessing from the player.
    if (!cellKnown || !channelTuned()) return;
    const channels::Airing air = channels::whatsOn(day, QDateTime::currentSecsSinceEpoch());
    if (air.valid && air.current.startUtc == cell.startUtc) return;    // the ordinary case: say nothing

    // THE CELL IS NOT WHAT IS ON. Two different sentences, because a live drive read the future one over a
    // programme that had finished eleven hours earlier and it was simply false. A broadcast cannot play a
    // programme early and cannot play one again, and the viewer is owed whichever of those two it is.
    const QString when = QDateTime::fromSecsSinceEpoch(cell.startUtc).toString(QStringLiteral("HH:mm"));
    if (cell.startUtc > QDateTime::currentSecsSinceEpoch())
        notify(tr("“%1” starts at %2 on %3 — tuned to what is on now.").arg(cell.title, when, ch.name),
               kFeedbackLong);
    else
        notify(tr("“%1” finished at %2 — tuned to what is on %3 now.").arg(
                   cell.title,
                   QDateTime::fromSecsSinceEpoch(cell.endUtc()).toString(QStringLiteral("HH:mm")),
                   ch.name),
               kFeedbackLong);
}

void MainWindow::awaitChannelProgramme(const QString& channelId, qint64 nowUtc, qint64 startsAtUtc)
{
    channels::Channel ch;
    if (!ChannelStore::get(channelId, ch)) return;
    const qint64 waitSec = startsAtUtc - nowUtc;
    if (waitSec <= 0 || waitSec > kChannelGapWaitMax)
    {
        notify(tr("“%1” is off air right now.").arg(ch.name), kFeedbackLong);
        exitTunedChannel();
        return;
    }

    // STILL TUNED. The ring is re-read here for the same reason tuneChannel re-reads it: a channel added or
    // deleted on another device during the wait must be in (or out of) it when Up is next pressed. Sitting in
    // a break is being on the channel, so surfing has to keep working.
    tunedChannelIds_.clear();
    for (const channels::Channel& c : ChannelStore::list()) tunedChannelIds_ << c.id;
    tunedChannelId_ = channelId;

    if (!channelGapTimer_)
    {
        channelGapTimer_ = new QTimer(this);
        channelGapTimer_->setSingleShot(true);
    }
    channelGapTimer_->disconnect();          // one pending wait at a time; a re-tune replaces the last
    const QString id = channelId;
    connect(channelGapTimer_, &QTimer::timeout, this, [this, id] {
        if (tunedChannelId_ == id) tuneChannel(id);   // untuned meanwhile -> the wait is simply abandoned
    });
    // One second past the start, not on it: mpv's open takes a moment either way, and being a second late to a
    // programme is a second of its top missed, where being a second early is `whatsOn` still answering with
    // the gap and this whole path running again.
    channelGapTimer_->start(int(waitSec + 1) * 1000);

    notify(tr("“%1” — next up at %2.")
               .arg(ch.name, QDateTime::fromSecsSinceEpoch(startsAtUtc).toString(QStringLiteral("HH:mm"))),
           kFeedbackShort);
}
