#pragma once
#include "../core/StreamHeaders.h"
#include "QueueEdit.h"   // issue #193: the pure index arithmetic behind insert/remove/move
#include <QHash>
#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QVector>
#include <functional>

// The audio-queue + resume state machine: owns the current track list/position, drives next/prev/
// track-end advance, and persists "where you left off" for any timed media (video/audio/audiobook),
// keyed by a stable path/id. The host (MainWindow) owns the actual player and playlist widget; this
// class only decides WHAT plays next and WHERE to resume, telling the host via signals.
class PlaybackSession : public QObject
{
    Q_OBJECT
public:
    // Empty settingsFile = the app's own store (AppPaths::dataDir()/everythingbox.ini); probes pass a
    // scratch path so tests never touch the real user store.
    explicit PlaybackSession(const QString& settingsFile = QString(), QObject* parent = nullptr);

    // A track's HTTP request headers, indexed exactly like `files`. Empty (the default) for every local
    // queue; see setQueue.
    using TrackHeaders = QVector<StreamHeaders::Headers>;

    // resumeKey empty (default) = the starting track resumes keyed by its own file path (current behavior).
    // Non-empty = the first-played track is resume-keyed by `resumeKey` instead (a stable catalog/audiobook
    // id), folded in atomically so callers no longer re-key with a separate beginResume() after setQueue.
    //
    // `trackHeaders` is the PER-TRACK header channel (#59). Before it, this class carried urls only, so every
    // queue-driven load — a gated audio stream, an IPTV channel list, and every advance within one — reached
    // the player bare and 403'd. Per TRACK and not per queue because an IPTV playlist's entries are separate
    // URLs on whatever hosts the list names, and the playlist's own headers belong to the playlist's origin
    // alone: the caller scopes each entry through StreamHeaders::forPlayUrl and hands the answers here.
    //
    // Shorter than `files` (or empty) is normal and means "no headers for the tracks past the end" rather
    // than being an error — playHeaders() reads it with value(), so a local queue passes nothing at all.
    void setQueue(const QStringList& files, int startIndex, const QStringList& titles = {},
                  const QString& resumeKey = QString(), const TrackHeaders& trackHeaders = {});
    void playIndex(int index);
    void next();
    void prev();
    void handleTrackEnd(); // advances the queue, or emits queueFinished() at the last track
    void clearQueue();     // persists then resets (was clearAudioQueue minus the widget lines)

    // ---- Gapless playback (issue #141) -----------------------------------------------------------------
    // OPT-IN, DEFAULT OFF. Gapless changes only HOW the next track reaches mpv: instead of a stop-start
    // loadfile-replace per track (the EOF-driven advance in handleTrackEnd), the host keeps mpv's OWN playlist
    // fed one-ahead via `loadfile <next> append`, so mpv's decoder never stops across a track boundary. Because
    // the decoder does not stop, EOF no longer fires the per-track advance — mpv crosses to the next appended
    // entry itself and reports it through its `playlist-pos` property. onPlaylistPos() is the boundary detector
    // that fires the SAME per-item bookkeeping handleTrackEnd does (finish the outgoing track's resume/stats,
    // advance the app's notion of current, emit trackChanged, keep the one-ahead append fed) — exactly once per
    // track. Applies to the AUDIO queue only; the host sets it false for video (IPTV) and single files.
    //
    // With gapless OFF this class is byte-for-byte the pre-#141 machine: setGapless(false) means playIndex takes
    // no gapless branch, onPlaylistPos() early-returns, appendRequested() is never emitted, and the host drives
    // every advance through the unchanged EOF -> handleTrackEnd path.
    void setGapless(bool on); // arm/disarm gapless BEFORE setQueue; the host decides (audio queue + the setting)
    bool gapless() const { return gapless_; }
    // Arm gapless on a queue that is ALREADY PLAYING (issue #193 increment 2). setGapless() says "before
    // setQueue" because playIndex's gapless branch is what SEEDS the one-ahead frontier, and a queue the host
    // armed nothing for — a single track, which is every queue with no boundary to bridge — has no such seed:
    // appendedThrough_ is still -1, so maybeAppendNext's one-ahead test refuses forever and merely flipping
    // gapless_ would arm a feed that never feeds. Adding a track to a one-track queue is the first thing that
    // can give such a queue a boundary, and without this the very first thing the reach verbs do to one is
    // silently re-introduce the gap #141 removed.
    //
    // The seed is exactly playIndex's: the playing entry is the highest index mpv has been handed, and mpv's
    // playlist-pos is 0 because a queue that never armed gapless never appended anything, so mpv is holding
    // the single entry its last replace-load gave it. Refuses when gapless is already on (its frontier is
    // live and re-seeding it would drop a real append) or when nothing is playing (there is no seed).
    void armGaplessLive();
    // Fed from mpv's `playlist-pos` (via the host) while gapless is armed. `mpvPos` is mpv's current playlist
    // index; the finished-track bookkeeping fires for each track crossed since the previous notification.
    void onPlaylistPos(int mpvPos);

