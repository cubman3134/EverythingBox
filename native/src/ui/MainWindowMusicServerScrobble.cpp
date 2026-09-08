// MainWindow — a music server as a scrobble destination (issue #193, increment 6).
//
// IN ITS OWN TRANSLATION UNIT, per CONTRIBUTING's rule and MainWindowPlayOn.cpp's precedent: this is one
// small, self-contained duty and folding it into MainWindow.cpp puts it in the hunk every other concurrent
// branch is also editing.
//
// It is two functions and both are about the same fact: **there is one scrobble provider per configured
// music server**, not one for all of them. SubsonicScrobbleProvider.h sets out why at length (a batch has
// to be homogeneous, one asleep box must not hold up another, and the status line has to name the server) —
// the consequence here is that the SET of providers is a function of the SET of servers, and the user can
// change that set at any moment from either settings surface.
#include "MainWindow.h"
#include "../core/Scrobbler.h"
#include "../core/SubsonicScrobbleProvider.h"
#include "../core/SubsonicServerStore.h"

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
