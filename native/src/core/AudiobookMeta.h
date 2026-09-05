// WHAT A PROVIDER SAID ABOUT A SCANNED AUDIOBOOK, AND HOW LITTLE OF IT IS ALLOWED TO LAND (issue #198).
//
// #139 reads what is in the tags. Audiobook tagging in the wild is a folder name and a bitrate, so a real
// collection browses as a wall of untitled books with no narrator, no series place, no cover and no year.
// This file is the half of the fix that has no I/O in it: the shape of a MATCH, the rule that decides which
// of its fields may be written, the confidence a match is offered with, and the merge when two providers
// answer. Everything that touches a store, a network or a thread is somewhere else on purpose —
// AudiobookMatchStore (the record), AudiobookMetaAggregator (the fan-out), MainWindowAudiobookMeta (the
// sweep). What is left here is a function of its arguments, which is what lets the precedence rule below be
// pinned by a probe rather than by care.
//
// ---- LOCAL TAGS ALWAYS WIN. THIS IS THE WHOLE SAFETY OF THE FEATURE ---------------------------------------
//
// Enrichment FILLS BLANKS. It never overwrites a value the file itself carried, for any field, at any
// confidence, from any provider. A person who tagged their library did the work already and an enricher that
// "improves" on it is a corrupter with a network connection — and one whose damage is invisible, because the
// wrong value looks exactly like a right one on a shelf. So there is exactly one predicate, `tagCarries`,
// and every field goes through `fill` / `fillInt`. Not a policy that could be configured, not a precedence
// table: one function, one probe, one answer.
//
// A TAG THAT IS PRESENT AND EMPTY IS A BLANK, NOT A VALUE. `NARRATOR=""` and `NARRATOR="   "` are what a
// tagger writes when it stamps a field it has nothing to put in, and they are the single commonest shape in
// a bulk-converted collection. Treating them as "the file said so" would make the feature dormant on exactly
// the libraries it exists for, and would leave a whitespace narrator on the shelf that no re-scan could ever
// clear. So `tagCarries` trims first: a field is carried when it has a non-space character in it. The same
// reading the rest of the library already makes — AudiobookLibrary::effectiveNarrator falls through an empty
// NARRATOR to COMPOSER for this reason, and narratorKeyFor yields "" rather than a whitespace bucket.
// Numeric fields (year, series position, runtime) spell "blank" as 0, which is the spelling the scan already
// uses (`seriesIndex = 0` means untagged, stated in AudiobookLibrary.h).
//
// ---- THE POSITION COMES FROM THE MATCH, NEVER FROM THE TITLE ----------------------------------------------
//
// Audiobook series numbering is usually absent from the tags and present in the title string ("Book 3 of the
// ..."), which is the reason #198 asks for MATCHING rather than parsing. It would be one regex to pull a 3
// out of that sentence and it would be wrong constantly: "Book 3" in a title is as often a boxed-set volume,
// a part number, a publisher's series or somebody's re-numbering as it is the position this library sorts a
// series by. So `fromDetail` reads the position out of the provider's own labelled field and NOWHERE ELSE —
// it never looks at `MediaDetail::title` — and a match that carries no such field yields 0, which reads as
// "still untagged" and leaves the book exactly where the scan put it. probe_audiobooks pins that a match
// whose title says "Book 3" and whose facts do not still produces seriesIndex 0.
//
// ---- WHERE THE FIELDS COME FROM ---------------------------------------------------------------------------
//
// A provider addon answers getMeta() with a MediaDetail, which is the same reply the four game-artwork
// providers give — this feature adds no protocol. The audiobook-specific fields ride the labelled `facts`
// list an addon already publishes, under the labels `labelsFor` names. Reading a fact by LABEL rather than
// by position is what lets an addon written next year serve narrators without a change here.
//
// NOTHING AUDIBLE-SHAPED IS COMPILED INTO THIS APP, and that is a decision rather than an omission. Audible's
// catalogue is what the audiobook ecosystem leans on for narrator/series/ASIN, and it has NO official public
// API; every tool that reads it is on an unofficial endpoint that can change or be closed at any time. A
// source like that belongs behind an addon the user installs, where it is their choice and its failure is
// its own, or it belongs nowhere. The app therefore ships Open Library and Google Books — both official,
// both public, both keyless — and treats ANY addon declaring `metaFor: ["audiobook"]` as an equal provider.
// If you came here looking for the Audible call, that is why there is not one.
//
// ---- CONFIDENCE IS A NUMBER BECAUSE THE MATCH IS SHOWN ------------------------------------------------------
//
// Audiobook titles match badly: abridged against unabridged, dramatisations, re-recordings with a different
// narrator, and a dozen public-domain editions of the same classic. So a match is never silently authoritative
// — `confidenceFor` scores it, a match under `kAcceptThreshold` is not applied at all, and one over it is
// applied AND named on the book's own level so the user can see what was matched and reject it. The score is
// deliberately crude (title agreement, author agreement, a nudge for a narrator we did not have): a
// sophisticated score would still be wrong about abridgements, and a crude one that is SHOWN is honest.
#pragma once
#include "AudiobookLibrary.h"
#include "../addons/AddonModels.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace AudiobookMeta
{
    // ------------------------------------------------------------------------------------------------
    // The record: one book, one provider answer, everything the app is willing to remember about it.
    // ------------------------------------------------------------------------------------------------
    // EVERY FIELD IS EMPTY / ZERO WHEN THE PROVIDER DID NOT SAY. Nothing here is ever defaulted to a guess,
    // for the reason BookMeta::Info states: a fabricated blank is indistinguishable from a real value one
    // level up, and this record is persisted.
    struct Match
    {
        QString provider;      // manifest id of the addon that supplied it — shown, and the merge order key
        QString matchId;       // the provider's own id for the edition ("" == the provider named none)
        QString matchTitle;    // WHAT WAS MATCHED, verbatim: the sentence the user is shown before rejecting
        QString matchAuthor;   // ...and by whom, same purpose

        QString narrator;      // the field that earns the feature
        QString series;
        int     seriesIndex = 0;   // from the provider's own position field; NEVER parsed out of a title
        QString description;
        QString coverUrl;
        int     year = 0;
        int     runtimeSec = 0;

        int     confidence = 0;    // 0..100, confidenceFor()'s score for the book it was matched to
        bool    rejected = false;  // the user said no. Permanent; see AudiobookMatchStore.h
        qint64  updatedAt = 0;     // ms epoch, stamped by the store

        // Nothing worth applying and nothing worth showing. `rejected` is deliberately NOT part of this: a
        // rejection with no fields left in it is still a record the sweep must respect.
        bool isEmpty() const;
        // Something a shelf could actually gain from this match.
        bool hasFields() const;
    };

    // ---- pure: canonical record <-> JSON ---------------------------------------------------------------
    // Omit-empty / omit-zero, so a record has ONE spelling on disk and two devices that matched the same
    // book store byte-identical bytes.
    Match       fromJson(const QJsonObject& o);
    QJsonObject toJson(const Match& m);

    // ---- pure: reading a provider's reply ---------------------------------------------------------------
    // The fact labels this reads, per field, lowercased and compared case-insensitively. Exposed because the
    // probe asserts on them and because an addon author needs to be able to look them up.
    QStringList labelsFor(const QString& field);   // "narrator" / "series" / "position" / "year" / "runtime"

    // A provider's getMeta() reply -> a Match. `providerId` is the manifest id, recorded as the source.
    // Reads title/overview/imageUrl/art for the shared fields and the LABELLED FACTS for the audiobook ones;
    // never reads the title for a series position (see the header). confidence is left at 0 — only
    // confidenceFor, which can see the scanned book, may set it.
    Match fromDetail(const MediaDetail& d, const QString& providerId);

    // Value parsers, exposed for the probe. Each yields 0 for anything it cannot read, never a partial guess.
    int parseYear(const QString& v);          // "1998", "1998-04-02", "April 1998" -> 1998; 0 otherwise
    int parseSeriesIndex(const QString& v);   // "3", "3.5" -> 3, "#3" -> 3, "Book 3" -> 3; 0 otherwise
    int parseRuntimeSec(const QString& v);    // "14h 20m", "860 min", "14:20:00", "51600" -> seconds; 0 else

    // ---- pure: two providers answered -------------------------------------------------------------------
    // Fixed merge order, the GameMetaAggregator idiom: the provider best-in-class for the most fields first,
    // the rest backfilling. Open Library carries edition-level contributions (which is where a narrator is),
    // Google Books carries the better descriptions; an addon the user installed sorts AFTER both but still
    // contributes — the app has no opinion about a source it did not ship, only about the two it did.
    int providerPriority(const QString& manifestId);

    // Merge `lo` in UNDER `hi`: every field `hi` already has is kept, `lo` fills the rest. Field-by-field,
    // so a match is a best-of rather than one provider's whole answer — the same shape MediaArt's
    // mergeLowerPriority has, and the reason the aggregator sorts by providerPriority before folding.
    // `matchId`/`matchTitle`/`provider` follow `hi` when `hi` named one, so "what was matched" always names
    // the record's leading provider rather than a field donor.
    Match mergeLowerPriority(const Match& hi, const Match& lo);

    // ---- pure: is this the same book? --------------------------------------------------------------------
    // 0..100. Title agreement dominates; the author agrees or it does not; a narrator we did not have is a
    // small nudge rather than evidence. A match with no title scores 0 — a provider that cannot name what it
    // found has not found anything.
    int confidenceFor(const AudiobookLibrary::Book& scanned, const Match& m);

    // Below this, a match is NOT applied and NOT stored: the sweep behaves as though the providers said
    // nothing, which is the "an item with no match is left exactly as the scan found it" rule.
    inline constexpr int kAcceptThreshold = 55;

    // Comparison spelling for a title/author: case-folded, punctuation dropped, whitespace collapsed, and a
    // leading article removed. Exposed because the probe pins it — "The Hobbit" and "hobbit, the" are the
    // same book and a scorer that says otherwise makes the feature dormant on half a library.
    QString normalizedName(const QString& s);

    // ---- THE PRECEDENCE RULE ------------------------------------------------------------------------------
    // The three functions the whole safety of this feature is: a file's value is kept, a blank is filled.
    bool    tagCarries(const QString& tagged);                        // trimmed non-empty
    QString fill(const QString& tagged, const QString& enriched);     // tagged wins; blank takes enriched
    int     fillInt(int tagged, int enriched);                        // non-zero wins; 0 takes enriched

    // Fill blanks on a SCAN'S ENTRIES from the per-book matches, keyed by AudiobookLibrary::bookKeyFor.
    // Returns how many entries were changed.
    //
    // ON THE ENTRIES AND NOT ON THE INDEX, because the narrator and series BUCKETS are derived from the
    // entries by buildIndex: filling a Book's narrator field after the fact would put the name on the tile
    // and leave the Narrators view empty, which is the one thing #198 asks for by name. Applying here and
    // rebuilding means an enriched book is filed under its narrator by exactly the code that files a tagged
    // one, so there is no second answer to "which bucket is this in".
    //
    // AND ON A COPY, NEVER ON WHAT IS PERSISTED. The index FILE holds what the tags said and nothing else:
    // an enriched value written into audiobookindex.json would, on the next scan, be indistinguishable from
    // a tag — it would out-rank a corrected match by this very precedence rule, and a rejection could never
    // revert it. The caller keeps the scanned entries and re-derives; see MainWindowAudiobookMeta.cpp.
    // A REJECTED match contributes nothing, at any confidence.
    int applyToEntries(QVector<AudiobookLibrary::FileEntry>& entries,
                       const QHash<QString, Match>& byBookKey);

    // One line naming what was matched, for the row on the book's own level and the editor's reject prompt.
    // Empty for a record with nothing in it. NOT translated here — the callers wrap it; this is the join.
    QString matchSummary(const Match& m);

    // Is there anything left for a provider to fill in on this book? A book whose tags already carry a
    // narrator, a series place, a year and a length is not worth a network call, and the sweep skips it.
    bool wantsEnrichment(const AudiobookLibrary::Book& b);
}
