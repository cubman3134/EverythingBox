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
    bool local = false;       // entries are files on disk, not remote chapter ids

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

    // Display order -> reading order. Compares the FIRST parsed number with the LAST rather than demanding
    // strict monotonicity: real chapter lists carry duplicates (several translations of one chapter) and gaps,
    // and a rule that bailed on the first non-monotonic pair would leave plainly-descending lists reversed the
    // wrong way. When too little parses to tell, list order stands — that is what the user was just looking at.
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
        if (parsedCount < 2 || first <= last) return listed;
        QVector<ChapterRun::Entry> out = listed;
        std::reverse(out.begin(), out.end());
        return out;
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
        run.local = false;
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
        run.local = true;
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
