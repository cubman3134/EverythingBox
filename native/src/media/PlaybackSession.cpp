#include "PlaybackSession.h"
#include "../core/AppBrand.h"
#include "../core/AppPaths.h"
#include "../core/ConsumptionStats.h"
#include "../core/DisplayTitle.h"  // issue #202: the shared rule for what a label may be derived from
#include "../core/ResumeStore.h"   // issue #150: the key scheme + the tombstoned clear
#include "../core/StoredUrl.h"     // issue #200: the shared rule for what a synced store may write down
#include <QCryptographicHash>
#include <QFileInfo>
#include <QDateTime>
#include <algorithm>
#include <cmath>

// Stable, path-derived key prefix for one file's resume state (shared by video / audio / audiobooks).
// ResumeStore owns the hash so the clear can name this item to the tombstone store the same way the merge
// document names it; the trailing '/' is this file's own convenience for appending leaves.
static QString mediaResumeKey(const QString& path)
{
    return ResumeStore::groupFor(path) + QStringLiteral("/");
}

// Pre-generalization audiobooks were stored under "audiobook/"; read those too so in-progress books resume.
static QString legacyAudiobookKey(const QString& path)
{
    const QByteArray h = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex().left(10);
    return QStringLiteral("audiobook/") + QString::fromLatin1(h) + QStringLiteral("/");
}

PlaybackSession::PlaybackSession(const QString& settingsFile, QObject* parent)
    : QObject(parent), settingsFile_(settingsFile)
{
}

QSettings& PlaybackSession::store()
{
    if (!settings_)
    {
        const QString file = settingsFile_.isEmpty()
            ? AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile)
            : settingsFile_;
        settings_ = new QSettings(file, QSettings::IniFormat, this);
    }
    return *settings_;
}

// WHAT THIS TRACK IS CALLED, for the host to put on screen (issue #202).
//
// This was `QFileInfo(tracks_.value(index)).completeBaseName()` and NOTHING ELSE — it never consulted
// titles_ at all. For a Subsonic queue that made it the worst instance of the whole family: the session was
// holding the track's real name, one member away, and handing out a slice of the signed url's QUERY instead.
// It is emitted with every trackChanged, so it reached the classic pause/menu rows, the now-playing chip and
// the LAN /state snapshot — the last of which is JSON served to another machine.
//
// The queue's own title first, then the shared display rule. DisplayTitle rather than a local `if`, so that
// a title which is ITSELF a url (the play routes that fall back to `title = url` for an unnamed link) is
// rejected here exactly as it is everywhere else.
QString PlaybackSession::titleAt(int index) const
{
    return DisplayTitle::choose(titles_.value(index), tracks_.value(index));
}

// The display list installed with a queue. Same rule, applied once per row: the caller's title when it is
// one, else a label derived from the location — never completeBaseName() of a url.
QStringList PlaybackSession::displayTitlesFor(const QStringList& titles) const
{
    QStringList displayTitles;
    for (int i = 0; i < tracks_.size(); ++i)
        displayTitles << DisplayTitle::choose(titles.value(i), tracks_.at(i));
    return displayTitles;
}

void PlaybackSession::setQueue(const QStringList& files, int startIndex, const QStringList& titles,
                               const QString& resumeKey, const TrackHeaders& trackHeaders)
{
    tracks_ = files;
    // Assigned WITH the tracks, never appended to: a queue replaces the previous one wholesale, and headers
    // left over from the last queue would be indexed by position into a completely different track list.
    trackHeaders_ = trackHeaders;
    // #193: the display list is now KEPT, not just emitted. A later edit has to renumber the titles alongside
    // the paths, and the caller's list is only handed over here — recomputing base names at edit time would
    // silently swap the caller's titles for file names on the first insert.
    titles_ = displayTitlesFor(titles);
    emit queueChanged(titles_, startIndex, /*replaced*/ true);   // a whole new queue: the host may present it
    playIndex(startIndex);
    // playIndex resume-keyed the starting track by its file path; when the caller has a stable id (a catalog
    // stream / audiobook whose URL changes on re-resolve), re-key that starting track here instead — folded in
    // atomically so the re-key can't be forgotten or reordered by a caller (the old begin-after-setQueue bug).
    if (!resumeKey.isEmpty())
        beginResume(resumeKey);
}

