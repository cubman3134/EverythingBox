// Headless test for PlaybackSession: queue advance (next/prev/track-end), resume position
// round-trip through a scratch settings file, and the one-shot resume seek. Prints PLAYBACK-OK.
#include <QCoreApplication>
#include <QTemporaryDir>
#include "../src/media/PlaybackSession.h"
#include "../src/core/Settings.h"   // #141: gaplessAudio() default (read from the probe's isolated data dir)
#include "../src/core/ResumeStore.h" // #220: where a multi-part book resumes (the scan both openers run)
#include <QSettings>

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    const QString ini = tmp.filePath("store.ini");

    PlaybackSession s(ini);
    QStringList played;
    QObject::connect(&s, &PlaybackSession::playRequested,
                     [&](const QString& p) { played << p; });
    int finished = 0;
    QObject::connect(&s, &PlaybackSession::queueFinished, [&] { ++finished; });

    s.setQueue({ "a.mp3", "b.mp3", "c.mp3" }, 1);
    CHECK(played == QStringList{ "b.mp3" }, "setQueue starts at startIndex");
    s.next();  CHECK(played.last() == "c.mp3", "next advances");
    s.prev();  CHECK(played.last() == "b.mp3", "prev steps back");
    s.handleTrackEnd();
    CHECK(played.last() == "c.mp3" && finished == 0, "track end auto-advances");
    s.handleTrackEnd();
    CHECK(finished == 1, "track end at the last track emits queueFinished");

    // Resume round-trip: position persists per file and is consumed once on re-open.
    s.beginResume("X:/book.m4b");
    s.setDuration(3600.0);
    s.setPosition(1234.0);
    s.persistResume();
    PlaybackSession s2(ini);
    s2.beginResume("X:/book.m4b");
    CHECK(qFuzzyCompare(s2.takeResumeSeek(), 1234.0), "resume position survives a new session");
    CHECK(qFuzzyCompare(s2.takeResumeSeek() + 1.0, 1.0), "resume seek is consumed once");
    // In-session: a pending (untaken) seek must die with finishResume — a late/duplicate
    // durationChanged after the file finished must never drive a stale seek.
    s2.beginResume("X:/book.m4b"); // re-arms the pending seek (1234) from the store
    s2.finishResume();
    CHECK(qFuzzyCompare(s2.takeResumeSeek() + 1.0, 1.0), "finishResume kills the pending seek in-session");
    PlaybackSession s3(ini);
    s3.beginResume("X:/book.m4b");
    CHECK(qFuzzyCompare(s3.takeResumeSeek() + 1.0, 1.0), "finishResume drops the saved position");

    // setQueue's resumeKey re-keys the starting track by a stable id instead of its file path, so a saved
    // position survives even when the queue's URL changes on re-resolve (folds in the old post-setQueue re-key).
    PlaybackSession s4(ini);
    s4.setQueue({ "a.mp3" }, 0, {}, "stable-id");
    s4.setDuration(1800.0);
    s4.setPosition(567.0);
    s4.persistResume();
    PlaybackSession s5(ini);
    s5.beginResume("stable-id");
    CHECK(qFuzzyCompare(s5.takeResumeSeek(), 567.0), "setQueue resumeKey keys resume by the stable id");

    // ---- the per-track header channel (#59) -------------------------------------------------------------
    // Before it, this class carried urls only, so every queue-driven load — a gated audio stream, an IPTV
    // channel list, and every advance within either — reached the player with no headers and 403'd. What is
    // asserted here is not "the headers arrive" but "TRACK N's headers arrive, and nobody else's": a queue is
    // exactly where a per-source secret is most likely to outlive its source.
    {
        PlaybackSession q(ini);
        QStringList paths;
        QVector<StreamHeaders::Headers> seen;
        QObject::connect(&q, &PlaybackSession::playRequested,
                         [&](const QString& p, const StreamHeaders::Headers& trackHeaders) {
                             paths << p; seen << trackHeaders;
                         });

        StreamHeaders::Headers first, second;
        first.insert("Referer", "https://one.test/");
        second.insert("X-Token", "TWO");
        // Deliberately SHORTER than the track list: a playlist whose third entry sits on another host gets no
        // headers at all, and the caller expresses that by not supplying one.
        q.setQueue({ "http://one.test/a", "http://two.test/b", "http://three.test/c" }, 0, {}, QString(),
                   { first, second });
        CHECK(paths == QStringList{ "http://one.test/a" }, "the queue starts where it was told to");
        CHECK(seen.size() == 1 && seen.at(0) == first, "track 1 is played with track 1's headers");

        q.next();
        CHECK(seen.size() == 2 && seen.at(1) == second, "an advance carries the NEXT track's headers");
        // The assertion this section exists for. An advance that emitted nothing, or emitted the previous
        // track's, is the leak: track 2 is a different host.
        CHECK(seen.size() == 2 && !seen.at(1).contains("Referer"),
              "…and not the previous track's, which belong to a different host");

        q.next();
        CHECK(seen.size() == 3 && seen.at(2).isEmpty(),
              "a track the caller supplied no headers for is played with an EMPTY set");
        // Empty is not "absent": it is what makes the player CLEAR what the last track set (MpvHeaderApply
        // writes all three properties unconditionally), so the signal has to fire for it at all.
        CHECK(paths.size() == 3 && paths.at(2) == QStringLiteral("http://three.test/c"),
              "…and is still played, rather than being skipped for having none");

        // A new queue REPLACES the header list. Appending — or keeping the old one — would index the
        // previous queue's secrets by position into a completely unrelated track list.
        q.setQueue({ "http://four.test/d" }, 0);
        CHECK(seen.size() == 4 && seen.at(3).isEmpty(),
              "a queue that supplies no headers plays with none, whatever the last queue had");

        StreamHeaders::Headers only;
        only.insert("X-Token", "FIVE");
        q.setQueue({ "http://five.test/e" }, 0, {}, QString(), { only });
        CHECK(seen.size() == 5 && seen.at(4) == only, "…and one that does, plays with its own");

        // There is deliberately NO assertion that clearQueue() drops the header list. One was written here
        // ("the same url replayed after a clear carries nothing") and mutation testing showed it inert:
        // deleting the clear from clearQueue left it passing, because trackHeaders_ is only ever READ from
        // playIndex, every route to playIndex goes through setQueue, and setQueue ASSIGNS the list. There is
        // no observable difference, so there is nothing to pin — and an assertion that survives the deletion
        // of the line it names reads as coverage while being none. (Same finding, and the same treatment, as
        // #43's forPlayUrl early return.) The clear itself stays: tracks_ and trackHeaders_ are a parallel
        // pair and clearing one without the other is the state a future reader would be bitten by.
    }

    // ---- gapless playback (#141) ------------------------------------------------------------------------
    // The load-bearing correctness requirement: with gapless on, mpv's decoder does NOT stop between tracks, so
    // the per-track EOF no longer advances. mpv crosses to the next appended entry itself and reports it via
    // playlist-pos; PlaybackSession::onPlaylistPos must fire the SAME per-item bookkeeping handleTrackEnd does —
    // EXACTLY once per track, no double-fire, no missed track. This section pins that at the session level
    // (headlessly drivable, unlike the real audio output), plus the pure boundary function and the default.

    // The PURE boundary function, pinned with a hand-computed oracle (independent of the function's own body):
    // mpv plays a playlist strictly forward one entry at a time, so cur>prev completes (cur-prev) tracks; a
    // duplicate (cur==prev) or a backward/manual jump (cur<prev) completes none.
    CHECK(PlaybackSession::tracksCompleted(0, 1) == 1, "tracksCompleted: a normal +1 advance completes one track");
    CHECK(PlaybackSession::tracksCompleted(2, 3) == 1, "tracksCompleted: +1 anywhere in the queue completes one");
    CHECK(PlaybackSession::tracksCompleted(0, 3) == 3, "tracksCompleted: a 0->3 skip completes the three passed");
    CHECK(PlaybackSession::tracksCompleted(5, 5) == 0, "tracksCompleted: a duplicate notification completes none");
    CHECK(PlaybackSession::tracksCompleted(3, 1) == 0, "tracksCompleted: a backward jump completes none (hard-reload path owns it)");
    CHECK(PlaybackSession::tracksCompleted(2, -1) == 0, "tracksCompleted: mpv idle (-1) completes none here (EOF owns the last track)");

    // The setting defaults ON. It shipped opt-in, which meant the gap — a real defect on any segued record,
    // where the stop-start path cuts the music mid-phrase — went on being heard by everyone who never found a
    // setting for a problem they could not name. Safe as a default because MpvWidget applies mpv's `weak`
    // mode, which joins two tracks only when their formats already match and otherwise behaves exactly as
    // before. Read from the probe's isolated data dir (never the real user store), which is empty, so this is
    // the coded default in Settings.cpp and not a leftover value.
    CHECK(Settings::gaplessAudio() == true, "gapless setting defaults to true (weak mode: a no-op where it cannot help)");

    // Session drive: a 3-track gapless audio queue advancing under mpv's playlist-pos, exactly as the live
    // player feeds it. Assert EACH per-item callback fires exactly once per track and in order.
    {
        PlaybackSession g(ini);
        QStringList plays;                 // playRequested = REPLACE-loads (must be the START track ONLY)
        QStringList appends;               // appendRequested = the one-ahead feed to mpv's playlist
        QVector<int> announced;            // trackChanged indices (the per-track "now playing" callback)
        int gFinished = 0;
        QObject::connect(&g, &PlaybackSession::playRequested, [&](const QString& p, const StreamHeaders::Headers&) { plays << p; });
        QObject::connect(&g, &PlaybackSession::appendRequested, [&](const QString& p, const StreamHeaders::Headers&) { appends << p; });
        QObject::connect(&g, &PlaybackSession::trackChanged, [&](int i, int, const QString&) { announced << i; });
        QObject::connect(&g, &PlaybackSession::queueFinished, [&] { ++gFinished; });

        g.setGapless(true);
        g.setQueue({ "a.flac", "b.flac", "c.flac" }, 0);
        // Bootstrap: the start track is REPLACE-loaded once; its successor is appended one-ahead; track 0 is
        // announced once. Nothing else has happened yet.
        CHECK(plays == QStringList{ "a.flac" }, "gapless start: the first track is replace-loaded exactly once");
        CHECK(appends == QStringList{ "b.flac" }, "gapless start: the next track is appended one-ahead");
        CHECK(announced == QVector<int>{ 0 }, "gapless start: track 0 announced once");

        // mpv finishes track 0 and crosses to playlist index 1 on its own (no EOF-driven replace).
        g.onPlaylistPos(1);
        CHECK(plays == QStringList{ "a.flac" }, "gapless advance does NOT replace-load — the decoder kept running");
        CHECK(appends == (QStringList{ "b.flac", "c.flac" }), "advancing to track 1 feeds track 2 one-ahead");
        CHECK(announced == (QVector<int>{ 0, 1 }), "track 1 announced exactly once on the boundary");

        // A DUPLICATE notification for the same index must do nothing (no re-announce, no re-append).
        g.onPlaylistPos(1);
        CHECK(appends == (QStringList{ "b.flac", "c.flac" }) && announced == (QVector<int>{ 0, 1 }),
              "a duplicate playlist-pos completes nothing (no double-fire)");

        // mpv crosses to the last track. Its successor would be index 3 (past the end), so nothing is appended.
        g.onPlaylistPos(2);
        CHECK(announced == (QVector<int>{ 0, 1, 2 }), "the last track is announced exactly once");
        CHECK(appends == (QStringList{ "b.flac", "c.flac" }), "no phantom append past the end of the queue");
        CHECK(gFinished == 0, "the queue is not finished while the last track is still playing");

        // The LAST track's completion is the ONE boundary that arrives as an EOF (there is no playlist-pos past
        // the last index), which the host routes to handleTrackEnd — exactly as it does with gapless off.
        g.handleTrackEnd();
        CHECK(gFinished == 1, "the last track's EOF finishes the queue exactly once");
        CHECK(plays == QStringList{ "a.flac" }, "across the WHOLE gapless queue there was exactly ONE replace-load");
        CHECK(announced == (QVector<int>{ 0, 1, 2 }), "every track was announced exactly once, none twice, none skipped");
    }

    // Gapless OFF is byte-for-byte the old machine: no append ever fires, and the EOF path advances by
    // replace-load as before. (The block at the top of this file already pins the EOF advance itself; here we
    // pin only that the gapless feed stays silent when it is off.)
    {
        PlaybackSession off(ini);
        int appendCount = 0;
        QStringList offPlays;
        QObject::connect(&off, &PlaybackSession::appendRequested, [&](const QString&, const StreamHeaders::Headers&) { ++appendCount; });
        QObject::connect(&off, &PlaybackSession::playRequested, [&](const QString& p, const StreamHeaders::Headers&) { offPlays << p; });
        off.setGapless(false);
        off.setQueue({ "x.mp3", "y.mp3" }, 0);
        off.onPlaylistPos(1);              // must be inert with gapless off
        off.handleTrackEnd();              // the old EOF advance: replace-load track 2
        CHECK(appendCount == 0, "gapless OFF never appends — no mpv playlist is built");
        CHECK(offPlays == (QStringList{ "x.mp3", "y.mp3" }), "gapless OFF still advances by replace-load on track end");
    }

    // ---- editing the queue you are listening to (#193) ---------------------------------------------------
    // QueueEdit's arithmetic is pinned by probe_queueedit; what is pinned HERE is that the session applies it
    // to the three parallel lists together and tells the host what it is owed. The load-bearing part is the
    // last one: an edit that crosses the gapless frontier must emit queueFeedInvalidated, because without it
    // mpv goes on playing the entry it was handed and the app announces a different one — the wrong song, with
    // nothing red anywhere.
    {
        PlaybackSession e(ini);
        QStringList plays, appends;
        QVector<QStringList> lists;      // every queueChanged payload, in order
        QVector<int> currents;
        int reseats = 0, stopped = 0, eFinished = 0;
        QObject::connect(&e, &PlaybackSession::playRequested, [&](const QString& p, const StreamHeaders::Headers&) { plays << p; });
        QObject::connect(&e, &PlaybackSession::appendRequested, [&](const QString& p, const StreamHeaders::Headers&) { appends << p; });
        // #193 inc 3: `replaced` is what tells a NEW queue apart from an EDIT of the one already playing, and
        // the host presents a now-playing surface only for the first. Recorded here because getting it wrong
        // is invisible to every other assertion in this block — the list is correct either way, and the
        // symptom is on screen: an "add to queue" from a browse row yanks the listener onto the player.
        QVector<bool> replacedFlags;
        QObject::connect(&e, &PlaybackSession::queueChanged,
                         [&](const QStringList& t, int c, bool replaced)
                         { lists << t; currents << c; replacedFlags << replaced; });
        QObject::connect(&e, &PlaybackSession::queueFeedInvalidated, [&] { ++reseats; });
        QObject::connect(&e, &PlaybackSession::playbackStopped, [&] { ++stopped; });
        QObject::connect(&e, &PlaybackSession::queueFinished, [&] { ++eFinished; });

        e.setGapless(true);
        e.setQueue({ "a.flac", "b.flac", "c.flac" }, 0, { "A", "B", "C" });
        CHECK(appends == QStringList{ "b.flac" }, "edit setup: the one-ahead feed handed mpv track 2");

        // ENQUEUE — past the frontier, so mpv is left alone.
        CHECK(e.enqueue({ "z.flac" }, { "Z" }), "enqueue returns true");
        CHECK(e.tracks() == (QStringList{ "a.flac", "b.flac", "c.flac", "z.flac" }), "enqueue appends the track");
        CHECK(e.titles() == (QStringList{ "A", "B", "C", "Z" }), "enqueue appends its title alongside");
        CHECK(reseats == 0, "an append never invalidates what mpv already holds");
        CHECK(lists.size() == 2 && lists.last() == e.titles(), "enqueue announces the new list once");
        // The install said "a whole new queue"; the append said "an edit". #193 inc 3 rides on that split:
        // the second one must bring NOTHING forward, or every browse-side add throws the listener onto the
        // player page — which is the state increment 2's Append arm exists to avoid.
        CHECK(replacedFlags == (QVector<bool>{ true, false }),
              "setQueue announces a REPLACED queue; an enqueue announces an edit");

        // PLAY NEXT — lands exactly on the entry mpv was handed, which is the crossing this all exists for.
        CHECK(e.playNext({ "n.flac" }, { "N" }), "playNext returns true");
        CHECK(e.tracks() == (QStringList{ "a.flac", "n.flac", "b.flac", "c.flac", "z.flac" }),
              "playNext inserts immediately after the playing track");
        CHECK(e.currentIndex() == 0, "playNext does not move the playing track");
        CHECK(reseats == 1, "playNext CROSSES the frontier: the host is told to re-seat mpv");
        CHECK(plays == QStringList{ "a.flac" }, "…and nothing was reloaded: the track playing kept playing");

        // A title-less insert falls back to the file's base name, the same rule an install uses.
        CHECK(e.enqueue({ "X:/deep/folder/untitled.flac" }), "an insert with no title is accepted");
        CHECK(e.titles().last() == QStringLiteral("untitled"), "…and is named by the file, not left blank");

        // MOVE that leaves the coming boundary alone (both rows below the playing track and its successor).
        const int reseatsBeforeMove = reseats;
        CHECK(e.moveTrack(4, 3), "moveTrack returns true");
        CHECK(e.tracks().at(3) == QStringLiteral("z.flac") && e.tracks().at(4) == QStringLiteral("c.flac"),
              "moveTrack reorders the two rows");
        CHECK(e.titles().at(3) == QStringLiteral("Z"), "…carrying each row's title with it");
        CHECK(reseats == reseatsBeforeMove, "a move clear of the frontier does not re-seat mpv");

        // REFUSALS change nothing and announce nothing.
        const int listsBefore = int(lists.size());
        CHECK(!e.removeTrack(99) && !e.moveTrack(0, 0) && !e.insertTracks(-1, { "q.flac" }),
              "out-of-range and no-op edits are refused");
        CHECK(int(lists.size()) == listsBefore, "…and a refused edit announces nothing");
    }
    {
        // ARMING GAPLESS ON A QUEUE THAT IS ALREADY PLAYING (issue #193 increment 2, armGaplessLive).
        //
        // THE CASE. A ONE-TRACK queue arms nothing: the host arms gapless from `queue.size() > 1`, because a
        // queue with no boundary has nothing to bridge. The reach verbs are the first thing in the app that
        // can give such a queue a boundary — "add another track to what I am listening to" — and if gapless
        // stays off, the very first thing the feature does is re-introduce the gap #141 removed.
        //
        // AND WHY IT IS NOT JUST setGapless(true). The one-ahead frontier is SEEDED by playIndex's gapless
        // branch, which did not run: appendedThrough_ is still -1 while trackIndex_ is 0, so maybeAppendNext's
        // one-ahead test (appendedThrough_ == trackIndex_) refuses forever. Flipping the flag alone arms a
        // feed that can never fire, and that failure is invisible — the track still plays, just late.
        PlaybackSession g2(ini);
        QStringList plays, appends;
        QObject::connect(&g2, &PlaybackSession::playRequested, [&](const QString& p, const StreamHeaders::Headers&) { plays << p; });
        QObject::connect(&g2, &PlaybackSession::appendRequested, [&](const QString& p, const StreamHeaders::Headers&) { appends << p; });

        g2.setGapless(false);                                   // a single-track queue: the host arms nothing
        g2.setQueue({ "solo.flac" }, 0, { "Solo" });
        CHECK(plays == QStringList{ "solo.flac" } && appends.isEmpty(), "live-arm setup: one track, no feed");

        CHECK(g2.enqueue({ "second.flac" }, { "Second" }), "a track can be added to a one-track queue");
        CHECK(appends.isEmpty(), "…and with gapless off nothing is handed to mpv yet");

        g2.armGaplessLive();
        CHECK(g2.gapless(), "armGaplessLive turns gapless on for the running queue");
        g2.feedNextTrack();
        CHECK(appends == QStringList{ "second.flac" },
              "…and the frontier is seeded, so the added track really is handed to mpv");
        CHECK(plays == QStringList{ "solo.flac" }, "…without reloading the track that is playing");

        // Idempotent at the same boundary (maybeAppendNext's one-ahead invariant), and inert once armed:
        // a second arm must not re-seed a frontier that is now live and drop a real append.
        g2.armGaplessLive();
        g2.feedNextTrack();
        CHECK(appends == QStringList{ "second.flac" }, "a second arm + feed appends nothing twice");

        // Nothing playing: there is no entry to seed the frontier onto, so it refuses rather than arming a
        // feed pointing at index -1.
        PlaybackSession idle(ini);
        idle.armGaplessLive();
        CHECK(!idle.gapless(), "armGaplessLive refuses when nothing is playing");
    }
    {
        // REMOVING THE TRACK THAT IS PLAYING. Pinned behaviour: advance onto whatever takes its place, with a
        // real (re)load — mpv cannot flow into an entry the app just deleted.
        PlaybackSession r(ini);
        QStringList plays;
        int stopped = 0, rFinished = 0;
        QObject::connect(&r, &PlaybackSession::playRequested, [&](const QString& p, const StreamHeaders::Headers&) { plays << p; });
        QObject::connect(&r, &PlaybackSession::playbackStopped, [&] { ++stopped; });
        QObject::connect(&r, &PlaybackSession::queueFinished, [&] { ++rFinished; });
        // The remove-the-playing-track branch is a SECOND queueChanged emit inside commitEdit, so it carries
        // its own `replaced` and can be forgotten on its own (#193 inc 3).
        QVector<bool> replacedFlags;
        QObject::connect(&r, &PlaybackSession::queueChanged,
                         [&](const QStringList&, int, bool replaced) { replacedFlags << replaced; });
        r.setGapless(true);
        r.setQueue({ "one.flac", "two.flac", "three.flac" }, 1);
        CHECK(plays == QStringList{ "two.flac" }, "remove-current setup: track 2 is playing");
        CHECK(r.removeTrack(1), "removing the playing track is accepted");
        CHECK(plays == (QStringList{ "two.flac", "three.flac" }),
              "removing the playing track advances onto the one that took its place");
        CHECK(r.currentIndex() == 1 && r.count() == 2, "…at the removed track's index, in a shorter queue");
        CHECK(stopped == 0, "…and does not stop playback while there is something after it");

        // ...and when there is nothing after it, playback STOPS. Deliberately not queueFinished: that means
        // "played to the end" and hands the moment to channel mode / the next-episode chain, and this track
        // did not play out.
        CHECK(r.removeTrack(1), "removing the playing LAST track is accepted");
        CHECK(stopped == 1, "…and stops playback");
        CHECK(rFinished == 0, "…without claiming the queue played out (no channel / next-episode hand-off)");
        CHECK(plays == (QStringList{ "two.flac", "three.flac" }), "…and starts nothing else");
        // One install, two removals: the install replaced the queue, both removals only edited it. Deleting a
        // track must not bring a now-playing surface forward over the browse row the delete was ordered from.
        CHECK(replacedFlags == (QVector<bool>{ true, false, false }),
              "removing the playing track announces an EDIT, on both its branches");
    }
    {
        // THE PER-TRACK HEADERS ARE RENUMBERED WITH THE TRACKS. An IPTV-shaped queue whose entries carry
        // different credentials is the case where a mis-indexed edit is not merely the wrong song: it is one
        // host's token sent to another. The insert goes ABOVE the playing track, which shifts every entry.
        PlaybackSession h(ini);
        QStringList paths;
        QVector<StreamHeaders::Headers> seen;
        QObject::connect(&h, &PlaybackSession::playRequested,
                         [&](const QString& p, const StreamHeaders::Headers& trackHeaders) { paths << p; seen << trackHeaders; });
        StreamHeaders::Headers one, two;
        one.insert("X-Token", "ONE");
        two.insert("X-Token", "TWO");
        h.setQueue({ "http://a.test/1", "http://a.test/2" }, 0, {}, QString(), { one, two });
        CHECK(h.insertTracks(0, { "http://a.test/0" }), "insert above the playing track is accepted");
        CHECK(h.currentIndex() == 1, "…and carries the cursor with it");
        h.next();
        CHECK(paths.size() == 2 && paths.last() == QStringLiteral("http://a.test/2"), "advancing reaches entry 2");
        CHECK(seen.size() == 2 && seen.last() == two, "…with ITS OWN headers, not the ones that were at that index");
    }

    {
        // ---- ONE IDENTITY PER TRACK, WHICHEVER ROUTE REACHED IT (issue #204) ----------------------------
        //
        // A music-server queue holds SIGNED STREAM URLS — links minted from the user's password — and this
        // class used to file the resume position and the consumption seconds under exactly those. So the
        // same track banked under two different keys depending on whether a playlist or the album view
        // opened it, and changing the password silently orphaned every row the album route had written.
        //
        // What is asserted is the property, not the plumbing: a position banked while playing a URL is found
        // again by the track's DURABLE name — including from a session that has never heard of that URL, and
        // including after the URL has been RE-SIGNED, which is what changing a password does.
        const QString url  = QStringLiteral("https://box.test/rest/stream.view?u=x&t=deadbeef&s=abc&id=t7");
        const QString url2 = QStringLiteral("https://box.test/rest/stream.view?u=x&t=cafef00d&s=zzz&id=t7");
        const QString id   = QStringLiteral("sub\u001Fsrv1\u001Ftrack\u001Ft7");
        {
            PlaybackSession m(ini);
            m.setTrackIdentities({ { url, id } });
            CHECK(m.identityFor(url) == id, "#204: a mapped play path resolves to its durable identity");
            CHECK(m.identityFor(QStringLiteral("D:/music/01.flac")) == QStringLiteral("D:/music/01.flac"),
                  "#204: an unmapped path is its own identity (a local queue is untouched)");
            m.setQueue({ url }, 0, { QStringLiteral("Airbag") });
            m.setDuration(300.0);
            m.setPosition(123.0);
            m.persistResume();
        }
        {
            // A DIFFERENT session, told nothing about any url: the position is under the track's own name.
            PlaybackSession r(ini);
            r.beginResume(id);
            CHECK(qFuzzyCompare(r.takeResumeSeek(), 123.0),
                  "#204: the position is filed under the qualified id, not the signed url");
        }
        {
            // The signed url is not a key anywhere: asking for it finds nothing at all.
            PlaybackSession r(ini);
            r.beginResume(url);
            CHECK(qFuzzyCompare(r.takeResumeSeek() + 1.0, 1.0),
                  "#204: nothing was banked under the stream url itself");
        }
        {
            // THE PASSWORD CHANGED, so the server signs a different url for the same track. The queue is
            // rebuilt from the new url and the listener is still where they left off — the whole issue.
            PlaybackSession r(ini);
            r.setTrackIdentities({ { url2, id } });
            r.setQueue({ url2 }, 0, { QStringLiteral("Airbag") });
            CHECK(qFuzzyCompare(r.takeResumeSeek(), 123.0),
                  "#204: a re-signed url finds the same position (a password change costs nothing)");
        }
        {
            // setQueue's explicit resumeKey — the PLAYLIST route (#203) — is already a durable name and must
            // pass through unchanged even while a map is installed.
            PlaybackSession r(ini);
            r.setTrackIdentities({ { url, QStringLiteral("WRONG") } });
            r.setQueue({ url }, 0, { QStringLiteral("Airbag") }, id);
            CHECK(qFuzzyCompare(r.takeResumeSeek(), 123.0),
                  "#204: an explicit resumeKey is its own identity and is not re-mapped");
        }
    }


    // ---- WHERE A BOOK RESUMES WHEN A PART BOUNDARY FAILS (#220) -----------------------------------------
    //
    // THE REPORT. Forty-five minutes into part one of a fifty-seven part book. Part one ends, the queue
    // advances to part two, and part two's link cannot be minted - the source answers with no link, which
    // #217 taught the app to stop and say. Re-open the book and it starts at PART ONE, FROM THE TOP.
    //
    // Every step that produced that was individually right. finishResume drops part one's mark BECAUSE part
    // one played to its end; persistResume writes nothing for part two BECAUSE part two never played a
    // second. What nothing wrote down is the fact the listener actually cares about: that they REACHED part
    // two. So the boundary writes it - a position-zero mark for the incoming part, in the same step that
    // finishes the outgoing one - and the scan reads "the last part carrying a mark" rather than "the last
    // part carrying more than a second of playback".
    //
    // This is driven at the PlaybackSession level, through the same store the app's scan reads, because that
    // is the whole mechanism: the boundary write and the scan are two halves that have to be pinned against
    // each other. The queue holds part TOKENS, which is what a remote book queues (a credential-free name for
    // one part; see core/RemoteAudiobook.h) - but nothing here is remote-specific, and a local book's file
    // paths take the identical path through both halves.
    {
        QTemporaryDir bookTmp;
        const QString bini = bookTmp.filePath("book.ini");
        const QStringList book{ QStringLiteral("book~part1"), QStringLiteral("book~part2"),
                                QStringLiteral("book~part3"), QStringLiteral("book~part4") };
        QSettings scan(bini, QSettings::IniFormat);
        // THE APP'S OWN SCAN, called exactly as MainWindow::openAudiobook and openRemoteAudiobook call it.
        auto resumesAt = [&] { scan.sync(); return ResumeStore::lastMarkedIndex(scan, book); };
        auto markOf = [&](int i, const char* leaf) {
            scan.sync();
            return scan.value(ResumeStore::groupFor(book.at(i)) + QLatin1Char('/') + QLatin1String(leaf), -1.0)
                       .toDouble();
        };

        QStringList played;
        PlaybackSession b(bini);
        QObject::connect(&b, &PlaybackSession::playRequested, [&](const QString& p) { played << p; });

        // Opening a book that has never been played must leave NOTHING behind. The reached-mark is written at
        // a BOUNDARY and nowhere else: if merely opening wrote one, a finished book would come back from the
        // dead on the next open (the guard-rail case at the end of this block), and every open would push a
        // row to the cross-device sync for a part nobody listened to.
        b.setQueue(book, 0);
        CHECK(resumesAt() == -1, "#220: opening a book writes no mark of its own");

        // Forty-five minutes into part one.
        b.setDuration(2700.0);
        b.setPosition(2705.0);
        b.persistResume();
        CHECK(resumesAt() == 0, "#220: the book resumes in the part being listened to");

        // ---- 1. the boundary that fails. Part one plays out; the queue advances; part two never plays.
        b.handleTrackEnd();
        CHECK(played.last() == book.at(1), "#220: the queue advanced to part two");
        CHECK(resumesAt() == 1,
              "#220: a part that was REACHED and never played is where the book resumes (the reported bug)");
        CHECK(qFuzzyCompare(markOf(1, "pos") + 1.0, 1.0),
              "#220: ...recorded honestly, at position zero - no invented number to clear a threshold");
        CHECK(markOf(1, "ts") > 0.0,
              "#220: ...stamped, so the cross-device merge can compare it like any other resume row");
        // The duration is deliberately NOT the finished part's. Nothing has opened part two, so its length is
        // unknown, and the Home progress bar wants BOTH a position and a duration past a second - an inherited
        // duration would draw a bar over a part nobody has heard a second of.
        CHECK(qFuzzyCompare(markOf(1, "dur") + 1.0, 1.0),
              "#220: ...with no duration, because nothing has opened the part to learn one");

        // ---- 4. re-open the book. The mint succeeds this time: it starts at part two, from the start of it.
        {
            QStringList reopened;
            PlaybackSession r(bini);
            QObject::connect(&r, &PlaybackSession::playRequested, [&](const QString& p) { reopened << p; });
            const int start = ResumeStore::lastMarkedIndex(scan, book);
            r.setQueue(book, start < 0 ? 0 : start);
            CHECK(reopened == QStringList{ book.at(1) },
                  "#220: the re-opened book starts at the part that failed");
            CHECK(qFuzzyCompare(r.takeResumeSeek() + 1.0, 1.0),
                  "#220: ...from the START of it - a reached part has no position to seek into");
        }

        // ---- 2. the ORDINARY boundary. Part two now plays for ninety seconds: the real position simply
        // overwrites the zero one, and nothing about the answer differs from before this existed.
        b.setDuration(3000.0);
        b.setPosition(90.0);
        b.persistResume();
        CHECK(resumesAt() == 1, "#220: a part being listened to is still where the book resumes");
        CHECK(qFuzzyCompare(markOf(1, "pos"), 90.0),
              "#220: a played part's real position overwrites the reached-mark (no stale zero left behind)");
        CHECK(qFuzzyCompare(markOf(1, "dur"), 3000.0),
              "#220: ...and brings the duration the Home progress bar needs with it");

        // ---- the guard rail the reached-mark must not break: it may never CLOBBER a position already
        // banked for the part being entered. Bank twenty minutes in part four, then play parts two and three
        // out into it - the boundary write has to find a mark there and leave it alone, or a listener who
        // jumped back a part loses the twenty minutes the moment the earlier part ends.
        {
            PlaybackSession j(bini);
            j.beginResume(book.at(3));
            j.setDuration(3600.0);
            j.setPosition(1200.0);
            j.persistResume();
        }
        b.handleTrackEnd();                 // part two ends -> part three reached
        CHECK(resumesAt() == 3, "#220: a later part's banked position still wins the scan");
        b.setDuration(1800.0);
        b.setPosition(600.0);
        b.persistResume();                  // ten minutes into part three
        b.handleTrackEnd();                 // part three ends -> part four reached, and it already has a mark
        CHECK(qFuzzyCompare(markOf(3, "pos"), 1200.0),
              "#220: entering a part that already carries a position leaves that position alone");
        CHECK(resumesAt() == 3, "#220: ...so the book still resumes twenty minutes into part four");

        // ---- 3. the LAST part ends. A finished book carries no mark on any part - which is what makes it
        // read as finished and start from the top next time. The reached-mark must not be written past the
        // end of the queue, and the last part's own mark must still go.
        b.setPosition(3595.0);
        b.handleTrackEnd();
        CHECK(resumesAt() == -1, "#220: a book played to its end carries no mark on any part");
        {
            QStringList reopened;
            PlaybackSession r(bini);
            QObject::connect(&r, &PlaybackSession::playRequested, [&](const QString& p) { reopened << p; });
            const int start = ResumeStore::lastMarkedIndex(scan, book);
            r.setQueue(book, start < 0 ? 0 : start);
            CHECK(reopened == QStringList{ book.first() }, "#220: ...so it re-opens at part one, from the top");
        }
    }

    // ---- the SAME boundary, crossed by the PLAYER itself (#220 over #141's gapless path) ----------------
    // A local book is an ordinary gapless audio queue: mpv holds the next part already and crosses into it on
    // its own, so the advance arrives as a playlist-pos report rather than as an EOF. That boundary owes the
    // book exactly what handleTrackEnd's owes it - it is the second of the two ways a part gets reached, and
    // the one a fix written only into handleTrackEnd would silently miss.
    {
        QTemporaryDir gapTmp;
        const QString gini = gapTmp.filePath("gapless.ini");
        const QStringList parts{ QStringLiteral("D:/Book/01.mp3"), QStringLiteral("D:/Book/02.mp3"),
                                 QStringLiteral("D:/Book/03.mp3") };
        QSettings scan(gini, QSettings::IniFormat);
        auto resumesAt = [&] { scan.sync(); return ResumeStore::lastMarkedIndex(scan, parts); };

        PlaybackSession g(gini);
        g.setGapless(true);
        g.setQueue(parts, 0);
        g.setDuration(1800.0);
        g.setPosition(1799.0);
        g.persistResume();
        CHECK(resumesAt() == 0, "#220/gapless: part one carries the position while it plays");
        g.onPlaylistPos(1);          // mpv crossed into part two by itself
        CHECK(g.currentIndex() == 1, "#220/gapless: the queue followed the player across the boundary");
        CHECK(resumesAt() == 1, "#220/gapless: the part the player crossed INTO is where the book resumes");
        // ...and the last-track guard holds here too: no mark is invented past the end of the queue.
        g.setPosition(1799.0);
        g.onPlaylistPos(2);
        g.setPosition(1799.0);
        g.handleTrackEnd();
        CHECK(resumesAt() == -1, "#220/gapless: a book played out to its last part carries no mark");
    }

    // ---- #83: A POSITION THAT BELONGS TO A SERVER IS NOT WRITTEN HERE ---------------------------------
    // The decision the issue states in as many words: for a Jellyfin item the server is the one authority
    // for where the user got to, so this device writes NO resume row and NO tombstone for it and reports
    // instead. Two authorities for one number means the last device to close wins.
    {
        const QString jf = QStringLiteral("jf:0123456789abcdef0123456789abcdef:aabbcc");
        {
            PlaybackSession sv(ini);
            QStringList reported;
            QObject::connect(&sv, &PlaybackSession::serverProgress,
                             [&](const QString& k, double s) {
                                 reported << k + QStringLiteral("@") + QString::number(int(s)); });
            int localSaves = 0;
            QObject::connect(&sv, &PlaybackSession::resumeSaved, [&] { ++localSaves; });
            sv.beginResume(jf);
            CHECK(!sv.resumeOwnedByServer(),
                  "#83: beginResume CLEARS the flag, so a server item cannot leave the next file unwritten");
            sv.setResumeOwnedByServer(true);
            // The server said 600s; this replaces whatever beginResume read out of the ini.
            sv.seedResume(600.0);
            CHECK(qFuzzyCompare(sv.takeResumeSeek(), 600.0),
                  "#83: seedResume is where the open starts from");
            sv.setDuration(3000.0);
            // setPosition IS the throttle -- it calls persistResume itself once the position has moved
            // five seconds, which is the hook this feature hangs off. So the report is already out before
            // the explicit call below; what is asserted is that it happened and what it carried.
            sv.setPosition(620.0);
            sv.persistResume();
            CHECK(!reported.isEmpty() && reported.last() == jf + QStringLiteral("@620"),
                  "#83: the throttled hook reports to the server, carrying the qualified id");
            CHECK(localSaves == 0,
                  "#83: ...and does NOT schedule the cloud progress push, which has no row to carry");
        }
        {
            // A DIFFERENT session, asked where that item resumes: NOTHING was banked locally. This is the
            // claim, and it is asserted from the outside rather than by inspecting the writer.
            PlaybackSession r(ini);
            r.beginResume(jf);
            CHECK(qFuzzyCompare(r.takeResumeSeek() + 1.0, 1.0),
                  "#83: no local resume row exists for a server-owned item");
        }
        {
            // ...and the identity is a MACHINE KEY, so it must not end up in a title field. #203's rule,
            // third key family: "jf:<server>:<item>" has no '/', no '.' and no unit separator, so every
            // earlier test hands it back whole.
            PlaybackSession t(ini);
            t.beginResume(jf);
            t.setResumeOwnedByServer(true);
            t.setDuration(3000.0);
            t.setPosition(700.0);
            t.persistResume();
            QSettings s(ini, QSettings::IniFormat);
            bool machineTitle = false;
            for (const QString& k : s.allKeys())
                if (s.value(k).toString().contains(QStringLiteral("jf:0123456789abcdef")))
                    machineTitle = true;
            CHECK(!machineTitle, "#83: a server item's id is never written into a title field");
        }
        {
            // FINISHING one clears nothing and TOMBSTONES nothing: there is no local row to clear, and a
            // dated tombstone for a position this device never stored would be carried to every other
            // device by the merge as an authoritative "this was finished".
            PlaybackSession f(ini);
            f.beginResume(jf);
            f.setResumeOwnedByServer(true);
            f.setDuration(3000.0);
            f.setPosition(2990.0);
            f.finishResume();
            QSettings s(ini, QSettings::IniFormat);
            bool tomb = false;
            for (const QString& k : s.allKeys())
                if (k.startsWith(QStringLiteral("deleted/resume/"))) tomb = true;
            CHECK(!tomb, "#83: finishing a server item writes no tombstone");
        }
        {
            // The CONTROL. The same three calls without the flag DO write a local row -- so the checks
            // above are about the flag and not about the fixture being unwritable.
            const QString local = QStringLiteral("X:/films/local.mkv");
            PlaybackSession c(ini);
            c.beginResume(local);
            c.setDuration(3000.0);
            c.setPosition(620.0);
            c.persistResume();
            PlaybackSession r(ini);
            r.beginResume(local);
            CHECK(qFuzzyCompare(r.takeResumeSeek(), 620.0),
                  "#83 control: without the flag the position IS written locally, as it always was");
        }
    }

    if (fails == 0) printf("PLAYBACK-OK\n");
    return fails == 0 ? 0 : 1;
}
