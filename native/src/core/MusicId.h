// CROSS-SOURCE MUSIC IDENTITY (issue #194, increment 1) — is the artist on my disk the same artist as the
// one on my Navidrome box, and is that album the same album? Pure, QtCore-only, no UI, no network, no
// MusicLibrary types: everything here takes strings and numbers and answers a question about them, for the
// same reason Scrobble.h and Subsonic.h do, so probe_musicid can drive every arm with no library and no
// server.
//
// ==================================================================================================
// WHAT THIS IS *NOT*, AND WHY THE DISTINCTION IS LOAD-BEARING
// ==================================================================================================
// Subsonic.h already solved a DIFFERENT identity problem one level down: two servers hand out the same
// opaque id constantly, so every remote id is server-qualified before it leaves that file. That makes an id
// unambiguous WITHIN a source. This file sits ABOVE it and never replaces it — an id it is handed is still
// whatever its source minted, and every key that comes out of a merge is one of those same per-source keys.
// Nothing here invents a new namespace, and nothing here is allowed to make a per-source key ambiguous.
//
// ==================================================================================================
// THE RULE THAT DECIDES EVERY JUDGEMENT CALL: A WRONG MERGE HIDES MUSIC
// ==================================================================================================
// A wrong merge makes one of two records unreachable — the user owns an album and the app will not show it
// to them, with nothing on screen to say why. A missed merge shows the same record twice, which is untidy
// and completely recoverable by looking at it. Those costs are not symmetric, so every rule below leans the
// same way, and the lean is expressed in the TYPE rather than hidden in a threshold:
//
//     Confidence { Certain, Likely, TooLowToMerge }      merges(c) == (c == Certain || c == Likely)
//
// The confidence functions are written as a chain of arms that each RETURN a verdict, and the fall-through
// at the bottom of every one of them is TooLowToMerge. So a case nobody thought of — a new tag shape, an
// empty field, a source that answers something unexpected — does not merge. Adding an arm is how a merge
// becomes possible; forgetting one can only ever cost a duplicate. Do not restructure these into a score
// with a cut-off: a score's default is "whatever the arithmetic happened to produce", which is exactly the
// property this shape exists to deny.
//
// ==================================================================================================
// MUSICBRAINZ IDS ARE THE GROUND TRUTH, AND THEY CUT BOTH WAYS
// ==================================================================================================
// PcGameId has igdb; music has MBIDs, and well-tagged libraries carry them (Picard writes them, Navidrome
// serves them). Where BOTH sides carry a comparable id, the id decides and the string matcher is not
// consulted at all — including the NEGATIVE direction: two different MBIDs are two different records even
// when the titles agree letter for letter. That is the same rule sameGame() applies to igdb ids, and it is
// the only thing that can separate a live album from the studio album it shares a name with.
//
// AN ALBUM HAS *TWO* MBIDS AND THEY ARE NOT INTERCHANGEABLE. The RELEASE id names one physical/digital
// release — a deluxe reissue has its own — while the RELEASE GROUP id names "the album" across all of them.
// Comparing a release id against a release-group id is meaningless, so groundAlbum() only ever compares
// like with like: release-group to release-group when both sides have one, otherwise release to release,
// otherwise it stays SILENT and the string matcher decides. A source that carries only one of the two (a
// Subsonic server's `musicBrainzId` on an album is the RELEASE) can still be matched against a Picard
// library, because that library carries both.
//
// ==================================================================================================
// THE NORMALISERS, AND THE TRAPS THEY ARE SHAPED BY
// ==================================================================================================
// Music tags are messier than game titles, and this repo has already been bitten once by a title matcher
// that computed a relevance key and then discarded it — a Final Fantasy V query returned IV and VI. The
// lessons that came out of that are applied here on purpose: Roman-numeral levelling on BOTH sides, a hard
// guard on a trailing digit, and whole-TOKEN comparison rather than substring containment (equality of two
// normalised strings, so a prefix can never bleed into a match).
//
// See normalizeArtist / normalizeAlbum for the step lists. The three that are most easily got wrong:
//
//   * EDITION NOISE IS DROPPED ONLY WHEN A *STRONG* NOISE WORD IS PRESENT. "(Deluxe Edition)",
//     "[2009 Remaster]" and "- Bonus Track Version" go; "(Live)", "(Mono)", "(Acoustic)", "(Demo)" and a
//     bare "(2009)" all STAY, because each of them can be the entire difference between two records. A
//     live album that shares a studio album's name is the case the issue names, and keeping "live" in the
//     key is half of what stops that merge (the year gate is the other half).
//   * A NUMBER IS NEVER STRIPPED. "Volume 2" cannot collapse onto "Volume 1", ever. The edition strip is a
//     whole-phrase rule anchored to the end, never "drop the trailing token".
//   * A ROMAN NUMERAL IS LEVELLED ONLY WHERE IT IS ONE. `Pt. II` -> `pt 2` and `Vol. V` -> `vol 5` because
//     the previous token is a sequel word; a TRAILING numeral levels too ("Led Zeppelin IV"), but only at
//     length >= 2 and only for a value in 1..30. That cap is not arbitrary tidiness: MIX is a valid Roman
//     numeral (1009), and so are CD (400), DC (600), MC (1100), MI (1001) and XL (40). Every real English
//     word that collides with a Roman numeral does so at a value no album sequel ever reaches, so one
//     bound removes the whole family. A single-token title is never levelled at all, which keeps the album
//     literally named "I" away from the album literally named "1".
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

