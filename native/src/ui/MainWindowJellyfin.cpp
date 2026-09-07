// PLAYING A JELLYFIN ITEM, AND REPORTING PROGRESS BACK TO THE SERVER THAT OWNS IT (issue #83, on #160's
// foundation). MainWindow's half of the feature, in its own translation unit for the #186 reason — this is
// the file every concurrent branch collides in, and a feature that adds three hundred lines to it makes
// every one of those merges worse.
//
// ==================================================================================================
// 1. AN OPEN IS THREE QUESTIONS, ASKED ONCE
// ==================================================================================================
// A row arrives here as a QUALIFIED ID and nothing else — no url, no session, no position. That is
// deliberate and is the whole credential design (JellyfinCatalogs.h and Jellyfin.h section 3): the stream
// url carries the token in its query, so it is minted at the moment the player is handed it and is never
// written into a row, a queue, a playlist or a recents entry.
//
// JellyfinClient::prepareOpen answers all three questions in one call — how may this be played, where did
// this user get to, and what must the progress reports quote — because they are one question, and because
// asking them separately would let the playback start before the resume point arrived and jump under the
// user a second later.
//
// ==================================================================================================
// 2. THE ORDER OF THE THREE THINGS THAT HAPPEN AFTER
// ==================================================================================================
// It matters, and each step is placed against the one before it:
//
//   1. playStream() — which reaches notePlaybackStart() -> resetSegmentState(), CLEARING jellyfinSegments_,
//      reaches PlaybackSession::beginResume(), which reads this device's own stored position, and reaches
//      stopScrobble(), which is where stopJellyfinPlayback() lives.
//   2. RECORDING WHAT IS NOW PLAYING — after step 1, because step 1 ends the PREVIOUS playback. Setting
//      these first is a real bug and the fixture drive caught it: the transcript showed a Stopped report
//      for the item at position zero, one millisecond before its own Start, and then no progress at all,
//      because the id the throttled hook checks had been cleared by the call that started the playback.
//      There is a gate section on this ordering in run-headless-probes.sh, since no probe can link
//      MainWindow.
//   3. setResumeOwnedByServer(true) + seedResume() — AFTER beginResume, because beginResume is what set the
//      local answer this replaces, and because the flag it clears is cleared there.
//   4. fetchMediaSegments() — AFTER step 1, so that step 1's clear cannot wipe the answer we are waiting
//      for. The reply re-arms through regatherSegments().
//
// ==================================================================================================
// 3. WHAT IS DELIBERATELY NOT HERE
// ==================================================================================================
// Quick Connect (username and password already work, from #160), SyncPlay, downloads and live TV are named
// in the issue as later work and none of them is started here. Neither is a Jellyfin MUSIC route: #194 owns
// the music surface and consumes the same client, and two features writing one queue is how the two would
// come to disagree about what a track is.
#include "MainWindow.h"

#include "FeedbackPolicy.h"          // kFeedbackShort / kFeedbackLong: the one place a notice length lives
#include "../core/AppBrand.h"
#include "../core/AppPaths.h"
#include "../core/Jellyfin.h"
#include "../core/JellyfinClient.h"
#include "../core/ResumeStore.h"
#include "../media/PlaybackSession.h"

#include <QSettings>
#include <QStatusBar>
#include <QStringList>
#include <QUuid>

namespace {

// The open's network budget. The user has pressed a row and is looking at the player page: waiting is what
// they asked for, and a shorter budget would turn a busy server into "couldn't play that".
constexpr int kOpenBudgetMs = 15000;
// The segments fetch is strictly optional — a pre-10.10 server has no such endpoint at all — so it gets a
// short budget and no error path. See JellyfinClient::fetchMediaSegments.
constexpr int kSegmentsBudgetMs = 6000;

// This device's own stored position for a key, if it has one. The same three lines HomeView's resume
// overlay uses; they are here rather than shared because the alternative is exporting a reader from
// HomeView into MainWindow for one call, and this is the smaller coupling.
//
// IT IS ONLY EVER A FALLBACK. Jellyfin::resumeSeconds prefers the server whenever the server answered,
// including when the server answers zero — see the rule at that function for why zero has to win.
double storedLocalPosition(const QString& key)
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    s.sync();
    return s.value(ResumeStore::groupFor(key) + QStringLiteral("/pos"), 0.0).toDouble();
}

