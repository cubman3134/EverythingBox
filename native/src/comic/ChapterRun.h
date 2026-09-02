// The chapters either side of the one you are reading, in READING order.
//
// "The next chapter" is not "the next row". A provider lists chapters in whichever direction it pleases and
// newest-first is common, so advancing by list position walks a descending list backwards — forward at the end
// of chapter 12 lands in chapter 11. This header normalises the order ONCE, when the run is captured, so every
// consumer downstream can just do index + 1.
//
// Pure: no widgets, no network, no disk. Unit-tested by probe_chapterrun.
#pragma once
#include "ComicPageOrder.h"   // ComicPages::collator()/lessThan() — the #205-safe natural-order collation

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>

// The chapters of one series (or the comic archives of one folder), plus which of them is open.
struct ChapterRun
{
    // `id` is a chapter item id for a remote run, or the full path to open for a local one. `title` is what
    // the user sees named in the hint and arrival toasts.
    struct Entry { QString id; QString title; };

    QVector<Entry> entries;   // READING order (ascending), already normalised
    int  index = -1;          // the entry currently open; -1 = no run (nothing to advance to)
    // WHAT THE ENTRIES ARE, which is also how a boundary press opens one. Three lanes, because there are
    // three genuinely different ways a comic reaches this reader and they share nothing but the ordering.
    enum class Lane
    {
        Files,      // `id` is a path on disk — the archives of one folder. Opened synchronously.
        Chapters,   // `id` is a manga chapter item id — resolved to page images, packed into a CBZ.
        Catalog,    // `id` is a catalog item id (a comic issue) — searched for at a file provider,
                    // downloaded, then opened. See `seriesTitle`, which is half of that search.
    };
    Lane lane = Lane::Files;  // Files is the default because a run built by hand, from nothing, is a folder
                              // one — which is what `bool local = false` meant before this was an enum.
    // WHAT THE CONTAINER IS — the facts about the series itself, as opposed to about any one entry. They
    // live on the RUN and not on each entry because that is what they are properties of: the capture guards
    // exist precisely to ensure every entry in a run comes from one container.
    //
    // Set for BOTH remote lanes and empty for Files, which is a folder rather than a container anybody
    // named. The Catalog lane's provider search is "<seriesTitle> <number>", which is what seriesTitle
    // started out for; the Recents row a chapter arrival writes (ChapterRecent.h) is titled, illustrated
    // and attributed from all three, which is why the Chapters lane carries them too. "Vol. 1 · Ch. 4" on
    // its own names nothing a reader could pick out of a Recents list, and a chapter has no cover of its own.
    QString seriesTitle;
    QString seriesThumb;    // the container's cover art (url or local path) — the row's artwork
    QString seriesAddonId;  // the addon whose list this run came from, so a resumed row can re-ask it
    // The MEDIA TYPE of the entries ("manga_chapter", …) on the Chapters lane; empty on the other two.
    // A crossing asks the addon for the next chapter's pages, and the protocol route is keyed by type
    // (#188) — so the type has to travel with the run, exactly as the addon id already does. Before this
    // existed the type was a constant in two places, and the constant was one provider's.
    QString entryType;

    bool isValid() const { return index >= 0 && index < entries.size(); }
    bool hasNext() const { return isValid() && index + 1 < entries.size(); }
    bool hasPrev() const { return isValid() && index > 0; }
};

