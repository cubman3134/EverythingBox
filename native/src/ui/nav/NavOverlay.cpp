#include "NavOverlay.h"
#include "Nav.h"
#include "NavGraph.h"

#include <QApplication>
#include <QCheckBox>
#ifdef MMV_NAV_DEBUG
#include <cstdio>
#endif
#include <QEventLoop>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

QVector<QPointer<NavOverlay>> NavOverlay::s_stack;
QVariantMap NavOverlay::s_themeColors;
int NavOverlay::s_panelFontPx = 14;  // desktop identity (see setPanelFontPx)
int NavOverlay::s_listFontPx  = 16;  // desktop identity

void NavOverlay::setThemeColors(const QVariantMap& colors) { s_themeColors = colors; }

void NavOverlay::setPanelFontPx(int panelFontPx, int listFontPx)
{
    s_panelFontPx = panelFontPx;
    s_listFontPx  = listFontPx;
}

// ---------------------------------------------------------------- NavOverlay

NavOverlay::NavOverlay(QWidget* window)
    : QWidget(window ? window : (NavContext::instance() ? NavContext::instance()->window() : nullptr))
{
    QWidget* host = parentWidget();
    Q_ASSERT(host); // overlays only exist inside the main window
    setGeometry(host->rect());
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("NavOverlay { background: rgba(8,10,16,150); }")); // dim the page behind

    // Themed palette from the active theme's settingsPanel block (hard fallbacks = the original darks). The
    // selection highlight uses `rowSelected` paired with `text` (both defined per theme so the selected row is
    // legible on light AND dark themes — accent alone can be low-contrast under dark text on the light themes).
    auto navCol = [](const char* key, const char* fallback) -> QString {
        const QString v = s_themeColors.value(QString::fromLatin1(key)).toString();
        return v.isEmpty() ? QString::fromLatin1(fallback) : v;
    };
    const QString panelBg = navCol("panel", "#14161d");
    const QString border  = navCol("separator", "#2c2f3a");
    const QString text    = navCol("text", "#e8eaf0");
    const QString rowBg   = navCol("row", "#1d2029");
    const QString accent  = navCol("accent", "#4a79e8");
    const QString sel     = navCol("rowSelected", "#2f5fd0");
    panel_ = new QFrame(this);
    panel_->setObjectName(QStringLiteral("navOverlayPanel"));
    // Body font sizes ride the form-factor tokens (s_panelFontPx / s_listFontPx, pushed by
    // applyFormFactorWidgets); defaults are today's 14px / 16px so desktop is a pixel-for-pixel no-op.
    panel_->setStyleSheet(QStringLiteral(
        "#navOverlayPanel { background: %1; border: 1px solid %2; border-radius: 10px; }"
        "QLabel { color: %3; font-size: %7px; }"
        "QPushButton { background: %4; color: %3; border: 1px solid %2;"
        "              border-radius: 6px; padding: 8px 18px; font-size: %7px; }"
        "QPushButton:focus { background: %6; border-color: %5; }"
        "QListWidget { background: transparent; border: none; color: %3; outline: none; font-size: %8px; }"
        "QListWidget::item { padding: 9px 14px; border-radius: 6px; }"
        "QListWidget::item:selected { background: %6; }")
        .arg(panelBg, border, text, rowBg, accent, sel)
        .arg(s_panelFontPx).arg(s_listFontPx));
    ring_ = new NavRing(panel_, this);

    prevFocus_ = QApplication::focusWidget();
    host->installEventFilter(this); // follow window resizes
    s_stack.push_back(this);
    show();
    raise();
    grabKeyboard(); // physical keys come to us; synthetic ones arrive via NavContext -> routeTopmost
}

NavOverlay::~NavOverlay()
{
    s_stack.removeAll(QPointer<NavOverlay>(this));
    s_stack.removeAll(QPointer<NavOverlay>(nullptr));
}

NavOverlay* NavOverlay::topmost()
{
    for (int i = s_stack.size() - 1; i >= 0; --i)
        if (s_stack[i] && !s_stack[i]->dismissed_) return s_stack[i];
    return nullptr;
}

