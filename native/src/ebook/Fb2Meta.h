// WHAT AN .fb2 SAYS ABOUT ITSELF (issue #144). FictionBook 2 is one XML document: a <description> block of
// metadata, one or more <body> trees of text, and <binary> elements holding every image the book uses,
// base64'd, at the end. So unlike EPUB there is no archive to walk and no manifest to follow — the whole
// book, cover included, is in one stream.
//
// WHY THIS IS SPLIT FROM Fb2Book, for the reason EpubMeta.h gives about EpubBook: the library scan (#134,
// BookMeta) wants a title, an author, a series and a cover WITHOUT staging chapters on disk, and the reader
// wants both. Two parsers would let a shelf disagree with the book it opens about who wrote it. There is one
// walk of <description> in this app and it is here; Fb2Book calls it and then reads the bodies.
//
// .fb2 AND .fb2.zip ARE THE SAME BOOK. The zipped form is how FB2 is almost always distributed (the format
// predates cheap bandwidth and its libraries still ship it that way), and it is a plain zip holding one .fb2
// member — not a container format with a manifest. documentBytes() unwraps it, and nothing above this line
// ever needs to know which of the two it was handed.
//
// WORKER-THREAD SAFE: file I/O, miniz and QXmlStreamReader, no Settings, no widgets, no static state.
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Fb2Meta
{
    // The same answer shape EpubMeta::Metadata carries, so BookMeta::Info fills from either without a
    // per-format branch beyond the dispatch. EVERY FIELD IS EMPTY WHEN THE FILE DID NOT SAY — never guessed
    // from the filename, which is BookLibrary's job and is marked as such there.
    struct Metadata
    {
        QString title;
        QString author;        // "First Last", or the <nickname> when that is all there is
        QString series;        // <sequence name>
        double  seriesIndex = 0.0;   // <sequence number>; 0 == unnumbered
        QString language;      // <lang>
        int     year = 0;      // <publish-info><year>, else the year of <document-info><date>
        QString coverId;       // the <binary id> the <coverpage> points at; empty == no declared cover
        int     sectionCount = 0;    // top-level <section>s of the main body == the reader's chapter count
    };

    // "Would this path be read as FB2": .fb2, or the zipped wire forms .fb2.zip and .fbz. Extension only —
    // the same classification-by-name rule the rest of the reading library uses (BookMeta.h).
    bool isFb2Path(const QString& path);

    // The FB2 XML for a path: the file's own bytes, or the .fb2 member of the zip when it is one of the
    // zipped forms. Empty on any failure. THE ONE DOOR: everything below takes the bytes this returned.
    QByteArray documentBytes(const QString& path);

    // Parse <description> out of FB2 XML. Returns false only when the document is not FictionBook at all; a
    // FictionBook that declares nothing parses to an empty Metadata and true, because "the file said
    // nothing" is a fact about the file and not a failure to read it.
    bool readXml(const QByteArray& xml, Metadata* out);

    // The decoded bytes of one <binary id="...">, whatever image type it declares. Empty when absent.
    QByteArray binary(const QByteArray& xml, const QString& id);

    // Path convenience for the scan: metadata, and (when `cover` is given AND the book declares one) the
    // cover image's encoded bytes.
    bool readFile(const QString& path, Metadata* out, QByteArray* cover = nullptr);
}
