#include "OverflowBar.h"
#include "nav/NavOverlay.h"   // NavMenu: "More…" is a nav-kit menu, never a QMenu popup

#include <QEvent>
#include <QHBoxLayout>
#include <QPointer>
#include <QResizeEvent>

#include <algorithm>

// ---- ElidedLabel --------------------------------------------------------------------------------------------

ElidedLabel::ElidedLabel(QWidget* parent) : QLabel(parent) {}

void ElidedLabel::setText(const QString& text)
{
    if (text == full_) return;
    full_ = text;
    setToolTip(text);          // the whole text is always one hover away
    relabel();
    updateGeometry();
}

QSize ElidedLabel::sizeHint() const
{
    const QSize base = QLabel::sizeHint();
    const QMargins m = contentsMargins();
    return QSize(fontMetrics().horizontalAdvance(full_) + m.left() + m.right() + 2 * margin() + 2,
                 base.height());
}

QSize ElidedLabel::minimumSizeHint() const
{
    const QMargins m = contentsMargins();
    return QSize(fontMetrics().horizontalAdvance(QStringLiteral("…")) + m.left() + m.right() + 2 * margin(),
                 QLabel::minimumSizeHint().height());
}

void ElidedLabel::resizeEvent(QResizeEvent* e)
{
    QLabel::resizeEvent(e);
    relabel();
}

void ElidedLabel::relabel()
{
    const int room = qMax(0, contentsRect().width() - 2 * margin());
    const QString shown = fontMetrics().elidedText(full_, Qt::ElideMiddle, room);
    if (shown != text()) QLabel::setText(shown);
}

// ---- OverflowBar --------------------------------------------------------------------------------------------

OverflowBar::OverflowBar(QWidget* parent) : QWidget(parent)
{
    row_ = new QHBoxLayout(this);
    row_->setContentsMargins(0, 0, 0, 0);
    more_ = new QPushButton(tr("More…"), this);
    more_->setToolTip(tr("The controls that do not fit on the bar"));
    more_->hide();
    connect(more_, &QPushButton::clicked, this, &OverflowBar::openMore);
}

void OverflowBar::addCollapsible(QPushButton* control, int priority)
{
    if (!control) return;
    Item it;
    it.w = control;
    it.priority = priority;
    it.order = items_.size();
    it.wanted = !control->isHidden();
    items_.push_back(it);
    control->installEventFilter(this);
}

bool OverflowBar::wanted(QPushButton* control) const
{
    for (const Item& it : items_)
        if (it.w == control) return it.wanted;
    return !control->isHidden();
}

QVector<QPushButton*> OverflowBar::overflowed() const
{
    QVector<QPushButton*> out;
    for (const Item& it : items_)
        if (it.wanted && it.w->isHidden()) out << it.w;
    return out;
}

// Everything on the row that is NOT collapsible, not "More…" and not the label, at its natural width, plus the
// row's margins and the spacing between every widget that will be on it (label and "More…" included).
int OverflowBar::essentialWidth() const
{
    const QMargins m = row_->contentsMargins();
    int w = m.left() + m.right();
    int count = 0;
    for (int i = 0; i < row_->count(); ++i)
    {
        QWidget* c = row_->itemAt(i)->widget();
        if (!c || (c->isHidden() && c != more_)) continue;
        bool collapsible = false;
        for (const Item& it : items_) if (it.w == c) { collapsible = true; break; }
        if (collapsible) continue;
        ++count;
        if (c == more_ || qobject_cast<ElidedLabel*>(c)) continue;   // counted by the callers
        w += c->sizeHint().width();
    }
    return w + qMax(0, count - 1) * qMax(0, row_->spacing());
}

// The label keeps enough room to say something useful before a control is let back on the bar: its whole text
// when that is short, else about eighteen characters of it (enough for "…Pages 12–13 / 240").
int OverflowBar::labelReserve() const
{
    for (int i = 0; i < row_->count(); ++i)
        if (auto* l = qobject_cast<ElidedLabel*>(row_->itemAt(i)->widget()))
            return qMin(l->sizeHint().width(), l->fontMetrics().averageCharWidth() * 18);
    return 0;
}

QSize OverflowBar::minimumSizeHint() const
{
    int w = essentialWidth() + more_->sizeHint().width();
    for (int i = 0; i < row_->count(); ++i)
        if (auto* l = qobject_cast<ElidedLabel*>(row_->itemAt(i)->widget()))
            w += l->minimumSizeHint().width();
    return QSize(w, row_->minimumSize().height());
}

void OverflowBar::refit()
{
    if (refitting_) return;
    refitting_ = true;

    const int spacing = qMax(0, row_->spacing());
    int budget = width() - essentialWidth() - labelReserve();

    QVector<Item*> order;
    for (Item& it : items_) if (it.wanted) order << &it;
    std::stable_sort(order.begin(), order.end(), [](const Item* a, const Item* b) {
        if (a->priority != b->priority) return a->priority > b->priority;
        return a->order < b->order;
    });

    int all = 0;
    for (const Item* it : order) all += it->w->sizeHint().width() + spacing;

    QVector<QPushButton*> show, hide;
    if (all <= budget)
    {
        for (const Item* it : order) show << it->w;
    }
    else
    {
        budget -= more_->sizeHint().width() + spacing;
        bool full = false;   // strict priority: once one does not fit, nothing below it jumps the queue
        for (const Item* it : order)
        {
            const int cost = it->w->sizeHint().width() + spacing;
            if (!full && cost <= budget) { show << it->w; budget -= cost; }
            else { full = true; hide << it->w; }
        }
    }
    for (const Item& it : items_) if (!it.wanted) hide << it.w;

    applying_ = true;
    for (QPushButton* w : show) if (w->isHidden()) w->setVisible(true);
    for (QPushButton* w : hide) if (!w->isHidden()) w->setVisible(false);
    bool anyOver = false;
    for (const Item& it : items_) if (it.wanted && !show.contains(it.w)) anyOver = true;
    if (more_->isHidden() == anyOver) more_->setVisible(anyOver);
    applying_ = false;

    refitting_ = false;
}

void OverflowBar::openMore()
{
    QStringList rows;
    QVector<QPointer<QPushButton>> targets;
    for (QPushButton* w : overflowed())
    {
        if (!w->isEnabled()) continue;   // a control that could not be pressed on the bar is not offered here
        rows << ((w->isCheckable() && w->isChecked()) ? QStringLiteral("✓ ") + w->text() : w->text());
        targets << QPointer<QPushButton>(w);
    }
    if (rows.isEmpty()) return;
    // The chosen control is CLICKED, so it does exactly what it does on the bar - one action, two ways in.
    new NavMenu(tr("More"), rows, [targets](int r) {
        if (r >= 0 && r < targets.size() && targets.at(r)) targets.at(r)->click();
    }, window());
}

bool OverflowBar::event(QEvent* e)
{
    const bool r = QWidget::event(e);
    switch (e->type())
    {
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::LayoutRequest:   // a child changed size or visibility (a caption, an owner's show/hide)
        refit();
        break;
    default:
        break;
    }
    return r;
}

bool OverflowBar::eventFilter(QObject* o, QEvent* e)
{
    if (!applying_ && (e->type() == QEvent::ShowToParent || e->type() == QEvent::HideToParent))
        for (Item& it : items_)
            if (it.w == o) { it.wanted = (e->type() == QEvent::ShowToParent); break; }
    return QWidget::eventFilter(o, e);
}