bool NavOverlay::routeTopmost(int key)
{
    NavOverlay* top = topmost();
    if (!top) return false;
    top->handleNavKey(key);
    return true; // an open overlay consumes every nav key — nothing may leak to the page behind
}

void NavOverlay::setNavGraph(NavGraph* graph)
{
    if (dismissed_ || levelPushed_) return;
    graph_ = graph;
    if (!graph_) return;
    // Mirror this overlay as a level. onPop dismisses us — but dismiss() itself pops the level (below), and
    // the dismissed_ latch makes that re-entrant onPop a no-op, so a Back that unwinds us through the graph
    // and a Back the overlay handles itself both close us exactly once (no double-dismiss).
    graph_->pushLevel(QStringLiteral("overlay"), [this] { dismiss(-1); });
    levelPushed_ = true;
}

void NavOverlay::dismiss(int result)
{
    if (dismissed_) return;
    dismissed_ = true;
    result_ = result;
    releaseKeyboard();
    hide();
    s_stack.removeAll(QPointer<NavOverlay>(this));
    s_stack.removeAll(QPointer<NavOverlay>(nullptr));
    if (NavOverlay* below = topmost())
        below->grabKeyboard();          // hand input back to the overlay underneath
    else if (prevFocus_ && prevFocus_->isVisible())
    {
        prevFocus_->setFocus(Qt::OtherFocusReason); // restore the selection from before we opened
        // A themed (QML) page: widget focus alone doesn't revive a QQuickWidget scene's active-focus item
        // after our keyboard grab, leaving every QML Keys handler (arrow nav) deaf until something kicks it.
        // This is the ONE focus-revival site for every overlay (esc menu / OSK / menus, themed or classic) —
        // topmost() above means we only reach it when no overlay remains beneath us, so it fires exactly once
        // per unwound stack. (The old duplicate esc-menu closed handler is gone.) ThemeEngine::buildView
        // exposes the scene root through this property; invoke by name so the nav kit stays QtQuick-free.
        if (QObject* sceneRoot = prevFocus_->property("mmvQuickRoot").value<QObject*>())
            QMetaObject::invokeMethod(sceneRoot, "forceActiveFocus");
    }
    else if (NavContext::instance())
        NavContext::instance()->ensureFocus();      // its widget died: land somewhere valid
    // Pop our mirror level so the graph's depth tracks reality (the overlay is no longer "on top"), keeping
    // syncThemedLevels' bookkeeping clean. Safe under re-entrancy — popLevel no-ops mid-onPop, and the onPop
    // (dismiss) short-circuits on the dismissed_ latch, so no double-dismiss.
    if (levelPushed_ && graph_) { levelPushed_ = false; graph_->popLevel(); }
    emit closed(result_);
    deleteLater();
}

bool NavOverlay::handleNavKey(int key)
{
    switch (key)
    {
    // Key_Back is Android's hardware/gesture/remote Back — same "close this overlay" as Escape/Backspace, so
    // a confirm card / the exit-menu / any overlay is dismissable by the OS Back and never swallows it dead.
    case Qt::Key_Backspace: case Qt::Key_Escape: case Qt::Key_Back:
        dismiss(-1);
        return true;
    default:
        if (ring_->handleKey(key)) return true;
        return true; // swallow everything else too — the page behind must never react
    }
}

void NavOverlay::keyPressEvent(QKeyEvent* e)
{
    // Physical keyboard: the grab routes real key presses here; drive the same nav handler.
    switch (e->key())
    {
    case Qt::Key_Up: case Qt::Key_Down: case Qt::Key_Left: case Qt::Key_Right:
    case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Backspace: case Qt::Key_Escape:
    case Qt::Key_Back: // Android hardware/remote Back: route it to handleNavKey (dismiss) like Escape
        handleNavKey(e->key());
        e->accept();
        return;
    default:
        e->accept(); // swallow; subclasses (the OSK) override to accept typed text
        return;
    }
}

