#pragma once
#include <QString>
#include <QVector>
#include "AddonModels.h"
#include "LocalLibrary.h"

namespace CatalogMatch
{
    // Lowercase, non-alphanumeric → space, whitespace-collapsed, leading article (the/a/an) dropped.
    QString normalizeTitle(const QString& t);

    // Is this search RESULT plausibly the thing that was asked for? The two titles come from different
    // databases, so they are compared normalized and by whole tokens, and one is allowed to be longer than the
    // other — a provider's "Hemingway: A Life Without Consequences" is the catalog's "Hemingway", and an
    // "(Unabridged)" suffix is not a different book.
    //
    // FAILS CLOSED: an empty title on either side, or no containment either way, is a NO. The caller's
    // alternative to rejecting is opening something the user did not choose and recording their progress
    // against it, which is worse than saying it could not be found.
    bool titleMatchesRequest(const QString& wantTitle, const QString& candidateTitle);

    // ---- ROM dump names: GAMES ONLY ----------------------------------------------------------------
    // A ROM dump is filed under a naming convention no other shelf uses:
    //
    //     Legend of Zelda, The - A Link to the Past (USA) (Rev 1) [!].7z
    //
    // — the leading article moved to the END after a comma, " - " where a catalog writes ":", parenthesised
    // region/revision/language tags, bracketed dump flags, and an archive or ROM extension. The catalog calls
    // the same game "The Legend of Zelda: A Link to the Past", so titleMatchesRequest compared two spellings
    // of one game as plain text and threw the ROM away — which is how pressing Romhacks fetched the patch and
    // then downloaded no base ROM at all.
    //
    // normalizeRomTitle undoes those four devices and then hands the result to normalizeTitle.
    //
    // WHY THIS IS A SEPARATE FUNCTION AND NOT A LOOSER normalizeTitle. Every rule here is WRONG for a book:
    // "(Unabridged)" and "(Illustrated)" name real, different editions; a book title may genuinely end in a
    // comma and an article; "Dune" is not "Dune.epub". titleMatchesRequest is shared with books, comics and
    // audiobooks through the same doc-bridge and is left byte-for-byte unchanged — the game catalog is routed
    // to gameTitleMatchesRequest instead, at the one call site that knows the catalog type.
    QString normalizeRomTitle(const QString& t);

    // titleMatchesRequest for a GAME. Same shape — normalize, compare by whole tokens, either side may be the
    // longer one — over normalizeRomTitle instead, plus ONE extra refusal the book rule does not need:
    //
    //   a sequel marker immediately after the matched run is a DIFFERENT GAME.
    //
    // "Zelda" would otherwise be contained in "Zelda II - The Adventure of Link", and "Mega Man" in
    // "Mega Man 2". That is not a cosmetic near-miss: a patch applies to ANY bytes at all, so a wrong base ROM
    // is not refused anywhere downstream — it is written out as a playable-looking file that is silently
    // corrupt. Refusing costs the user a "no copies found"; accepting costs them a broken game with no error.
    bool gameTitleMatchesRequest(const QString& wantTitle, const QString& candidateTitle);

    // ---- format, asked of the same candidate the title rule just accepted (#207) ---------------------
    //
    // A book and its audiobook are the same work in two formats, filed on two shelves, and a title search of
    // one shelf answers with releases of both. "The Poppy War by R. F. Kuang EPUB" is word-for-word the book
    // that was asked for and is not a thing anyone can listen to.

    // What a search of `catalogType` is asking for. `Any` is not "don't care" — it is "the question is
    // meaningless here": a ROM search has no ebook/audio axis, and answering it would be inventing a rule.
    enum class WantedFormat { Any, Audio, Text };
    WantedFormat catalogWantsFormat(const QString& catalogType);

    // The formats a release NAMES, in the part of its title that is not the work's own name. `wantTitle` is
    // cut out first because a format word inside the title is part of the work — "The PDF Handbook" is a book
    // about PDFs, not a PDF of something, and only the words a release adds AROUND the title describe its file.
    //
    // Matched on whole normalized tokens, so "PDF" inside a longer word is not a tag. Both flags can be set
    // (a pack that carries the ebook and the audiobook), and neither being set is the ordinary case: most
    // releases say nothing about format at all.
    struct ReleaseFormats { bool audio = false; bool text = false; };
    ReleaseFormats releaseFormats(const QString& wantTitle, const QString& candidateTitle);

