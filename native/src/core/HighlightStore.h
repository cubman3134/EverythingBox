// Per-book highlights (issue #136). A highlight is a passage of a book in a colour — and, structurally, it is
// the bookmark anchor with its RESERVED range end filled in. ReaderAnchor.h has said so since the bookmarks
// half shipped ("a highlight is simply an anchor with endOffset >= 0"), so this store adds a colour and the
// highlighted text and NOTHING about position: no second identity, no second position model, no second shape.
//
// SHAPE — the bookmarks store, twin for twin. Per-PROFILE ("Per-profile, like everything else"), the
// {items, tombs} layout with a real delete tombstone, on the shared portable everythingbox.ini:
//   highlights/<profile>/items  -> a JSON array of {id, bookKey, anchor:{...}, color, text, ts}, ALL books
//   deleted/highlights/<profile>/<md5(id)> -> the delete tombstone for a removed highlight id (Tombstones)
// The CloudMerge section is the same near-verbatim twin of favourites/bookmarks: union by id, newest-ts wins,
// a tombstone at-or-after an item's ts suppresses it.
//
// IDENTITY is the POSITION, exactly as a bookmark's is: md5(bookKey | canonical-anchor), and the anchor now
// carries the range end, so two devices that highlight the same passage converge on ONE row. A RECOLOUR keeps
// the id (same passage, same row, new colour, fresh ts) — which is what makes "newest ts wins" mean the last
// colour a person chose, on any device.
//
// MERGING IS THE RULE THE ISSUE ASKS FOR: "tap an existing highlight to extend". Two highlights of the same
// book and the same spine that OVERLAP — or merely TOUCH, end-to-start with nothing between — are one
// highlight, not two abutting ones, because that is what they look like on the page and a reader who extends a
// highlight by a word did not create a second annotation. add() therefore absorbs every range it meets: the
// survivors are tombstoned and one union row is written, carrying the NEW colour (the most recent statement
// about the passage wins, the same rule the ts merge follows). planMergeIn() is the pure half of that, so the
// exactly-touching case is pinned without a store at all.
//
// CLOUD SYNC. "highlights/" is in CloudSync::isPerItemStoreKey (NOT isDeviceLocalKey): a highlight is a
// statement about the BOOK, not about this device, so it rides the lightweight CloudMerge document with the
// bookmarks. probe_cloudmerge pins the classification; probe_highlights pins the store and the merge.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include "../ebook/ReaderAnchor.h"

namespace HighlightStore
{
    // The fixed palette — FOUR colours, chosen from a menu, exactly as the issue scopes it ("a choice of a few
    // colors"). A small closed set is one press per colour on a pad and needs no colour picker; the INDEX is
    // what is stored, so renaming or re-tinting one later does not rewrite anybody's highlights.
    int     colorCount();
    QString colorName(int index);   // "Yellow" / "Green" / "Blue" / "Pink"; out of range -> the default's name
    QString colorHex(int index);    // "#RRGGBB" for the reader's tint and the panel's swatch
    int     normalizeColor(int index);  // any int -> a valid palette index (a forged/older value reads as 0)

    struct Highlight
    {
        QString      id;       // stable merge id = md5(bookKey | canonical anchor); position-derived
        QString      bookKey;  // the book's natural stable key (its file path / addon item id), pre-hash
        ReaderAnchor anchor;   // the RANGE: spine + offset + endOffset (a highlight is never a point anchor)
        int          color = 0; // index into the fixed palette above
        QString      text;     // the highlighted words, for the annotation panel's excerpt
        qint64       ts = 0;   // epoch seconds of the last write (multi-device merge: newest-ts wins per id)
    };

    // The ini key the active profile's highlight list lives under ("highlights/<profile>/items"), and the
    // tombstone store namespace for it — named here so the store and CloudMerge cannot drift on the spelling.
    QString itemsKey();
    QString tombstoneStore();

    // The deterministic merge id for a highlight covering `anchor` in `bookKey`. Independent of time and of
    // colour, so the same passage always maps to the same id. Empty bookKey -> empty id.
    QString idFor(const QString& bookKey, const ReaderAnchor& anchor);

    // Do two book ranges belong to ONE highlight? Same spine, and overlapping OR exactly touching (the end of
    // one is the start of the other, with nothing between them). Non-range or cross-kind anchors never touch.
    bool touches(const ReaderAnchor& a, const ReaderAnchor& b);

    // What adding `range` would absorb. PURE: it reads the list it is handed and nothing else, so the merge
    // rule is pinned without a store. `merged` is the union of `range` and everything it touches — applied
    // repeatedly, because absorbing one neighbour can bring the union up against the next one.
    struct MergePlan
    {
        ReaderAnchor merged;     // the range to store
        QStringList  absorbed;   // ids of the existing highlights the union swallowed (to be removed)
    };
    MergePlan planMergeIn(const QVector<Highlight>& existing, const ReaderAnchor& range);

    // The same plan against the active profile's stored list for `bookKey`. A caller that needs the merged
    // range BEFORE the add (to extract the excerpt the union covers) asks for it here.
    MergePlan planMerge(const QString& bookKey, const ReaderAnchor& range);

    // Add (or refresh) a highlight, stamped now, absorbing every stored highlight the range touches. `text` is
    // the excerpt for the MERGED range — a caller that may be extending an existing highlight should take it
    // from planMerge()'s `merged`, not from the raw range. Idempotent by position; clears any delete tombstone.
    // A non-range anchor (endOffset < 0) or an empty bookKey is a no-op returning a default Highlight.
    Highlight add(const QString& bookKey, const ReaderAnchor& anchor, int color, const QString& text = QString());

    // Recolour in place: same id, same passage, new colour, fresh ts (so the newest colour wins on merge).
    // No-op for an unknown id.
    void setColor(const QString& id, int color);

    // Remove the highlight with `id` and record a delete tombstone so a peer that still holds it cannot
    // resurrect it on merge. No-op for an empty/unknown id.
    void remove(const QString& id);

    // This book's highlights, sorted in reading order (ReaderAnchor::inReadingOrder). Empty bookKey -> empty.
    QVector<Highlight> list(const QString& bookKey);

    // The highlight covering a spot (spine + offset) in `bookKey`, for "re-select an existing highlight to
    // recolour or remove it". Returns a default-constructed Highlight (empty id) when the spot is not inside
    // one. A spot exactly ON the end offset is NOT inside — the range is half-open, like every other one here.
    Highlight at(const QString& bookKey, int spine, int offset);

    // Every highlight in the active profile (all books), unsorted. For the annotation panel / diagnostics.
    QVector<Highlight> all();

    // Multi-device sync trigger, mirroring BookmarkStore::setChangeHook: fired after every mutation so
    // MainWindow can (re)arm the debounced push. QtCore-clean; unset in probes.
    void setChangeHook(std::function<void()> hook);
}