// The server's own segment token -> the type the existing stack acts on. "Outro" is Jellyfin's word for
// what this app has always called Credits, and mapping it is the whole reason the skip chip appears at the
// end of a server episode with no new UI at all.
//
// An UNKNOWN token is dropped rather than guessed at: a range armed as the wrong type is a skip that fires
// in the wrong place, which is worse than no skip.
bool segmentTypeFor(const QString& token, MediaSegments::SegmentType& out)
{
    const QString t = token.toLower();
    if (t == QLatin1String("intro"))      { out = MediaSegments::SegmentType::Intro;      return true; }
    if (t == QLatin1String("outro"))      { out = MediaSegments::SegmentType::Credits;    return true; }
    if (t == QLatin1String("credits"))    { out = MediaSegments::SegmentType::Credits;    return true; }
    if (t == QLatin1String("recap"))      { out = MediaSegments::SegmentType::Recap;      return true; }
    if (t == QLatin1String("commercial")) { out = MediaSegments::SegmentType::Commercial; return true; }
    // "Preview" is asked for by mediaSegmentsQuery and deliberately has no mapping: it is a trailer for a
    // LATER episode, not a range of this one worth skipping, and the four types above are the whole of
    // what MediaSegments::SegmentType can express.
    return false;
}

} // namespace

// ---- The open ----------------------------------------------------------------------------------------

void MainWindow::openJellyfinItem(const QString& qualifiedId, const QString& title, const QString& thumb)
{
    if (!Jellyfin::isQualified(qualifiedId)) return;   // not ours; the caller's other routes own it

    // #110: A DOWNLOADED COPY WINS, AND IS ASKED ABOUT BEFORE ANY SOCKET IS OPENED. That order is the whole
    // offline story: prepareOpen's two round trips would otherwise have to time out before a file already
    // on this disk could play, so an aeroplane would cost thirty seconds and a sentence about the server
    // being unreachable for something that needs no server at all. The same prefer-local rule the catalogue
    // rows already follow, applied at the one door every Jellyfin open comes through.
    if (const QString local = jellyfinLocalCopy(qualifiedId); !local.isEmpty())
    { playJellyfinLocalCopy(qualifiedId, local, title); return; }

    // "Finding" rather than "Loading": two round trips are about to happen against somebody's server, and
    // on a slow link the difference between a window that says nothing and one that says this is the
    // difference between a bug report and a wait.
    statusBar()->showMessage(tr("Opening “%1”…").arg(title), kFeedbackShort);

    const double local = storedLocalPosition(qualifiedId);
    // -1 for both stream indexes: THE USER HAS NOT CHOSEN. Passing a real index would pin the server to it
    // — and pinning the subtitle index to -1 in particular would suppress the server's own default
    // subtitle, which is a visible regression for anyone who set one. Jellyfin::playbackInfoBody omits the
    // fields entirely for -1, which is what makes "the server decides" true rather than nearly true.
    JellyfinClient::instance().prepareOpen(qualifiedId, local, /*audio*/ -1, /*subtitle*/ -1, kOpenBudgetMs,
        [this, qualifiedId, title, thumb](const JellyfinClient::OpenPlan& plan) {
            if (!plan.ok())
            {
                // The client's own sentence, which has never seen a url. An empty one still gets a
                // readable line rather than a silent no-op: a press that does nothing at all is the
                // failure this app has shipped before.
                notify(plan.error.isEmpty()
                           ? tr("“%1” could not be played from its Jellyfin server.").arg(title)
                           : plan.error, kFeedbackLong);
                return;
            }

            // Step 1. THE URL CARRIES THE TOKEN AND IS HANDED STRAIGHT OVER. `qualifiedId` as the resume
            // key is what makes playStream record the ID rather than the link — see the recents write at
            // the end of playStream, and Jellyfin::recordedPath for the rule.
            //
            // AND IT GOES FIRST, BEFORE THIS ITEM'S STATE IS RECORDED. playStream reaches stopScrobble,
            // which is where stopJellyfinPlayback lives — so anything set here beforehand is immediately
            // reported as STOPPED and cleared, by the very call that starts the playback. Found on the
            // fixture drive: the transcript showed a Stopped report for the item, at position zero, one
            // millisecond before its own Start, and no progress report ever followed because the id the
            // hook checks had been wiped. Setting them after playStream also means the stop that DOES go
            // out from in there is the previous item's, which is exactly what it should be.
            playStream(plan.url, qualifiedId, title);
            jellyfinPlayingId_      = qualifiedId;
            jellyfinPlaySessionId_  = plan.playSessionId;
            jellyfinMediaSourceId_  = plan.mediaSourceId;
            jellyfinLastReportedS_  = -1.0;     // the first tick always goes out
            jellyfinPlayingOffline_ = false;    // #110: this one IS streaming from the server

            // Step 2. THE POSITION IS THE SERVER'S. Both lines are after playStream on purpose:
            // beginResume (inside it) clears the flag and reads this device's stored position, and this is
            // what replaces both.
            if (session_)
            {
                session_->setResumeOwnedByServer(true);
                session_->seedResume(plan.resumeSeconds);
            }

            // ...and tell the server the playback has started, so its own session list shows it now rather
            // than in ten seconds' time.
            // #110: through the ONE report site, which sends straight at the server for a streamed item and
            // queues for a downloaded one. Every report in this feature goes through it, so there is one
            // place that knows whether there is anybody listening.
            reportJellyfinProgress(qualifiedId, Jellyfin::ProgressEvent::Start, plan.resumeSeconds,
                                   plan.playSessionId, plan.mediaSourceId);

            // Step 3. The server's intro/credits detection, as one more provider tier. AFTER playStream,
            // because playStream's resetSegmentState clears exactly this field.
            JellyfinClient::instance().fetchMediaSegments(qualifiedId, kSegmentsBudgetMs,
                [this, qualifiedId](const QVector<Jellyfin::RemoteSegment>& segments) {
                    // A REPLY FOR SOMETHING ELSE CHANGES NOTHING. The user may have moved on to another
                    // item while this was in flight, and arming its ranges over the file now playing would
                    // skip the wrong minute of the wrong episode.
                    if (jellyfinPlayingId_ != qualifiedId) return;
                    QVector<MediaSegments::Segment> out;
                    for (const Jellyfin::RemoteSegment& s : segments)
                    {
                        MediaSegments::Segment seg;
                        if (!segmentTypeFor(s.type, seg.type)) continue;
                        seg.start = s.start;
                        seg.end   = s.end;
                        out.push_back(seg);
                    }
                    if (out.isEmpty()) return;   // nothing to arm; leave the existing tiers alone
                    jellyfinSegments_ = out;
                    // Re-arm: gatherSegments is once-per-open and has very likely already run (mpv reports
                    // a duration long before a second network round trip completes). regatherSegments is
                    // the existing, explicit way past that latch, and it carries over the one piece of
                    // tracker state a re-arm must not throw away.
                    regatherSegments();
                });
        });
}