    // ---- Crossfade (issue #141) -------------------------------------------------------------------------
    // DEFERRED ONE-AHEAD FEED. Gapless and crossfade both own track boundaries, and they must not own the same
    // one: gapless hands mpv the next entry so mpv can cross into it ITSELF, which leaves no window for an
    // overlap, and a boundary already appended is a boundary the crossfade can no longer have. But WHICH
    // boundary a crossfade may take is not knowable at track start - the music-vs-audiobook split needs mpv's
    // parsed chapters, and the album tags of both sides need the file. So with crossfade armed the host calls
    // setDeferAppend(true) and the automatic feed in playIndex/onPlaylistPos stands down; once the host has
    // decided the boundary it either calls feedNextTrack() (no crossfade - gapless takes it, exactly as
    // before) or does not (the crossfade takes it). The append still lands seconds before the boundary, which
    // is all mpv needs.
    //
    // With crossfade OFF this is never armed and the feed is the immediate one it has always been.
    void setDeferAppend(bool on);
    // Feed mpv the ONE entry past the current track, if it does not already have it. Idempotent by the
    // one-ahead invariant (see maybeAppendNext), so a second call at the same boundary appends nothing.
    void feedNextTrack();
    // The crossfade handover happened: the player is already several seconds into the NEXT entry on its other
    // deck. Advance the app's notion of current by exactly one WITHOUT a reload - the same per-item work the
    // gapless playlist-pos boundary does (flush the finished track's accrual, drop its resume mark, re-key and
    // re-announce) - and re-seat the gapless bookkeeping onto the fresh deck, whose playlist holds exactly the
    // one entry now playing.
    void onCrossfadePromoted();
    // The crossfade handover crossed a boundary this queue does not contain — channel mode's, where the file
    // now playing is the channel's next pick and belongs to a queue of its own (#141). Install `files` around
    // the entry at `index`, which the player is ALREADY several seconds into: the same per-item work
    // onCrossfadePromoted does (flush the finished track's accrual, drop its resume mark, re-key and
    // re-announce, re-seat the gapless bookkeeping onto the fresh deck) minus the one thing that would undo
    // the handover, which is a load. Deliberately NOT setQueue with the load removed: setQueue's job is to
    // START something, and every caller of it is entitled to playRequested firing.
    void adoptPlayingQueue(const QStringList& files, int index, const QStringList& titles = {});

    // PURE boundary logic, pinned by probe_playback: given mpv's previous and current playlist-pos under gapless
    // auto-advance, how many queue tracks finished at this boundary. mpv plays a playlist strictly forward, one
    // entry at a time, so curPos > prevPos means the (curPos - prevPos) entries in [prevPos, curPos) each played
    // to their end. curPos == prevPos is a duplicate/no-op notification (0). curPos < prevPos is a backward /
    // manual jump, which the gapless path resolves by a hard reload (see playIndex) rather than as a completion,
    // so it reports 0 here and never double-counts. Static + side-effect-free so a fixture can pin it directly.
    static int tracksCompleted(int prevPos, int curPos)
    {
        return curPos > prevPos ? curPos - prevPos : 0;
    }

