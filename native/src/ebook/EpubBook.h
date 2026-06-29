// Minimal EPUB (2 & 3) reader: unzips the archive (miniz), follows container.xml -> OPF, and exposes the
// spine documents (chapters) as on-disk XHTML file paths plus the table of contents and metadata. Parsing
// is defensive - a malformed book yields an empty chapter list rather than throwing. Ported from the
// Unity UEPubReader. Qt's rich-text engine renders the XHTML, so we keep chapters as files (for images/CSS).
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <QHash>
#include "EbookSource.h"

class EpubBook : public EbookSource
{
public:
    bool open(const QString& epubPath, QString* error = nullptr) override;
    bool isOpen() const override { return !chapterFiles_.isEmpty(); }

    const QString& title() const override { return title_; }
    const QString& author() const override { return author_; }
    const QString& sourcePath() const override { return sourcePath_; } // original .epub path (per-book settings key)

    const QStringList& chapterFiles() const override { return chapterFiles_; } // absolute paths, spine order
    const QVector<EpubTocEntry>& toc() const override { return toc_; }

    // Chapter index whose file name matches a TOC href (or -1).
    int chapterIndexForHref(const QString& hrefFileName) const override;

private:
    bool extract(const QString& epubPath, const QString& destDir, QString* error);
    bool parseContainer(QString* opfRelPath, QString* error);
    bool parseOpf(const QString& opfRelPath, QString* error);
    void parseNav(const QString& navPath);
    void parseNcx(const QString& ncxPath);
    void addTocEntry(const QString& rawTitle, const QString& rawHref);

    QString rootDir_;     // extracted epub root (temp)
    QString htmlRoot_;    // directory containing the OPF (chapter hrefs are relative to this)
    QString sourcePath_;
    QString title_, author_;

    QStringList chapterFiles_;  // absolute paths, spine order
    QStringList chapterHrefs_;  // parallel file names (for TOC matching)
    QVector<EpubTocEntry> toc_;
};