void PlaybackSession::playIndex(int index)
{
    if (index < 0 || index >= tracks_.size()) return;
    persistResume();              // save where we were in the outgoing track (if any)
    trackIndex_ = index;
    beginResume(tracks_[index]);  // track the new file's position (and resume it if seen before)
    emit trackChanged(index, tracks_.size(), titleAt(index));
    // trackHeaders_ is allowed to be SHORTER than tracks_ (a local queue passes none at all), and a track
    // with no entry gets an empty set — which is the correct answer, and the one that makes the player clear
    // the previous track's headers rather than keep them.
    //
    // Spelled with an explicit bound and at(), not with value(): the log-discipline gate reads
    // `<name>Headers.value(…)` as pulling ONE header's value out into a local, and it is right to — that is
    // the hole no grep over log calls can see. This container is a LIST of header sets rather than a set, so
    // the match would be a false positive, and the fix for a false positive is not to teach the gate a
    // nuance it would then apply everywhere. It is to not write the shape.
    const StreamHeaders::Headers trackHeaders =
        index < trackHeaders_.size() ? trackHeaders_.at(index) : StreamHeaders::Headers();
    emit playRequested(tracks_[index], trackHeaders);
    // #141: a playRequested is a REPLACE-load — it resets mpv's playlist to this one entry — so (re-)arm the
    // gapless one-ahead feed from here. This is the single bootstrap for both the initial track (setQueue) and
    // every MANUAL jump (next/prev/a playlist-row click): a deliberate skip does a hard reload (nobody expects
    // gaplessness across a manual skip) and re-seeds the append frontier at the new position. AUTO-advance never
    // reaches here — it is owned by onPlaylistPos(), which moves trackIndex_ without a reload. No-op when off,
    // which is what keeps the gapless-off path byte-for-byte the pre-#141 machine.
    if (gapless_)
    {
        prevPos_ = 0;             // mpv's playlist-pos after a replace-load is 0 (one entry, about to grow)
        appendedThrough_ = index; // the entry mpv is now playing; its successor is the frontier to feed
        // #141 crossfade: with the deferral armed the host feeds this boundary itself, once it knows whether
        // the boundary is a crossfade (which needs the file mpv has not even loaded yet). Feeding here anyway
        // would hand mpv the entry it is then free to cross into on its own, and the crossfade would find the
        // boundary already spent. See setDeferAppend.
        if (!deferAppend_) maybeAppendNext(); // hand mpv the next track so its decoder crosses into it with no gap
    }
}

void PlaybackSession::setGapless(bool on)
{
    // Armed by the host BEFORE setQueue, and only for an audio queue with the setting on. Off is the default
    // and the no-regression guarantee: with it off, playIndex takes no gapless branch, onPlaylistPos()
    // early-returns, and appendRequested() is never emitted — the EOF -> handleTrackEnd advance is unchanged.
    gapless_ = on;
}

void PlaybackSession::armGaplessLive()
{
    // See the header for why merely setting gapless_ here would arm a feed that can never fire.
    if (gapless_ || trackIndex_ < 0) return;
    gapless_ = true;
    prevPos_ = 0;                    // mpv holds the one entry its last replace-load gave it
    appendedThrough_ = trackIndex_;  // …which is the playing entry: its successor is the frontier to feed
}

void PlaybackSession::setDeferAppend(bool on)
{
    // Armed by the host alongside setGapless, for a queue where crossfade is enabled. Off is the default and
    // the no-regression guarantee: unarmed, playIndex and onPlaylistPos feed the one-ahead entry immediately,
    // which is exactly what they did before crossfade existed.
    deferAppend_ = on;
}

void PlaybackSession::feedNextTrack()
{
    // The host's half of the deferral. maybeAppendNext's one-ahead invariant makes this idempotent, so the
    // host may call it from more than one place at the same boundary (the file-loaded and the duration
    // callback both reach the decision, whichever arrives second) without appending the same entry twice.
    if (gapless_) maybeAppendNext();
}

void PlaybackSession::maybeAppendNext()
{
    // Keep mpv's own playlist exactly one entry ahead of what it is playing: append the single queue index past
    // the frontier, if any. Fed incrementally (one append per boundary) so a long album never front-loads the
    // whole list, and carrying the per-track headers for symmetry with playRequested (empty for the local audio
    // queue gapless applies to). Emits nothing at the end of the queue — the last track has no successor.
    // The one-ahead INVARIANT, stated rather than assumed (#141 crossfade): mpv is fed the single entry after
    // the one playing, and only when it does not already have it. Both original callers satisfy this by
    // construction (playIndex seeds appendedThrough_ = index, onPlaylistPos bumps trackIndex_ first), so this
    // changes nothing for them - it is what makes the host-driven feedNextTrack() safe to call more than once
    // at the same boundary, which is the only way the deferred feed can be written without a second latch.
    if (appendedThrough_ != trackIndex_) return;
    const int next = appendedThrough_ + 1;
    if (next >= 0 && next < tracks_.size())
    {
        // Named with 'header' and read with at() (not value()), exactly as playIndex does: the log-discipline
        // gate needs the container to carry 'header' so its value-read check can see it, and a bounded at()
        // rather than value() so it is not read as pulling one header's value into a local. Same shape, same
        // reason — see the long note in playIndex.
        const StreamHeaders::Headers trackHeaders =
            next < trackHeaders_.size() ? trackHeaders_.at(next) : StreamHeaders::Headers();
        emit appendRequested(tracks_[next], trackHeaders);
        appendedThrough_ = next;
    }
}