QString NavOverlay::describe() const
{
    // Default: the focused button's caption (confirm cards, the OSK's key grid).
    QWidget* fw = QApplication::focusWidget();
    if (!fw && window()) fw = window()->focusWidget();
    if (auto* b = qobject_cast<QAbstractButton*>(fw); b && panel_ && panel_->isAncestorOf(b))
        return b->text();
    return {};
}

bool NavOverlay::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == parentWidget() && ev->type() == QEvent::Resize)
        setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(obj, ev);
}

void NavOverlay::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    // Size + centre the panel once its layout knows its content, and land the selection. (Deferred: the
    // subclass ctor is still adding the content when the base ctor's show() lands here.)
    QTimer::singleShot(0, this, [this] {
        if (dismissed_) return;
        relayoutPanel();
        ring_->ensureSelection();
    });
}

// Size the panel to fit its content with NOTHING cut off. adjustSize() under-measures word-wrapped labels
// (they report a near-single-line hint) and a stale layout cache reports empty content — both cut dialog
// text. So: polish everything (style/fonts applied), INVALIDATE the layout cache, fix the width from the
// fresh hint (clamped to the window), then let heightForWidth lay the wrapped text out at that width.
void NavOverlay::relayoutPanel()
{
    QLayout* lay = panel_->layout();
    if (lay)
    {
        // The content was added AFTER the panel became visible (the base ctor shows the overlay before the
        // subclass builds its widgets), and Qt keeps children created on an already-visible parent hidden
        // until each is explicitly shown — and hidden widgets are EMPTY layout items, which made the panel
        // size to its bare margins and cut everything off. Show every layout-managed widget (recursing into
        // nested layouts); anything the subclass explicitly hid stays hidden.
        std::function<void(QLayoutItem*)> showItems = [&showItems](QLayoutItem* it) {
            if (!it) return;
            if (QWidget* cw = it->widget())
            {
                if (!cw->testAttribute(Qt::WA_WState_ExplicitShowHide)) cw->show();
            }
            else if (QLayout* cl = it->layout())
                for (int i = 0; i < cl->count(); ++i) showItems(cl->itemAt(i));
        };
        for (int i = 0; i < lay->count(); ++i) showItems(lay->itemAt(i));
        panel_->ensurePolished();
        const QList<QWidget*> kids = panel_->findChildren<QWidget*>();
        for (QWidget* c : kids) c->ensurePolished();
        lay->invalidate();
        lay->activate();
        // +headroom: the panel's stylesheet border (1px a side) is painted inside the widget but is invisible
        // to the layout, which otherwise shaves the last couple of pixels off the bottom line of text.
        const int maxW = qMax(300, width() - 120);
        const int w = qBound(320, panel_->sizeHint().width() + 4, maxW);
#ifdef MMV_NAV_DEBUG
        {
            QWidget* k0 = lay->itemAt(0) ? lay->itemAt(0)->widget() : nullptr;
            std::fprintf(stderr, "[relayout v2] overlay=%dx%d hint=%dx%d w=%d hfw=%d items=%d "
                                 "kid0=%p kid0visible=%d kid0hidden=%d kid0hint=%dx%d panelVisible=%d\n",
                         width(), height(), panel_->sizeHint().width(), panel_->sizeHint().height(),
                         w, lay->hasHeightForWidth() ? lay->heightForWidth(w) : -1, lay->count(),
                         static_cast<void*>(k0), k0 ? int(k0->isVisible()) : -1,
                         k0 ? int(k0->isHidden()) : -1,
                         k0 ? k0->sizeHint().width() : -1, k0 ? k0->sizeHint().height() : -1,
                         int(panel_->isVisible()));
        }
#endif
        panel_->setFixedWidth(w);
        lay->invalidate();
        lay->activate();
        int h = lay->hasHeightForWidth() ? lay->heightForWidth(w) : panel_->sizeHint().height();
        panel_->setFixedHeight(qMin(h + 6, qMax(200, height() - 80)));
    }
    else panel_->adjustSize();
    panel_->move((width() - panel_->width()) / 2, (height() - panel_->height()) / 2);
}

