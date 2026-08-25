// KEEPING WHAT THE USER BANKED WHEN THE PREFERRED SOURCE CHANGES (issue #194, increment 2).
//
// Increment 1 merged the local music library and every Subsonic server into one set of artists and albums,
// and made a merged row carry the KEY OF THE INSTANCE THE PREFERENCE PICKED (MusicMerge.h says why nothing
// new is minted). The honest cost it wrote down is this file's whole reason to exist: change
// `music/preferredSource` and the row is keyed on the other copy, so everything already accrued against the
// first one is stranded — invisible rather than deleted, which is worse, because nothing on screen says
// where it went.
//
// This is the half that moves it, and it is PcGameRemap's shape deliberately: the issue names that unit as
// the precedent, and the property that made it safe is the property that matters here too.
//
// ==================================================================================================
// WHAT IS ACTUALLY KEYED ON A MUSIC IDENTITY, AND WHAT IS NOT
// ==================================================================================================
// This was surveyed rather than assumed, and the survey is most of the answer:
//
//   * A MUSIC ALBUM OR ARTIST ROW'S id ("musicalbum:<key>" / "musicartist:<key>") REACHES NO STORE AT ALL.
//     Every verb that would write one — the themed leaf chooser, the detail panel, bulk-select, "add to
//     playlist" — refuses a "_"-prefixed type (LeafRoute.cpp's themedEnterFor drills instead of opening the
//     chooser; HomeView::themedDetailData returns {} for a "_" type; addItemToPlaylistInteractive returns
//     early). So there are no favourites, marks, tags or playlist entries under a music album key to move.
//   * THE SCROBBLE QUEUE HOLDS NO MUSIC KEY. A queued listen is (artist, title, album, albumArtist) plus the
//     moment it started (Scrobble::Play), filed per profile and per provider. It is named here because the
//     brief asked what happens to it, and the answer is the important one: NOTHING HAPPENS TO IT. A remap
//     that rewrote a pending submission could only duplicate, drop or re-date a listen the user cannot undo
//     on a third-party service, and there is no key in it that a source preference can invalidate.
//   * WHAT IS KEYED, AND STRANDED, IS PER-TRACK PLAYBACK DATA. A queue is built from the picked instance's
//     tracks, so the resume position and the accrued listening seconds are filed under THAT copy's track
//     identity — a local file path, or (for a server copy) the stable signed stream url. Flip the preference
//     and the album on screen is the other copy, whose tracks have never been played.
//
// The verdict for every store is in the report; the four this unit moves are the four that key on a track
// identity and would otherwise silently read zero.
//
// ==================================================================================================
// ONE IDENTITY PER TRACK, WHICHEVER ROUTE REACHED IT (issue #204) — AND WHY THERE IS NOW ONE TABLE
// ==================================================================================================
// Increment 2 of #194 shipped TWO tables, because a track answered to two names and two families of store
// keyed on one each:
//   playId    what the PLAYER was handed — MusicSupply::playUrl(). A local path passes through unchanged;
//             a qualified track id becomes a SIGNED STREAM URL. PlaybackSession keyed the resume record and
//             the consumption seconds by exactly this (its `resumePath_`).
//   indexId   what the INDEX calls the track — IndexTrack::path: the same local path, or the qualified,
//             credential-free track id. MainWindow's `syncKey_` is this, and SpeedStore and SyncOffsets key
//             on it — and so does every browse row, whose progress bar reads `resume/md5(IndexTrack::path)`.
//
// #204 IS THE FINDING THAT THE FIRST OF THOSE WAS NEVER AN IDENTITY AT ALL. A Subsonic stream url is signed
// from the user's password (Subsonic::authParams — `t` is md5(password + salt)), so keying on it meant:
//   * CHANGING THE PASSWORD SILENTLY ORPHANED every resume position and play count banked through the album
//     route. No error, no message: a listening history that appears to reset itself.
//   * TWO BUCKETS FOR ONE TRACK. The playlist route already re-keyed to the qualified id (#203), the album
//     route did not, so half an album played from a playlist and half from the album view banked under two
//     different keys and neither read what the listener had actually done.
//   * A browse row's progress bar, which has ALWAYS read md5(indexId), could never find a remote track's
//     position — the strongest evidence about which of the two names was meant to be the identity.
// PlaybackSession now keys on the DURABLE identity for every route (PlaybackSession::setTrackIdentities), so
// all four stores key on the indexId and one table serves all four. The old two-table rule — "a local path
// would have two different destinations" — was a consequence of writing into two key spaces at once, and it
// goes away with the second key space rather than being waived.
//
// A playId MAY BE A SIGNED URL, so it carries a credential in its query. It is never written anywhere: the
// stores hash it, and this unit only ever hands it to a hash — as the SOURCE of a move, never as a
// destination. Do not log a Table, and do not persist one — if either is ever needed, it goes through
// CredentialScrub first (#200).
//
// ==================================================================================================
// THE TWO PRODUCERS, AND WHY THEY ARE APPLIED IN TWO SEPARATE CALLS
// ==================================================================================================
//   streamKeyTable()  a pre-#204 playback identity (the signed stream url) -> the SAME track's durable
//                     identity. The migration this issue is about. A local track self-maps and is absent.
//   tableFor()        a durable identity -> the durable identity of the copy the source preference picked.
//                     #194 increment 2's table, unchanged in meaning, now expressed once instead of twice.
// A destination of the first is a source of the second, so they are NOT merged into one hash: a single pass
// over a table containing both A->B and B->C is order-dependent, and the order is a QHash's. Run the stream
// re-key first, to completion, and the merge remap after it — each is a complete, committed, idempotent
// migration on its own, and their composition is idempotent because each of them is.
//
// WHAT CANNOT BE MOVED, AND IS THEREFORE LEFT ALONE. The old key is md5(signed url), and md5 is one-way, so
// a row can only be re-keyed by RECOMPUTING the url it was filed under — which needs the password that
// signed it. Rows banked under a password that has ALREADY been changed (or against a server whose url or
// username was edited, or that has been removed) cannot be named again by anything, and this unit does not
// guess: it never invents a source it cannot derive and never deletes a row it cannot map. They stay exactly
// where they are, which is where they already were. Everything banked under the CURRENT credential moves the
// first time a music surface is opened, and nothing is ever keyed on a credential again.
//
// ==================================================================================================
// THE FOUR RULES, WHICH ARE PcGameRemap'S FOUR RULES
// ==================================================================================================
//   1. AN IDENTITY WITH NO DESTINATION IS ABSENT FROM THE TABLE — never mapped to an empty string. A record
//      whose album is not merged, whose copy has not been fetched, or whose track cannot be matched inside a
//      merged album SURVIVES UNTOUCHED under its own key. Losing a user's listening is far worse than
//      leaving a few rows behind on old keys, so every uncertainty resolves to "do nothing".
//   2. A record is never removed until its replacement has been written and flushed without error.
//   3. A destination that already holds a record is MERGED into, never overwritten (seconds sum; a resume
//      position is a single point in a single stream, so the newer one wins outright).
//   4. Running the remap twice equals running it once.
//
// RULE 3 IS ALSO #204'S "TWO BUCKETS" ANSWER, and it is worth spelling out per store rather than leaving it
// as a general principle, because the issue asks for a decision and the three arms are decided differently:
//   * LISTENING SECONDS SUM. They are an additive quantity and the listener heard both halves. Half an album
//     played from a playlist and half from the album view is one album listened to once, and any rule other
//     than a sum throws away time that was genuinely spent. (The sum is over two ABSOLUTE totals, which is
//     what makes re-running it not inflate anything: the source is retired in the same pass.)
//   * A RESUME POSITION: THE NEWER `ts` WINS OUTRIGHT. Not the larger position, which is the tempting one and
//     is wrong — restart a track today that you were 90% through last month and largest-wins throws you back
//     to 90% of a track you deliberately began again. Not a sum either: two positions in one stream do not
//     add up to a place anybody ever reached. The newer timestamp is the only one of the three that answers
//     the question the store is actually asked, which is "where did I leave off".
//   * A PLAYBACK SPEED AND A SYNC OFFSET ARE SETTINGS, not history, so the DESTINATION'S OWN value wins and
//     an incoming one is discarded rather than merged. The last thing the user set for the track they can
//     see is the answer; a value arriving from a key they cannot see must not silently override it.
//
// ==================================================================================================
// WHY IT IS REPEATABLE RATHER THAN A ONE-SHOT STAMPED MIGRATION
// ==================================================================================================
// A stamp would be wrong here twice over, and PcGameRemap's header argues the first half already:
//   * THE MAPPING IS NOT A FIXED FACT, IT IS A FUNCTION OF A SETTING THE USER CAN CHANGE BACK. Prefer the
//     server, then prefer this device again, and the records must come home. A one-shot stamp delivers the
//     first move and strands the second.
//   * A REMOTE COPY'S TRACK LIST ARRIVES ASYNCHRONOUSLY, and often not until the user opens that album. A
//     migration that ran once, at the first merge, would find no destination for any album that had not been
//     fetched yet and would mark itself done anyway. Running on every merged rebuild instead migrates each
//     album the moment both its copies are known — which, conveniently, is also the moment the user could
//     first have accrued anything against either.
// So idempotence is the safety property and it is structural (self-maps skipped, source removed only after
// the destination is durable, sums replaced by a merge of two absolute records). Cheapness is separate: with
// nothing to move the Table is EMPTY and applyRemap does not so much as open the ini — which is what makes
// "an install with one music source is untouched" a fact about control flow rather than a claim.
#pragma once
#include <QHash>
#include <QString>
#include <QVector>
#include <functional>

