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
