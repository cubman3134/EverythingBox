// Reads a (text) PDF as an e-book: extracts each page's text with QtPdf and reflows it into HTML chapter
// files, so EbookView renders it with font sizing / pagination like an EPUB. open() returns false for a
// scanned PDF with no text layer (the caller falls back to the page-image view). Desktop-only (QtPdf isn't
// shipped for Android); the Android build gets a stub that just reports "not available".
#pragma once
#include "EbookSource.h"

class PdfTextBook : public EbookSource
{
public:
    bool open(const QString& path, QString* error = nullptr) override;
    bool isOpen() const override { return !chapterFiles_.isEmpty(); }

    const QString& title() const override { return title_; }
    const QString& author() const override { return author_; }
    const QString& sourcePath() const override { return sourcePath_; }

    const QStringList& chapterFiles() const override { return chapterFiles_; }
    const QVector<EpubTocEntry>& toc() const override { return toc_; }
    int chapterIndexForHref(const QString&) const override { return -1; }

private:
    QString sourcePath_, title_, author_, rootDir_;
    QStringList chapterFiles_;
    QVector<EpubTocEntry> toc_;
};
