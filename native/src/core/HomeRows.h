// Custom home rows (issue #161): a per-profile, synced list that says WHICH rows the home screen shows, in
// what order, and how many items each one may show. The rows themselves are producers the app already has —
// this file adds no content, only an arrangement over content.
//
// TWO HALVES, and they are deliberately separable:
//   * `homerows::plan()` is PURE. It takes the rows a surface can actually produce RIGHT NOW (in the order
//     the app produces them today) plus the stored list, and returns the render plan. No store, no profile,
//     no UI — a probe drives it with hand-built vectors.
//   * `HomeRowStore` is the per-profile QSettings wrapper, the same posture FavoritesStore/FilterPresetStore
//     have (QtCore only, keyed by the active profile id, riding the shared portable everythingbox.ini).
//
// THE LOAD-BEARING RULE: an EMPTY list renders exactly what the app renders today. plan() with no stored
// rows returns `available` verbatim, in order, uncapped — so an untouched profile's home is byte-for-byte the
// home it had before this existed. The list only comes into existence when the user edits it, and "Reset to
// default" puts it back to empty. Every other rule below is layered on top of that one and none of them can
// reach a profile that has not opted in.
//
// ROW IDS name a producer, not a widget. The vocabulary is deliberately open — an id this build does not
// recognise is KEPT in the stored list and merely SKIPPED at render (see plan()), never dropped. That single
// rule covers four different situations that would otherwise each need their own: a preset the user deleted,
// an add-on that was removed, a peer device that has a producer this one does not, and a row that belongs to
// the OTHER layout's home. A device that drops what it cannot render would quietly erase a peer's row on the
// next sync, which is the one outcome a synced list must never produce.
//
//   continue            the recently-played shelf (this app's Continue Watching: RecentStore, with resume %)
//   favorites           the ★ Favorites shelf
//   downloads           the fully-downloaded items shelf
//   new                 "New"           (#155): followed series' unseen children, UNIONED with #25's rows
//   trakt:calendar      "Airing Soon"   (#23)
//   playlist:<id>       one saved playlist's items (#playlists)
//   preset:<id>         one #63 saved filter, evaluated over the rows the home already holds
//   category:<key>      one media-type bucket on the themed home ("video" | "game" | "audio" | …)
//   source:<navKey>     one catalogue's row on the themed home (the nav key = the catalogue id)
//   recents, trakt:missed
//                       accepted vocabulary with no producer in this build — kept and skipped (see above).
//                       "trakt:missed" HAD one until #155: the New shelf absorbed those rows, so a stored
//                       list that still names it keeps it and skips it like any other unrecognised id.
//
// CAP: the maximum number of ITEMS the row may show; 0 means "no cap", which is what today does. A row whose
// producer yields a single tile (category:/source:) stores a cap like any other and ignores it — the editor
// only offers the cap action where a cap can change anything.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

class QJsonObject;

namespace homerows
{
    // One entry of the stored list.
    struct Row
    {
        QString rowId;
        bool    visible = true;
        int     cap     = 0;   // max items; 0 = uncapped (today's behaviour)
    };

    // A row a surface can produce right now, in the order the app produces it TODAY. `count` is how many
    // items the producer has — carried so a caller can show it in the editor; plan() does not read it.
    struct Available
    {
        QString rowId;
        int     count = 0;
    };

    // The render plan for one surface: the rows to draw, in order, each with the cap to apply.
    struct Planned
    {
        QString rowId;
        int     cap = 0;
    };

    // Order/hide/cap `available` by `list`.
    //
    //   * an EMPTY list returns `available` verbatim and uncapped — default == today (see the header note);
    //   * otherwise the listed rows render in the LIST's order, hidden ones dropped, each capped;
    //   * a listed row this surface cannot produce is skipped (and stays in the store — never dropped);
    //   * a producible row the list has never heard of is APPENDED, in the app's default order, so a producer
    //     that appears after the list was written (a new console, a Trakt account, another device's add-on)
    //     shows up instead of silently not existing. It lands at the end because the list, not the app, owns
    //     the order now — the app has no place to claim in it.
    //   * a duplicated rowId renders once (the first entry wins).
    QVector<Planned> plan(const QVector<Available>& available, const QVector<Row>& list);

    // The home's BUILT-IN shelf rows, in the order the app has always produced them. HomeView iterates this
    // list to assemble what it hands plan() as `available`, so the order on screen IS the order pinned here —
    // rather than a constant in one file that a probe reads and a loop in another file that a user sees.
    const QStringList& defaultShelfOrder();

    // A shelf the home CAN produce but does not show unless the list asks for it. These are the rows "Add
    // row…" offers; they are deliberately absent from `available` until the list names one, because plan()
    // appends any producible row the list has not heard of — so an always-available Downloads producer would
    // grow a Downloads row on every untouched profile in the world, which is exactly what must not happen.
    bool isOptInShelf(const QString& rowId);

    // ---- the synced document ------------------------------------------------------------------------------
    // Whole-list last-writer-wins on `updatedAt`, with a UNION of the row SET so neither side loses a row.
    struct Doc
    {
        qint64       updatedAt = 0;
        QVector<Row> rows;
    };

    Doc         fromJson(const QJsonObject& o);
    QJsonObject toJson(const Doc& d);

    // Fold two devices' documents. The NEWER `updatedAt` decides the ORDER, the visibility and the caps —
    // last-writer-wins, which is the honest rule for an ordering (there is no meaningful way to merge two
    // orders, and pretending otherwise produces a third order neither user asked for). Rows the loser knows
    // and the winner does not are APPENDED in the loser's order, so a row is never lost on either side: two
    // devices that each added a different row end up with both.
    //
    // ONE DELIBERATE EXCEPTION, and it is the same husk MetaOverrides uses: a winner with an EMPTY row list
    // is a RESET, and a reset clears. Unioning there would let the peer's copy put back the list the user
    // just reset — the reset could then never propagate at all. An empty document that is NOT the winner
    // contributes nothing and is harmless.
    //
    // Order-independent: the winner is chosen by `updatedAt` (equal stamps break on the lexically greater
    // canonical JSON bytes, the tie-break the rest of the merge document uses), never by argument position,
    // so merge(a,b) and merge(b,a) agree.
    Doc merge(const Doc& local, const Doc& remote);

    // Is `id` a rowId this build knows how to render? Purely informational — plan() does NOT gate on it (an
    // unknown id is skipped because no surface offers it, which is the same answer without a second list to
    // keep in step). The editor uses it to label a stale entry.
    bool isKnownRowId(const QString& id);
}

// The per-profile store. Empty list == the default layout; the key is absent until the user edits.
namespace HomeRowStore
{
    QVector<homerows::Row> list();                       // active profile; {} = default layout
    void save(const QVector<homerows::Row>& rows);       // stamps updatedAt = now
    void reset();                                        // back to default: an empty list with a fresh stamp
    bool isCustomised();                                 // has this profile got a non-empty list?

    // UI refresh hook, mirroring FavoritesStore/FilterPresetStore: fired after every mutation. A
    // std::function, not a Qt signal (the store stays QtCore-clean); unset in probes.
    void setChangeHook(std::function<void()> hook);
}
