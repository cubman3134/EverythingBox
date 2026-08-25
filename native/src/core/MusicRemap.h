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
// TWO IDENTITIES PER TRACK, AND WHY THEY CANNOT SHARE ONE TABLE
// ==================================================================================================
// A track is named two different ways by two different families of store:
//   playId    what the PLAYER was handed — MusicSupply::playUrl(). A local path passes through unchanged;
//             a qualified track id becomes a signed stream url. PlaybackSession keys the resume record and
//             the consumption seconds by exactly this (its `resumePath_`).
//   indexId   what the INDEX calls the track — IndexTrack::path: the same local path, or the qualified,
//             credential-free track id. MainWindow's `syncKey_` is this, and SpeedStore and SyncOffsets key
//             on it.
// For a LOCAL track the two strings are identical. That is precisely why they must not be merged into one
// table: a local track path would then have two different destinations (the remote copy's stream url AND the
// remote copy's qualified id), and whichever won would send half the stores to a key nothing reads.
//
// A playId MAY BE A SIGNED URL, so it carries a credential in its query. It is never written anywhere: the
// stores hash it, and this unit only ever hands it to a hash. Do not log a Table, and do not persist one —
// if either is ever needed, it goes through CredentialScrub first (#200).
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

    struct Table
    {
        QHash<QString, QString> play;    // playId  -> the primary copy's playId
        QHash<QString, QString> index;   // indexId -> the primary copy's indexId
        bool isEmpty() const { return play.isEmpty() && index.isEmpty(); }
    };

    // PURE. No settings, no network, no clock, no store — the mapping is decided entirely away from the
    // records it will rewrite, which is what makes it probe-testable over a table of strings.
    //
    // Tracks are matched inside a merged album, never across one: first by track NUMBER when the number is
    // non-zero and unique on both sides, then by normalised TITLE for whatever is left, again only when the
    // normalised title is non-empty and unique on both sides. Anything ambiguous is omitted, and an identity
    // that two groups would send to two different destinations is REMOVED from the table rather than
    // arbitrated — a record that cannot be placed confidently keeps the key it has.
    Table tableFor(const QVector<AlbumGroup>& groups);

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
