// Common interface for an opened e-book (EPUB or MOBI). A book is a list of on-disk (X)HTML chapter files
// in reading order, plus a table of contents and metadata; EbookView renders each chapter with a
// QTextBrowser. EpubBook and MobiBook implement this so the view doesn't care about the source format.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

struct EpubTocEntry
{
    QString title;
    QString href; // spine file name only (no '#fragment'/directory), for matching a chapter
    int depth = 0;
};

class EbookSource
{
public:
    virtual ~EbookSource() = default;

    virtual bool open(const QString& path, QString* error) = 0;
    virtual bool isOpen() const = 0;

    virtual const QString& title() const = 0;
    virtual const QString& author() const = 0;
    virtual const QString& sourcePath() const = 0; // original file path (per-book settings key)

    virtual const QStringList& chapterFiles() const = 0;  // absolute paths, reading order
    virtual const QVector<EpubTocEntry>& toc() const = 0;
    virtual int chapterIndexForHref(const QString& hrefFileName) const = 0; // chapter for a TOC href, or -1
};
