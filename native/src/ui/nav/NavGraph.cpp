#include "NavGraph.h"

#include <algorithm>
#include <climits>

NavGraph::NavGraph(QObject* parent) : QObject(parent) {}

// ---------------------------------------------------------------------------------------- registry

void NavGraph::registerZone(const QString& id, int count, int row, int col, Qt::Orientation axis, bool wraps)
{
    if (id.isEmpty()) return;
    Zone z;
    z.count = std::max(0, count);
    z.row = row;
    z.col = col;
    z.axis = axis;
    z.wraps = wraps;
    const bool isNew = !m_zones.contains(id);
    if (isNew) { z.order = m_order.size(); m_order.push_back(id); }
    else       { const Zone& old = m_zones[id];   // a rebuild re-registers: keep identity + wiring + memory
                 z.order = old.order; z.unsel = old.unsel; z.memory = old.memory; z.edges = old.edges; }
    m_zones[id] = z;

    if (m_defaultZone.isEmpty()) m_defaultZone = id;
    // First zone ever, or the current selection is empty: adopt this one.
    if (m_zone.isEmpty()) { setSelection(id, 0); return; }
    // Re-registering the CURRENTLY SELECTED zone: the count may have shrunk (or dropped to 0) —
    // re-snap the held index, or reassign away if the zone just hid itself.
    if (id == m_zone) {
        if (z.count == 0) reassignFrom(id);
        else setSelection(id, snapIndex(id, m_index));
    }
}

void NavGraph::setZoneCount(const QString& id, int count)
{
    auto it = m_zones.find(id);
    if (it == m_zones.end()) return;
    it->count = std::max(0, count);
    if (id == m_zone) {
        if (it->count == 0) reassignFrom(id);            // hidden — reassign away
        else setSelection(id, snapIndex(id, m_index));   // clamp/snap into the new count
    }
}

void NavGraph::removeZone(const QString& id)
{
    if (!m_zones.contains(id)) return;
    if (m_zones.size() == 1) return;   // refusing no-op: the last zone can never be removed (no null state)
    // Compute the successor BEFORE erasing: nearestPositiveZone needs the dead zone's grid coords.
    const bool wasSelected = (m_zone == id);
    QString target = wasSelected ? nearestPositiveZone(id) : QString();
    m_zones.remove(id);
    m_order.removeAll(id);
    if (m_defaultZone == id) m_defaultZone = m_order.front();
    if (wasSelected) {
        if (target.isEmpty()) target = m_zones.contains(m_defaultZone) ? m_defaultZone : m_order.front();
        // Restore the successor's REMEMBERED index (never the dead zone's carried index) — see header.
        setSelection(target, snapIndex(target, m_zones[target].memory));
    }
}

void NavGraph::setDefaultZone(const QString& id)
{
    if (m_zones.contains(id)) m_defaultZone = id;
}

void NavGraph::setUnselectable(const QString& zone, const QSet<int>& indices)
{
    auto it = m_zones.find(zone);
    if (it == m_zones.end()) return;
    it->unsel = indices;
    if (zone == m_zone) setSelection(zone, snapIndex(zone, m_index)); // current index may now be a divider
}

void NavGraph::setDividers(const QString& zone, const QVariantList& indices)
{
    QSet<int> s;
    for (const QVariant& v : indices) s.insert(v.toInt());
    setUnselectable(zone, s);
}

void NavGraph::addEdge(const QString& fromZone, Qt::Key arrow, const QString& toZone)
{
    auto it = m_zones.find(fromZone);
    if (it == m_zones.end() || toZone.isEmpty() || fromZone == toZone) return;
    for (const auto& e : it->edges)
        if (e.first == int(arrow) && e.second == toZone) return;   // idempotent
    it->edges.push_back({ int(arrow), toZone });
}

// ---------------------------------------------------------------------------------------- accessors

QString NavGraph::zone() const { return m_zone; }
int NavGraph::index() const { return m_index; }

// ---------------------------------------------------------------------------------------- index snapping