// Walk the panel and report any text that doesn't fully fit its widget. This is the CI-probed contract
// behind "no dialog text is ever cut off": labels (plain + word-wrapped), buttons, and list rows.
QStringList NavOverlay::clippedTexts() const
{
    QStringList bad;
    auto clip = [](const QString& s) { return s.length() > 40 ? s.left(37) + QStringLiteral("...") : s; };
    if (!rect().contains(panel_->geometry()))
        bad << QStringLiteral("panel %1x%2 exceeds the window %3x%4")
                   .arg(panel_->width()).arg(panel_->height()).arg(width()).arg(height());
    const QList<QWidget*> kids = panel_->findChildren<QWidget*>();
    for (QWidget* w : kids)
    {
        if (!w->isVisible()) continue;
        if (auto* lbl = qobject_cast<QLabel*>(w))
        {
            if (lbl->text().isEmpty()) continue;
            const QFontMetrics fm = lbl->fontMetrics();
            if (lbl->wordWrap())
            {
                if (lbl->heightForWidth(lbl->width()) > lbl->height() + 1)
                    bad << QStringLiteral("label wrapped text cut: \"%1\"").arg(clip(lbl->text()));
            }
            else if (fm.horizontalAdvance(lbl->text()) > lbl->contentsRect().width())
                bad << QStringLiteral("label text clipped: \"%1\"").arg(clip(lbl->text()));
        }
        else if (auto* btn = qobject_cast<QAbstractButton*>(w))
        {
            if (btn->text().isEmpty()) continue;
            const int need = btn->fontMetrics().horizontalAdvance(btn->text())
                             + (qobject_cast<QCheckBox*>(btn) ? 28 : 16); // indicator / frame allowance
            if (need > btn->width())
                bad << QStringLiteral("button text clipped: \"%1\" (needs %2px, has %3px)")
                           .arg(clip(btn->text())).arg(need).arg(btn->width());
        }
        else if (auto* list = qobject_cast<QListWidget*>(w))
        {
            const bool scrolls = list->verticalScrollBar() && list->verticalScrollBar()->isVisible();
            int totalH = 0;
            for (int i = 0; i < list->count(); ++i)
            {
                const QRect r = list->visualItemRect(list->item(i));
                totalH += r.height();
                if (!list->wordWrap() && list->sizeHintForColumn(0) > list->viewport()->width())
                {
                    bad << QStringLiteral("list row wider than the menu: \"%1\"").arg(clip(list->item(i)->text()));
                    break;
                }
                if (r.width() > list->viewport()->width() + 1)
                    bad << QStringLiteral("list row clipped: \"%1\"").arg(clip(list->item(i)->text()));
            }
            if (!scrolls && totalH > list->viewport()->height() + 2)
                bad << QStringLiteral("list rows overflow the menu (%1px in %2px, no scrollbar)")
                           .arg(totalH).arg(list->viewport()->height());
        }
    }
    return bad;
}

// ---------------------------------------------------------------- NavMenu