void PlaybackSession::onPlaylistPos(int mpvPos)
{
    if (!gapless_) return;                       // off path never routes here; guard so a stray call is inert
    const int done = tracksCompleted(prevPos_, mpvPos);
    prevPos_ = mpvPos;                           // advance the cursor even for a dup/backward (which report 0)
    for (int k = 0; k < done; ++k)
    {
        // The SAME per-item work handleTrackEnd does for a track that just finished: flush its final accrual,
        // then drop its resume mark because it played to the end. audioPos_ still holds the FINISHING track's
        // last position here — mpv reports playlist-pos as it loads the next entry, before that entry's time-pos
        // resets — so the tail seconds accrue to the right track. (persistResume then finishResume, the exact
        // order handleTrackEnd uses so the last ≤5s window is not dropped.)
        persistResume();
        finishResume();
        // Advance the app's current track WITHOUT a reload — mpv already crossed into the next entry itself,
        // which is the whole point of gapless. beginResume + trackChanged are the same per-track callbacks
        // playIndex fires; the one-ahead append is refed so the FOLLOWING boundary is already decoding.
        if (trackIndex_ + 1 < tracks_.size())
        {
            advanceWithoutReload();
            if (!deferAppend_) maybeAppendNext();  // #141: deferred, the host refeeds once it has decided
        }
        else
        {
            // Defensive only. The normal final-track completion is an EOF the host routes to handleTrackEnd:
            // there is no playlist-pos PAST the last index for mpv to report, so this branch is unreachable in
            // the ordinary flow and exists so a mpv that somehow skips past the end still finishes the queue
            // rather than silently stranding it. (probe_playback documents this as a tripwire, not a path.)
            emit queueFinished();
        }
    }
}

// The per-item work a boundary the PLAYER crossed by itself still owes the app: move current on by one, re-key
// the resume to the new file, and tell the host. Extracted (#141 crossfade) because there are now two ways a
// boundary can happen without a reload - mpv's own gapless playlist advance, and the crossfade handover - and
// they owe the queue exactly the same thing. A second hand-written copy is how one of them ends up not
// re-keying the resume, which is invisible until somebody reopens the album.
void PlaybackSession::advanceWithoutReload()
{
    if (trackIndex_ + 1 >= tracks_.size()) return;
    trackIndex_ += 1;
    beginResume(tracks_[trackIndex_]);
    // #220: the OTHER way an entry gets reached. A local audiobook is an ordinary gapless queue, so its part
    // boundaries arrive here rather than through handleTrackEnd — and a reached-mark written into only one of
    // the two is the "second hand-written copy" this function's own header warns about, one book shape at a
    // time. Before trackChanged, so a host reacting to the boundary reads a store that already agrees with it.
    noteEntryReached();
    emit trackChanged(trackIndex_, tracks_.size(), titleAt(trackIndex_));
}

void PlaybackSession::onCrossfadePromoted()
{
    if (trackIndex_ < 0) return;
    // The finishing track's tail seconds accrue to IT, then its resume mark goes because it played out - the
    // same persist-then-finish order handleTrackEnd and the gapless boundary use, for the same reason (the
    // last <=5s window would otherwise be dropped by finishResume forgetting the file first).
    persistResume();
    finishResume();
    if (trackIndex_ + 1 < tracks_.size())
    {
        advanceWithoutReload();
        // RE-SEAT THE GAPLESS BOOKKEEPING. The handover swapped players: the deck now playing is a different
        // mpv instance whose playlist holds exactly ONE entry, the track that just became current. So its
        // playlist-pos starts at 0 again and its append frontier is that entry - carrying the old deck's
        // numbers across would make the next playlist-pos read as a backward jump and the next append land at
        // the wrong index. Then feed the FOLLOWING boundary the same way the gapless path does, unless the
        // host is deferring (it is, whenever crossfade is armed - which is whenever we are here).
        prevPos_ = 0;
        appendedThrough_ = trackIndex_;
        if (!deferAppend_) maybeAppendNext();
    }
    else
    {
        // Defensive, like the gapless boundary's last-track branch: a crossfade is only ever started for a
        // boundary that HAS a successor, so there is no way to promote past the end of the queue. If one ever
        // happens the queue finishes properly rather than stranding on a track nothing owns.
        emit queueFinished();
    }
}