    // Is this release a valid answer to a `want` request? Refused ONLY when it names the opposite format and
    // not the wanted one. SILENCE IS NOT A MISMATCH — a rule that demanded a positive audio signal would
    // reject nearly every audiobook release there is, which is a worse bug than the one it fixes.
    bool formatMatchesRequest(WantedFormat want, const QString& wantTitle, const QString& candidateTitle);

    // What a RESOLVED payload plainly is, judged on its url and mime alone — nothing is fetched and nothing is
    // sniffed, so this is only ever the honest, cheap answer. `Unknown` is the common one and means "no reason
    // to think it isn't what was asked for"; the caller must treat it as playable, exactly as before.
    //
    // A debrid link often carries no filename at all: the "zip the whole release" endpoint is a path verb
    // (…/zip/<id>) with no extension and no mime, which is what an audiobook request resolved to in #207.
    enum class PayloadShape { Unknown, Audio, Document, Archive };
    PayloadShape payloadShape(const QString& url, const QString& mime);

    // One copy already on this machine, as the Downloads/Recent stores record it. Deliberately the four
    // fields both stores share, so ONE rule serves both rather than two that drift.
    struct LocalCopy
    {
        QString path;
        QString title;
        QString kind;   // the STORE's kind ("document"/"audio"/...), not the catalog's type
        QString key;    // the addon item id this copy was saved under; may be empty
    };

    // The copy of `wantTitle` we already hold, or an empty string. Asking a provider to find a book again —
    // and waiting on the network for it — when the file is already on disk is pure delay, and it reads as a
    // re-download to the person watching "Finding …" sit there.
    //
    // Matched on the saved KEY first, which is the id the copy was recorded under and therefore exact. Title
    // is the fallback for a copy saved before an id was known, and then only within the SAME kind and only on
    // an exact normalized match: a book and an audiobook of one work are different things, and a loose title
    // rule here opens the wrong file with no network step left to notice.
    //
    // Purely a decision over what it is given: whether the path still EXISTS is the caller's to check, so this
    // stays testable without a filesystem.
    QString localCopyFor(const QString& wantId, const QString& wantTitle, const QString& wantKind,
                         const QVector<LocalCopy>& have);

    // The chapter/issue number the user asked for, taken from the trailing number of a doc-bridge query.
    // The caller builds the query as "<parentTitle> <issueNumber>", so the requested number is at the very
    // end ("Doubutsu Ningen 1" → "1", "One Piece 1052.5" → "1052.5"). Returns "" when the query carries no
    // trailing number ("Berserk" → ""), which the drill reads as "open the lowest-numbered chapter".
    QString requestedChapterNumber(const QString& query);

    // Does a chapter item titled `itemTitle` (e.g. "Chapter 1", "Ch. 12", "Vol.2 Chapter 1.0") carry the
    // requested number `want`? The item's first embedded number is parsed and compared to `want` NUMERICALLY,
    // so "1" == "1.0" and "12" != "1". False when either side has no parseable number.
    bool chapterNumberMatches(const QString& itemTitle, const QString& want);

    // The sibling document catalog to fall back to when a title is not in the catalog it was filed under, or
    // an empty string when there is none. Manga and Western comics are both readable .cbz documents served by
    // DIFFERENT providers (MangaDex vs GetComics), and the client can only guess which shelf a title lives on
    // from its type — so a manga classified as a comic_issue searches only the Comics catalog and is never
    // found. The two are the only siblings: "comic" ↔ "manga". book/audiobook/game/etc. have no sibling.
    QString docCatalogSibling(const QString& type);

    // Return the index into `candidates` of the accepted match, or -1. Strict:
    //  - if `want.imdbId` is set and a candidate's id equals it (case-insensitive) → that index (outright).
    //  - else the SINGLE candidate that is a movie and whose normalized title equals want's; -1 if none or >1.
    // (Search results carry no year, so same-title remakes are ambiguous → -1: conservative, never mis-badge.)
    int bestMatch(const LocalLibrary::VideoEntry& want, const QVector<MediaItem>& candidates);

    // Index of the accepted SERIES candidate, or -1. Strict, series-only:
    //  - if `seriesImdbId` is set and a candidate's id equals it (case-insensitive) → that index (outright).
    //  - else the SINGLE candidate whose type is series/tv and whose normalized title equals `showTitle`'s;
    //    a "tt…" candidate that contradicts a non-empty `seriesImdbId` is skipped; -1 if none or >1.
    int bestSeriesMatch(const QString& showTitle, const QString& seriesImdbId, const QVector<MediaItem>& candidates);
}
