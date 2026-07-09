#include "Nav.h"
#include "NavOverlay.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QTextEdit>
#include <QTimer>
#include <algorithm>
#include <limits>

// ---------------------------------------------------------------- NavRing

NavRing::NavRing(QWidget* container, QObject* parent)
    : QObject(parent), container_(container)
{
}

// A widget is a ring member when a user could land on it: visible, enabled, and focusable by Tab (which
// covers buttons, checkboxes, combos, sliders, line edits). Scrollbars/viewports are skipped — arrows act
// on rows, and QScrollArea::ensureWidgetVisible keeps the focused row in view. A plain scroll CONTAINER
// (QScrollArea holds StrongFocus by default!) is skipped too: its ROWS are the stops — treating it as a
// member made the nested filter below drop every row inside it, and the watchdog then kept snapping the
// selection to the first ring widget (the settings panels' Back button). Item views (lists/tables) stay
// members: there the view itself IS the row-carrying stop.
static bool ringMember(const QWidget* w)
{
    if (!w || !w->isVisible() || !w->isEnabled()) return false;
    if (!(w->focusPolicy() & Qt::TabFocus)) return false;
    if (qobject_cast<const QScrollBar*>(w)) return false;
    if (qobject_cast<const QAbstractScrollArea*>(w) && !qobject_cast<const QAbstractItemView*>(w))
        return false;
    return true;
}

// The current focus, tolerant of the window being INACTIVE: UI-test injection drives the app in the
// background, where QApplication::focusWidget() is null — but the window still remembers its focus child,
// which is where the selection really is.
static QWidget* focusIn(const QWidget* container)
{
    QWidget* fw = QApplication::focusWidget();
    if (!fw && container && container->window()) fw = container->window()->focusWidget();
    return fw;
}

QVector<QWidget*> NavRing::widgets() const
{
    QVector<QWidget*> out;
    if (!container_) return out;
    const QList<QWidget*> all = container_->findChildren<QWidget*>();
    for (QWidget* w : all)
        if (ringMember(w)) out.push_back(w);
    // A compound row (a spinbox/combo with its internal line edit, a list's viewport…) is ONE ring member:
    // drop anything nested inside another member, or arrows would stop twice on the same row and Enter
    // would open the wrong editor.
    for (int i = out.size() - 1; i >= 0; --i)
        for (QWidget* p = out[i]->parentWidget(); p && p != container_; p = p->parentWidget())
            if (out.contains(p)) { out.remove(i); break; }
    // Geometry order (top-to-bottom, then left-to-right) so "first" and remember-by-index are stable.
    std::sort(out.begin(), out.end(), [this](QWidget* a, QWidget* b) {
        const QPoint pa = a->mapTo(container_, QPoint(0, 0));
        const QPoint pb = b->mapTo(container_, QPoint(0, 0));
        if (pa.y() != pb.y()) return pa.y() < pb.y();
        return pa.x() < pb.x();
    });
    return out;
}

QWidget* NavRing::pickNext(QWidget* from, const QVector<QWidget*>& candidates, int key)
{
    if (!from) return candidates.isEmpty() ? nullptr : candidates.first();
    const QPoint c = from->mapToGlobal(from->rect().center());
    QWidget* best = nullptr;
    double bestScore = std::numeric_limits<double>::max();
    for (QWidget* w : candidates)
    {
        if (w == from) continue;
        const QPoint p = w->mapToGlobal(w->rect().center());
        int primary = 0, orth = 0;
        switch (key)
        {
        case Qt::Key_Up:    primary = c.y() - p.y(); orth = qAbs(p.x() - c.x()); break;
        case Qt::Key_Down:  primary = p.y() - c.y(); orth = qAbs(p.x() - c.x()); break;
        case Qt::Key_Left:  primary = c.x() - p.x(); orth = qAbs(p.y() - c.y()); break;
        case Qt::Key_Right: primary = p.x() - c.x(); orth = qAbs(p.y() - c.y()); break;
        default: return nullptr;
        }
        if (primary <= 0) continue;                      // not in that direction
        // Prefer the geometrically nearest widget, weighting sideways drift heavily so a grid steps within
        // its column/row instead of diagonally.
        const double score = primary + 2.5 * orth;
        if (score < bestScore) { bestScore = score; best = w; }
    }
    return best;
}