// The OTHER thing a crossfade handover can land on: a file that is not in this queue at all. Channel mode
// (#141) airs one playlist item at a time, each as its own queue, so the boundary out of the LAST entry of
// one item and into the channel's next pick crosses between two queues — and by the time this is called the
// player is already several seconds into the second one, on the deck that just took over. So the queue is
// installed AROUND what is playing instead of starting it.
//
// Every line below is onCrossfadePromoted's, in its order and for its reasons; the only difference is that
// "advance by one" becomes "this list, at this index". That is deliberate — a channel boundary owes the app
// exactly what an in-queue boundary owes it, and the day one of them stops re-keying the resume it should be
// because somebody changed both.
void PlaybackSession::adoptPlayingQueue(const QStringList& files, int index, const QStringList& titles)
{
    if (files.isEmpty() || index < 0 || index >= files.size()) return;
    // The finishing track's tail seconds accrue to IT, then its resume mark goes because it played out — the
    // same persist-then-finish order every boundary in this class uses, so the last <=5 s window survives.
    persistResume();
    finishResume();
    tracks_ = files;
    // Cleared WITH the tracks, exactly as setQueue assigns them: this is a new track list, and the previous
    // queue's per-track headers would be indexed by position into it. A local music queue carries none, which
    // is the only kind of queue that reaches here.
    trackHeaders_ = {};
    titles_ = displayTitlesFor(titles);   // #193: kept for the same reason setQueue keeps it
    emit queueChanged(titles_, index, /*replaced*/ true);   // a channel handover installs a queue wholesale
    trackIndex_ = index;
    beginResume(tracks_[index]);
    emit trackChanged(index, tracks_.size(), titleAt(index));
    // RE-SEAT THE GAPLESS BOOKKEEPING onto the deck that is now the player — a different mpv instance, whose
    // playlist holds exactly the one entry playing. Its playlist-pos starts at 0 again and its append frontier
    // is that entry; carrying the old deck's numbers over would read the next position as a backward jump and
    // land the next append at the wrong index. Identical to onCrossfadePromoted's re-seat, for its reason.
    prevPos_ = 0;
    appendedThrough_ = index;
    if (!deferAppend_) maybeAppendNext();
}

// ================= Editing the queue you are listening to (issue #193) ======================================
//
// The list surgery is three lines each; everything that is actually difficult about editing a LIVE queue is
// either in QueueEdit (which entry mpv is holding, and whether this edit invalidated it) or in commitEdit
// below (what the host is owed as a result). The verbs themselves exist only to keep the three parallel lists
// in step, and they do that BEFORE consulting the plan's cursor, because the plan is computed from the
// pre-edit state on purpose — the arithmetic is stated against the list the caller was looking at.

// The queue's per-track headers are a SHORTER-OR-EQUAL parallel list (a local queue carries none at all). An
// edit that renumbers the tracks has to renumber them too, or an IPTV queue plays entry 5 with entry 4's
// credentials — so pad to the full length first and let the same surgery run over both. Left empty when it
// was empty, which is every local music queue and therefore the common case: padding an empty list would turn
// "this queue has no headers" into "this queue has N empty header sets", which playIndex reads identically
// but which would then be carried, copied and cleared for no reason.
void PlaybackSession::padTrackHeaders()
{
    if (trackHeaders_.isEmpty()) return;
    while (trackHeaders_.size() < tracks_.size()) trackHeaders_.push_back(StreamHeaders::Headers());
}

bool PlaybackSession::insertTracks(int at, const QStringList& files, const QStringList& titles)
{
    const QueueEdit::Plan plan =
        QueueEdit::planInsert({ int(tracks_.size()), trackIndex_, appendedThrough_ }, at, int(files.size()));
    if (!plan.valid) return false;
    padTrackHeaders();
    for (int k = 0; k < files.size(); ++k)
    {
        tracks_.insert(at + k, files.at(k));
        // Each inserted row's own title, through the same DisplayTitle rule displayTitlesFor uses for an
        // install — one rule for what a row is called, whether it arrived with the queue or was added to it
        // an hour later, and (issue #202) one place where a url can never become a label.
        titles_.insert(at + k, DisplayTitle::choose(titles.value(k), files.at(k)));
        if (!trackHeaders_.isEmpty()) trackHeaders_.insert(at + k, StreamHeaders::Headers());
    }
    return commitEdit(plan);
}

bool PlaybackSession::removeTrack(int at)
{
    const QueueEdit::Plan plan =
        QueueEdit::planRemove({ int(tracks_.size()), trackIndex_, appendedThrough_ }, at);
    if (!plan.valid) return false;
    padTrackHeaders();
    tracks_.removeAt(at);
    titles_.removeAt(at);
    if (!trackHeaders_.isEmpty()) trackHeaders_.removeAt(at);
    return commitEdit(plan);
}

bool PlaybackSession::moveTrack(int from, int to)
{
    const QueueEdit::Plan plan =
        QueueEdit::planMove({ int(tracks_.size()), trackIndex_, appendedThrough_ }, from, to);
    if (!plan.valid) return false;
    padTrackHeaders();
    tracks_.move(from, to);
    titles_.move(from, to);
    if (!trackHeaders_.isEmpty()) trackHeaders_.move(from, to);
    return commitEdit(plan);
}