namespace MusicId
{
    // ---- The normalisers --------------------------------------------------------------------------------

    // The ARTIST matching key. Case-folded, diacritics removed, "&" read as "and", a featured-artist tail
    // ("feat." / "ft." / "featuring" and everything after it) dropped, punctuation to space, a leading
    // definite/indefinite article dropped, whitespace collapsed.
    //
    // NUMERALS ARE UNTOUCHED HERE — no Roman levelling, no digit rule. Blink-182, Sum 41, Maroon 5 and
    // Matchbox 20 are names, not sequels, and there is no artist whose two spellings differ by a numeral
    // system. Levelling would only ever create a collision.
    //
    // The article strip refuses to empty the string: an artist called "The" keeps its one token rather than
    // normalising to nothing and bucketing with every other unnameable act.
    QString normalizeArtist(const QString& raw);

    // The ALBUM matching key. Trademark marks and typographic apostrophes folded, case-folded, diacritics
    // removed, "&" read as "and", EDITION NOISE dropped (bracketed groups and trailing segments, both only
    // when a strong noise word is present — see the header), punctuation to space, sequel words canonicalised
    // (volume -> vol, part -> pt, number -> no), Roman numerals levelled where they are sequel positions,
    // whitespace collapsed.
    //
    // NO LEADING-ARTICLE STRIP, deliberately — unlike the artist key. "The Wall" and "Wall" are two things a
    // person could plausibly own, tag variance on an album's article is rare, and the strip could only ever
    // ADD merges. The asymmetry is the bias doing its job, not an oversight.
    QString normalizeAlbum(const QString& raw);

    // Roman numeral -> value, or 0 for anything that is not one. Strict (subtractive form only), so "iiii"
    // and "vx" are not numerals. Exposed because the probe pins it directly: the value cap is what keeps
    // "mix" and "cd" out of the levelling, and a rule tested only through its caller is a rule that drifts.
    int romanValue(const QString& token);

    // ---- MusicBrainz ground truth -----------------------------------------------------------------------

    // An album's two MusicBrainz ids. Either or both may be empty; see the header for why they are never
    // compared against each other.
    struct AlbumMbid
    {
        QString releaseGroup;   // MUSICBRAINZ_RELEASEGROUPID — "the album", across every reissue of it
        QString release;        // MUSICBRAINZ_ALBUMID — one release. A Subsonic album's musicBrainzId.
        bool isEmpty() const { return releaseGroup.isEmpty() && release.isEmpty(); }
    };