// ---- Progress ------------------------------------------------------------------------------------------

void MainWindow::onJellyfinProgress(const QString& key, double seconds)
{
    // THE HOOK FIRES FOR EVERY SERVER-OWNED ITEM, so the first thing it does is check that the item is
    // ours and is the one playing. `key` is PlaybackSession's durable identity, which for this route is
    // the qualified id it was opened with.
    if (jellyfinPlayingId_.isEmpty() || key != jellyfinPlayingId_) return;
    // ...and the SECOND gate is Jellyfin's own report interval, on top of PlaybackSession's five-second
    // throttle. It lives in Jellyfin.h because it is a fact about that API rather than about playback,
    // and it reports immediately on a backward seek — see the rule there.
    if (!Jellyfin::shouldReportProgress(jellyfinLastReportedS_, seconds)) return;
    jellyfinLastReportedS_ = seconds;
    reportJellyfinProgress(jellyfinPlayingId_, Jellyfin::ProgressEvent::Progress, seconds,
                           jellyfinPlaySessionId_, jellyfinMediaSourceId_);
}

void MainWindow::stopJellyfinPlayback()
{
    if (jellyfinPlayingId_.isEmpty()) return;   // nothing Jellyfin is playing: a cheap no-op
    // THE FINAL POSITION, TAKEN FROM THE SESSION RATHER THAN FROM THE LAST REPORT. The last report can be
    // up to ten seconds stale, and this is the number the server will show every other device — the one
    // moment in the whole feature where being a few seconds out is visible to somebody else.
    const double pos = session_ ? session_->position() : jellyfinLastReportedS_;
    const QString id = jellyfinPlayingId_;
    const QString ps = jellyfinPlaySessionId_;
    const QString ms = jellyfinMediaSourceId_;
    // #110: READ BEFORE THE CLEAR, for the offer below. "Was the thing that just played a file on this
    // disk?" is the whole precondition of remove-after-watched, and the flag that answers it is cleared at
    // the bottom of this function.
    const bool wasLocal = jellyfinPlayingOffline_;
    // CLEARED BEFORE THE REPORT, not after: this function is reached from several routes, and a second
    // entry while the first was still in flight would report the same stop twice.
    jellyfinPlayingId_.clear();
    jellyfinPlaySessionId_.clear();
    jellyfinMediaSourceId_.clear();
    jellyfinLastReportedS_ = -1.0;
    reportJellyfinProgress(id, Jellyfin::ProgressEvent::Stop, pos > 0.0 ? pos : 0.0, ps, ms);
    // #110: cleared AFTER the report, unlike the four fields above — reportJellyfinProgress reads it to
    // decide whether there is a server to talk to, and clearing it first would send an offline session's
    // final position straight at a server that is not there, losing the one report that matters most.
    // The re-entrancy the fields above guard against is already handled by jellyfinPlayingId_ being empty.
    jellyfinPlayingOffline_ = false;
    // #110: AND THE HOUSEKEEPING OFFER, LAST. This is the one site every leave-the-media route reaches, so
    // it is the only place that can be sure a finished download is noticed however the viewing ended — and
    // it is placed after the report because the server hearing about the viewing matters more than this
    // device's disk does. `duration_` is the length mpv measured for the file that just played, the same
    // number stopScrobble computes its watched percentage from. It never deletes anything itself.
    maybeOfferRemoveAfterWatched(id, pos > 0.0 ? pos : 0.0, duration_, wasLocal);
}