// Clamp `idx` into [0, count) then, if it landed on a divider, walk outward (nearest-first) to the closest
// selectable index. If every index is a divider, return the clamped index. Bounded by count → terminates.
int NavGraph::snapIndex(const QString& zone, int idx) const
{
    auto it = m_zones.constFind(zone);
    if (it == m_zones.constEnd() || it->count <= 0) return 0;
    const int n = it->count;
    int c = std::clamp(idx, 0, n - 1);
    const QSet<int>& u = it->unsel;
    if (!u.contains(c)) return c;
    for (int d = 1; d < n; ++d) {
        if (c - d >= 0 && !u.contains(c - d)) return c - d;
        if (c + d < n  && !u.contains(c + d)) return c + d;
    }
    return c; // all dividers — nothing selectable
}

// Next selectable index strictly in direction `dir` (+1/-1) from `from`, or -1 if the edge is hit first.
int NavGraph::stepSelectable(const QString& zone, int from, int dir) const
{
    auto it = m_zones.constFind(zone);
    if (it == m_zones.constEnd()) return -1;
    const int n = it->count;
    for (int i = from + dir; i >= 0 && i < n; i += dir)
        if (!it->unsel.contains(i)) return i;
    return -1;
}

// ---------------------------------------------------------------------------------------- spatial resolution

// Nearest zone in an arrow direction. (dRow,dCol) is the unit arrow vector; exactly one component is
// non-zero. Candidates are zones strictly past `from` along that axis; rank by (primary distance, secondary
// distance, registration order).
QString NavGraph::nearestZone(const QString& from, int dRow, int dCol) const
{
    auto fit = m_zones.constFind(from);
    if (fit == m_zones.constEnd()) return QString();
    const int fr = fit->row, fc = fit->col;
    QString best;
    int bestPrimary = INT_MAX, bestSecondary = INT_MAX, bestOrder = INT_MAX;
    for (auto it = m_zones.constBegin(); it != m_zones.constEnd(); ++it) {
        if (it.key() == from) continue;
        const int dr = it->row - fr, dc = it->col - fc;
        int primary, secondary;
        if (dRow != 0) { if (dr * dRow <= 0) continue; primary = std::abs(dr); secondary = std::abs(dc); }
        else           { if (dc * dCol <= 0) continue; primary = std::abs(dc); secondary = std::abs(dr); }
        if (primary < bestPrimary ||
            (primary == bestPrimary && (secondary < bestSecondary ||
             (secondary == bestSecondary && it->order < bestOrder)))) {
            bestPrimary = primary; bestSecondary = secondary; bestOrder = it->order; best = it.key();
        }
    }
    return best;
}

QString NavGraph::nearestPositiveZone(const QString& deadId) const
{
    auto dit = m_zones.constFind(deadId);
    const int dr = (dit != m_zones.constEnd()) ? dit->row : 0;
    const int dc = (dit != m_zones.constEnd()) ? dit->col : 0;
    QString best;
    int bestDist = INT_MAX, bestOrder = INT_MAX;
    for (auto it = m_zones.constBegin(); it != m_zones.constEnd(); ++it) {
        if (it.key() == deadId || it->count <= 0) continue;
        const int d = std::abs(it->row - dr) + std::abs(it->col - dc);
        if (d < bestDist || (d == bestDist && it->order < bestOrder)) {
            bestDist = d; bestOrder = it->order; best = it.key();
        }
    }
    return best;
}

void NavGraph::reassignFrom(const QString& deadId)
{
    QString target = nearestPositiveZone(deadId);
    if (target.isEmpty() && m_zones.contains(m_defaultZone)) target = m_defaultZone;
    if (target.isEmpty() && !m_order.isEmpty()) target = m_order.front();
    if (target.isEmpty()) { m_zone.clear(); m_index = 0; emit selectionChanged(m_zone, m_index); return; }
    // Restore the target's REMEMBERED index, never the dead zone's carried index — so a transient overlay
    // zone (the inline action chooser) hiding itself lands the selection back where its neighbor left off.
    setSelection(target, snapIndex(target, m_zones[target].memory));
}

void NavGraph::setSelection(const QString& zone, int idx)
{
    if (m_zone == zone && m_index == idx) return;
    // Record the departed zone's last index (NavRing::rememberSelection per zone): declared-edge crossings
    // and reassignment restore it.
    if (m_zone != zone) {
        auto it = m_zones.find(m_zone);
        if (it != m_zones.end()) it->memory = m_index;
    }
    m_zone = zone;
    m_index = idx;
    emit selectionChanged(m_zone, m_index);
}