bool PlaybackSession::playNext(const QStringList& files, const QStringList& titles)
{
    // With nothing playing there is no "next": append instead of inserting at 0, so a play-next fired at an
    // idle queue does not jump the whole list. (An empty queue makes both the same thing.)
    return insertTracks(trackIndex_ < 0 ? int(tracks_.size()) : trackIndex_ + 1, files, titles);
}

bool PlaybackSession::enqueue(const QStringList& files, const QStringList& titles)
{
    return insertTracks(int(tracks_.size()), files, titles);
}

// The single tail every edit runs. Announce the new list, carry the cursor and the append frontier, and say
// what mpv still owes — in that order, because the host rebuilds its row model from queueChanged and both of
// the later steps can move the highlighted row.
bool PlaybackSession::commitEdit(const QueueEdit::Plan& plan)
{
    if (plan.cursorRemoved)
    {
        // THE TRACK PLAYING WAS DELETED. Announce the shortened list first so the host is not briefly holding
        // a row model with a track in it that no longer exists, then either advance onto the entry that took
        // its place — a hard replace-load, which is also what reseeds prevPos_/appendedThrough_ onto mpv's
        // reset playlist, so no separate mpv repair is owed — or stop, when there was nothing after it.
        //
        // EITHER WAY the removed track's position is SAVED first — playIndex's own persistResume on the
        // advance branch, the explicit one on the stop branch — and it matters WHICH file that names:
        // resumePath_ still holds the removed track at this point, so the position lands under its own key
        // and the listener can come back to it. finishResume is deliberately NOT called on either branch:
        // the track did not play out, it was deleted, and dropping its bookmark would be the queue edit
        // quietly forgetting where you were in a track you only meant to take out of tonight's listening.
        trackIndex_ = plan.cursor;
        appendedThrough_ = -1;
        prevPos_ = 0;
        emit queueChanged(titles_, plan.playIndex, /*replaced*/ false);   // an EDIT: present nothing
        if (plan.playIndex >= 0)
            playIndex(plan.playIndex);
        else
        {
            persistResume();
            emit playbackStopped();
        }
        return true;
    }
    trackIndex_ = plan.cursor;
    appendedThrough_ = plan.frontier;
    emit queueChanged(titles_, trackIndex_, /*replaced*/ false);   // an EDIT: present nothing
    // The reseat is the host's, not ours: this class never talks to mpv. It drops the entries mpv holds past
    // the one playing and calls feedNextTrack(), which lands back in maybeAppendNext — whose one-ahead
    // invariant is exactly the state the plan just restored by shrinking the frontier to the cursor.
    if (plan.reseat) emit queueFeedInvalidated();
    return true;
}

void PlaybackSession::next()
{
    if (trackIndex_ >= 0 && trackIndex_ + 1 < tracks_.size()) playIndex(trackIndex_ + 1);
}

void PlaybackSession::prev()
{
    if (trackIndex_ > 0) playIndex(trackIndex_ - 1);
}

void PlaybackSession::handleTrackEnd()
{
    // Flush the final ≤5s accrual BEFORE clearing the resume mark: the last heartbeat window (up to the throttle
    // interval of un-accrued playback) would otherwise be dropped when finishResume() forgets the file. Mirrors
    // clearQueue's persist-then-clear ordering so a completed track's tail seconds land in ConsumptionStats.
    persistResume();
    finishResume(); // the file played to the end -> drop its resume mark (next open starts fresh)
    // Auto-advance the audio queue when a track finishes (ignored for video / single files).
    //
    // #220: and record that the next entry was REACHED, on this branch only — the queue-finished branch
    // below must leave a played-out book carrying nothing. It follows playIndex rather than preceding it
    // because playIndex is what makes the next entry current; the host's playRequested handler runs inside
    // that call and may not be able to play the entry at all (a part whose link cannot be minted), which is
    // exactly the case the mark exists for, and the mark lands either way.
    if (trackIndex_ >= 0 && trackIndex_ + 1 < tracks_.size()) { playIndex(trackIndex_ + 1); noteEntryReached(); return; }
    emit queueFinished();
}

