// Miximage: one composited "game card" per item, built from the artwork roles we already download —
// a screenshot base with box art, title logo and (optionally) the physical-media shot layered over it.
// This is the ES-DE trick that makes a library look uniform even when scrape coverage is patchy: whichever
// single role happens to exist per game, the card degrades to a DEFINED layout rather than a blank tile.
//
// Two halves, deliberately split so the layout math is testable without touching disk or the metadata cache:
//
//   * composeImages()/compose() are PURE. Given the layers (as QImages, or as file paths) and a canvas size
//     they return the composited QImage. Same inputs -> same pixels; nothing here reads a clock, the ini, or
//     a random source. The layout is anchored PER ROLE (logo top-centre, box lower-left, disc lower-right,
//     screenshot as the cover base) and does not move when a neighbour is absent — that fixed geometry is
//     what "graceful degradation" means here: a game with only a screenshot, or only a box, still lands its
//     layer in the same place a fully-scraped game would, so the shelf reads as one set.
//
//   * ensureForKey() is the app-side glue: it reads an item's cached input roles out of MetaCache, decides
//     whether the stored composite is stale (all inputs are local files, so staleness is an mtime check),
//     regenerates it when it is, and records it under the "miximage" role so MetaCache::loadArt surfaces it
//     to themes like any other art role. It is called from the display path (on hover / on opening detail),
//     NOT as a library-wide batch job — the first time a card is actually asked for is when it is built.
#pragma once
#include <QImage>
#include <QSize>
#include <QString>

namespace Miximage
{
    // The four input layers, as local file paths. Any of them may be empty — that is the whole point.
    struct Inputs
    {
        QString screenshot; // the cover base
        QString box;        // box / cover art, lower-left
        QString logo;       // title / clear logo, top-centre
        QString disc;       // physical media (disc / cartridge), lower-right
    };

    bool hasAnyInput(const Inputs& in); // false only when every layer is empty (nothing to composite)

    // A stable name for the layout a given availability combination lands on ("screenshot+box+logo+disc",
    // "screenshot+logo", "box", "empty", …). Roles always appear in the same order, so the name is a pure
    // function of WHICH layers are present — it is what the degradation matrix is keyed on and asserted by.
    QString layoutName(const Inputs& in);

    // The default canvas. 1280x960 (4:3) is the ES-DE "1x" size and the sane default here; the height is
    // read from everythingbox.ini "miximage/height" (width follows at 4:3) so it is configurable without a
    // layout editor — the same ini-only knob the image-cache cap uses. Clamped to a sensible range.
    QSize defaultCanvas();

    // PURE compositor. Draws the present layers onto a `canvas`-sized image at their anchored positions; an
    // empty QImage means that layer is absent and is simply skipped. When NO screenshot is given the canvas
    // is filled with a solid neutral backing first, so a box-only / logo-only card is never a blank frame.
    // Deterministic: identical inputs yield identical pixels.
    QImage composeImages(const QImage& screenshot, const QImage& box, const QImage& logo, const QImage& disc,
                         QSize canvas);

    // Same, loading each non-empty path first. A path that fails to load is treated as an absent layer (so a
    // truncated download degrades like a missing one rather than aborting the card). Returns a null QImage
    // only when nothing loaded at all.
    QImage compose(const Inputs& in, QSize canvas);

    // Display-path entry point. Resolves the item's cached input roles from MetaCache, and if the stored
    // composite is missing or older than any input, (re)generates it and records it under the "miximage"
    // role. Returns the composite's path, or "" when the item has no input art (no card is made — and, by
    // design, no blank one either). Cheap when nothing changed: an mtime check and an early return.
    QString ensureForKey(const QString& key, QSize canvas = defaultCanvas());
}