namespace ChapterOrder
{
    // The chapter number a title names, or ok=false when it names none. The chapter MARKER wins over any
    // number in front of it, because "Vol. 3 Ch. 24" is chapter 24 and reading it as volume 3 would order a
    // whole series by its volumes. Only when there is no marker does the first number in the title count.
    inline double chapterNumber(const QString& title, bool* ok)
    {
        static const QRegularExpression marked(
            QStringLiteral("(?:\\bch(?:apter)?\\b\\.?|#)\\s*(\\d+(?:\\.\\d+)?)"),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression anyNum(QStringLiteral("(\\d+(?:\\.\\d+)?)"));
        QRegularExpressionMatch m = marked.match(title);
        if (!m.hasMatch()) m = anyNum.match(title);
        if (!m.hasMatch()) { if (ok) *ok = false; return -1.0; }
        bool parsed = false;
        const double v = m.captured(1).toDouble(&parsed);
        if (ok) *ok = parsed;
        return parsed ? v : -1.0;
    }

    // WHAT TO ASK A FILE PROVIDER FOR, for one entry of a Catalog run. Pressing an issue row builds this
    // string by hand from the series it drilled into and the number in the title; a crossing has to build
    // the same one, or "next volume" searches for something the row press would never have searched for.
    //
    // The NUMBER, not the title. An entry title is written for a human ("#3 — Volume 3") and a provider
    // search on it finds nothing. When no number parses the title goes in WHOLE rather than being dropped:
    // searching for the series alone would return some copy of some issue, and the crossing would open it.
    inline QString providerQuery(const QString& seriesTitle, const QString& entryTitle)
    {
        bool ok = false;
        const double n = chapterNumber(entryTitle, &ok);
        // 'g' so 12.5 stays "12.5" and 3 stays "3" rather than becoming "3.000000".
        const QString tail = ok ? QString::number(n, 'g', 10) : entryTitle.trimmed();
        const QString series = seriesTitle.trimmed();
        // NO SERIES: the title, whole. The number alone is not a query — searching a provider for "3"
        // matches nothing, or matches anything. A run with no series name is one built by hand or from
        // a container that never named itself, and the title is then the only real information there is.
        if (series.isEmpty()) return entryTitle.trimmed();
        if (tail.isEmpty()) return series;
        return series + QLatin1Char(' ') + tail;
    }

    // Display order -> reading order.
    //
    // When EVERY entry names a number, sort ascending by it. The sort must be STABLE: real lists carry
    // duplicates (several translations of one chapter) and their relative order is the provider's, not ours to
    // shuffle.
    //
    // WHY A SORT AND NOT JUST A DIRECTION. This used to only compare the FIRST parsed number with the LAST and
    // reverse the whole list when it ran downwards — deliberately conservative about ragged lists. Live against
    // a real MangaDex series that conservatism is exactly the bug: the provider sorts by volume first and files
    // a stray "Ch. 232" inside volume 1, so the display list runs 7, 7.5, 232, 8, 9. It is ascending end to
    // end, so nothing was reversed and list order stood — and the chapter after "Vol. 1 · Ch. 7.5" came out as
    // Ch. 232 instead of Ch. 8. With the crossing wired up that press does not merely misreport the next
    // chapter, it OPENS it, dropping the reader 200 chapters into a series it has not read. Do not restore the
    // "conservative" end-comparison for fully-numbered lists; it silently brings that back.
    //
    // WHAT THE SORT COSTS, recorded for the same reason the bug above is. A provider that restarts numbering
    // per volume — Vol. 1 Ch. 1-10, then Vol. 2 Ch. 1-10 — has every chapter number duplicated, so the two
    // volumes INTERLEAVE (both Ch. 1s adjacent, then both Ch. 2s…) where the old rule left the provider's
    // order alone. Stability keeps each pair in the listed order, so "next" still lands on a real neighbouring
    // chapter, but it is the other volume's. That is the accepted trade: the interleave misreads a boundary
    // by one chapter, the bug above misread it by two hundred. Fixing it properly means parsing the volume as
    // a major key, which needs a real per-provider sample of how volumes are titled.
    //
    // When only SOME entries name a number the old end comparison still decides, because sorting would have to
    // invent a position for the unnumbered ones and the provider's order is the better guess there. Comparing
    // the ends rather than demanding strict monotonicity is deliberate for the same reason as before:
    // duplicates and gaps must not leave a plainly-descending list reversed the wrong way. When too little
    // parses to tell, list order stands — that is what the user was just looking at.
    inline QVector<ChapterRun::Entry> inReadingOrder(const QVector<ChapterRun::Entry>& listed)
    {
        double first = 0.0, last = 0.0;
        int parsedCount = 0;
        for (const ChapterRun::Entry& e : listed)
        {
            bool ok = false;
            const double v = chapterNumber(e.title, &ok);
            if (!ok) continue;
            if (parsedCount == 0) first = v;
            last = v;
            ++parsedCount;
        }
        if (parsedCount < 2) return listed;                 // too little parses to tell: list order stands
        if (parsedCount == listed.size())                   // every entry numbered: true reading order
        {
            QVector<ChapterRun::Entry> out = listed;
            std::stable_sort(out.begin(), out.end(),
                             [](const ChapterRun::Entry& a, const ChapterRun::Entry& b) {
                                 // Both parse — the branch guarantees it — so the ok-out is not needed here.
                                 return chapterNumber(a.title, nullptr) < chapterNumber(b.title, nullptr);
                             });
            return out;
        }
        if (first <= last) return listed;                   // ragged, but pointing the right way already
        QVector<ChapterRun::Entry> out = listed;
        std::reverse(out.begin(), out.end());
        return out;
    }

    // IS THIS FOLDER THE APP'S OWN DOWNLOAD CACHE? A folder run reads "the archives beside this one" as
    // the chapters either side of it, which is true of a folder somebody FILED comics into and false of
    // every folder this app writes to for its own reasons. A remote book/comic/ROM is fetched into one flat
    // cache folder under a SHA1 of its url, so what sits beside a manga volume there is whatever else was
    // ever opened — in hash order, which is to say in no order at all.
    //
    // Live: Fairy Tail Vol. 2, cached as b3ddbd79….cbz, was followed in hash order by b969cec8….zip — a
    // 985 MB ROM archive (7-Zip inside, despite the extension isComicFile() accepts). Paging off the
    // volume's last page opened THAT as chapter two, and the reader's "this isn't a readable comic
    // archive" was the only thing standing between the reader and it. This is the local-lane twin of the
    // fault the caller of fromChapterItems already guards against remotely: a run spanning unrelated
    // works is worse than no run, because nothing about it looks wrong until the reader is already lost
    // in it.
    //
    // A prefix test, so the whole cache tree counts (remote-docs/, manga/, anything added later) — but only
    // ON a separator, or `…/cache-comics` would be swallowed by `…/cache`. Case-insensitive because the
    // folder arrives from QFileInfo and the root from QStandardPaths, and on Windows those disagree. An
    // empty root matches nothing: QStandardPaths can return "", and a root that matched everything would
    // silently turn every folder run off.
    inline bool isCachePath(const QString& folder, const QString& cacheRoot)
    {
        if (folder.isEmpty() || cacheRoot.isEmpty()) return false;
        const QString f = QDir::cleanPath(folder);
        const QString root = QDir::cleanPath(cacheRoot);
        return f.compare(root, Qt::CaseInsensitive) == 0
            || f.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
    }

    inline int indexOfId(const ChapterRun& run, const QString& id)
    {
        for (int i = 0; i < run.entries.size(); ++i)
            if (run.entries[i].id == id) return i;
        return -1;
    }

    // A run over a browsed chapter list. `currentId` not being in the list leaves index at -1, which reads as
    // "no neighbours" everywhere downstream — a chapter opened from somewhere the list never covered simply
    // behaves as it did before this feature existed.
    inline ChapterRun fromChapterItems(const QVector<ChapterRun::Entry>& listed, const QString& currentId)
    {
        ChapterRun run;
        run.lane = ChapterRun::Lane::Chapters;
        run.entries = inReadingOrder(listed);
        run.index = indexOfId(run, currentId);
        return run;
    }

    // A run over the comic archives sitting in one folder. Natural filename order (ch2 before ch10) through
    // the shared collation, and NEVER the newest-first reversal above: a folder listing is not a provider's
    // display order, and a file named "Chapter 12.cbz" beside "Chapter 2.cbz" is already in reading order.
    inline ChapterRun fromFileNames(const QString& folder, const QStringList& fileNames,
                                    const QString& currentFileName)
    {
        ChapterRun run;
        run.lane = ChapterRun::Lane::Files;
        QStringList names = fileNames;
        const QCollator coll = ComicPages::collator();
        std::sort(names.begin(), names.end(),
                  [&coll](const QString& a, const QString& b) { return ComicPages::lessThan(coll, a, b); });
        for (const QString& n : names)
            run.entries.append({ folder + QStringLiteral("/") + n, QFileInfo(n).completeBaseName() });
        run.index = indexOfId(run, folder + QStringLiteral("/") + currentFileName);
        return run;
    }
}