// THE ONE PLACE A TRACK'S FILING NAME IS DECIDED (issue #204).
//
// `pathOrKey` is what the queue holds, and for a music-server track that is a SIGNED STREAM URL — a link
// minted from the user's password, not a name. Every route resolves it through the same table here rather
// than at its own call site, which is the whole point: the playlist route already handed a durable id in
// (setQueue's `resumeKey`, #203) and the album route handed the url, so the same track was filed under two
// keys and one of them changed whenever the password did. identityFor() is the identity for anything the
// host has NOT mapped, so a local file, a video, an audiobook and an IPTV channel are byte-for-byte
// unchanged: the map is empty for all of them.
void PlaybackSession::beginResume(const QString& pathOrKey)
{
    const QString path = identityFor(pathOrKey);
    resumePath_ = path;
    // #197: A SERVER'S OWN ITEM IS ASKED OF THE SERVER FIRST, and its answer wins over anything on this
    // disk — including a mark this build wrote before the hook existed. `< 0` is the hook saying "not
    // mine"; 0 is a real answer and must not be confused with one (which is why the test is on the hook's
    // return and not on the position being positive). See PlaybackSession.h.
    const double remote = remoteResume_ ? remoteResume_(path) : -1.0;
    double pos = remote >= 0.0 ? remote
                               : store().value(mediaResumeKey(path) + QStringLiteral("pos"), 0.0).toDouble();
    if (remote < 0.0 && pos <= 0.0)
        pos = store().value(legacyAudiobookKey(path) + QStringLiteral("pos"), 0.0).toDouble();
    resumeSeek_ = pos;       // applied once the duration is known (see onDuration)
    audioPos_ = 0.0;
    lastSavedPos_ = -100.0;
    // Consumption stats: start accrual from the resume point so the resume jump itself never dumps time, and
    // clear any carried remainder from the previous track (per-track reset).
    lastAccruedPos_ = pos;
    statsAccum_ = 0.0;
}

// THE STORED TITLE OF WHATEVER IS PLAYING — and the one thing it must never be derived from is a REMOTE
// URL's filename.
//
// This was `QFileInfo(resumePath_).completeBaseName()` for everything. For a file that is exactly right. For
// a url it is a request: QFileInfo splits on the last '/' and then on the last '.', so the "base name" of a
// stream url is a slice of its QUERY STRING — and issue #193's Subsonic streams carry the user's salted
// token and salt in that query. Whether the slice happens to include them depends on where the last dot in
// the url falls, i.e. on the server's own id format, which is exactly the kind of accident that puts a
// credential in an ini file and in the consumption-stats store and leaves it there.
//
// So: a remote track is titled by the queue's own DISPLAY title, which is both safe and better (it is the
// track name the user is looking at, rather than "stream"). Local files are untouched.
//
// GENERALISED FOR #200, in two ways, because #193 fixed the Subsonic instance of a rule that is not about
// Subsonic. The "is this remote" test is now StoredUrl::isNetworkUrl — the http/https pair missed rtsp, rtmp
// and the rest of the schemes an IPTV or live source arrives on, each of which carries credentials in
// exactly the same place. And the queue's display title is passed through StoredUrl::label on its way to
// disk: it is normally a track name and untouched, but the play routes that fall back to `title = url` when
// a link has no name would otherwise hand this function a signed url and call it a title.
//
// GENERALISED AGAIN FOR #203, for the same reason a third time: a resume identity may now be a QUALIFIED
// MUSIC ID rather than a url or a file, because that is what a playlist entry names its track by. It is not
// a credential — that is the whole point of it — but it is not a NAME either, and QFileInfo hands it back
// whole (it has no '/' and no '.'), so the ini filled up with `sub<US><uuid><US>track<US>tr-2` where a title
// belongs. The test is for the UNIT SEPARATOR that every composite key in this app is joined with
// (Subsonic::idSep(), and MusicLibrary's own keys before it) rather than for Subsonic specifically: no file
// path and no url contains one, and a machine key is a machine key whoever minted it. Spelled here as a
// character rather than reached for, so this file stays QtCore-only and probe_playback keeps its link set.
QString PlaybackSession::resumeDisplayTitle() const
{
    const bool notAName = StoredUrl::isNetworkUrl(resumePath_) || resumePath_.contains(QChar(0x1F));
    if (notAName && trackIndex_ >= 0 && trackIndex_ < titles_.size() && !titles_.at(trackIndex_).isEmpty())
        return StoredUrl::label(titles_.at(trackIndex_));
    if (notAName) return QString();   // no display title either: store nothing rather than a machine string
    return StoredUrl::label(QFileInfo(resumePath_).completeBaseName());
}

