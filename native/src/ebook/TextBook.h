// PLAIN TEXT AND MARKDOWN THROUGH THE BOOK READER (issue #144). A .txt or .md is the cheapest possible
// EbookSource: no container, no manifest, no compression — one file, decoded, turned into the (X)HTML the
// reader already renders. Font size, pagination, the contents panel, per-book resume, bookmarks and reading
// stats all come along for free, because nothing above EbookSource knows what a chapter file was made from.
//
// ---- THE ENCODING LADDER, and why a .txt needs one at all ------------------------------------------------
//
// An .epub declares its encoding, a .fb2 declares its encoding, a MOBI header states its encoding — a .txt
// states NOTHING. It is a bag of bytes whose meaning depends on what wrote it, and the two failure modes are
// both bad in the same silent way: read a CP1252 file as UTF-8 and every curly quote becomes U+FFFD; read a
// UTF-8 file as CP1252 and every accent becomes two letters of mojibake. So the ladder is explicit, ordered,
// and REPORTED (decode() hands back which rung answered) rather than guessed at once and forgotten:
//
//   1. A BYTE-ORDER MARK, if there is one. UTF-8, UTF-16 LE and UTF-16 BE. A BOM is the file telling us, and
//      nothing below this line gets a vote when it does. The BOM itself is consumed, never left in the text
//      as a zero-width space at the top of page one.
//   2. STRICT UTF-8. Not "does it mostly work" — QStringDecoder's error flag, which trips on any byte
//      sequence that is not valid UTF-8. Valid UTF-8 is essentially never valid-looking anything else, so a
//      clean decode here is as close to proof as this problem has.
//   3. THE SYSTEM'S OWN 8-BIT CODEC (QStringConverter::System — the ANSI codepage on Windows). This is the
//      rung that reads the Notepad-era files people actually have.
//   4. LATIN-1, which cannot fail. On a platform whose "system" codec IS UTF-8, rung 3 fails exactly where
//      rung 2 did, and the choice is between a page of replacement characters and a page that is readable
//      with the wrong accents. Byte-for-byte recoverable beats destroyed.
//
// ---- WHAT BECOMES A CHAPTER -------------------------------------------------------------------------------
//
// A .md splits at its TOP-LEVEL (`#`) headings, so a manuscript written as one file reads as its own
// chapters; with no `#` in it, it is one chapter. A .txt is always ONE chapter: a blank line is a paragraph
// break and nothing in a plain text file distinguishes a chapter heading from a line of dialogue in capitals,
// so inventing chapters from it would be a guess the reader then paginated as fact.
#pragma once
#include "EbookSource.h"

#include <QByteArray>

class TextBook : public EbookSource
{
public:
    // Which rung of the ladder above answered. Returned by decode() so the caller (and the probe) can assert
    // the DECISION and not merely the text it produced.
    enum class Encoding { Utf8Bom, Utf16LeBom, Utf16BeBom, Utf8, System, Latin1 };

    // Extension gates. Plain-text: .txt/.text. Markdown: .md/.markdown/.mdown/.mkd.
    static bool isPlainTextPath(const QString& path);
    static bool isMarkdownPath(const QString& path);
    static bool isTextBookPath(const QString& path) { return isPlainTextPath(path) || isMarkdownPath(path); }

    // The ladder, as a pure function over bytes. `used` receives the rung that answered.
    static QString decode(const QByteArray& bytes, Encoding* used = nullptr);
    static const char* encodingName(Encoding e);   // for logs and reports, not for the UI

    // Plain text -> HTML: a blank line ends a paragraph, and the lines inside one are joined with a space
    // (hard-wrapped prose is one paragraph, not forty). HTML metacharacters are escaped, always.
    static QString plainTextToHtml(const QString& text);

    bool open(const QString& path, QString* error = nullptr) override;
    bool isOpen() const override { return !chapterFiles_.isEmpty(); }

    const QString& title() const override { return title_; }
    const QString& author() const override { return author_; }
    const QString& sourcePath() const override { return sourcePath_; }

    const QStringList& chapterFiles() const override { return chapterFiles_; }
    const QVector<EpubTocEntry>& toc() const override { return toc_; }
    int chapterIndexForHref(const QString& hrefFileName) const override;

private:
    QString sourcePath_, title_, author_, rootDir_;
    QStringList chapterFiles_;
    QVector<EpubTocEntry> toc_;
};
