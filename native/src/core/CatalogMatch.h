#pragma once
#include <QString>
#include <QVector>
#include "AddonModels.h"
#include "LocalLibrary.h"

namespace CatalogMatch
{
    // Lowercase, non-alphanumeric → space, whitespace-collapsed, leading article (the/a/an) dropped.
    QString normalizeTitle(const QString& t);

    // Return the index into `candidates` of the accepted match, or -1. Strict:
    //  - if `want.imdbId` is set and a candidate's id equals it (case-insensitive) → that index (outright).
    //  - else the SINGLE candidate that is a movie and whose normalized title equals want's; -1 if none or >1.
    // (Search results carry no year, so same-title remakes are ambiguous → -1: conservative, never mis-badge.)
    int bestMatch(const LocalLibrary::VideoEntry& want, const QVector<MediaItem>& candidates);
}
