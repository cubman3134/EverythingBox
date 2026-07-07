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
// on rows, and QScrollArea::ensureWidgetVisible keeps the focused row in view.
static bool ringMember(const QWidget* w)
{
    if (!w || !w->isVisibleTo(nullptr) || !w->isVisible() || !w->isEnabled()) return false;
    if (!(w->focusPolicy() & Qt::TabFocus)) return false;
    if (qobject_cast<const QScrollBar*>(w)) return false;
    return true;
}

QVector<QWidget*> NavRing::widgets() const
{
    QVector<QWidget*> out;
    if (!container_) return out;
    const QList<QWidget*> all = container_->findChildren<QWidget*>();
    for (QWidget* w : all)
        if (ringMember(w)) out.push_back(w);
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
    QWidget* cur = QApplication::focusWidget();
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
        QWidget* cur = QApplication::focusWidget();
        // A slider/spinbox row edits its value with Left/Right — hand those through instead of moving.
        if ((key == Qt::Key_Left || key == Qt::Key_Right)
            && (qobject_cast<QSlider*>(cur) || qobject_cast<QAbstractSpinBox*>(cur)))
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
        QWidget* cur = QApplication::focusWidget();
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
    QWidget* cur = QApplication::focusWidget();
    if (cur && ring.contains(cur))
    {
        lastFocus_ = cur;
        lastCenter_ = cur->mapTo(container_, cur->rect().center());
        return cur; // selection is already valid
    }
    QWidget* pick = nullptr;
    if (lastFocus_ && ring.contains(lastFocus_)) pick = lastFocus_;
    if (!pick && !lastCenter_.isNull())
    {
        // The remembered widget died (row deleted, list rebuilt): land on the nearest survivor so the
        // selector stays where the user was looking instead of snapping to the top.
        double bestD = std::numeric_limits<double>::max();
        for (QWidget* w : ring)
        {
            const QPoint p = w->mapTo(container_, w->rect().center());
            const double d = QPoint(p - lastCenter_).manhattanLength();
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
    QWidget* cur = QApplication::focusWidget();
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
    // Live text entry via a physical keyboard: leave the caret alone.
    if ((qobject_cast<QLineEdit*>(fw) || qobject_cast<QTextEdit*>(fw) || qobject_cast<QPlainTextEdit*>(fw))
        && fw->hasFocus() && activeRing_->widgets().contains(fw))
        return;
    activeRing_->ensureSelection();
}
