// ANIME / MANGA TRACKERS (issue #156), the MainWindow half — a SEPARATE translation unit that defines
// MainWindow's #156 members, the way MainWindowPlayOn.cpp defines #143's.
//
// INCREMENT 3 ADDED KITSU AND CHANGED NOTHING ELSE IN THIS FILE except one element of trackerList() and
// one status line. That is the claim the increment exists to test, and it is visible here: the prompt,
// the fan-out, the reconcile and the “Track…” verb all work off tracker::Tracker and never name a
// service.
//
// WHY IT MOVED HERE IN INCREMENT 2. Increment 1 put this glue at the bottom of MainWindow.cpp, where one
// tracker's worth of it was small enough not to matter. A SECOND tracker turned every "AniList" in it into
// "each configured tracker", and MainWindow.cpp is the single busiest merge surface in the repository —
// several branches land in it at once and each pays for the others' conflicts. Nothing about the class
// changed: these are ordinary member functions, declared in MainWindow.h beside the rest.
//
// WHAT LIVES HERE AND WHAT DOES NOT. The rules are all pure and live elsewhere — TrackerRules (both wire
// formats, the debounce, the queue, furthest-wins, the match confidence), TrackerLinks (which entry an item
// is), TrackerQueue (the one offline queue, per tracker) and TrackerFanout (what "several trackers at once"
// means). What is here is the impure remainder: the prompting, the marks write, and the two hooks that
// decide WHEN a progress event happened.
//
// SEVERAL TRACKERS AT ONCE, in one sentence: a completion event goes to EVERY connected tracker, each with
// its own link, its own queue and its own rate limit, and one of them being off, unlinked or refusing does
// not stop another from receiving it. The one thing that is NOT fanned out is the PROMPT — at most one
// tracker is asked about per event, because two nested pick loops stacked on one page turn is both the
// #28/#211 crash family and an unusable interruption. The next event offers the next tracker.
#include "MainWindow.h"

#include "FeedbackPolicy.h"   // kFeedbackShort/Long — feedback duration policy

#include "../core/AniListTracker.h"
#include "../core/ItemMarks.h"
#include "../core/KitsuTracker.h"
#include "../core/MyAnimeListTracker.h"
#include "../core/TrackerFanout.h"
#include "../core/TrackerLinks.h"
#include "../core/TrackerQueue.h"

#include "nav/NavOverlay.h"   // NavMenu::pick

// The trackers this window owns, in a STABLE order — the order they shipped in, so a user with all three
// connected is asked about them in the same order every time rather than in whatever order a hash
// happened to produce. Nulls are tolerated: on an early path a member may not be constructed yet, and
// TrackerFanout::active drops them.
//
// THIS LIST IS THE WHOLE OF WHAT A THIRD TRACKER COST THIS FILE (issue #156 increment 3). Everything
// below works off the seam, over trackerList(), so adding Kitsu is one element here and one status
// line — not a third branch in the fan-out, the prompt, the refresh or the detail verb.
QVector<tracker::Tracker*> MainWindow::trackerList() const
{
    return QVector<tracker::Tracker*>{ static_cast<tracker::Tracker*>(anilist_),
                                       static_cast<tracker::Tracker*>(mal_),
                                       static_cast<tracker::Tracker*>(kitsu_) };
}

tracker::Tracker* MainWindow::trackerById(tracker::Id id) const
{
    for (tracker::Tracker* t : trackerList())
        if (t && t->id() == id) return t;
    return nullptr;
}

// The status line's shape, spelled ONCE for every tracker. Both settings builders and both trackers read it,
// so the four surfaces cannot tell the user different things about the same queue.
static QString trackerStatusLineFor(bool configured, bool connected, int queued, const QString& err,
                                    const QString& setupHint)
{
    if (!configured) return setupHint;
    if (!connected)  return MainWindow::tr("Set up, but not connected.");
    // The queue depth is the ONLY thing that distinguishes "connected and delivering" from "connected and
    // silently accumulating" - without it a broken push looks exactly like nothing to push.
    QString s = queued > 0
        ? MainWindow::tr("Connected. %n update(s) waiting to be sent.", nullptr, queued)
        : MainWindow::tr("Connected. Everything has been sent.");
    // Never a credential: see AniListTracker.h / MyAnimeListTracker.h. These lines are sentences of ours.
    if (!err.isEmpty()) s += QStringLiteral("  ") + err;
    return s;
}

QString MainWindow::anilistStatusLine()
{
    return trackerStatusLineFor(AniListTracker::isConfigured(), AniListTracker::isConnected(),
                                AniListTracker::queuedCount(), AniListTracker::lastError(),
                                tr("Not set up. Paste a Client ID and Secret to begin."));
}