// ---------------------------------------------------------------------------------------- movement

// A declared-edge crossing: enter `target` at its REMEMBERED index; if the target is co-located with the
// source (the two-cursor case) and the arrow runs along the target's axis, continue with one step from
// that index (divider-skipped; wrap if the target wraps) — one press = switch cursor AND move it.
// Returns whether the target's DISPLAYED index changed (a pure cursor flip returns false); the selection
// (zone) changes either way, emitting selectionChanged for the host bridge.
bool NavGraph::crossByEdge(const QString& target, int dRow, int dCol)
{
    const Zone& from = m_zones[m_zone];
    const Zone& t = m_zones[target];
    const int displayed = t.memory;                       // what the host last showed for this zone
    int final_ = snapIndex(target, t.memory);
    const bool coLocated = (from.row == t.row && from.col == t.col);
    const int along = (t.axis == Qt::Horizontal) ? dCol : dRow;
    if (coLocated && along != 0) {
        int ni = stepSelectable(target, final_, along);
        if (ni >= 0) final_ = ni;
        else if (t.wraps && t.count > 0) {
            int wrap = (along > 0) ? snapIndex(target, 0) : snapIndex(target, t.count - 1);
            final_ = wrap;
        }
    }
    setSelection(target, final_);
    return final_ != displayed;
}

bool NavGraph::move(int arrow)
{
    if (m_zone.isEmpty()) return false;
    int dRow = 0, dCol = 0;
    switch (arrow) {
        case Qt::Key_Up:    dRow = -1; break;
        case Qt::Key_Down:  dRow =  1; break;
        case Qt::Key_Left:  dCol = -1; break;
        case Qt::Key_Right: dCol =  1; break;
        default: break;   // non-arrow keys resolve ONLY via declared edges below
    }

    // 1. Declared edges first (exact from-zone + key match). A hidden target (count 0) makes the edge
    //    inert — resolution falls through to the axis/geometric logic, so e.g. the grid themes' Down-to-
    //    buttons edge simply doesn't exist on an XMB theme whose button bar is empty.
    {
        const Zone& z = m_zones[m_zone];
        for (const auto& e : z.edges) {
            if (e.first != arrow) continue;
            auto tit = m_zones.constFind(e.second);
            if (tit == m_zones.constEnd() || tit->count <= 0) continue;
            return crossByEdge(e.second, dRow, dCol);
        }
    }
    if (dRow == 0 && dCol == 0) return false;   // a non-arrow key with no matching edge

    // 2. An arrow along the SELECTED zone's axis steps the index within the strip (Horizontal:
    // Left/Right; Vertical: Up/Down); it only crosses zones at an edge. The cross-axis arrow always
    // crosses zones.
    const Zone& z = m_zones[m_zone];
    const int along = (z.axis == Qt::Horizontal) ? dCol : dRow;
    if (along != 0) {
        int ni = stepSelectable(m_zone, m_index, along);
        if (ni >= 0) { setSelection(m_zone, ni); return true; }
        if (z.wraps && z.count > 0) {
            int wrap = (along > 0) ? snapIndex(m_zone, 0) : snapIndex(m_zone, z.count - 1);
            if (wrap != m_index) { setSelection(m_zone, wrap); return true; }
        }
    }

    // 3. Geometric crossing (carries the index). Hidden zones are not navigable targets.
    QString nz = nearestZone(m_zone, dRow, dCol);
    if (nz.isEmpty()) return false;
    if (m_zones[nz].count <= 0) return false;
    int ni = snapIndex(nz, m_index);
    if (nz == m_zone && ni == m_index) return false;
    setSelection(nz, ni);
    return true;
}

void NavGraph::select(const QString& zone, int index)
{
    auto it = m_zones.constFind(zone);
    if (it == m_zones.constEnd() || it->count <= 0) return;   // can't steer onto a missing/hidden zone
    setSelection(zone, snapIndex(zone, index));
}