namespace MusicRemap
{
    // One track of one copy of an album, named both ways. `number` is the track number as tagged (0 when
    // untagged) and `title` is its title; both exist only to match this track against the same track on
    // another supplier.
    struct TrackId
    {
        int     number = 0;
        QString title;
        QString playId;
        QString indexId;
    };

    // One copy of a merged album. `tracks` is EMPTY when that copy's track list has not been fetched — which
    // is not an error and not a reason to do anything but wait (rule 1).
    struct Instance
    {
        QString          key;
        QVector<TrackId> tracks;
    };

    // A merged album, its instances PRIMARY FIRST — the copy the preference picked, and therefore the copy
    // every record must end up under. That is exactly the order MusicMerge::Merged::albumGroup stores.
    struct AlbumGroup
    {
        QVector<Instance> instances;
    };

    // ONE map, because there is now one identity per track (see the header). Source identity -> destination
    // identity, applied to EVERY store this unit sweeps rather than to a nominated pair of them.
    struct Table
    {
        QHash<QString, QString> map;
        bool isEmpty() const { return map.isEmpty(); }
    };

    // PURE. No settings, no network, no clock, no store — the mapping is decided entirely away from the
    // records it will rewrite, which is what makes it probe-testable over a table of strings.
    //
    // Tracks are matched inside a merged album, never across one: first by track NUMBER when the number is
    // non-zero and unique on both sides, then by normalised TITLE for whatever is left, again only when the
    // normalised title is non-empty and unique on both sides. Anything ambiguous is omitted, and an identity
    // that two groups would send to two different destinations is REMOVED from the table rather than
    // arbitrated — a record that cannot be placed confidently keeps the key it has.
    //
    // Maps INDEX identities only. It reads `TrackId::playId` for nothing at all — that is streamKeyTable's
    // input, and the two producers are deliberately kept apart so a merge decision can never reach into the
    // credential-shaped half of a track's name.
    Table tableFor(const QVector<AlbumGroup>& groups);