    // ---- Editing the queue you are listening to (issue #193) --------------------------------------------
    // The three verbs nothing could do before: nothing here replaces the queue, so what is playing keeps
    // playing across every one of them. The INDEX ARITHMETIC is not written here — it is QueueEdit's, pure and
    // probe-driven, for the reason its header gives at length: the hard part is not the list surgery but
    // deciding whether the edit landed on an entry mpv has already been handed under gapless, and that
    // decision has to be testable without mpv.
    //
    // Each returns false and changes NOTHING for an out-of-range or no-op edit (an insert of nothing, a move
    // onto itself), so a caller may ask without pre-checking. On success each emits queueChanged with the new
    // list, and — when the edit invalidated what mpv is holding — queueFeedInvalidated(), which is the host's
    // cue to drop mpv's committed-but-unplayed entries and let the one-ahead feed run again.
    bool insertTracks(int at, const QStringList& files, const QStringList& titles = {});
    bool removeTrack(int at);
    bool moveTrack(int from, int to);
    // The two verbs a listener actually names. playNext puts `files` immediately after the track playing (the
    // edit that ALWAYS crosses the gapless frontier, which is why the reseat exists at all); enqueue appends.
    // Both are plain wrappers over insertTracks so there is one implementation of the surgery.
    bool playNext(const QStringList& files, const QStringList& titles = {});
    bool enqueue(const QStringList& files, const QStringList& titles = {});

    // The whole queue, for "save this as a playlist" and for a host rebuilding its own row model.
    QStringList tracks() const { return tracks_; }
    // The DISPLAY titles queueChanged last carried, index-parallel to tracks(). Held (rather than recomputed)
    // because an edit has to renumber them alongside the paths: a caller's title list is only supplied at
    // install time, and re-deriving base names for a queue that was installed WITH titles would quietly
    // replace "Track 3 — Sibelius" with "03 sibelius" on the first edit.
    QStringList titles() const { return titles_; }

    // ---- THE QUEUE'S DURABLE IDENTITIES (issue #204) ---------------------------------------------------
    //
    // WHAT A TRACK IS FILED UNDER, when that is not the string the player is handed. A music queue holds what
    // mpv can open — for a Subsonic track that is a SIGNED STREAM URL, and a stream url is signed from the
    // user's password, so it is a one-shot artefact and not a name. Keying the resume position and the
    // consumption seconds by it meant changing the password silently orphaned every one of them, and it meant
    // the same track banked under two different keys depending on whether a playlist or the album view opened
    // it (the playlist route re-keyed to the qualified id in #203; the album route did not).
    //
    // A HASH KEYED BY PLAY PATH, not a list parallel to `files`, and that is the load-bearing half: a queue is
    // EDITED — enqueue, play-next, remove, shuffle — and a parallel list is one renumbering bug away from
    // filing track 4's position under track 7's name. A lookup by the string itself cannot be renumbered
    // wrong, and two rows that play the same file resolve to the same identity, which is the right answer.
    //
    // Anything absent from the map is its own identity, so a local queue is byte-for-byte what it always was
    // and no caller that has nothing to say has to say it. The map is REPLACED with each music queue and
    // otherwise left alone: its keys are signed stream urls, which no local path, no IPTV channel url and no
    // addon stream is or can be, so a stale entry is unreachable rather than merely harmless.
    void setTrackIdentities(const QHash<QString, QString>& playPathToIdentity)
    { trackIdentities_ = playPathToIdentity; }
    // The durable name of a play path — itself, unless the caller said otherwise. Public so the host can key
    // its own per-track state (syncKey_) through the identical table rather than a second copy of it.
    QString identityFor(const QString& playPath) const
    { return trackIdentities_.value(playPath, playPath); }

    void beginResume(const QString& pathOrKey); // start tracking this file/key (and queue its saved spot)

    // ---- WHEN THE POSITION BELONGS TO A SERVER (issue #83) ---------------------------------------------
    // A Jellyfin item's watch position is the SERVER's, not this device's: #83 says so in as many words,
    // and the reason is what a media server is for - the position has to be the same one the phone and the
    // web client see, and two authorities for one number means the last device to close wins.
    //
    // So for such an item persistResume writes NO resume/<hash> row and records NO tombstone, and emits
    // serverProgress() instead. Consumption stats still accrue: the seconds this device spent watching are
    // a fact about this device and are not the server's business.
    //
    // It is a FLAG rather than a test on the key's spelling because this file must not learn what a
    // Jellyfin id looks like: PlaybackSession is the app's playback state machine, and a `jf:` prefix test
    // in it would be the second place that knowledge lived. The open site knows, and says so.
    //
    // Reset by beginResume, so a server item cannot leave the next local file's position unwritten.
    void setResumeOwnedByServer(bool on) { resumeServerOwned_ = on; }
    bool resumeOwnedByServer() const { return resumeServerOwned_; }