    // What the ids say. SILENT is not "different" — it means the ids cannot answer, and the caller must fall
    // through to the string matcher. Collapsing the two is how a library with no MBIDs would stop merging
    // entirely, and how a library with them on one side only would refuse every match.
    enum class Ground { Same, Different, Silent };

    Ground groundArtist(const QString& mbidA, const QString& mbidB);
    Ground groundAlbum(const AlbumMbid& a, const AlbumMbid& b);

    // ---- Confidence -------------------------------------------------------------------------------------

    // See the header. There is no fourth value and no numeric score on purpose.
    enum class Confidence { Certain, Likely, TooLowToMerge };

    // THE ONE PLACE THE BIAS IS SPENT. Everything that decides whether two records become one row asks this
    // and nothing else, so widening what merges is a one-line, reviewable change rather than a threshold
    // buried in a caller.
    inline bool merges(Confidence c) { return c == Confidence::Certain || c == Confidence::Likely; }

    // Everything known about one instance of an album, from whichever source it came from. Raw strings: the
    // normalisers are applied inside, exactly once, for the reason PcGameId.h states at length (normalizeAlbum
    // is not idempotent — the edition strip runs before punctuation becomes space, so "Album (Remastered)"
    // is not a fixed point and a second pass would build a key nothing ever wrote).
    struct AlbumFacts
    {
        QString   albumArtist;    // raw, as the source spells it
        QString   title;          // raw
        QString   artistMbid;     // the ALBUM ARTIST's MBID, when the source carries one
        AlbumMbid mbid;
        int       year = 0;           // 0 == unknown
        int       trackCount = 0;     // 0 == unknown; a TIEBREAK, never a gate (see albumConfidence)
        int       durationSec = 0;    // 0 == unknown; the second tiebreak
    };

    // Two artists. Raw names.
    //   * a user verdict about the pair wins outright;
    //   * then the MBIDs, both ways;
    //   * then equality of the normalised names — and an EMPTY normalised name never matches anything,
    //     including another empty one, because "the untagged bucket" on two sources is two different piles
    //     of files and fusing them is the wrong-merge failure at its largest scale.
    Confidence artistConfidence(const QString& nameA, const QString& mbidA,
                                const QString& nameB, const QString& mbidB);

    // Two albums. Raw facts.
    //   * a user verdict about the pair wins outright;
    //   * then the MBIDs, both ways;
    //   * then ALL THREE of: the artists match (by the rule above), the normalised titles are equal, and the
    //     years are compatible.
    //
    // YEAR IS A GATE, TRACK COUNT IS NOT. They are different kinds of evidence. A year that disagrees by more
    // than one is positive evidence AGAINST — it is what separates a 1983 live record from the 1980 studio
    // album it is named after, which nothing in the title can do — so it refuses. A track count that
    // disagrees is not evidence against anything: the commonest reason for it is that one source has the
    // bonus disc, which is the same album and the user wants it merged. Track count therefore only ever
    // breaks a TIE between several candidates (see closeness), which is the role the issue gives it.
    //
    // An unknown year (0) on either side is compatible with everything: absence is not disagreement.
    Confidence albumConfidence(const AlbumFacts& a, const AlbumFacts& b);

    inline bool sameArtist(const QString& nameA, const QString& mbidA,
                           const QString& nameB, const QString& mbidB)
    { return merges(artistConfidence(nameA, mbidA, nameB, mbidB)); }

    inline bool sameAlbum(const AlbumFacts& a, const AlbumFacts& b)
    { return merges(albumConfidence(a, b)); }

    // How far apart two albums are once they are ALREADY known to match — the tiebreak, lower is closer.
    // Track-count difference first, then duration difference. Used when one source offers several instances
    // that all match the same record (a standard and a deluxe pressing on the same server, both normalising
    // to the same key): the closest one wins the pairing and the others stay separate rows rather than being
    // silently swallowed. Never consulted to DECIDE a match.
    int closeness(const AlbumFacts& a, const AlbumFacts& b);

