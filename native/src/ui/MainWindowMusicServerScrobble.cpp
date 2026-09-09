// MainWindow — a music server as a scrobble destination (issue #193, increment 6).
//
// IN ITS OWN TRANSLATION UNIT, per CONTRIBUTING's rule and MainWindowPlayOn.cpp's precedent: this is one
// small, self-contained duty and folding it into MainWindow.cpp puts it in the hunk every other concurrent
// branch is also editing.
//
// The first two functions are both about the same fact: **there is one scrobble provider per configured
// music server**, not one for all of them. SubsonicScrobbleProvider.h sets out why at length (a batch has
// to be homogeneous, one asleep box must not hold up another, and the status line has to name the server) —
// the consequence here is that the SET of providers is a function of the SET of servers, and the user can
// change that set at any moment from either settings surface.
//
// AND THEN THE OTHER HALF OF THAT FACT (issue #337): a provider that goes away leaves a QUEUE behind. The
// queue is filed under the provider's id, the id is built from the server's uuid, and re-adding the same
// server mints a new uuid — so a queue whose server is gone can never be matched to a destination again.
// Two duties follow, and neither of them deletes anything without saying so first:
//
//   armScrobbleRemovalOffers   lends the browse surface the last-chance flush, since it owns the removal
//                              confirmation and this window owns the Scrobbler.
//   sweepOrphanScrobbleQueues  finds the queues that are ALREADY orphaned — on the disk of anybody who
//                              removed a server before this was written — and offers to be rid of them.
#include "MainWindow.h"
#include "../core/Scrobbler.h"
#include "../core/ScrobbleQueue.h"
#include "../core/ScrobbleRemoval.h"
#include "../core/ProfileStore.h"
#include "../core/SubsonicScrobbleProvider.h"
#include "../core/SubsonicServerStore.h"
#include "HomeView.h"
#include "nav/NavOverlay.h"

#include <QPointer>
#include <QSet>

void MainWindow::syncSubsonicScrobbleProviders()
{
    if (!scrobbler_) return;

    // NEVER A REBUILD, and that is deliberate rather than lazy. Rebuilding the whole set would destroy and
    // recreate the ListenBrainz and Last.fm providers alongside these — and LastFmClient carries signal
    // connections made once at startup (connectedChanged, authUrl) plus an authorisation poll that a
    // recreation would silently abandon mid-flight, which a user half way through linking Last.fm would
    // experience as it simply never finishing.
    //
    // SO THE SET IS RECONCILED IN BOTH DIRECTIONS, ONE PROVIDER AT A TIME (issue #299). It used to be
    // add-only, on the argument that a removed server's provider answers configured() == false and so
    // accepts nothing, queues nothing and posts nothing. True, and it is still installed: counted by
    // providers(), walked by every pump, and there until the next launch. It is removed here by ID — the
    // one whose server went away and nothing else, so no other destination's backoff, in-flight submission
    // or authorisation poll is touched.
    QStringList installed;
    for (const ScrobbleProvider* p : scrobbler_->providers()) installed.push_back(p->id());
    QStringList serverIds;
    for (const SubsonicServer& s : SubsonicServerStore::list())
        if (!s.id.isEmpty()) serverIds.push_back(s.id);
    for (const QString& stale : SubsonicScrobbleProvider::staleIds(installed, serverIds))
        scrobbler_->removeProvider(stale);

    QSet<QString> have;
    for (const ScrobbleProvider* p : scrobbler_->providers()) have.insert(p->id());

    for (const SubsonicServer& s : SubsonicServerStore::list())
    {
        if (s.id.isEmpty()) continue;
        const QString id = SubsonicScrobbleProvider::idFor(s.id);
        if (have.contains(id)) continue;
        scrobbler_->addProvider(new SubsonicScrobbleProvider(s.id, scrobbler_));
        have.insert(id);
    }
    // The settings line says how many destinations there are and what each of them last did; adding one
    // changes it, and the surface that is up owns the refresh (the scrobbleStatusUpdate_ idiom).
    if (scrobbleStatusUpdate_) scrobbleStatusUpdate_();
}

