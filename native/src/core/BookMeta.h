// WHAT ONE BOOK OR COMIC FILE SAYS ABOUT ITSELF (issue #134) — the container half of the reading library's
// scan, kept apart from the grouping half so that neither can be changed by accident while reading the
// other.
//
// Three formats, three completely different places to look, one answer shape:
//
//   * .epub  — the OPF, through EpubMeta (which is the walk EpubBook already did for its spine, factored
//              out; this file adds no XML of its own). Title, author, series, language, year, chapter count
//              and the declared cover, all from ONE pass over ONE member of the archive.
//   * .pdf   — the document information dictionary, through QtPdf's metaData(). A PDF's Title/Author are
//              routinely blank or filled in by whatever produced it; both are taken VERBATIM when present
//              and left empty when not, because "Microsoft Word - draft3.doc" is a real Title field and the
//              only honest thing to do with a bad one is show it. There is no series field in a PDF.
//   * .cbz   — nothing at all. A comic archive is a bag of images: the page COUNT is free (it is the number
//              of image members) and the cover is page one, and the title/series come from the FILENAME,
//              which is ComicName's job and not this file's. ComicInfo.xml is issue #152 and deliberately
//              out of scope here.
//
// WHY THE COVER IS A SEPARATE CALL. read() is asked for every file of every scan; coverBytes() is asked
// only for a book whose cover is not already in the cache, which after the first scan is none of them.
// Folding the two together would inflate a cover image per file per scan and throw all of them away.
//
// WORKER-THREAD SAFE, and that is a requirement rather than an observation: the whole reading-library scan
// runs off the GUI thread (BookLibrary.h), and everything here is file I/O plus parsing — no Settings, no
// AppPaths, no widget, no static state. QPdfDocument is used as a plain local object with no view attached.
#pragma once
#include <QByteArray>
#include <QString>

namespace BookMeta
{
    // What a container told us. EVERY FIELD IS EMPTY / ZERO WHEN IT DID NOT SAY, and none of them is ever
    // guessed from the filename — that fallback belongs to BookLibrary, which is the layer that knows what
    // the file is called and can mark the result as having come from the name.
    struct Info
    {
        QString title;
        QString author;
        QString series;
        double  seriesIndex = 0.0;
        QString language;
        int     year = 0;
        int     pageCount = 0;    // EPUB: spine documents. PDF: pages. CBZ: page images. 0 == unknown.
        bool    hasCover = false; // a cover coverBytes() would have a real chance of returning

        // "The container said nothing a shelf could use." Deliberately NOT about pageCount or hasCover: a
        // 300-page PDF with no Title is still an untitled file, and the point of this flag is to tell the
        // browse that what it is about to show came from the filename.
        bool isEmpty() const { return title.isEmpty() && author.isEmpty() && series.isEmpty(); }
    };

    // Read one file. Dispatches on the extension and NEVER on the contents: a .pdf under a reading root is
    // a book because of where it is and what it is called, which is the classification rule this whole
    // feature is built on (see Settings::readingFolder). An unreadable or unrecognised file yields an empty
    // Info rather than a failure — a file that will not parse still belongs on the shelf under its own name.
    Info read(const QString& path);

    // The ENCODED bytes of this file's cover image (JPEG/PNG/whatever it happens to be), or empty. Not a
    // decoded QImage, for the reason AudioTags::Picture gives: the decode is a policy decision belonging to
    // whoever is going to scale and store it, which here is MusicArt::writeKeyedCover.
    //
    //   * EPUB — the manifest item the package DECLARES as its cover. Never a member that merely happens to
    //     be called cover.jpg; see EpubMeta.h.
    //   * CBZ  — page one, in the reader's own page order (ComicPages), so the shelf's picture is literally
    //     the first thing the reader will show.
    //   * PDF  — page one, rendered at kPdfCoverWidth and handed back as PNG.
    QByteArray coverBytes(const QString& path);

    // Render width for a PDF cover page. Comfortably above MusicArt::kMaxEdge so the downscale that follows
    // is a downscale, and low enough that rendering one is not an event.
    inline constexpr int kPdfCoverWidth = 600;
}