    // Where the SERVER says to start. Applied exactly like a stored position (consumed once the duration is
    // known), and it also moves the stats accrual point, so the resume jump itself never dumps minutes into
    // this device's watch time. Called after beginResume, which is what fills in the local answer this
    // replaces.
    void seedResume(double seconds);
    // The identity beginResume was given — what the position, and the LENGTH, are filed under. Read by the
    // host's duration callback so a measured runtime can be recorded against the same key the rest of the
    // app knows the item by (issue #179: a channel's lineup needs lengths that outlive the resume group,
    // which finishResume deletes). Empty when nothing timed is playing.
    QString resumePath() const { return resumePath_; }
    void persistResume();                       // save the current position (throttled / on leave / on exit)
    void finishResume();                        // played to the end -> drop the saved position
    // A queue boundary was just crossed INTO the current entry: record that it was reached (issue #220).
    // Durable at position zero, and only where the entry carries nothing already. See the definition.
    void noteEntryReached();

    // ---- A SERVER THAT OWNS ITS OWN PROGRESS (issue #197) ----------------------------------------------
    //
    // Some entries are not ours to remember. An Audiobookshelf item's position belongs to the server: every
    // client that touches it reports to the same place and reads back from the same place, which is the
    // whole reason somebody runs one. #83 sets the identical rule for Jellyfin and #153 for PSE. So for
    // those entries this object must do TWO things differently, and both of them are here rather than in
    // the host because both are inside persistResume/beginResume — the throttled hook and the arm — and a
    // host that wanted to intervene from outside would have to duplicate the throttle.
    //
    // THE REPORTING HOOK. Called from persistResume with the entry's resume identity, its position and its
    // duration. Returning TRUE means "this one is mine, I have taken it" — and this object then writes
    // NOTHING into the resume store for it. That is the point: a position duplicated into our synced
    // resume categories would be merged across devices by OUR rules (newest-timestamp-wins) alongside the
    // server's own, and the two would disagree the first time a phone and a TV listened out of order. One
    // owner, and for these ids the owner is the server.
    //
    // Consumption stats still accrue, deliberately: those are this DEVICE's "how much have I listened"
    // accumulator, keyed by an identity that carries no credential, and the server has no equivalent of
    // them to be the owner of.
    // `leaving` is true for the LAST report this media will make — the persistResume clearQueue does on
    // the way out. It exists because the network gate on the far side is necessarily slower than this
    // object's (a round trip to somebody's Raspberry Pi is not an ini write), and a listener who stops
    // nine seconds into that interval would otherwise leave the server holding a position from before the
    // last thing they heard. Every other report is ordinary and rides the far side's throttle.
    using RemoteProgressFn = std::function<bool(const QString& identity, double pos, double dur,
                                                bool leaving)>;
    void setRemoteProgress(RemoteProgressFn fn) { remoteProgress_ = std::move(fn); }

    // THE RESUME HOOK. Called from beginResume BEFORE the local mark is read. A return of < 0 means "not
    // mine" and the local mark is used exactly as it always was; anything >= 0 is the server's answer and
    // WINS — including 0, which is a real answer ("the server has this entry at its top") and not an
    // absence. Deferring to the server over a local mark is the rule; a local mark for one of these ids
    // could only be a leftover from before this existed.
    using RemoteResumeFn = std::function<double(const QString& identity)>;
    void setRemoteResume(RemoteResumeFn fn) { remoteResume_ = std::move(fn); }

    // "IS THIS ONE THEIRS?" — the same question with NO side effect, which is why it is a third hook rather
    // than a reading of the two above. setRemoteProgress REPORTS when it is asked, and the host's
    // setRemoteResume CONSUMES the seed it was holding, so neither can be used to merely ask.
    //
    // It exists for noteEntryReached (#220), which is the one write that is not a position: crossing a part
    // boundary banks a ZERO for the entry it moved into, so a book whose next part cannot be fetched still
    // knows where the listener got to. That is a row in `resume/`, which SYNCS — so for an entry a server
    // owns it is exactly the duplication #197 rules out, in the one place that does not look like a
    // position write. The server already knows the listener reached this part: it was told, in book time,
    // by the report the outgoing part made on its way out.
    using RemoteOwnsFn = std::function<bool(const QString& identity)>;
    void setRemoteOwns(RemoteOwnsFn fn) { remoteOwns_ = std::move(fn); }