bool NavRing::step(int key)
{
    const QVector<QWidget*> ring = widgets();
    if (ring.isEmpty()) return false;
    QWidget* cur = focusIn(container_);
    if (!cur || !ring.contains(cur)) { ensureSelection(); return true; }
    QWidget* next = pickNext(cur, ring, key);
    if (!next) return false; // at an edge: not consumed (the screen may hop zones or just stay put)
    next->setFocus(Qt::OtherFocusReason);
    lastFocus_ = next;
    lastCenter_ = next->mapTo(container_, next->rect().center());
    // Keep the focused row in view when the ring lives in a scroll area.
    for (QWidget* p = next->parentWidget(); p; p = p->parentWidget())
        if (auto* sc = qobject_cast<QScrollArea*>(p)) { sc->ensureWidgetVisible(next, 24, 24); break; }
    return true;
}

void NavRing::activate(QWidget* w)
{
    if (auto* btn = qobject_cast<QAbstractButton*>(w)) { btn->click(); return; }
    if (auto* combo = qobject_cast<QComboBox*>(w)) { combo->showPopup(); return; }
    if (auto* edit = qobject_cast<QLineEdit*>(w)) { NavOverlay::editLineEdit(edit); return; }
    if (auto* spin = qobject_cast<QAbstractSpinBox*>(w)) { NavOverlay::editSpinBox(spin); return; }
    // Anything else: synthesize the Return it would have received.
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
}

bool NavRing::handleKey(int key)
{
    switch (key)
    {
    case Qt::Key_Up: case Qt::Key_Down: case Qt::Key_Left: case Qt::Key_Right:
    {
        QWidget* cur = focusIn(container_);
        // A slider row edits its value with Left/Right — hand those through instead of moving. (Spinners
        // are NOT value-edited by arrows: they hover like everything else and Enter opens the OSK.)
        if ((key == Qt::Key_Left || key == Qt::Key_Right) && qobject_cast<QSlider*>(cur))
            return false;
        // Inside a list, Up/Down move the list's own current row; the ring only takes over when the
        // selection is already at the first/last row (stepping out to the widgets around it).
        if (auto* view = qobject_cast<QAbstractItemView*>(cur);
            view && view->model() && (key == Qt::Key_Up || key == Qt::Key_Down))
        {
            const int row = view->currentIndex().row();
            const int count = view->model()->rowCount(view->rootIndex());
            const bool atEdge = (key == Qt::Key_Up && row <= 0)
                                || (key == Qt::Key_Down && row >= count - 1);
            if (!atEdge && count > 0) return false; // the view itself moves its row
        }
        step(key); // even at an edge the key is consumed: arrows never leak into a page behind
        return true;
    }
    case Qt::Key_Return: case Qt::Key_Enter:
    {
        QWidget* cur = focusIn(container_);
        const QVector<QWidget*> ring = widgets();
        if (!cur || !ring.contains(cur)) { ensureSelection(); return true; }
        activate(cur);
        return true;
    }
    default:
        return false; // Backspace/Escape belong to the screen's back action (NavContext)
    }
}

QWidget* NavRing::ensureSelection()
{
    const QVector<QWidget*> ring = widgets();
    if (ring.isEmpty()) return nullptr;
    QWidget* cur = focusIn(container_);
    if (cur && ring.contains(cur))
    {
        lastFocus_ = cur;
        lastCenter_ = cur->mapTo(container_, cur->rect().center());
        return cur; // selection is already valid
    }
    QWidget* pick = nullptr;
    if (lastFocus_ && ring.contains(lastFocus_)) pick = lastFocus_;
    // Reference point for "nearest": where the user last was — the still-focused non-member widget when
    // there is one (recovery should land beside it, not at the top), else the last ring position.
    QPoint ref = lastCenter_;
    if (cur && container_ && container_->isAncestorOf(cur))
        ref = cur->mapTo(container_, cur->rect().center());
    if (!pick && !ref.isNull())
    {
        // The remembered widget died (row deleted, list rebuilt): land on the nearest survivor so the
        // selector stays where the user was looking instead of snapping to the top.
        double bestD = std::numeric_limits<double>::max();
        for (QWidget* w : ring)
        {
            const QPoint p = w->mapTo(container_, w->rect().center());
            const double d = QPoint(p - ref).manhattanLength();
            if (d < bestD) { bestD = d; pick = w; }
        }
    }
    if (!pick) pick = ring.first();
    pick->setFocus(Qt::OtherFocusReason);
    lastFocus_ = pick;
    lastCenter_ = pick->mapTo(container_, pick->rect().center());
    return pick;
}

