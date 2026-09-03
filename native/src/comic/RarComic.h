// WHAT A .cbr SAYS IT HOLDS, AND HOW TO GET IT OUT (issue #144). The RAR half of the comic archive layer,
// beside the CBZ (miniz), CB7 (LZMA SDK) and CBT (Tar.h) readers — one more container answering the SAME two
// questions the others do: which members are pages, and what are that page's bytes. The page ORDER is not
// asked here; it is ComicPages' (ComicPageOrder.h), exactly as it is for the other three.
//
// WHY A SHARED UNIT AND NOT A METHOD ON ComicView. Two callers, and they must not disagree: the reader opens
// the archive, and the library scan (#134, BookMeta) reads its page COUNT and its COVER. A cover that came
// out of a second listing rule is the bug ComicPageOrder.h was factored out to prevent, arriving through the
// RAR door instead of the zip one.
//
// WHY EVERY ENTRY IS DECOMPRESSED IN ONE SEQUENTIAL PASS, INCLUDING THE ONES WE THROW AWAY. RAR archives can
// be SOLID: the compressor treats the whole archive as one stream, so entry N's data is only decodable with
// the window entry N-1 left behind. unarr recovers from an out-of-order read by silently re-decompressing
// the archive from its first entry (rar_restart_solid) — correct, and quadratic if you ask for pages one at
// a time. So the readers below walk the entries ONCE, in the archive's own order, and hand every entry's
// bytes to the decompressor whether or not the caller wants them (a discarded entry costs its own inflate
// and nothing more; a CBR is almost entirely page images anyway). Nothing here ever seeks to a page.
//
// The one exception is imageNames(), which reads HEADERS ONLY and decompresses nothing at all — RAR's block
// headers are a linked list of sizes, so a page count is a walk, not an extraction. That is why a .cbr can
// join the #134 scan when a .cb7 and a .cbt cannot (BookLibrary.h states the cost rule they fail).
//
// RAR5 IS NOT READ, and says so. unarr 1.1.1 decodes RAR 2.9/3.x/4.x only; upstream's README says RAR5 is
// still unimplemented. The signature is sniffed HERE, ahead of unarr, so a RAR5 comic is refused by name
// instead of falling into the generic "this isn't a readable archive" bucket that a corrupt file lands in.
// (No DRM, no encryption: an encrypted RAR is refused, never guessed at.)
//
// WORKER-THREAD SAFE. File I/O plus unarr, no Settings, no widgets, no static state — the #134 scan runs off
// the GUI thread and calls straight in.
#pragma once
#include <QByteArray>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace RarComic
{
    // Why a read produced nothing. Kept separate from the message so a caller can branch on the CAUSE (the
    // reader shows text; the scan just wants to know it got nothing) and so a probe asserts the cause rather
    // than a translated string.
    enum class Status
    {
        Ok,          // at least one page image came back
        NotRar,      // the file does not start with a RAR signature at all (or could not be opened)
        Rar5,        // it IS a RAR, of the one version this decoder does not read
        Unreadable,  // a RAR the decoder rejected: truncated, encrypted, or otherwise damaged
        NoPages      // a readable RAR that holds no page images
    };

    // "Is this file's first bytes a RAR, and which one" — signature only, no unarr, no allocation. Returns
    // NotRar / Rar5 / Ok, where Ok means "a RAR4 signature", NOT "this archive is sound".
    Status signatureOf(const QString& path);

    // One line for the user, already translated. Ok maps to an empty string.
    QString message(Status s);

    // The image members, in the archive's own order, WITHOUT decompressing anything. For a page count.
    //
    // `otherNames`, when given, is filled with the members that are NOT page images — from the SAME header
    // walk, so a caller looking for a sidecar (ComicInfo.xml, issue #152) learns whether it is there without
    // a second pass and without inflating a byte. The returned list and the Status are exactly what they
    // were before: "no images" is still NoPages however many other members there are.
    QStringList imageNames(const QString& path, Status* status = nullptr, QStringList* otherNames = nullptr);

    // Every image member's encoded bytes, name-keyed, in the archive's own order. ONE pass (see the header).
    QVector<QPair<QString, QByteArray>> imagePages(const QString& path, Status* status = nullptr);

    // The bytes of ONE page: the first in ComicPages' natural order, which is the page the reader will open
    // on. Walks the headers to learn the order, then makes the same single sequential pass and keeps only
    // that member — so a cover costs the archive up to page one, and never a second full extraction.
    QByteArray coverBytes(const QString& path, Status* status = nullptr);

    // The bytes of ONE member by its exact name, page image or not — the sidecar door (ComicInfo.xml, issue
    // #152). Costs the SAME single sequential pass coverBytes() does, for the same solid-archive reason, so
    // ask imageNames() for `otherNames` first and come here only when the member is known to be there.
    // Empty when the archive holds no such member.
    QByteArray memberBytes(const QString& path, const QString& name, Status* status = nullptr);
}