void PlaybackSession::persistResume()
{
    if (resumePath_.isEmpty() || audioPos_ <= 1.0) return; // nothing meaningful to remember yet
    // #197: OFFER IT TO THE OWNER FIRST. A `true` here means a server took this position, and this object
    // then writes NOTHING into the resume store for the entry — one owner per position, and for a server's
    // own item the owner is the server. The consumption-stats accrual below still runs: that is this
    // DEVICE's accumulator, which no server has an equivalent of. PlaybackSession.h argues both halves.
    if (remoteProgress_ && remoteProgress_(resumePath_, audioPos_, duration_, leavingMedia_))
    {
        lastSavedPos_ = audioPos_;   // the throttle is still this object's, so the hook is not called per tick
        const double rdpos = std::min(std::max(audioPos_ - lastAccruedPos_, 0.0), 30.0);
        lastAccruedPos_ = audioPos_;
        statsAccum_ += rdpos;
        const qint64 rwhole = qint64(statsAccum_);
        if (rwhole > 0)
        {
            statsAccum_ -= double(rwhole);
            ConsumptionStats::addMediaSeconds(resumePath_,
                mediaIsVideo_ ? QStringLiteral("video") : QStringLiteral("audio"),
                rwhole, resumeDisplayTitle());
        }
        // No resumeSaved(): that signal schedules the cloud "continue watching" push, and there is nothing
        // to push — this position was never written into a synced category, which is the whole point.
        return;
    }
    const QString k = mediaResumeKey(resumePath_);
    store().setValue(k + QStringLiteral("pos"), audioPos_);
    store().setValue(k + QStringLiteral("dur"), duration_); // lets the home screen show a progress bar
    store().setValue(k + QStringLiteral("title"), resumeDisplayTitle());
    store().setValue(k + QStringLiteral("ts"), QDateTime::currentSecsSinceEpoch()); // for cross-device merge-by-recency
    store().sync();
    // A position undoes an earlier clear of the same item (issue #150): re-watching something you finished must
    // not be suppressed by the tombstone that finishing it left. Newest-wins would carry all but the same-second
    // case on its own — the merge's `tomb >= item` rule, which favourites share — so this closes that, and says
    // out loud that "cleared" is not permanent. Cheap: a lookup that finds nothing on every ordinary save.
    ResumeStore::noteResumed(resumePath_);
    lastSavedPos_ = audioPos_;

    // Consumption stats: accrue the forward-only playback delta since the last heartbeat, clamped to [0, 30]s so
    // a seek-forward can't dump minutes and a seek-backward accrues nothing. The exact float position drives the
    // diff (seeks handled correctly, no runaway); a sub-second remainder carries in statsAccum_ so whole-second
    // rounding never drifts. Keyed by the resume identity (its own per-profile store — NOT the global resume
    // keys), category from the file's kind, title as the current resume title.
    const double dpos = std::min(std::max(audioPos_ - lastAccruedPos_, 0.0), 30.0);
    lastAccruedPos_ = audioPos_;
    statsAccum_ += dpos;
    const qint64 whole = qint64(statsAccum_);
    if (whole > 0)
    {
        statsAccum_ -= double(whole);
        ConsumptionStats::addMediaSeconds(resumePath_,
            mediaIsVideo_ ? QStringLiteral("video") : QStringLiteral("audio"),
            whole, resumeDisplayTitle());
    }

    emit resumeSaved(); // host schedules the cloud "continue watching" push (debounced)
}

// A BOUNDARY WAS CROSSED INTO THIS ENTRY, WHICH IS A FACT (issue #220).
//
// THE BUG THIS CLOSES. A boundary does persistResume() then finishResume() on the entry that just played
// out — correctly: it reached its end, so its position is meaningless and its mark goes. The entry the
// queue moves TO gets beginResume(), which sets its position to zero IN MEMORY, and persistResume refuses
// to write a position under a second because there is nothing meaningful to remember yet — also correct,
// for a POSITION. So between the boundary and the incoming entry's first second of playback, the store
// holds nothing at all for this queue. Ordinarily that window is a few hundred milliseconds and nobody
// can see it. For a fifty-seven part audiobook whose next part cannot be fetched (#217: the source
// answers with no link) it is permanent, and MainWindow's book scan — "the last part carrying a mark" —
// then answers PART ONE, throwing away the forty-five minutes the listener had just spent.
//
// So the boundary writes down the one fact nothing else does: the listener REACHED this entry. It is an
// ordinary ResumeStore row, in the same group persistResume writes, which is deliberate and is the whole
// design: no new mark kind, no schema change, nothing for the cross-device merge or the Home screen to
// learn. A position of zero is already a legal state for an entry (beginResume has always set exactly
// that); all this does is make it DURABLE one second earlier, at a boundary.
//
// WHAT IT DOES NOT DO, and each of these is load-bearing:
//
//   * it never CLOBBERS a position already banked for this entry. Play part four for twenty minutes, jump
//     back to part two, let part two play out — the boundary lands on part four again, and overwriting
//     there would throw away the twenty minutes at the exact moment the listener could least explain it.
//     (An entry whose only position is a pre-#139 legacy audiobook bookmark still resumes correctly if a
//     zero is written here: beginResume's fallback fires on `pos <= 0`, so the legacy value is read
//     exactly as before, and the part is now visible to the scan as well.)
//
//   * it writes NO DURATION. duration_ still holds the OUTGOING entry's length — nothing has opened the
//     incoming one, so its length is genuinely unknown — and the Home progress bar wants a position and a
//     duration both past a second before it draws anything. Inheriting a length would paint a bar over
//     something nobody has heard a second of.
//
//   * it is not called when the queue ENDS, and that is what keeps a finished book finished. handleTrackEnd
//     and the gapless boundary only reach here on the branch that has a successor; the last entry's own
//     mark is dropped by finishResume and no mark is invented past the end, so a book played to its end
//     still carries nothing on any part and still opens at part one.
//
//   * it is not called by playIndex, so opening a queue — or skipping through one by hand — writes nothing.
//     A manual skip loses nothing anyway: playIndex persists the outgoing entry's real position first, so
//     the book still resumes where it was genuinely left. Writing here on every open would instead make a
//     FINISHED book carry a mark again the moment it was looked at.
//
// The tombstone is lifted for the same reason persistResume lifts it (#150): reaching an entry again is a
// later event than the clear that finishing it recorded, and without this the merge's `tomb >= item` rule
// would suppress the row on a re-listen that crossed the boundary in the same second.
void PlaybackSession::noteEntryReached()
{
    if (resumePath_.isEmpty()) return;
    // #197: ...and nothing at all for an entry whose position a SERVER owns. This is the one write in this
    // class that is not a position, which is exactly why it needed saying separately: a zero banked here is
    // still a row in `resume/`, and `resume/` syncs. See PlaybackSession.h.
    if (remoteOwns_ && remoteOwns_(resumePath_)) return;
    const QString k = mediaResumeKey(resumePath_);
    if (store().contains(k + QStringLiteral("pos"))) return;   // already carries a position: leave it alone
    store().setValue(k + QStringLiteral("pos"), 0.0);
    store().setValue(k + QStringLiteral("dur"), 0.0);
    store().setValue(k + QStringLiteral("title"), resumeDisplayTitle());
    store().setValue(k + QStringLiteral("ts"), QDateTime::currentSecsSinceEpoch());
    store().sync();
    ResumeStore::noteResumed(resumePath_);
    // lastSavedPos_ is deliberately left at beginResume's sentinel: this is not a saved POSITION, and moving
    // it would make setPosition's 5-second throttle swallow the first real write of the entry.
}

