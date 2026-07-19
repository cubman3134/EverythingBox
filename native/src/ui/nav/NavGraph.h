#pragma once
// NavGraph — one selection model + back stack for a themed screen, as a plain QObject (no QML, no widgets,
// so it is unit-testable headlessly; see tools/probe_navqml.cpp). Selection is a (zone, index) pair and can
// NEVER be null once a zone is registered: every mutation reassigns deterministically instead of clearing.
//
// Spatial model: each zone sits in a coarse grid cell (row, col). A zone is a strip of `count` items laid
// along its `axis`. Horizontal (the default): Left/Right step the index within the strip and, at an edge
// (unless the zone wraps), cross to the nearest zone in that column direction; Up/Down always cross by row.
// Vertical (an XMB item column): Up/Down step the index and cross by row only at an edge; Left/Right always
// cross by column. Zone crossing carries the index (clamped + divider-snapped). "Nearest" = smallest
// primary-axis grid distance in the arrow's direction, then smallest secondary-axis distance, then
// registration order.
//
// Reassignment order (when the SELECTED zone is removed or its count drops to 0):
//   1. the nearest OTHER zone that still has count > 0, by grid distance to the dead zone (ties: reg order);
//   2. else the default zone (even if hidden);
//   3. else the first-registered zone.
//   Then the carried index is clamped into the new zone's count and snapped off any unselectable (divider)
//   entry to the nearest selectable one. There is no API that can produce a null selection: removeZone on
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
    Q_INVOKABLE void setZoneCount(const QString& id, int count);   // 0 hides the zone (selection reassigns away)
    void removeZone(const QString& id);                // refusing no-op on the last remaining zone
    void setDefaultZone(const QString& id);

    // Non-selectable indices (dividers): the resolver skips them; a set index snaps to the
    // nearest selectable — the model owns what QML's seekSelectable did.
    void setUnselectable(const QString& zone, const QSet<int>& indices);
    // QML-friendly divider setter: a JS array of int indices (marshaled as a QVariantList) -> setUnselectable.
    Q_INVOKABLE void setDividers(const QString& zone, const QVariantList& indices);

    QString zone() const;   int index() const;
    // arrow is a Qt::Key (int for QML): spatial step; returns false if nothing changed. Kept int-typed so the
    // themed QML can call nav.move(Qt.Key_Down) directly; C++ callers pass Qt::Key values as before.
    Q_INVOKABLE bool move(int arrow);
    Q_INVOKABLE void select(const QString& zone, int index); // request (clamped + divider-snapped)
    Q_INVOKABLE void activate();              // emits activated(zone, index)

    // Back stack (Invariant 4). Root behavior: back() at empty stack emits rootBack()
    // (host opens the exit menu). Levels: screens, drills, overlays (esc menu, NavMenu, OSK).
    void pushLevel(const QString& name, std::function<void()> onPop);
    void popLevel();                          // runs onPop
    int  levelDepth() const;
    Q_INVOKABLE bool back();                   // pops one level, or emits rootBack(); always true

    bool validate(QString* whyNot = nullptr) const; // Invariant 2: zone graph connected, counts sane

signals:
    void selectionChanged(const QString& zone, int index);
    void activated(const QString& zone, int index);
    void rootBack();
    void levelsChanged(int depth);

private:
    struct Zone { int count = 0; int row = 0; int col = 0; Qt::Orientation axis = Qt::Horizontal;
                  bool wraps = false; int order = 0; QSet<int> unsel; };
    struct Level { QString name; std::function<void()> onPop; };

    // Grid resolution helpers.
    int snapIndex(const QString& zone, int idx) const;             // clamp into count + snap off dividers
    int stepSelectable(const QString& zone, int from, int dir) const; // next selectable in a direction, or -1
    QString nearestZone(const QString& from, int dRow, int dCol) const; // nearest zone in an arrow direction
    QString nearestPositiveZone(const QString& deadId) const;      // nearest zone with count > 0
    void reassignFrom(const QString& deadId);                      // re-home the selection off a dead zone
    void setSelection(const QString& zone, int idx);               // set + emit selectionChanged if changed

    QHash<QString, Zone> m_zones;
    QStringList m_order;          // registration order (tie-break + "first zone")
    QString m_defaultZone;
    QString m_zone;
    int m_index = 0;

    QVector<Level> m_stack;
    bool m_popping = false;       // onPop re-entrancy guard: push AND pop from inside an onPop are no-ops
};