QString NavRing::rememberSelection() const
{
    QWidget* cur = focusIn(container_);
    const QVector<QWidget*> ring = widgets();
    const int idx = ring.indexOf(cur);
    if (idx < 0) return QString();
    // objectName first (stable across rebuilds); fall back to the button text, then the position.
    if (!cur->objectName().isEmpty()) return QStringLiteral("n:") + cur->objectName();
    if (auto* btn = qobject_cast<QAbstractButton*>(cur); btn && !btn->text().isEmpty())
        return QStringLiteral("t:") + btn->text();
    return QStringLiteral("i:") + QString::number(idx);
}

void NavRing::restoreSelection(const QString& key)
{
    const QVector<QWidget*> ring = widgets();
    if (ring.isEmpty()) return;
    QWidget* pick = nullptr;
    if (key.startsWith(QStringLiteral("n:")))
    {
        for (QWidget* w : ring) if (w->objectName() == key.mid(2)) { pick = w; break; }
    }
    else if (key.startsWith(QStringLiteral("t:")))
    {
        for (QWidget* w : ring)
            if (auto* b = qobject_cast<QAbstractButton*>(w); b && b->text() == key.mid(2)) { pick = w; break; }
    }
    else if (key.startsWith(QStringLiteral("i:")))
    {
        const int idx = key.mid(2).toInt();
        if (idx >= 0 && idx < ring.size()) pick = ring[idx];
    }
    if (!pick) pick = ring.first();
    pick->setFocus(Qt::OtherFocusReason);
    lastFocus_ = pick;
    lastCenter_ = pick->mapTo(container_, pick->rect().center());
}

// ---------------------------------------------------------------- NavContext

NavContext* NavContext::s_instance = nullptr;
bool NavContext::syntheticKey_ = false;

NavContext* NavContext::instance() { return s_instance; }

NavContext::NavContext(QWidget* mainWindow)
    : QObject(mainWindow), window_(mainWindow)
{
    s_instance = this;
    // The focus watchdog: a cheap periodic check that a ring-managed screen always has a live selection.
    // Event-driven recovery (ensureFocus after routing) covers the common cases; this catches the rest
    // (a row deleted by a background refresh, focus dropped by a hidden widget...).
    auto* watchdog = new QTimer(this);
    watchdog->setInterval(400);
    connect(watchdog, &QTimer::timeout, this, &NavContext::ensureFocus);
    watchdog->start();
}

NavContext::~NavContext()
{
    if (s_instance == this) s_instance = nullptr;
}

bool NavContext::routeKey(int key)
{
    // 1. The topmost in-window overlay (menu / confirm / on-screen keyboard) owns input while open.
    if (NavOverlay::routeTopmost(key)) return true;
    // 2. The active screen's ring.
    if (activeRing_)
    {
        if (activeRing_->handleKey(key)) { ensureFocus(); return true; }
        // On a ring-managed screen a pad Back ALWAYS means "go back" — never "delete a character in
        // whatever text row happens to be focused".
        if (key == Qt::Key_Backspace && backAction_) { runBackAction(); return true; }
    }
    return false; // not ours: the caller's legacy delivery (themed QML, readers, player) takes it
}

bool NavContext::runBackAction()
{
    if (!backAction_) return false;
    const auto back = backAction_; // the action usually replaces the screen (and this member) — copy first
    back();
    ensureFocus();
    return true;
}

void NavContext::setActiveRing(NavRing* ring)
{
    activeRing_ = ring;
    if (ring)
    {
        // New screen: land the selection once the layout has settled (rows built this event-loop turn).
        QTimer::singleShot(0, this, [this, guard = QPointer<NavRing>(ring)] {
            if (guard && activeRing_ == guard) guard->ensureSelection();
        });
    }
}

void NavContext::ensureFocus()
{
    if (!window_ || !window_->isActiveWindow()) return;
    if (NavOverlay::topmost()) return;         // an overlay owns focus (and guards its own)
    if (!activeRing_) return;                  // screen manages itself (themed QML, readers, player)
    QWidget* fw = QApplication::focusWidget();
    if (!fw && window_) fw = window_->focusWidget(); // inactive window (UI-test): its focus child is the selection
    // A live, focusable widget on this screen IS a valid selection — never steal it, even when the ring's
    // own membership rules would skip it (a dialog's exotic control, Qt's own focus handling landing
    // somewhere reasonable). The watchdog only recovers when focus is genuinely gone: null, dead, hidden,
    // disabled, or outside the screen entirely.
    if (fw && fw->isVisible() && fw->isEnabled() && activeRing_->container()
        && activeRing_->container()->isAncestorOf(fw))
        return;
    activeRing_->ensureSelection();
}