NavMenu::NavMenu(const QString& title, const QStringList& items,
                 const std::function<void(int)>& onChosen, QWidget* window)
    : NavOverlay(window), onChosen_(onChosen)
{
    auto* v = new QVBoxLayout(panel());
    v->setContentsMargins(22, 18, 22, 18);
    v->setSpacing(10);
    auto* t = new QLabel(title, panel());
    t->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600;"));
    t->setWordWrap(true); // a long game title wraps instead of blowing the menu wide / getting cut
    v->addWidget(t);
    list_ = new QListWidget(panel());
    list_->addItems(items);
    list_->setFocusPolicy(Qt::NoFocus);       // the overlay drives it; no Qt focus fights
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setWordWrap(true);                 // an over-long row wraps rather than eliding
    list_->setUniformItemSizes(false);
    // Width: fit the longest row (and give the title room), capped to the window; every row shown in
    // full, no scrolling. Heights are measured AFTER the width is fixed so wrapped rows count fully.
    const int cap = qMax(320, parentWidget()->width() - 200);
    int w = qMax(280, list_->sizeHintForColumn(0) + 44);
    w = qMax(w, qMin(t->fontMetrics().horizontalAdvance(title) + 24, 520));
    w = qMin(w, cap);
    list_->setFixedWidth(w);
    t->setMaximumWidth(w);
    int h = list_->frameWidth() * 2;
    for (int i = 0; i < list_->count(); ++i) h += list_->sizeHintForRow(i) + 2;
    list_->setFixedHeight(h + 6);
    list_->setCurrentRow(0);
    // Mouse path: a click chooses the row directly (same flow as controller Enter).
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem*) { handleNavKey(Qt::Key_Return); });
    v->addWidget(list_);
}

bool NavMenu::handleNavKey(int key)
{
    switch (key)
    {
    case Qt::Key_Up:     list_->setCurrentRow(qMax(0, list_->currentRow() - 1)); return true;
    case Qt::Key_Down:   list_->setCurrentRow(qMin(list_->count() - 1, list_->currentRow() + 1)); return true;
    case Qt::Key_Return: case Qt::Key_Enter:
    {
        const int row = list_->currentRow();
        const auto chosen = onChosen_;
        dismiss(row);
        if (chosen && row >= 0) chosen(row); // after dismiss: the handler may open panels/overlays itself
        return true;
    }
    default:
        return NavOverlay::handleNavKey(key); // Back/Escape close
    }
}

QString NavMenu::describe() const
{
    return list_ && list_->currentItem() ? list_->currentItem()->text() : QString();
}

int NavMenu::pick(const QString& title, const QStringList& items, QWidget* window)
{
    int result = -1;
    QEventLoop loop;
    auto* menu = new NavMenu(title, items, [&result](int row) { result = row; }, window);
    QObject::connect(menu, &NavOverlay::closed, &loop, [&loop](int) { loop.quit(); });
    loop.exec();
    return result;
}

// ---------------------------------------------------------------- NavConfirm

NavConfirm::NavConfirm(const QString& title, const QString& message, const QStringList& buttons,
                       int focusIndex, QWidget* window)
    : NavOverlay(window)
{
    auto* v = new QVBoxLayout(panel());
    v->setContentsMargins(26, 20, 26, 20);
    v->setSpacing(14);
    auto* t = new QLabel(title, panel());
    t->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600;"));
    t->setWordWrap(true);       // long questions wrap, never clip
    t->setMaximumWidth(560);
    v->addWidget(t);
    if (!message.isEmpty())
    {
        auto* m = new QLabel(message, panel());
        m->setWordWrap(true);
        m->setMaximumWidth(560);
        v->addWidget(m);
    }
    auto* row = new QHBoxLayout;
    row->setSpacing(10);
    row->addStretch(1);
    QPushButton* focusBtn = nullptr;
    for (int i = 0; i < buttons.size(); ++i)
    {
        auto* b = new QPushButton(buttons[i], panel());
        connect(b, &QPushButton::clicked, this, [this, i] { dismiss(i); });
        row->addWidget(b);
        if (i == focusIndex) focusBtn = b;
    }
    v->addLayout(row);
    if (focusBtn) QTimer::singleShot(0, this, [focusBtn] { if (focusBtn->isVisible()) focusBtn->setFocus(); });
}

int NavConfirm::ask(const QString& title, const QString& message, const QStringList& buttons,
                    int focusIndex, int cancelIndex, QWidget* window)
{
    auto* card = new NavConfirm(title, message, buttons, focusIndex, window);
    int result = cancelIndex;
    QEventLoop loop;
    QObject::connect(card, &NavOverlay::closed, &loop, [&](int r) {
        result = (r < 0) ? cancelIndex : r;
        loop.quit();
    });
    loop.exec(); // pad polling keeps running (timers fire inside the nested loop)
    return result;
}
