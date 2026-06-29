// Minimal MOBI (Mobipocket / older Kindle) reader. Parses the PalmDB container + MOBI header, decompresses
// the text records (PalmDoc LZ77, or uncompressed), and stages the resulting HTML as a single chapter file
// that EbookView renders like an EPUB chapter. Handles MOBI6; HUFF/CDIC compression and KF8-only (AZW3)
// payloads aren't supported (reported as an error). Images are stripped for now.
#pragma once
#include "EbookSource.h"

class MobiBook : public EbookSource
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
