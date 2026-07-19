#pragma once
// NavGraph — one selection model + back stack for a themed screen, as a plain QObject (no QML, no widgets,
// so it is unit-testable headlessly; see tools/probe_navqml.cpp). Selection is a (zone, index) pair and can
// NEVER be null once a zone is registered: every mutation reassigns deterministically instead of clearing.
//
// Spatial model: each zone sits in a coarse grid cell (row, col). A zone is a strip of `count` items laid
// along its `axis`. Horizontal (the default): Left/Right step the index within the strip and, at an edge
// (unless the zone wraps), cross to the nearest zone in that column direction; Up/Down always cross by row.
// Vertical (an XMB item column): Up/Down step the index and cross by row only at an edge; Left/Right always
// cross by column. GEOMETRIC zone crossing carries the index (clamped + divider-snapped). "Nearest" =
// smallest primary-axis grid distance in the arrow's direction, then smallest secondary-axis distance, then
// registration order. A hidden zone (count 0) is never a crossing target.
//
// Declared edges (addEdge): a screen can declare "from zone A, this key crosses to zone B" transitions that
// pure geometry cannot express — the themed screens carry TWO independent always-visible cursors (the XMB
// category axis + its item column, co-located in one grid cell) plus focus handoffs (grid -> bottom button
// bar). move() consults declared edges FIRST (exact from-zone + key match; a hidden target makes the edge
// inert), then falls back to the axis/geometric logic above. An edge crossing enters the target zone at its
// REMEMBERED index (below), never the carried index. When the target is CO-LOCATED with the source (same
// grid cell — the two-cursor case) and the arrow runs along the target's axis, the same move continues with
// one step from that remembered index: a single press both switches cursor and moves it (without the fused
// step a cursor switch would eat a press). move() returns whether a zone's DISPLAYED index changed — a pure
// cursor flip whose fused step clamped returns false (it still emits selectionChanged so the host bridge
// tracks the zone), preserving "no visible move, no sound".
//
// Per-zone remembered index: the model records each zone's last index when the selection LEAVES it; entering
// that zone via a declared edge or via reassignment restores it (snapped + clamped). This is the
// NavRing::rememberSelection concept (Nav.h) applied per zone. An explicit select(zone, index) uses the
// given index (that zone's memory then updates when it is next left).
//
// Reassignment order (when the SELECTED zone is removed or its count drops to 0):
//   1. the nearest OTHER zone that still has count > 0, by grid distance to the dead zone (ties: reg order);
//   2. else the default zone (even if hidden);
//   3. else the first-registered zone.
//   Then the new zone's REMEMBERED index is restored (clamped + snapped off any unselectable/divider entry
//   to the nearest selectable one) — reassignment never carries the dead zone's index, so a transient
//   overlay zone (the XMB inline action chooser) that hides itself always lands the selection back on the
//   index its neighbor last displayed. There is no API that can produce a null selection: removeZone on
//   the LAST remaining zone is a refusing no-op, and a registry whose every zone is hidden (all counts 0)
//   intentionally parks the selection at (zone, 0) — that is the terminal state, not a null.
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>
#include <functional>

class NavGraph : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString zone READ zone NOTIFY selectionChanged)
    Q_PROPERTY(int index READ index NOTIFY selectionChanged)
