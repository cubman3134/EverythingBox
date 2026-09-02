// THE PALM/MOBI CONTAINER, READ ONCE (issue #144). MOBI, AZW and AZW3 are all one file format: a PalmDB of
// numbered records, whose record 0 is a PalmDOC header followed by a MOBI header followed by an EXTH block of
// metadata, and whose next N records are the compressed text. What differs between them is which header the
// text belongs to, and whether the book is encrypted.
//
// WHY THIS IS ITS OWN UNIT. Three callers want three different amounts of the same walk and must never
// disagree: MobiBook wants the text, the library scan (#134, BookMeta) wants the title/author/cover without
// decompressing a whole novel to get them, and the reader's format sniffing wants to know a container when it
// sees one. Before this file existed the walk was inline in MobiBook::open and there was no way to ask it a
// question short of opening the book.
//
// ---- AZW3 / KF8 ------------------------------------------------------------------------------------------
//
// KF8 ("Kindle Format 8", the thing an .azw3 holds) is not a new container: it is a SECOND MOBI header inside
// the same PalmDB, with its own text records after it, holding a better-formed HTML/CSS payload of the same
// book. Two shapes exist and both are handled:
//
//   * A STANDALONE .azw3 — record 0 IS the KF8 header (its MOBI header states file version 8), and its text
//     records follow it as usual.
//   * A COMBINED .mobi — record 0 is the old MOBI6 header, and EXTH record 121 gives the record INDEX of the
//     KF8 header sitting further in. That record is then read exactly as a record 0, and ITS text records are
//     the ones after IT. (KindleUnpack calls this the "boundary".)
//
// The KF8 part is PREFERRED when a file has both, because it is the same book in the better markup and it is
// what every other reader shows — but a KF8 part that will not read falls back to the MOBI6 one rather than
// failing the open, so a combined file can only get better than it was.
//
// ---- DRM ---------------------------------------------------------------------------------------------------
//
// The PalmDOC header's encryption field is read FIRST, and a non-zero one ENDS the read: DrmProtected, with a
// sentence saying so. There is no decoder attempt, no partial render, no "try anyway" path, and nothing in
// this project removes DRM from anything. A DRM'd book must fail LOUDLY and by name, because the alternative
// — a garbled page of decompressed ciphertext — reads as a bug in the reader instead of a fact about the file.
//
// WORKER-THREAD SAFE: pure functions over a QByteArray. No Settings, no widgets, no static state.
#pragma once
#include <QByteArray>
#include <QString>

namespace MobiHeader
{
    enum class Result
    {
        Ok,
        NotMobi,                 // no PalmDB BOOKMOBI/TEXtREAd signature
        Corrupt,                 // the record list or record 0 does not hold together
        DrmProtected,            // the PalmDOC header declares encryption — see the header
        UnsupportedCompression,  // HUFF/CDIC (17480), which this reader does not implement
        NoText                   // a sound container whose text records decompressed to nothing
    };

    struct Info
    {
        QString title;
        QString author;
        uint    textEncoding = 1252;
        int     fileVersion = 0;   // the MOBI header's stated version; 8 == KF8
        bool    kf8 = false;       // the part that was READ is a KF8 one (a standalone AZW3, or the
                                   // KF8 half of a combined file)
        int     bootRecord = 0;    // which PalmDB record that part's header lives in (0 for a plain MOBI)
        bool    hasCover = false;  // an EXTH cover record coverBytes() would have a real chance of returning
    };

    // "Do these first bytes look like a Palm/MOBI container at all" — the signature at offset 60, and
    // nothing else. Cheap enough for the reader's format sniff, which has read 68 bytes and no more.
    bool isMobiContainer(const QByteArray& head);

    // Metadata only: title, author, encoding, which part would be read, whether there is a cover. Costs the
    // headers and the EXTH block; decompresses NOTHING.
    Result read(const QByteArray& data, Info* info);

    // The same walk, plus the chosen part's decompressed text as raw (undecoded) bytes.
    Result readText(const QByteArray& data, Info* info, QByteArray* text);

    // The EXTH cover image's encoded bytes (record 201's, else the thumbnail 203's), or empty.
    QByteArray coverBytes(const QByteArray& data);

    // MOBI's own text encodings: 65001 is UTF-8, everything else is treated as Windows-1252 (which differs
    // from Latin-1 only in 0x80..0x9F, and those are the smart quotes and dashes every book is full of).
    QString decodeText(const QByteArray& bytes, uint encoding);

    // One sentence for the user, already translated. Ok maps to an empty string.
    QString message(Result r);
}