    // Consumption stats: the host reports the loaded file's media kind (mpv's fileLoaded isVideo flag) so the
    // persistResume heartbeat accrues watch (video) vs listen (audio) seconds into the right category.
    void setMediaVideo(bool isVideo) { mediaIsVideo_ = isVideo; }
    // The same app-stamped kind, readable. Every open site stamps it SYNCHRONOUSLY (mpv loads async), so it is
    // already settled when the host's duration callback runs — unlike mpv's own fileLoaded isVideo flag, which
    // is an async event with no ordering guarantee against `duration` and mis-reports cover-art audio as video.
    bool mediaIsVideo() const { return mediaIsVideo_; }

    void setPosition(double s); // fed from the host's mpv position callback
    void setDuration(double s); // fed from the host's mpv duration callback

    double takeResumeSeek(); // returns the pending resume target once, then 0 (consumed by onDuration)

    // START THIS ENTRY SOMEWHERE THE STORE DID NOT CHOOSE (issue #139 increment 2): the audiobook chapter
    // list, which is the one caller that knows a position the resume marks do not hold — a chapter's start
    // inside its file. Called right after setQueue and before mpv has loaded anything, so it replaces the
    // target beginResume just queued rather than racing it; onDuration consumes it exactly as it would the
    // stored one, including its "essentially the end, so start fresh" guard.
    //
    // ZERO IS A REAL ANSWER HERE and it is why this is a setter rather than an argument on setQueue: jumping
    // to PART THREE of a multi-file book means "the top of part three", and the position that part may still
    // carry from a previous listen is exactly what the listener is asking not to be sent to.
    //
    // The stats cursor moves with it. beginResume seeded lastAccruedPos_ from the STORED position, and
    // leaving that behind while the file starts somewhere else would accrue the difference as listening that
    // never happened (or, jumping backwards, silently discard real seconds).
    void overrideResumeSeek(double seconds);

    int currentIndex() const { return trackIndex_; }
    int count() const { return tracks_.size(); }
    QString trackAt(int i) const { return tracks_.value(i); }
    double position() const { return audioPos_; }

signals:
    // The host hands both to mpv. The headers ride the SIGNAL rather than being read back off this object
    // afterwards, for the reason #43 gave for the resolve callback: a getter returning "the current track's
    // headers" can be called at a moment when "current" has moved on, and the value that outlives its track
    // is the leak. Emitted on every track — empty for the ones that need none, which is what clears the
    // previous track's headers at the player (MpvHeaderApply writes all three properties unconditionally).
    void playRequested(const QString& path, const StreamHeaders::Headers& trackHeaders);
    // Gapless one-ahead feed (issue #141): emitted ONLY while gapless is armed, to have the host append the
    // next queue entry to mpv's own playlist (`loadfile <path> append`) rather than stop-start it. Carries the
    // same per-track header channel as playRequested for symmetry (empty for a local audio queue, which is the
    // only queue gapless applies to). Never emitted with gapless off — the off path stays exactly as it was.
    void appendRequested(const QString& path, const StreamHeaders::Headers& trackHeaders);
    void trackChanged(int index, int count, const QString& displayTitle);
    // `replaced` distinguishes the two things this signal has always meant, which only became two things in
    // #193 increment 3. TRUE = a whole NEW queue was installed (setQueue, or a channel handover) — a
    // deliberate play, so the host brings a now-playing surface forward. FALSE = an EDIT of the queue already
    // playing (insert / remove / move). The host must present NOTHING for an edit: the music can now be
    // playing with its page CLOSED, and an "add to queue" from a browse row would otherwise yank the listener
    // off the row they are standing on and onto the player. Defaulted so a caller that only cares about the
    // list (probe_playback, the classic row model) connects with two arguments exactly as before.
    void queueChanged(const QStringList& titles, int current, bool replaced = true);   // host rebuilds playlist_
    void queueCleared();
    void queueFinished();                                             // host runs scrobble-stop / next-episode
    void resumeSaved();                                                // host schedules the cloud progress push
    // #83: the throttled position hook, for an item whose position belongs to a server. Fired from
    // persistResume - i.e. at the SAME cadence a resume write would have happened - carrying the durable
    // identity and the position. The host applies the server's own report interval on top (see
    // Jellyfin::shouldReportProgress); this signal is deliberately not the place that decision lives,
    // because it is a fact about the Jellyfin API and not about playback.
    void serverProgress(const QString& key, double seconds);
    // #193: a queue edit landed on an entry mpv had ALREADY been handed under gapless, so mpv's own playlist
    // no longer agrees with this one. The host drops every mpv playlist entry AFTER the one playing (none of
    // which has produced a sample yet, so nothing is audible) and lets the one-ahead feed run again. Emitted
    // only for a crossing edit — an edit above the frontier leaves mpv correct and stays silent.
    void queueFeedInvalidated();
    // #193: the track that was PLAYING was removed and the queue had nothing after it. The host STOPS the
    // player. Deliberately not queueFinished(): that means "played to the end" and hands the moment to the
    // channel / next-episode chain, and nothing here played out — the listener deleted it.
    void playbackStopped();

private:
    QSettings& store();
    QString titleAt(int index) const;
    // The display list queueChanged carries: the caller's title for each entry, falling back to the file's
    // base name. Extracted (#141) because two entry points now install a track list — setQueue and
    // adoptPlayingQueue — and a second copy of this fallback is how one of them would announce a queue of
    // blank rows for a caller that passed no titles.
    QStringList displayTitlesFor(const QStringList& titles) const;
    void maybeAppendNext(); // #141: emit appendRequested for the one entry past the append frontier, if any
    void advanceWithoutReload(); // #141: the per-item work a boundary mpv crossed ITSELF still owes (gapless + crossfade)

