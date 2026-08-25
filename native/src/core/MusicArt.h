// ALBUM ARTWORK for the local music library (issue #74, increment 3) — where the picture on an album tile
// comes from, and where the expensive half of getting it happens.
//
// Two sources, in this order, and the order is the point:
//   1. THE EMBEDDED COVER. AudioTags::read() already returns it (increment 1), and MusicLibrary::TrackEntry
//      records per track whether there is one (`hasCover`). It is preferred because it is the art the album
//      actually ships with — a folder that carries no cover.jpg is the common case for anything ripped or
//      bought as files, and an app that only understood sibling images would show a wall of placeholders.
//   2. THE SIBLING FILE. cover.* / folder.* / front.* / albumart.* next to the tracks — the convention this
//      codebase ALREADY honours for a local audiobook's now-playing art. siblingCover() below is that rule,
//      moved here so there is exactly one copy of it (MainWindow's localCoverFor now calls it).
//
// WHY THE EMBEDDED HALF IS A CACHE ON DISK AND NOT A LOOKUP. The index deliberately does NOT hold cover
// bytes (MusicLibrary.h says why: twenty thousand JPEGs in RAM to draw a few dozen tiles), so the bytes have
// to be re-read from the file. Doing that per tile, per navigation, on the GUI thread, is a tag parse plus a
// full-size JPEG decode per album — this repo has a documented history of exactly that class of stall. So
// extractCovers() runs ONCE per scan, on the scan's own worker thread, and writes one small JPEG per album
// into <data>/musicart. The browse then hands the UI a plain file path, which is what every other local tile
// in this app already is.
//
// THE DECODE IS DELIBERATE, not incidental. extractCovers() decodes and DOWNSCALES to kMaxEdge before
// writing: embedded art is routinely 1500x1500 or larger, and leaving it at that size would only move the
// cost to whoever draws the tile. Decoding is precisely the work AudioTags::Picture left to its caller
// ("the bytes are the encoded JPEG/PNG, not a decoded image, because the scan runs off the GUI thread and
// QImage decoding is the caller's decision") — this is that caller, making that decision, off that thread.
//
// Everything here takes its directories as EXPLICIT arguments, for the reason MusicLibrary's pure half does:
// the one call site that needs AppPaths reads it on the main thread (cacheDir()) and passes the string into
// the worker. Nothing below reaches for Settings or AppPaths on its own except cacheDir() itself.
#pragma once
#include "MusicLibrary.h"

#include <QString>

namespace MusicArt
{
    // Longest edge of a written cover, in pixels. Big enough for a full-bleed tile on a 4K TV, small enough
    // that decoding one at draw time is not an event worth measuring.
    inline constexpr int kMaxEdge = 512;

    // The sibling-image convention: cover/folder/front/albumart . jpg/jpeg/png/webp inside `folder`, in that
    // fixed precedence. Returns an ABSOLUTE FILE PATH (not a URL — that is the caller's wrapping) or an empty
    // string. Pure but for the existence checks; no Settings, no cache, no decode.
    QString siblingCover(const QString& folder);

    // <data>/musicart — the extracted-cover cache. Reads AppPaths, so main-thread only, exactly like
    // MusicLibrary::indexFilePath(); pass the result into a worker rather than calling this from one.
    QString cacheDir();

    // The deterministic path an album's extracted cover would live at. A digest of the album KEY, because the
    // key is the album's identity everywhere else in this feature and because it contains separators and
    // arbitrary tag text that no filesystem would accept verbatim. The file may or may not exist — that is
    // what albumCover() is for.
    QString cachedCoverPath(const QString& cacheDir, const QString& albumKey);

    // The art for one album: the extracted cover when it has been written, else the sibling file, else empty.
    // Cheap (two existence checks) and side-effect free — safe to call per tile while building a catalog.
    QString albumCover(const MusicLibrary::Album& album, const QString& cacheDir);

    // ---- The same two rules, over anything with a KEY and a FOLDER (issue #139) -------------------------
    // The audiobook library wants exactly what albumCover and extractCovers do — extracted embedded art
    // first, the folder's own cover.* second, one small JPEG per item in the same cache — over a different
    // value type. These two are that logic with the MusicLibrary types lifted out, and albumCover/
    // extractCovers are implemented BY them, so there is still one copy of the precedence rule and one copy
    // of the decode/downscale. A second art module for books would have been a second answer to "which
    // picture is this", and the first thing it would have got wrong is the sibling-file precedence.
    //
    // The cache is shared (<data>/musicart) and that is safe by construction: cachedCoverPath digests the
    // KEY, an album key and a book key are built from different things, and a collision would need a SHA-1
    // one.
    QString keyedCover(const QString& key, const QString& folder, const QString& cacheDir);

    // Extract ONE item's embedded cover into the cache, from `sourceFile`. WORKER-THREAD WORK (a tag read
    // plus a decode/scale/encode). Returns whether a file was written — false for "already cached", "no
    // source", and "the picture would not decode", none of which is an error a user can act on.
    bool extractCoverFor(const QString& key, const QString& sourceFile, const QString& cacheDir);

    // Extract every album cover that is missing from the cache. WORKER-THREAD WORK: one tag read plus one
    // decode/scale/encode per album that needs one, and nothing at all for an album already cached or one
    // whose tracks carry no embedded picture. Returns how many files were written (0 is the steady state, so
    // a rescan of an unchanged library costs only the existence checks).
    //
    // An album whose embedded picture cannot be decoded is skipped silently and retried next scan: a corrupt
    // APIC frame is a property of one file, not an error the user can act on, and the sibling fallback still
    // applies.
    int extractCovers(const MusicLibrary::Index& idx, const QString& cacheDir);
}
