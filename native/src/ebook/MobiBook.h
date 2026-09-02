// MOBI / AZW / AZW3 (Mobipocket and Kindle) through the shared reader. The container walk - the PalmDB
// record list, the MOBI header, EXTH, the KF8 (AZW3) boundary and the DRM refusal - is MobiHeader's; this
// class decompresses nothing and parses nothing. It takes the HTML that walk produced, strips the wrappers
// and the unresolvable image/stylesheet references, and stages it as a single chapter file that EbookView
// renders like an EPUB chapter.
//
// Handles MOBI6 and KF8 (issue #144); HUFF/CDIC compression is still not supported, and a DRM-protected file
// is refused BY NAME rather than decoded (MobiHeader.h says why at length). Images are stripped: MOBI
// references them by a record index this reader has nowhere to resolve to.
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
