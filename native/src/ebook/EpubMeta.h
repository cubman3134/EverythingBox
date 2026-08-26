// THE ONE READING OF AN EPUB'S PACKAGE FILE (issue #134). container.xml -> the OPF -> title, author,
// series, cover — as pure functions over BYTES, plus one archive-level reader that gets those bytes without
// unpacking the book.
//
// WHY THIS IS A SPLIT-OUT OF EpubBook RATHER THAN A SECOND PARSER. EpubBook already followed container.xml
// to the OPF and read `dc:title` / `dc:creator` out of it on its way to the spine — the reader has always
// known the book's title, it simply had nowhere to put it. #134 wants the same facts plus two more (the
// series and the cover) for every file under a root, and the wrong way to get them is a second XML walk
// living beside the first: two parsers over one grammar drift, and the drift shows up as a shelf that
// disagrees with the open book about who wrote it. So the walk moved HERE, and EpubBook calls it. There is
// one reading of container.xml (opfPathFromContainer) and one reading of the OPF's metadata
// (parseOpfMetadata) in this tree.
//
// WHY THE ARCHIVE READER DOES NOT EXTRACT. EpubBook::open unpacks the whole zip into a per-book temp folder,
// which is right for READING a book — the XHTML needs its images and CSS on disk — and completely wrong for
// SCANNING a shelf of them: a thousand-book library would be unpacked in full to learn a thousand titles,
// and the unpacked copies would outlive the scan. readEpubFile() instead pulls exactly three members out of
// the archive in memory (container.xml, the OPF, and — only when asked — the cover image) and writes
// nothing. A book with no cover costs two.
//
// EVERYTHING HERE IS SAFE ON A WORKER THREAD: no Settings, no AppPaths, no QWidget, no static state. The
// scan calls it from QtConcurrent (BookLibrary.h states that contract).
#pragma once
#include <QByteArray>
#include <QString>

namespace EpubMeta
{
    // What one OPF says about the book it describes. Every field is "" / 0 when the package did not say —
    // NEVER a guess, and never the filename: the filename fallback belongs to the layer that knows the file
    // name (BookLibrary), because a fabricated title here would be indistinguishable from a real one and
    // would then be grouped on.
    struct Metadata
    {
        QString title;
        QString author;      // the FIRST dc:creator; see parseOpfMetadata for why not all of them
        QString series;      // calibre:series, else EPUB3 belongs-to-collection — both EXPLICIT markers
        double  seriesIndex = 0.0;   // 0 == the package named no position. Decimal on purpose (see below).
        QString language;    // dc:language, verbatim ("en", "en-GB", …) — carried, not yet browsed on
        int     year = 0;    // the leading year of dc:date, when it plainly is one
        QString coverHref;   // manifest href of the cover image, RELATIVE TO THE OPF; "" when unnamed
        int     spineCount = 0;   // how many documents the spine names — the book's chapter count

        bool isEmpty() const { return title.isEmpty() && author.isEmpty() && series.isEmpty(); }
    };

    // META-INF/container.xml -> the `full-path` of the first <rootfile>, or "" when the bytes are not a
    // container. THE one reader: EpubBook::parseContainer reads the extracted file and calls this.
    QString opfPathFromContainer(const QByteArray& containerXml);

    // The OPF's <metadata> and <manifest>, read once. PURE — bytes in, a struct out, no filesystem — which
    // is what lets the probe pin every rule below against hand-written packages with no zip anywhere.
    //
    // SERIES, AND WHY BOTH SPELLINGS. `<meta name="calibre:series">` is what Calibre writes and therefore
    // what the overwhelming majority of managed libraries carry; `belongs-to-collection` is what EPUB 3
    // standardised. Calibre wins where a file has both, because a file that Calibre manages is a file whose
    // owner curates that field. Neither is inferred from anything: a book with no series meta gets no
    // series, which is the whole conservatism rule this feature is written to (see ComicName.h).
    //
    // THE SERIES INDEX IS A DECIMAL and that is not fussiness. Calibre's series_index is documented as a
    // real number and novellas genuinely ship as 2.5; truncating it to an int would file "Book 2.5" at
    // position 2, where it collides with book 2 and the tie is broken by title — i.e. the half-book lands
    // either side of the real one at random. 0 still means "unnumbered" and still sorts last.
    //
    // THE COVER IS ONLY EVER A DECLARED ONE: a manifest item carrying `properties="cover-image"` (EPUB 3),
    // else the item named by `<meta name="cover" content="…">` (EPUB 2). Nothing here looks for a file
    // CALLED cover.jpg inside the archive — that is a filename heuristic, and the first book it is wrong
    // about shows somebody else's picture with nothing on screen to say so.
    Metadata parseOpfMetadata(const QByteArray& opfXml);

    // Read one .epub straight out of the archive: container.xml -> the OPF -> `out`. When `coverOut` is
    // given AND the package declared a cover, that member is extracted too (encoded bytes, not a decoded
    // image — the decode is the caller's decision, exactly as AudioTags::Picture leaves it).
    //
    // Returns false for "not a readable zip" and for "no OPF"; a package that parses but says nothing is a
    // SUCCESS with an empty Metadata, because "this book carries no metadata" is a fact the shelf must
    // render (as its filename), not an error that drops it.
    bool readEpubFile(const QString& epubPath, Metadata* out, QByteArray* coverOut = nullptr);
}