    // ---- The manual override ----------------------------------------------------------------------------
    //
    // The escape hatch that makes a fuzzy matcher shippable, in both directions: "these are the same album"
    // and "these are not". Keyed on the pair of normalised keys, SORTED, so the verdict is symmetric by
    // construction rather than by a second lookup somebody can forget to write — PcGameRemap's shape.
    //
    // THE PAIR KEY IS BUILT FROM RAW STRINGS AND NORMALISED EXACTLY ONCE, HERE. Never hand these a key you
    // have already normalised: normalizeAlbum is not idempotent, so a second pass builds a key nothing ever
    // wrote and the read or the removal silently misses. The …Keys() variants below take STORED keys verbatim
    // and normalise nothing; they are what an "undo my corrections" surface, which walks overrides(), calls.

    // The stored key for one album instance: normalised artist and title joined by '!'. Both halves have had
    // every non-alphanumeric character removed, so '!' cannot occur inside either and the split back out is
    // exact. Empty when either half normalises to nothing — an unkeyable album takes no verdict.
    QString albumKeyOf(const QString& rawArtist, const QString& rawTitle);

    void setAlbumOverride(const QString& artistA, const QString& titleA,
                          const QString& artistB, const QString& titleB, bool same);
    // Forget a verdict — NOT the same as recording "not the same". A negative is the user separating a wrong
    // merge and has to keep beating the matcher for ever; clearing hands the decision back to it. Two states,
    // two calls.
    void clearAlbumOverride(const QString& artistA, const QString& titleA,
                            const QString& artistB, const QString& titleB);
    // -1 nothing said, 0 "not the same", 1 "the same".
    int  albumOverrideVerdict(const QString& artistA, const QString& titleA,
                              const QString& artistB, const QString& titleB);

    void setArtistOverride(const QString& nameA, const QString& nameB, bool same);
    void clearArtistOverride(const QString& nameA, const QString& nameB);
    int  artistOverrideVerdict(const QString& nameA, const QString& nameB);

    // One stored verdict, both sides already in their stored (normalised) form.
    struct Verdict { QString a; QString b; bool same = false; };
    QVector<Verdict> albumOverrides();
    QVector<Verdict> artistOverrides();
    void clearAlbumOverrideKeys(const QString& keyA, const QString& keyB);
    void clearArtistOverrideKeys(const QString& keyA, const QString& keyB);

    // ---- Which copy plays -------------------------------------------------------------------------------
    //
    // The preference is ONE stored string so that "prefer a specific server" needs no second setting and no
    // second migration:
    //     "local"          the copy on this disk, when there is one          (the default)
    //     "server"         any music server, in the order they were added
    //     "<a server id>"  that server specifically
    // Anything unrecognised reads as "local", which is the answer that works with no network.
    inline const char* kPreferLocal  = "local";
    inline const char* kPreferServer = "server";

    // One instance of a merged record: where it came from. An empty serverId means the LOCAL library.
    struct SourceRef
    {
        QString serverId;         // "" == local
        bool    available = true; // a source that cannot answer right now must never be picked to play
    };

    // Which instance should play, as an index into `all`. Never -1: a merged row has to render under exactly
    // one key, and "ask the user" is not available at the moment a browse level is built. (PcGameId's picker
    // CAN answer -1 because there a Play press starts a download; here every instance is already playable,
    // so the honest failure mode is a preference that was not met, not a stall.)
    //
    // Total and deterministic: the preference first, then local before remote, then the order the caller
    // supplied — which is the order the servers were added. Two identical libraries therefore pick the same
    // copy, which is what stops a merged row's identity flapping between two refreshes. -1 only for an empty
    // list.
    int pickAutoSource(const QVector<SourceRef>& all, const QString& preference);

#ifdef EB_MUSICID_TEST_SEAM
    // Test-only ini redirect, declared and compiled ONLY for probe_musicid (the ProfilePasscode /
    // PcGameId rule): without the macro this does not exist, so a production call is a compile error rather
    // than a silent process-wide redirect. The override store PERSISTS, so a probe that wrote into the app's
    // real ini would leave state the next run reads.
    void setIniPathForTesting(const QString& path);
#endif
}
