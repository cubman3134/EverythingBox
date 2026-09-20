// Content-FORMAT preference at scan time (issue #190, item 2). A retro COMPUTER title is routinely present
// in several interchangeable formats: the same Amiga game as a WHDLoad "Game.lha" and as a raw "Game.adf",
// the same C64 game as "Game.d64" and "Game.t64". Both are the same title, and listing both is the "one game,
// three tiles" mess #190 exists to end — worse than the console case, because here the formats are not equal:
// one of them boots with no setup and the others want a model, a disk swap or a three-minute tape load.
//
// This header is the PURE, testable heart: given a flat list of file names/paths and an ORDERED format
// ranking it decides which files are the same title in different formats, picks the best-ranked one, and
// hands the rest back as ALTERNATES of that entry. No disk I/O, no clock, no ini — QtCore only. The
// library-scan glue (RomLibrary::scan) does the I/O and the hiding, exactly as it does for #49's disc sets
// and #50's region duplicates.
//
// WHERE THE RANKING COMES FROM: the system's launch recipe (`"formats": [...]` in
// native/systems/recipes/<system>.json), so the folklore "a WHDLoad .lha beats a raw .adf" is DATA a user can
// re-order, not a C++ table. This header never reads a recipe — it takes the list — which is what lets
// probe_regioncollapse drive every rule against literals.
//
// THE RULES, and the reason each one is the way it is:
//   * "same title" is DiscGroup::normalizedKey — the SAME normalisation the disc grouping and the region
//     collapse use (and GamelistStore::cleanTitle before them). A second definition of "same title" in this
//     codebase would eventually disagree with the first one, and the disagreement would show up as a game
//     that collapses on one pass and splits on the other.
//   * a format NOT in the ranking keeps today's behaviour exactly: it is its own entry, never a winner's
//     alternate and never hidden. An empty ranking (every console, and any computer whose recipe says
//     nothing) therefore passes the whole list through unchanged — that is the safety property, and it is
//     what makes this pass free to run for every system.
//   * only files in DIFFERENT formats collapse. Two files of the SAME listed format that normalise to the
//     same title differ by region/revision, not by format, and that is #50's decision to make (it is
//     off by default and asks the user's language); so the runner-up of the winner's own extension stays its
//     own entry rather than being swallowed here.
#pragma once
#include "DiscGroup.h"

#include <QFileInfo>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <algorithm>

namespace FormatCollapse
{
    // One surviving library entry: the file to list, its readable title, the format it is in, and the
    // same-title files in OTHER listed formats that it stands for. `alternates` is empty for every entry
    // that collapsed nothing, which is every entry on a system with no ranking.
    struct FormatGroup
    {
        QString          chosenPath;
        QString          chosenTitle;    // DiscGroup::titleWithoutDiscTag(chosenPath)
        QString          chosenFormat;   // lowercase extension of chosenPath ("lha")
        QVector<QString> alternates;     // the other-format files, best-ranked first
    };

    // A path's lowercase extension, with no dot ("C:/roms/amiga/Game.LHA" -> "lha").
    inline QString formatOf(const QString& nameOrPath)
    {
        return QFileInfo(nameOrPath).suffix().toLower();
    }

    // The rank of a path's format against an ordered ranking: its 0-based index (lower = more preferred), or
    // -1 when the format is not listed at all. -1 is deliberately NOT "worst rank": an unlisted format is not
    // a worse variant of the same game, it is a file this feature has no opinion about, and it keeps its own
    // entry.
    inline int formatRank(const QString& nameOrPath, const QStringList& ranking)
    {
        return int(ranking.indexOf(formatOf(nameOrPath)));
    }

    // Collapse same-title files in different listed formats into one entry each. See the header comment for
    // the rules. Deterministic: ordered by chosenTitle then chosenPath, alternates by (rank, path), so a
    // re-scan yields identical output.
    inline QVector<FormatGroup> collapseByFormat(const QVector<QString>& paths, const QStringList& ranking)
    {
        auto lone = [](const QString& p) {
            FormatGroup g;
            g.chosenPath   = p;
            g.chosenTitle  = DiscGroup::titleWithoutDiscTag(p);
            g.chosenFormat = formatOf(p);
            return g;
        };

        QVector<FormatGroup> out;
        out.reserve(paths.size());

        // No ranking: nothing to prefer, so every file is its own entry and the caller's list is unchanged.
        // Stated as its own branch rather than left to fall out of the loops below, because "a system with
        // no formats list scans exactly as it did before #190" is the property this pass is safe under.
        if (ranking.isEmpty())
        {
            for (const QString& p : paths) out.push_back(lone(p));
            return out;
        }

        QHash<QString, QVector<QString>> byKey;  // normalised title key -> the ranked files under it
        QVector<QString>                 order;  // keys in first-seen order (stable before the final sort)
        for (const QString& p : paths)
        {
            if (formatRank(p, ranking) < 0) { out.push_back(lone(p)); continue; }  // unlisted: its own entry
            const QString key = DiscGroup::normalizedKey(p);
            // A name that normalises to nothing (all punctuation) cannot be matched to anything meaningfully;
            // treat it as its own entry rather than collapsing every such oddity onto one another.
            if (key.isEmpty()) { out.push_back(lone(p)); continue; }
            if (!byKey.contains(key)) order.push_back(key);
            byKey[key].push_back(p);
        }

        for (const QString& key : order)
        {
            QVector<QString> variants = byKey.value(key);
            std::sort(variants.begin(), variants.end(), [&ranking](const QString& a, const QString& b) {
                const int ra = formatRank(a, ranking), rb = formatRank(b, ranking);
                if (ra != rb) return ra < rb;   // the better-ranked FORMAT wins the entry
                return a < b;                   // stable, deterministic tie-break within one format
            });

            FormatGroup g = lone(variants.front());
            for (int i = 1; i < variants.size(); ++i)
            {
                // Same format as the winner => a region/revision sibling, not another format. Left alone as
                // its own entry, for #50 to have its say (see the header comment).
                if (formatOf(variants[i]) == g.chosenFormat) out.push_back(lone(variants[i]));
                else                                         g.alternates.push_back(variants[i]);
            }
            out.push_back(std::move(g));
        }

        std::sort(out.begin(), out.end(), [](const FormatGroup& a, const FormatGroup& b) {
            const int c = a.chosenTitle.compare(b.chosenTitle, Qt::CaseInsensitive);
            if (c != 0) return c < 0;
            return a.chosenPath < b.chosenPath;
        });
        return out;
    }
}
