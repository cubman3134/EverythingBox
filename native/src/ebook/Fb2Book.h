// FictionBook 2 (.fb2 / .fb2.zip / .fbz) through the SAME reader every other book format goes through
// (issue #144). EbookSource's contract is "a list of on-disk (X)HTML files in reading order plus a table of
// contents", so FB2 is not a new renderer, a new pagination or a new set of typography settings — it is one
// more mapping onto the model EpubBook already fills. Chapters, the contents panel, per-book resume, font
// size, bookmarks and reading stats all work because nothing above EbookSource can tell the difference.
//
// THE MAPPING. FB2's main <body> is a sequence of top-level <section>s, and those are the chapters; each
// section's <title> is its TOC entry. A body with no sections at all is one chapter of prose rather than a
// book with no chapters. FB2's footnote body (<body name="notes">) is deliberately NOT a chapter: it is
// apparatus, and in a paginated reader it would land in the middle of the book as an unheralded chapter of
// numbered fragments. Its metadata comes from Fb2Meta, which is the same walk of <description> the library
// scan makes (Fb2Meta.h says why there is only one).
//
// IMAGES ARE REAL FILES. FB2 carries every image as base64 inside a <binary id="…"> at the end of the
// document, and QTextBrowser resolves <img src> against the chapter file's own directory — so the binaries
// are decoded once into the staging folder and referenced by name. That is also why <image> renders at all
// here when MobiBook's images are stripped: MOBI's recindex numbers have nowhere to point, and FB2's do.
#pragma once
#include "EbookSource.h"

class Fb2Book : public EbookSource
{
public:
    bool open(const QString& path, QString* error = nullptr) override;
    bool isOpen() const override { return !chapterFiles_.isEmpty(); }

    const QString& title() const override { return title_; }
    const QString& author() const override { return author_; }
    const QString& sourcePath() const override { return sourcePath_; }

    const QStringList& chapterFiles() const override { return chapterFiles_; }
    const QVector<EpubTocEntry>& toc() const override { return toc_; }
    // A TOC entry names the chapter file it belongs to, so the lookup is by that name — the same answer
    // EpubBook gives, reached without a spine because FB2 has no separate manifest to disagree with.
    int chapterIndexForHref(const QString& hrefFileName) const override;

private:
    QString sourcePath_, title_, author_, rootDir_;
    QStringList chapterFiles_;
    QVector<EpubTocEntry> toc_;
};