    // THE #204 MIGRATION, and the whole of it: every track's pre-#204 PLAYBACK identity -> its durable one.
    // PURE for the same reason tableFor is; the impure half (what a stream url for this track looks like
    // right now) is MusicSupply::playUrl, called by the caller that already had to call it.
    //
    // `number` and `title` are ignored — a track is only ever mapped onto ITSELF here, so there is nothing to
    // match and nothing to be ambiguous about at the track level. What CAN still go wrong is handled by the
    // same `offer` rule tableFor uses, and each arm of it is the difference between a repair and a loss:
    //   * an EMPTY playId is never a source. MusicSupply::playUrl returns "" for a track whose server is no
    //     longer configured, and md5("") is a real key that real rows can sit under — treating it as a source
    //     would move some unrelated bucket onto a track at random.
    //   * an empty indexId is never a destination, for the mirror-image reason (#194's `md5("")` hazard).
    //   * a LOCAL track self-maps and is simply absent, so a library with no music server produces an EMPTY
    //     table and applyRemap never opens the ini. That is what "an install with nothing to migrate is
    //     untouched" means here, and it is a fact about control flow rather than a claim.
    //   * one url claimed by two different track ids is BANNED rather than arbitrated (rule 1).
    Table streamKeyTable(const QVector<TrackId>& tracks);

    // Move every record from each old identity to its new one, across EVERY profile (and, for the
    // device-namespaced accumulator, every device namespace) present in the ini — a record belongs to
    // whichever profile accrued it. Self-maps are skipped, missing records are skipped, a destination that
    // already holds a record is merged into. Safe to call on every merged rebuild; returns immediately on an
    // empty table without opening the store.
    void applyRemap(const Table& table);

    // applyRemap rewrites the ini underneath ConsumptionStats' lazily-built cache. Handed in once by the app
    // for the same reason PcGameRemap takes one: calling ConsumptionStats::invalidate() from here would drag
    // ProfileStore and Settings into every probe that links this unit. Unset in probes, which read back the
    // ini directly.
    void setCacheInvalidator(std::function<void()> fn);

#ifdef EB_MUSICID_TEST_SEAM
    // Test-only ini redirect, the same macro and the same rule as MusicId::setIniPathForTesting: without the
    // define the symbol does not exist, so a production call is a compile error rather than a silent
    // process-wide redirect.
    void setRemapIniPathForTesting(const QString& path);
#endif
}