void PlaybackSession::finishResume()
{
    if (resumePath_.isEmpty()) return;
    // Through ResumeStore, which removes the group AND records a dated tombstone (issue #150). A bare remove()
    // made "played to the end" and "this device has never opened that file" the same fact on disk, and the
    // merge — which cannot delete, because an absence carries no timestamp — read the second one: the cloud
    // document still holds THIS device's own pre-finish row, so the next sync put the position back with no
    // second device involved. The tombstone is a clear with a time on it, which the merge can compare.
    ResumeStore::clear(store(), resumePath_);
    store().remove(legacyAudiobookKey(resumePath_)); // also clear any legacy audiobook bookmark
    store().sync();
    resumePath_.clear();
    resumeSeek_ = 0.0; // a finished file has no pending seek; don't let a stale target leak forward
    lastSavedPos_ = -100.0;
}

void PlaybackSession::clearQueue()
{
    // #197: this is the LAST position this media will report, and a hook whose own gate is a network round
    // trip needs to be told so. Scoped to exactly this call so nothing else can see the flag set.
    leavingMedia_ = true;
    persistResume();      // save where we left off before leaving this media (also flushes final accrual)
    leavingMedia_ = false;
    resumePath_.clear();
    resumeSeek_ = 0.0;
    lastSavedPos_ = -100.0;
    lastAccruedPos_ = 0.0; // consumption-stats: per-track reset (next media starts a fresh accrual span)
    statsAccum_ = 0.0;
    tracks_.clear();
    titles_.clear();      // #193: the display list is part of the queue and dies with it
    // The queue's headers die with the queue. NOT observable — every read of trackHeaders_ is a playIndex
    // reached through a setQueue, which assigns the list wholesale — so probe_playback pins no assertion on
    // this line and says why. It stays because tracks_ and trackHeaders_ are a parallel pair: clearing one
    // and not the other leaves two members disagreeing about how many tracks there are, which is the state a
    // future change to the read path would be bitten by.
    trackHeaders_.clear();
    trackIndex_ = -1;
    emit queueCleared();
}

void PlaybackSession::setPosition(double s)
{
    audioPos_ = s;
    // Throttle resume writes so we're not hammering the ini every position tick.
    if (!resumePath_.isEmpty() && std::abs(s - lastSavedPos_) >= 5.0)
        persistResume();
}

void PlaybackSession::setDuration(double s)
{
    duration_ = s;
}

void PlaybackSession::overrideResumeSeek(double seconds)
{
    if (resumePath_.isEmpty()) return;      // nothing is being tracked: there is no entry to place
    resumeSeek_ = qMax(0.0, seconds);
    lastAccruedPos_ = resumeSeek_;          // see the header: the stats cursor follows the start point
    statsAccum_ = 0.0;
}

double PlaybackSession::takeResumeSeek()
{
    // Keep the old onDuration guard: with no tracked file there is no valid seek target — a stale
    // value must never drive a seek on a late/duplicate durationChanged after finishResume().
    if (resumePath_.isEmpty()) return 0.0;
    const double s = resumeSeek_;
    resumeSeek_ = 0.0;
    return s;
}