void NavGraph::activate()
{
    if (!m_zone.isEmpty()) emit activated(m_zone, m_index);
}

// ---------------------------------------------------------------------------------------- back stack

void NavGraph::pushLevel(const QString& name, std::function<void()> onPop)
{
    if (m_popping) return;                 // a push from inside an onPop is ignored (no re-push loops)
    m_stack.push_back({name, std::move(onPop)});
    emit levelsChanged(m_stack.size());
}

void NavGraph::popLevel()
{
    if (m_popping) return;                 // a reentrant pop from inside an onPop is a no-op (symmetry)
    if (m_stack.isEmpty()) return;
    Level lvl = m_stack.back();
    m_stack.pop_back();
    if (lvl.onPop) {
        const bool wasPopping = m_popping;
        m_popping = true;
        lvl.onPop();
        m_popping = wasPopping;
    }
    emit levelsChanged(m_stack.size());
}

void NavGraph::popLevelSilent()
{
    if (m_popping) return;                 // symmetry with pop/push: no mirror churn from inside an onPop
    if (m_stack.isEmpty()) return;
    m_stack.pop_back();                    // drop it WITHOUT running onPop (navigation already moved)
    emit levelsChanged(m_stack.size());
}

int NavGraph::levelDepth() const { return m_stack.size(); }

int NavGraph::countLevels(const QString& name) const
{
    int n = 0;
    for (const Level& l : m_stack) if (l.name == name) ++n;
    return n;
}

bool NavGraph::isPopping() const { return m_popping; }

bool NavGraph::back()
{
    emit backInvoked();                    // fires for BOTH a level pop and a rootBack (the host's back sound)
    if (!m_stack.isEmpty()) popLevel();
    else emit rootBack();
    return true;
}

// ---------------------------------------------------------------------------------------- validation

bool NavGraph::validate(QString* whyNot) const
{
    auto fail = [&](const QString& msg) { if (whyNot) *whyNot = msg; return false; };

    if (m_zones.isEmpty()) return true;    // nothing registered yet — vacuously valid
    if (m_zone.isEmpty() || !m_zones.contains(m_zone))
        return fail(QStringLiteral("selection points at no live zone"));

    // Counts sane + the selected index in range / not a divider (when the zone is non-empty).
    const Zone& sel = m_zones[m_zone];
    for (auto it = m_zones.constBegin(); it != m_zones.constEnd(); ++it)
        if (it->count < 0) return fail(QStringLiteral("negative count on ") + it.key());
    if (sel.count > 0) {
        if (m_index < 0 || m_index >= sel.count)
            return fail(QStringLiteral("index out of range in ") + m_zone);
        if (sel.unsel.contains(m_index) && sel.unsel.size() < sel.count)
            return fail(QStringLiteral("index sits on a divider in ") + m_zone);
    }

    // Connectivity: BFS from the default zone over the UNION of geometric nearest-neighbor edges
    // (4 directions) and declared edges. Both edge kinds count as undirected here — connectivity is a
    // structural property (a declared A->B transition proves the pair is one navigable surface even when
    // the reverse leg is an activation/dismissal rather than an arrow, e.g. the inline action chooser).
    QHash<QString, QSet<QString>> declared;   // undirected declared adjacency (both endpoints registered)
    for (auto it = m_zones.constBegin(); it != m_zones.constEnd(); ++it)
        for (const auto& e : it->edges)
            if (m_zones.contains(e.second)) { declared[it.key()].insert(e.second); declared[e.second].insert(it.key()); }

    QString start = m_zones.contains(m_defaultZone) ? m_defaultZone : m_order.front();
    QSet<QString> seen{start};
    QVector<QString> q{start};
    while (!q.isEmpty()) {
        QString cur = q.takeFirst();
        const int dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (auto& d : dirs) {
            QString nb = nearestZone(cur, d[0], d[1]);
            if (!nb.isEmpty() && !seen.contains(nb)) { seen.insert(nb); q.push_back(nb); }
        }
        for (const QString& nb : declared.value(cur))
            if (!seen.contains(nb)) { seen.insert(nb); q.push_back(nb); }
    }
    if (seen.size() != m_zones.size())
        return fail(QStringLiteral("zone graph is not connected"));
    return true;
}