void MainWindow::armScrobbleRemovalOffers()
{
    // THE LAST-CHANCE FLUSH, lent to the surface that owns the removal confirmation. The adaptation is the
    // whole body: HomeView is handed three plain values rather than this feature's own result type, so its
    // header carries no scrobble include and a probe could stand in for this hook in two lines.
    if (home_)
    {
        QPointer<MainWindow> self(this);
        home_->setMusicServerFlushHook(
            [self](const QString& serverId, std::function<void(int, int, const QString&)> done) {
                if (!self || !self->scrobbler_)
                {
                    // No orchestrator: nothing was sent and everything is still waiting. Answered rather
                    // than dropped — a caller waiting to tell the user what happened must never be left
                    // waiting, and the browse surface's card is the one that says what is being discarded.
                    if (done) done(0, ScrobbleQueue::count(SubsonicScrobbleProvider::idFor(serverId)),
                                   QString());
                    return;
                }
                self->scrobbler_->flushProvider(SubsonicScrobbleProvider::idFor(serverId),
                                                [done](ScrobbleFlush f) {
                    if (done) done(f.sent, f.left, f.message);
                });
            });
    }

    // The sweep is NOT armed here. It is per PROFILE and it needs one — see below.
}

// THE ORPHANS THAT ALREADY EXIST (issue #337).
//
// Everything above this point is about not creating new ones. This is about the disk of somebody who
// removed a music server months ago: their listens are still in the ini, filed under a provider id whose
// server is gone, and nothing in the app will ever look at that row again. It cannot be found through the
// INSTALLED providers — nothing installs a provider for a server that is not configured, which is precisely
// why the rows became invisible — so the sweep reads the ids out of the STORE and asks the same pure
// question #299 already answers about installed ones: which of these belong to no server?
//
// NOTHING CAN BE OFFERED HERE. There is no sign-in for a removed server and no address to send to, so
// "send them first" is not on the table; that offer exists only at the one moment before the removal, which
// is the whole reason it had to be built there. What is left is an honest account and a choice.
//
// KEEPING THEM IS NOT REMEMBERED ACROSS LAUNCHES, on purpose. A persisted "do not ask again" would turn
// this back into exactly the state the issue is about — data on disk that nobody will ever be told about
// again — so a user who keeps them is asked once more the next time the app starts, and that is the cost of
// keeping them.
//
// ONCE PER PROFILE, AND NOT BEFORE THERE IS ONE. Every key this reads is namespaced by the active profile
// (Scrobble::stateKeyPrefix), so running it during startup — while the profile PICKER is still up, which is
// where it first landed — asks about the default slot's queues on behalf of a user who is about to open a
// different profile, and never asks about theirs at all. So it is called from the two places the home
// actually appears for a chosen profile, and remembers which profiles it has already asked about: a
// mid-session switch to another profile sweeps that one too.
void MainWindow::sweepOrphanScrobbleQueues()
{
    static QSet<QString> asked;
    const QString profile = ProfileStore::currentId();
    if (asked.contains(profile)) return;
    // Marked BEFORE the work, not after: this runs on every arrival at the home screen, and the answer for a
    // profile does not change until something is removed (which asks its own question).
    asked.insert(profile);

    QStringList serverIds;
    for (const SubsonicServer& s : SubsonicServerStore::list())
        if (!s.id.isEmpty()) serverIds.push_back(s.id);

    // staleIds over what is ON DISK rather than over what is installed. Same predicate, and deliberately
    // the same function: it already names our own ids only, so a ListenBrainz or Last.fm queue — which is
    // owed to a service that is still perfectly reachable — can never be swept up by this.
    const QStringList orphans = SubsonicScrobbleProvider::staleIds(ScrobbleQueue::providerIdsOnDisk(),
                                                                   serverIds);
    QStringList withPlays;
    int plays = 0;
    for (const QString& pid : orphans)
    {
        const int n = ScrobbleQueue::count(pid);
        if (n <= 0) continue;      // a leftover counter is bookkeeping, not somebody's listening history
        withPlays.push_back(pid);
        plays += n;
    }
    if (withPlays.isEmpty()) return;

    const int choice = NavConfirm::ask(tr("Unsent listening history"),
        ScrobbleRemoval::sweepMessage(int(withPlays.size()), plays),
        { ScrobbleRemoval::sweepKeepLabel(), ScrobbleRemoval::sweepDiscardLabel(plays) },
        /*focusIndex*/ 0, /*cancelIndex*/ 0, this);
    if (choice != 1) return;                       // Keep, or backed out: nothing is deleted
    for (const QString& pid : withPlays) ScrobbleQueue::forget(pid);
    if (scrobbleStatusUpdate_) scrobbleStatusUpdate_();
}
