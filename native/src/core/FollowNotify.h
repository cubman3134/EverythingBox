// Grouped new-item notifications for followed series (issue #155, increment 2) — the PURE half.
//
// What one refresh cycle found becomes AT MOST ONE notification. Everything that decides whether there is one
// and what it says lives here, QtCore-only, so probe_follow drives every rule headless with no tray, no window
// and no event loop:
//
//   * newsFor      — which of a series' pending children this cycle actually announced, minus anything the
//                    user has already dealt with (seen, completed, hidden) by the time the cycle ended.
//   * dropMuted    — a series whose follow mark carries the per-series mute never reaches the grouping.
//   * mergeNews    — two batches folded into one, by series and then by child id, so a child is never
//                    counted twice however many cycles it rides through.
//   * summarize    — the text. One series: "<Series>: 3 new episodes" with the newest child's title as the
//                    body. Several: "New in 4 series you follow", naming up to three and "and N more".
//   * Outbox       — the consent / one-time prompt / full-screen hold state machine that turns a cycle into
//                    zero or one Notice, holding a summary while playback owns the screen and merging it with
//                    whatever later cycles find.
//
// The DELIVERY (a QSystemTrayIcon message on desktop; nothing yet on Android/iOS) is MainWindow's, in
// ui/MainWindowFollowNotify.cpp. It asks this file what to show and shows it; it decides nothing.
#pragma once
#include "FollowSnapshot.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace follownotify
{
    // One announced child.
    struct NewChild
    {
        QString id;
        QString title;
        qint64  foundAt = 0;
    };

    // What one series contributed to a notification.
    struct SeriesNews
    {
        QString seriesId;
        QString seriesTitle;
        QVector<NewChild> children;   // de-duplicated by id
        int count() const { return int(children.size()); }
        qint64 newestAt() const;
        QString newestTitle() const;  // the title of the newest child ("" when there are none)
    };

    // The single message a cycle (or a held run of cycles) produces.
    struct Notice
    {
        QString title;
        QString body;
        int     seriesCount = 0;
        int     itemCount = 0;
    };

    // The global "Notify me about new episodes" setting. UNSET is its own state because the one-time prompt
    // is offered only while the user has never answered.
    enum class Consent { Unset, On, Off };

    // Build one series' contribution from the snapshot's pending list: the children whose id this cycle
    // announced (`foundIds`) and that are still pending and not dealt with. `dealtWith(childId)` is the same
    // completion/hidden test the New shelf's rows use. A child the user marked seen before the cycle ended is
    // no longer pending, or is dealt with — either way it does not count.
    SeriesNews newsFor(const QString& seriesId, const QString& seriesTitle,
                       const QVector<FollowSnapshot::Pending>& pending, const QStringList& foundIds,
                       const std::function<bool(const QString& childId)>& dealtWith);

    // Drop every series the user muted, and every series left with no children. Runs BEFORE grouping.
    QVector<SeriesNews> dropMuted(const QVector<SeriesNews>& in,
                                  const std::function<bool(const QString& seriesId)>& isMuted);

    // Fold `later` into `earlier`: series matched by id, children unioned by id (a child already present keeps
    // its first entry), series order = earlier's, then later's new ones.
    QVector<SeriesNews> mergeNews(const QVector<SeriesNews>& earlier, const QVector<SeriesNews>& later);

    // The grouped text. Precondition: `news` non-empty and every entry has count() > 0 (dropMuted's output).
    // Series are ranked newest-first (then by count, then by title) so the names a multi-series message
    // lists are the most recent ones.
    Notice summarize(const QVector<SeriesNews>& news);

    // "A", "A and B", "A, B and C", "A, B, C and 2 more" — the name list of a multi-series body.
    QString nameList(const QStringList& names, int maxNamed = 3);

    // ---- the state machine ------------------------------------------------------------------------------
    struct Decision
    {
        QVector<Notice> notices;   // what to show now — never more than one
        bool prompt = false;       // show the one-time "turn notifications on?" toast now
        bool held = false;         // something was queued behind playback
    };

    class Outbox
    {
    public:
        // Whether the one-time prompt has ever been shown (persisted by the caller).
        void setPrompted(bool p) { prompted_ = p; }
        bool prompted() const { return prompted_; }

        // One finished cycle. `news` is newsFor() per series that grew; this applies the mute, the consent,
        // the prompt and the hold. `holding` = playback owns the screen right now.
        Decision onCycle(const QVector<SeriesNews>& news, Consent consent, bool holding,
                         const std::function<bool(const QString& seriesId)>& isMuted);

        // Playback stopped (or the screen was released). Delivers the held summary — re-filtered through
        // `stillNew(seriesId, childId)` and the mute, so what the user dealt with while watching drops out —
        // or the held prompt. Returns an empty Decision when nothing is held or consent went away.
        Decision release(Consent consent,
                         const std::function<bool(const QString& seriesId)>& isMuted,
                         const std::function<bool(const QString& seriesId, const QString& childId)>& stillNew);

        bool hasPending() const { return !pending_.isEmpty() || promptPending_; }
        const QVector<SeriesNews>& pending() const { return pending_; }

    private:
        QVector<SeriesNews> pending_;
        bool prompted_ = false;
        bool promptPending_ = false;
    };
}
