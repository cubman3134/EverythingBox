// THE RECENTS ROW A COMIC ARRIVAL WRITES, and the rows that arrival replaces.
//
// Every other way into a reader records itself: MainWindow::open()'s recordDocument() writes a row for an
// epub, a PDF, a downloaded comic volume, a photo. The chapter lanes did not — a manga chapter reaches the
// reader through openImagePages() -> openCrossedComic(), which never touched RecentStore — so reading manga
// left no trace at all, and crossing a boundary left the row naming the volume you STARTED on rather than
// the one you were reading. This is what those three arrivals now write, kept pure so a probe can hold it.
//
// ONE ROW PER SERIES, NOT PER CHAPTER. A Recents list holds forty rows and a sitting with a manga is ten
// chapters, so a row per chapter would evict everything else the profile has watched, played or read in a
// single evening — and ten rows of one series is not a list of what you were doing. So an arrival first
// drops the run's OTHER entries: they are the same reading position, one chapter ago. The row that lands
// still keys on the CHAPTER, because that is what re-opening it has to re-open and where the reader's own
// resume position is filed; only its lifetime is per-series.
//
// The Files lane is deliberately exempt from that drop. Its "run" is the archives sitting in one folder,
// which is a heuristic about a filesystem and not a list anybody published: a flat Comics folder would have
// opening one archive quietly delete every other comic's row. It records its arrival like the rest and
// supersedes nothing.
//
// PURE — no widgets, no network, no disk. Unit-tested by probe_chapterrun.
#pragma once
#include "ChapterRun.h"
#include "RecentStore.h"

#include <QString>
#include <QStringList>

namespace ChapterRecent
{
    // "Chainsaw Man" + "Vol. 1 · Ch. 4" -> "Chainsaw Man — Vol. 1 · Ch. 4". The series name is the half a
    // reader recognises, and a chapter title alone is unreadable out of context; a chapter title that
    // ALREADY carries the series (some providers write "Chainsaw Man Ch. 4") is left as it is rather than
    // being told twice.
    inline QString displayTitle(const QString& seriesTitle, const QString& entryTitle)
    {
        const QString series = seriesTitle.trimmed();
        const QString entry  = entryTitle.trimmed();
        if (series.isEmpty()) return entry;
        if (entry.isEmpty()) return series;
        if (entry.contains(series, Qt::CaseInsensitive)) return entry;
        return series + QStringLiteral(" — ") + entry;
    }

    // WHAT THE ROW IS, given the file that was opened and the run it belongs to.
    //
    // `key` is the entry's own id for a remote lane and EMPTY for a Files one, whose ids are paths: a local
    // document row has always identified by its path, and RecentStore::add's keyless adoption then inherits
    // whatever a richer earlier row knew about the same file. An invalid run (a chapter opened from a level
    // whose list was never captured) leaves it empty for the same reason — the path is the only identity
    // there is, and it is a stable one, because a packed chapter is cached under a hash of its id.
    //
    // `sourceType` is the app's own vocabulary for what that id NAMES, which the lane already decides. A
    // Recent records "document" as its kind, and a kind is the reader a row opens in rather than a type any
    // addon would recognise — so without this a resumed row asking its addon "what series is this?" gets a
    // route match on nothing and an empty answer. It is NOT half a #224 re-mint recipe: that needs a route
    // and an item id as well, both deliberately left empty here, so RecentStore::reopenFor still replays
    // the path exactly as a document row always has.
    inline RecentItem rowFor(const QString& path, const QString& entryTitle, const ChapterRun& run)
    {
        RecentItem row;
        row.path  = path;
        row.kind  = QStringLiteral("document");
        row.title = displayTitle(run.seriesTitle, entryTitle);
        row.thumb = run.seriesThumb;
        if (run.isValid() && run.lane != ChapterRun::Lane::Files) row.key = run.entries[run.index].id;
        row.sourceAddonId = run.seriesAddonId;
        // The run's own entry type on the Chapters lane, falling back to the type this lane carried before
        // it was a protocol answer rather than one provider's. A resumed row asks its addon what series a
        // chapter belongs to, and the addon routes /meta BY TYPE — so a wrong type here is an empty answer.
        if (run.lane == ChapterRun::Lane::Chapters)
            row.sourceType = run.entryType.isEmpty() ? QStringLiteral("manga_chapter") : run.entryType;
        else if (run.lane == ChapterRun::Lane::Catalog) row.sourceType = QStringLiteral("comic_issue");
        return row;
    }

    // The identities this arrival replaces: every OTHER entry of the same run. Empty for a Files run (see
    // the header comment) and for a run that names no current entry, because "every entry except index"
    // with no index is every entry — which would have a chapter opened outside its own list delete the
    // whole series' history on its way in.
    inline QStringList superseded(const ChapterRun& run)
    {
        QStringList out;
        if (!run.isValid() || run.lane == ChapterRun::Lane::Files) return out;
        for (int i = 0; i < run.entries.size(); ++i)
            if (i != run.index && !run.entries[i].id.isEmpty()) out << run.entries[i].id;
        return out;
    }
}