// ...and Kitsu's (increment 3). The SAME builder again, which is the point: a third tracker added a row
// to the list above and a hint to the line below, and nothing else in this file moved.
QString MainWindow::kitsuStatusLine()
{
    // configured() and connected() are the same question on Kitsu - there is no client to register, so
    // "set up but not signed in" is a state the user cannot be in. Passing the same fact twice is what
    // makes the shared builder skip straight from the hint to the connected line.
    return trackerStatusLineFor(KitsuTracker::isConfigured(), KitsuTracker::isConnected(),
                                KitsuTracker::queuedCount(), KitsuTracker::lastError(),
                                tr("Not signed in. Enter your Kitsu email and password to begin."));
}

QString MainWindow::malStatusLine()
{
    // The hint differs because MAL's requirement differs: it issues PUBLIC clients with no secret at all,
    // so the Client ID alone is enough to be "set up" (MyAnimeListTracker::isConfigured).
    return trackerStatusLineFor(MyAnimeListTracker::isConfigured(), MyAnimeListTracker::isConnected(),
                                MyAnimeListTracker::queuedCount(), MyAnimeListTracker::lastError(),
                                tr("Not set up. Paste a Client ID to begin."));
}

void MainWindow::trackerNoteProgress(const QString& itemKey, const QString& title, int year,
                                     tracker::Kind kind, int unit, bool completes)
{
    if (itemKey.isEmpty() || unit <= 0) return;

    // EVERY connected tracker, each with its own link and its own queue. One being off, unlinked or failing
    // cannot stop another receiving this — see TrackerFanout.
    const TrackerFanout::Result r = TrackerFanout::push(trackerList(), itemKey, kind, unit, completes);

    // AT MOST ONE PROMPT per progress event, for the tracker that has waited longest in the fixed order.
    // The others are offered on the next event, or straight away from the detail view's "Track…" verb.
    if (r.needLink.isEmpty()) return;
    tracker::Tracker* ask = r.needLink.first();
    tracker::Update pending;
    pending.itemKey = itemKey;
    pending.kind = kind;
    pending.unit = unit;
    pending.completes = completes;
    // DEFERRED A TURN (issues #28 / #211). Both callers are inside a signal delivery - a page-changed
    // emission from the comic reader, a stop from the player - and the prompt spins NavMenu::pick, a nested
    // event loop. Every value it needs is captured BY VALUE here, at the boundary, so nothing it reads a
    // turn later can have been cleared underneath it.
    const QString t = title;
    const tracker::Id id = ask->id();
    deferPastQmlEmission([this, id, itemKey, t, year, kind, pending] {
        trackerPromptLink(id, itemKey, t, year, kind, pending);
    });
}

void MainWindow::trackerPromptLink(tracker::Id id, QString itemKey, QString title, int year,
                                   tracker::Kind kind, tracker::Update pending)
{
    tracker::Tracker* tr_ = trackerById(id);
    if (!tr_ || itemKey.isEmpty() || title.trimmed().isEmpty()) return;
    // Re-checked after the deferral: the user may have declined this very item from the detail view in the
    // turn between the progress event and this call.
    if (!TrackerLinks::shouldPrompt(id, itemKey) && pending.unit > 0) return;
    const QString who = tr_->displayName();
    tr_->search(title, year, kind, [this, id, who, itemKey, title, kind, pending](QVector<tracker::Match> ms) {
        if (ms.isEmpty())
        {
            notify(tr("%1 had nothing matching \u201C%2\u201D.").arg(who, title), kFeedbackShort);
            return;
        }
        QStringList rows;
        for (const tracker::Match& m : ms)
        {
            QString row = m.title;
            if (m.year > 0) row += QStringLiteral(" (%1)").arg(m.year);
            if (!m.altTitle.isEmpty()) row += QStringLiteral("  \u00B7  ") + m.altTitle;
            rows << row;
        }
        // The refusal is a ROW, not a Back: Back means "not now" and asks again next chapter, this means
        // "never" and is remembered. Two different answers, so two different ways to give them.
        const int declineRow = rows.size();
        rows << tr("This is not on %1 \u2014 stop asking").arg(who);
        const int pick = NavMenu::pick(tr("Track \u201C%1\u201D on %2").arg(title, who), rows, this);
        if (pick < 0) return;                                  // Back: ask again next time
        if (pick == declineRow) { TrackerLinks::decline(id, itemKey); return; }
        if (pick >= ms.size()) return;
        // NOTHING IS WRITTEN THAT THE USER DID NOT CHOOSE. The match list is ranked and its noise dropped
        // before it gets here (tracker::rankMatches, on the MAL path), but the LINK is only ever the row a
        // person pressed: a wrong link writes somebody's progress onto the wrong series in a list they
        // curate by hand, which is worse than no sync at all.
        const tracker::Match& m = ms[pick];
        TrackerLinks::set(id, itemKey, m.mediaId, m.kind, m.title, m.totalUnits);
        notify(tr("Linked to \u201C%1\u201D on %2.").arg(m.title, who), kFeedbackShort);
        // PULL FIRST, then replay the progress that triggered the prompt. In that order because the pull is
        // what tells us whether the account is already ahead of this chapter - reversing it would push a
        // lower number at an account that had read further, and then have to be corrected by the pull.
        trackerRefreshItem(id, itemKey);
        if (pending.unit > 0)
            trackerNoteProgress(itemKey, title, 0, m.kind, pending.unit, pending.completes);
    });
}