// ---- The offline route (issue #110) --------------------------------------------------------------------
// A DOWNLOADED ITEM IS A LOCAL ITEM, and this is what that sentence costs: no prepareOpen, no PlaybackInfo,
// no user-state read, no segments fetch. Four network round trips that a file on this disk does not need,
// and that on a plane are four timeouts between the press and the picture.
//
// Everything else is deliberately IDENTICAL to the streaming route, because the item is the same item:
//
//   * the resume key is still the qualified id, so Recents records the ID (Jellyfin::recordedPath), the
//     Downloads shelf's row re-opens through this same door, and the position is filed where the streaming
//     route would look for it;
//   * jellyfinPlayingId_ is still set, so the throttled progress hook still fires and the Stop still runs;
//   * ...but setResumeOwnedByServer is NOT called. The server cannot be asked where this user got to, so
//     THIS DEVICE's store is the authority for the length of the flight — which is exactly what
//     PlaybackSession::beginResume has already done inside playStream. Marking it server-owned would seed
//     a position nobody supplied.
//
// The play session id is minted here. The server never issued one for this viewing — there was no session —
// and every report the queue collects has to quote the same one, or the server sees a different playback per
// report. It is a uuid: an identifier, not a credential.
void MainWindow::playJellyfinLocalCopy(const QString& qualifiedId, const QString& local,
                                       const QString& title)
{
    statusBar()->showMessage(tr("Playing “%1” from your downloads…").arg(title), kFeedbackShort);
    playStream(local, qualifiedId, title);
    jellyfinPlayingId_      = qualifiedId;
    jellyfinPlaySessionId_  = QUuid::createUuid().toString(QUuid::WithoutBraces);
    jellyfinMediaSourceId_.clear();
    jellyfinLastReportedS_  = -1.0;
    jellyfinPlayingOffline_ = true;
    // THE IDENTITY IS STILL A MACHINE KEY EVEN THOUGH THE POSITION IS OURS. Without this the resume row
    // this playback writes gets "jf:<server>:<item>" as its title, and that is what the Continue Watching
    // shelf shows. See PlaybackSession::setResumeIdentityNotAName for why the two halves of #83's flag come
    // apart exactly here. AFTER playStream, because beginResume (inside it) clears the flag.
    if (session_) session_->setResumeIdentityNotAName(true);
    reportJellyfinProgress(qualifiedId, Jellyfin::ProgressEvent::Start,
                           session_ ? session_->position() : 0.0,
                           jellyfinPlaySessionId_, jellyfinMediaSourceId_);
}