public:
    explicit NavGraph(QObject* parent = nullptr);

    // Zone registry. `count` may change any time via setZoneCount (Repeater data swaps).
    // `row`/`col` place the zone on a coarse grid for spatial arrow resolution (Invariant 3).
    // `axis` is the direction the strip's index runs (Vertical = an XMB item column).
    void registerZone(const QString& id, int count, int row, int col,
                      Qt::Orientation axis = Qt::Horizontal, bool wraps = false);
    // QML-facing overload: Qt::Orientation doesn't marshal cleanly from QML, so the themed elements call this
    // (axis defaults to Horizontal — a 1-count input field has no meaningful strip axis). The C++ registerZone
    // above is untouched. This is the registration path the QML input components use on Component.onCompleted.
    Q_INVOKABLE void registerZoneQml(const QString& id, int count, int row, int col);
    Q_INVOKABLE void setZoneCount(const QString& id, int count);   // 0 hides the zone (selection reassigns away)
    void removeZone(const QString& id);                // refusing no-op on the last remaining zone
    void setDefaultZone(const QString& id);

    // Non-selectable indices (dividers): the resolver skips them; a set index snaps to the
    // nearest selectable — the model owns what QML's seekSelectable did.
    void setUnselectable(const QString& zone, const QSet<int>& indices);
    // QML-friendly divider setter: a JS array of int indices (marshaled as a QVariantList) -> setUnselectable.
    Q_INVOKABLE void setDividers(const QString& zone, const QVariantList& indices);

    // Declared transition: from `fromZone`, key `arrow` crosses to `toZone` (entering at toZone's remembered
    // index; fused step for a co-located target — see the header comment). Consulted before axis/geometric
    // resolution; inert while the target is hidden (count 0). `fromZone` must already be registered; the
    // target may be registered later (the edge simply stays inert until it exists). validate() walks these
    // edges (undirected, both endpoints registered) unioned with the geometric neighbors, so a two-cursor
    // screen whose zones are co-located still forms a connected graph.
    void addEdge(const QString& fromZone, Qt::Key arrow, const QString& toZone);

    QString zone() const;   int index() const;
    // arrow is a Qt::Key (int for QML): declared-edge, then axis, then geometric resolution; returns false
    // if no zone's displayed index changed. Kept int-typed so the themed QML can call nav.move(Qt.Key_Down)
    // directly; C++ callers pass Qt::Key values as before. Non-arrow keys resolve ONLY via declared edges.
    Q_INVOKABLE bool move(int arrow);
    // Request (clamped + divider-snapped). Refused for an unregistered or hidden (count 0) zone — selection
    // can never be steered onto a zone that isn't visible.
    Q_INVOKABLE void select(const QString& zone, int index);
    Q_INVOKABLE void activate();              // emits activated(zone, index)

    // Back stack (Invariant 4). Root behavior: back() at empty stack emits rootBack()
    // (host opens the exit menu). Levels: screens, drills, overlays (esc menu, NavMenu, OSK).
    void pushLevel(const QString& name, std::function<void()> onPop);
    void popLevel();                          // runs onPop
    // Drop the topmost level WITHOUT running its onPop — for the host to keep the graph's level mirror in
    // lockstep with navigation that has ALREADY moved out-of-band (a search/category switch that reset the
    // underlying stack). Guarded by the same re-entrancy latch as pop/push (a no-op mid-onPop).
    void popLevelSilent();
    int  levelDepth() const;
    int  countLevels(const QString& name) const;   // how many live levels carry this name (mirror bookkeeping)
    bool isPopping() const;                          // true while an onPop runs (mirror reconcile must stand off)
    Q_INVOKABLE bool back();                   // pops one level, or emits rootBack(); always true

    // Invariant 2: the zone graph — the UNION of geometric neighbors and declared edges (undirected) —
    // is connected from the default zone, and counts/indices are sane.
    bool validate(QString* whyNot = nullptr) const;

signals:
    void selectionChanged(const QString& zone, int index);
    void activated(const QString& zone, int index);
    void rootBack();
    void backInvoked();               // every back() gesture (pop OR rootBack) — the host plays the back sound here
    void levelsChanged(int depth);

private:
    struct Zone { int count = 0; int row = 0; int col = 0; Qt::Orientation axis = Qt::Horizontal;
                  bool wraps = false; int order = 0; QSet<int> unsel;
                  int memory = 0;                                  // last index when the selection left (NavRing::rememberSelection per zone)
                  QVector<QPair<int, QString>> edges; };           // declared transitions: (key, target zone)
    struct Level { QString name; std::function<void()> onPop; };

    // Grid resolution helpers.
    int snapIndex(const QString& zone, int idx) const;             // clamp into count + snap off dividers
    int stepSelectable(const QString& zone, int from, int dir) const; // next selectable in a direction, or -1
    QString nearestZone(const QString& from, int dRow, int dCol) const; // nearest zone in an arrow direction
    QString nearestPositiveZone(const QString& deadId) const;      // nearest zone with count > 0
    void reassignFrom(const QString& deadId);                      // re-home the selection off a dead zone
    void setSelection(const QString& zone, int idx);               // set + emit selectionChanged if changed
    bool crossByEdge(const QString& target, int dRow, int dCol);   // declared-edge crossing (+ fused co-located step)

    QHash<QString, Zone> m_zones;
    QStringList m_order;          // registration order (tie-break + "first zone")
    QString m_defaultZone;
    QString m_zone;
    int m_index = 0;

    QVector<Level> m_stack;
    bool m_popping = false;       // onPop re-entrancy guard: push AND pop from inside an onPop are no-ops
};