void MainWindow::trackerRefreshItem(tracker::Id id, QString itemKey)
{
    tracker::Tracker* tr_ = trackerById(id);
    if (!tr_ || itemKey.isEmpty()) return;
    const TrackerLinks::Link link = TrackerLinks::get(id, itemKey);
    if (!link.linked()) return;
    const QString who = tr_->displayName();
    tr_->fetchEntry(link.mediaId, link.kind, [this, id, who, itemKey](bool ok, tracker::Entry e) {
        if (!ok) { notify(tr("Couldn't read your %1 progress.").arg(who), kFeedbackShort); return; }
        const TrackerLinks::Link l = TrackerLinks::get(id, itemKey);
        if (!l.linked()) return;   // unlinked while the request was in flight
        switch (tracker::reconcile(l.localUnits, e.progress))
        {
        case tracker::Reconcile::AdvanceLocal:
            // The tracker is ahead: take its number, and mark the series finished locally when the tracker
            // says it is. NEVER the other way - nothing here ever clears or lowers a local mark.
            TrackerLinks::noteLocalProgress(id, itemKey, e.progress);
            if (e.status == tracker::Status::Completed
                || (e.totalUnits > 0 && e.progress >= e.totalUnits))
                ItemMarks::setCompletion(itemKey, ItemMarks::Completion::Finished);
            notify(tr("%1 was ahead \u2014 caught up to %2.").arg(who).arg(e.progress), kFeedbackShort);
            break;
        case tracker::Reconcile::PushRemote:
        {
            // We are ahead: send what we have. Queued and debounced like any other push, on THIS tracker's
            // queue - the other one may be perfectly in step and must not be written to.
            tracker::Tracker* t2 = trackerById(id);
            if (!t2) return;
            tracker::Update u;
            u.itemKey = itemKey;
            u.mediaId = l.mediaId;
            u.kind = l.kind;
            u.unit = l.localUnits;
            u.completes = (l.totalUnits > 0 && l.localUnits >= l.totalUnits);
            t2->pushProgress(u);
            notify(tr("%1 was behind \u2014 sending your progress.").arg(who), kFeedbackShort);
            break;
        }
        case tracker::Reconcile::Nothing:
            notify(tr("%1 already matches.").arg(who), kFeedbackShort);
            break;
        }
    });
}

void MainWindow::trackerLinkVerb(QString itemKey, QString title, int year, tracker::Kind kind)
{
    if (itemKey.isEmpty()) return;
    const QVector<tracker::Tracker*> on = TrackerFanout::active(trackerList());
    if (on.isEmpty())
    {
        notify(tr("Connect an anime/manga tracker in Settings first."), kFeedbackLong);
        return;
    }
    // WHICH TRACKER, when there is more than one. This is also the escape hatch for the prompt that is only
    // ever offered for ONE tracker per progress event: from here a user can link the other one whenever
    // they like, rather than waiting for the next chapter.
    tracker::Tracker* chosen = on.first();
    if (on.size() > 1)
    {
        QStringList who;
        for (tracker::Tracker* t : on)
        {
            const TrackerLinks::Link l = TrackerLinks::get(t->id(), itemKey);
            who << (l.linked() ? tr("%1 \u2014 %2").arg(t->displayName(), l.title)
                               : tr("%1 \u2014 not linked").arg(t->displayName()));
        }
        const int pick = NavMenu::pick(tr("Track \u201C%1\u201D on\u2026").arg(title), who, this);
        if (pick < 0 || pick >= on.size()) return;
        chosen = on[pick];
    }
    const tracker::Id id = chosen->id();
    const QString name = chosen->displayName();
    const TrackerLinks::Link link = TrackerLinks::get(id, itemKey);
    if (!link.linked())
    {
        // The ESCAPE HATCH the issue calls not optional: a user who declined, or whose auto-match never
        // fired, reaches the same prompt from here. No pending progress to replay.
        trackerPromptLink(id, itemKey, title, year, kind, tracker::Update{});
        return;
    }
    const QStringList rows = { tr("Refresh from %1").arg(name), tr("Link to a different entry\u2026"),
                               tr("Unlink") };
    const int pick = NavMenu::pick(tr("Tracking \u201C%1\u201D").arg(link.title), rows, this);
    if (pick == 0) { trackerRefreshItem(id, itemKey); return; }
    if (pick == 1) { trackerPromptLink(id, itemKey, title, year, kind, tracker::Update{}); return; }
    if (pick == 2)
    {
        TrackerLinks::clear(id, itemKey);
        notify(tr("No longer tracking this on %1.").arg(name), kFeedbackShort);
    }
}