    // The one place a queue edit is COMMITTED (#193). The verbs above do their own list surgery — paths,
    // titles and headers permuted together, or a track plays under another's name and, on an IPTV queue, with
    // another's credentials — and then hand the plan here, which is the single copy of "announce the new
    // list, carry the cursor and the append frontier, and tell the host what mpv still owes". Three
    // hand-written copies of that tail is how one verb ends up not asking for the reseat.
    bool commitEdit(const QueueEdit::Plan& plan);
    void padTrackHeaders();   // #193: make the header list full-length before an edit renumbers it (see the .cpp)
    // What the resume row and the consumption-stats row are TITLED. Never a remote url's "filename", which
    // is a slice of its query string and can therefore carry a credential — see the .cpp.
    QString resumeDisplayTitle() const;

    QStringList tracks_;           // current audio queue (absolute paths)
    QStringList titles_;           // #193: display titles, index-parallel to tracks_ (see titles())
    TrackHeaders trackHeaders_;    // per-track request headers, parallel to tracks_ (usually empty)
    int trackIndex_ = -1;          // index into tracks_, or -1 when not playing a queue
    bool gapless_ = false;         // #141: gapless feed armed (audio queue + the setting); false = the old EOF path
    bool deferAppend_ = false;     // #141: crossfade armed -> the host, not playIndex, decides when to feed
    int prevPos_ = 0;              // #141: mpv's last-seen playlist-pos, to diff against the next notification
    int appendedThrough_ = -1;     // #141: highest queue index handed to mpv's playlist so far (one-ahead frontier)
    QHash<QString, QString> trackIdentities_; // #204: play path -> the durable name it is filed under
    QString resumePath_;           // the DURABLE IDENTITY of the timed media whose position we track, or empty
                                   // (#204: for everything but a music server track this is still its path)
    double resumeSeek_ = 0.0;      // pending resume target applied once the file's duration is known
    bool   resumeServerOwned_ = false; // #83: this item's position is a server's, not this ini's
    double audioPos_ = 0.0;        // last reported playback position
    double lastSavedPos_ = -100.0; // throttle resume writes
    double lastAccruedPos_ = 0.0;  // consumption-stats: position through which watch/listen seconds were accrued
    double statsAccum_ = 0.0;      // consumption-stats: sub-second remainder carried between heartbeats (no drift)
    bool   mediaIsVideo_ = true;   // consumption-stats: the loaded file's kind (set by the host from mpv fileLoaded)
    double duration_ = 0.0;        // last reported duration (for the "dur" progress hint)
    QString settingsFile_;
    QSettings* settings_ = nullptr;
    // #197: the two seams a server that owns its own progress needs. Unset by default and unset in every
    // headless probe that does not install them, so a queue of local files is byte-for-byte what it was.
    RemoteProgressFn remoteProgress_;
    RemoteResumeFn   remoteResume_;
    RemoteOwnsFn     remoteOwns_;
    bool             leavingMedia_ = false;   // set only around clearQueue's own persistResume
};
