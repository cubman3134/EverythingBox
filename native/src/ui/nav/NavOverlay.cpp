#include "NavOverlay.h"
#include "Nav.h"
#include "NavGraph.h"

#include <QApplication>
#include <QCheckBox>
#ifdef EB_NAV_DEBUG
#include <cstdio>
#endif
#include <QEventLoop>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>  // #347: the confirm card's message viewport
#include <QScrollBar>
#include <QStyle>     // PM_ScrollBarExtent, when a long NavMenu has to scroll
#include <QTimer>
#include <QVBoxLayout>

QVector<QPointer<NavOverlay>> NavOverlay::s_stack;
QVariantMap NavOverlay::s_themeColors;
int NavOverlay::s_panelFontPx = 14;  // desktop identity (see setPanelFontPx)
int NavOverlay::s_listFontPx  = 16;  // desktop identity

void NavOverlay::setThemeColors(const QVariantMap& colors) { s_themeColors = colors; }

QString NavOverlay::themeColor(const char* key, const char* fallback)
{
    const QString v = s_themeColors.value(QString::fromLatin1(key)).toString();
    return v.isEmpty() ? QString::fromLatin1(fallback) : v;
}

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
    auto navCol = [](const char* key, const char* fallback) { return themeColor(key, fallback); };
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

void NavOverlay::mousePressEvent(QMouseEvent* e)
{
    // Tap/click on the scrim — anywhere outside the card — dismisses, the way every phone sheet does
    // (and the way desktop popups close on an outside click). Presses on the card land on its children
    // and never reach this handler.
    if (panel_ && !panel_->geometry().contains(e->pos()))
    {
        e->accept();
        dismiss(-1);
        return;
    }
    QWidget::mousePressEvent(e);
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
        const int maxW = qMax(300, width() - 120);
        // #349: content that has to be ARRANGED to fit the widest the card can be — a confirmation's button
        // row — is arranged now, against that width, so the size hint measured next is its real requirement.
        fitPanelWidth(maxW);
        lay->invalidate();
        lay->activate();
        // +headroom: the panel's stylesheet border (1px a side) is painted inside the widget but is invisible
        // to the layout, which otherwise shaves the last couple of pixels off the bottom line of text.
        const int w = qBound(320, panel_->sizeHint().width() + 4, maxW);
#ifdef EB_NAV_DEBUG
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
        // #347: the width is settled and the children are at their real widths, so this is the moment a
        // subclass can bound its own content to the height the card may have — measured at the width the
        // text is painted at, which is the thing heightForWidth below cannot see (see the declaration).
        fitPanelContent(qMax(200, height() - 80));
        lay->invalidate();
        lay->activate();
        int h = lay->hasHeightForWidth() ? lay->heightForWidth(w) : panel_->sizeHint().height();
        panel_->setFixedHeight(qMin(h + 6, qMax(200, height() - 80)));
    }
    else panel_->adjustSize();
    panel_->move((width() - panel_->width()) / 2, (height() - panel_->height()) / 2);
}

// Default: the panel sizes to its content exactly as it always has. NavConfirm overrides it (#347).
void NavOverlay::fitPanelContent(int /*heightBudget*/) {}

// Default: nothing to arrange. NavConfirm overrides it to pack its button row (#349).
void NavOverlay::fitPanelWidth(int /*maxPanelWidth*/) {}

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
            // The WIDEST LINE of the label: #349 lets a button label that cannot fit a line of the card
            // break at a space, and one advance over the whole string would then read as clipped when
            // every line of it is painted in full. Single-line labels — all but a handful — measure
            // exactly as they always have.
            int adv = 0;
            for (const QString& ln : btn->text().split(QLatin1Char('\n')))
                adv = qMax(adv, btn->fontMetrics().horizontalAdvance(ln));
            const int need = adv
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
                 const std::function<void(int)>& onChosen, QWidget* window, int initialRow)
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
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setWordWrap(true);                 // an over-long row wraps rather than eliding
    list_->setUniformItemSizes(false);
    // Width: fit the longest row (and give the title room), capped to the window. Heights are measured
    // AFTER the width is fixed so wrapped rows count fully.
    const int cap = qMax(320, parentWidget()->width() - 200);
    int w = qMax(280, list_->sizeHintForColumn(0) + 44);
    w = qMax(w, qMin(t->fontMetrics().horizontalAdvance(title) + 24, 520));
    w = qMin(w, cap);
    list_->setFixedWidth(w);
    t->setMaximumWidth(w);

    // Height: show every row when they fit, and SCROLL when they don't. The short fixed menus this
    // primitive was written for (esc menu, XMB game menu, Downloads chooser) always fit, so it used to set
    // the full summed height unconditionally — but the panel clamps ITSELF to the window
    // (setFixedHeight(qMin(h + 6, qMax(200, height() - 80))) above), so an over-tall list was simply cut
    // off at the panel edge: the ring still walked onto rows that were not on screen. That clipping
    // happened one level UP from the list, which is why clippedTexts() — it measures rows against the
    // LIST's own viewport, and the list claimed to be tall enough — never reported it. Clamping here puts
    // the overflow back inside the list, where a real scrollbar handles it and the probe can see it.
    auto measure = [this] {
        int h = list_->frameWidth() * 2;
        for (int i = 0; i < list_->count(); ++i) h += list_->sizeHintForRow(i) + 2;
        return h + 6;
    };
    const int titleH = qMax(t->sizeHint().height(), t->heightForWidth(w));
    // Mirror the panel's own budget: window - 80, less this layout's margins (18+18), spacing (10+10)
    // and the title. Never shrink below a few rows, however small the window gets.
    const int availH = qMax(140, parentWidget()->height() - 80 - (18 + 18 + 20 + titleH));
    if (measure() > availH)
    {
        // Re-fit the width for the scrollbar the rows now have to share with, then re-measure: a narrower
        // viewport wraps more rows, so the old height would be an undercount.
        list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        const int sb = list_->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, list_);
        list_->setFixedWidth(qMin(w + sb, cap));
        list_->setFixedHeight(qMin(measure(), availH));
    }
    else
    {
        list_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list_->setFixedHeight(measure());
    }
    // Clamped, and scrolled to: an initial row the viewport is not showing looks exactly like a menu that
    // opened at the top and lost the highlight.
    list_->setCurrentRow(qBound(0, initialRow, qMax(0, list_->count() - 1)));
    list_->scrollToItem(list_->currentItem());
    // Mouse path: a click chooses the row directly (same flow as controller Enter).
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem*) { handleNavKey(Qt::Key_Return); });
    v->addWidget(list_);
}

bool NavMenu::handleNavKey(int key)
{
    switch (key)
    {
    // scrollToItem: the list scrolls when the rows don't fit (see the ctor), and the ring must drag the
    // viewport with it — otherwise the highlight walks off-screen and the menu looks frozen.
    case Qt::Key_Up:
        list_->setCurrentRow(qMax(0, list_->currentRow() - 1));
        list_->scrollToItem(list_->currentItem());
        return true;
    case Qt::Key_Down:
        list_->setCurrentRow(qMin(list_->count() - 1, list_->currentRow() + 1));
        list_->scrollToItem(list_->currentItem());
        return true;
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

int NavMenu::pick(const QString& title, const QStringList& items, QWidget* window, int initialRow)
{
    int result = -1;
    QEventLoop loop;
    auto* menu = new NavMenu(title, items, [&result](int row) { result = row; }, window, initialRow);
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
    title_ = t;                 // #347: fitPanelContent measures it at the width it is painted at
    if (!message.isEmpty())
    {
        message_ = new QLabel(message, panel());
        message_->setWordWrap(true);
        message_->setMaximumWidth(560);
        v->addWidget(message_);
    }
    // The buttons live in an AREA of lines rather than a single row (#349). It holds exactly one line
    // until fitButtonRow finds that the labels do not fit the card's width, so a card whose row fits —
    // which is nearly all of them — is laid out to the pixel it always was.
    buttonArea_ = new QVBoxLayout;
    buttonArea_->setContentsMargins(0, 0, 0, 0);
    buttonArea_->setSpacing(10);
    auto* row = new QHBoxLayout;
    row->setSpacing(10);
    row->addStretch(1);
    QPushButton* focusBtn = nullptr;
    for (int i = 0; i < buttons.size(); ++i)
    {
        auto* b = new QPushButton(buttons[i], panel());
        connect(b, &QPushButton::clicked, this, [this, i] { dismiss(i); });
        row->addWidget(b);
        buttons_ << b;
        buttonLabels_ << buttons[i];
        if (i == focusIndex) focusBtn = b;
    }
    buttonArea_->addLayout(row);
    buttonLines_ = QVector<int>{ int(buttons.size()) };
    v->addLayout(buttonArea_);
    if (focusBtn) QTimer::singleShot(0, this, [focusBtn] { if (focusBtn->isVisible()) focusBtn->setFocus(); });
}


// ---------------------------------------------------------------- #347: nothing is cut off in silence
//
// The card's contract is that the message is either wholly on screen or wholly REACHABLE, and that the
// buttons are on the card either way. This is where both are made true, once per relayout:
//
//   1. every item gets the height its text needs AT THE WIDTH IT IS PAINTED AT. The layout could not work
//      that out for itself — see fitPanelContent's declaration — and nine shipped confirmations were
//      losing between one and seventeen lines to it at every window size, 1920x1080 included;
//   2. when that is more than the card may have, the message moves into a scrolling viewport sized to
//      what is left over once the title and the buttons have taken theirs. The scrollbar is ALWAYS shown
//      while it scrolls, so the card says there is more, and Up/Down scroll it with the pad.
//
// A message that fits builds no scroll area at all: the widget tree, the panel's size and every child's
// geometry are what they have always been. That is deliberate — this widget is on the path of every
// confirmation in the app, including the ones that delete things.
void NavConfirm::fitPanelContent(int heightBudget)
{
    auto* v = qobject_cast<QVBoxLayout*>(panel()->layout());
    if (!v || !title_) return;

    // Measure the TEXT, never the previous measurement: a card that is relabelled (NavCountdown every
    // second, #137's lookup card when the answer lands) would otherwise re-fit around its own old height.
    auto unfix = [](QWidget* w) {
        if (!w) return;
        w->setMinimumHeight(0);
        w->setMaximumHeight(QWIDGETSIZE_MAX);
    };
    unfix(title_); unfix(message_); unfix(body_); unfix(scroll_);

    const QMargins cm = v->contentsMargins();
    const int sp   = qMax(0, v->spacing());
    // #349: the button row was packed by fitPanelWidth before the card's width was measured, so this is its
    // real height — one line of buttons, or as many as it took to show every label whole. Everything below
    // is measured against what it leaves, so it has to be the real one.
    const int rowH = buttonArea_ ? buttonArea_->sizeHint().height() : 0;
    const int line = qMax(1, title_->fontMetrics().lineSpacing());
    // relayoutPanel adds 6px of headroom for the panel's border before it clamps; budget for it here too,
    // or the panel it builds from these numbers is 6px past the clamp and gets squeezed after all.
    const int budget = heightBudget - 6;

    if (!message_)                     // a message-less card is a title and buttons; give the title its due
    {
        title_->setMinimumHeight(title_->heightForWidth(title_->width()));
        scrollable_ = false;
        return;
    }

    const int titleW = titleInBody_ && body_ ? body_->width() : title_->width();
    const int titleH = title_->heightForWidth(titleW);
    const int pinnedWithTitle = cm.top() + cm.bottom() + 2 * sp + titleH + rowH;
    int avail = budget - pinnedWithTitle;

    // A TITLE LONG ENOUGH TO EAT THE CARD. Clipping it would be the very defect this exists to remove, and
    // shrinking the message to nothing is the same defect wearing a different hat — so the title goes into
    // the scrolling area with the message and only the buttons stay pinned. Real titles never reach this;
    // a user-named playlist or a 40-line game title on a 480px screen can.
    const bool titleScrolls = avail < 2 * line;
    const int pinned = titleScrolls ? (cm.top() + cm.bottom() + sp + rowH) : pinnedWithTitle;
    if (titleScrolls) avail = budget - pinned;

    // The width the message is painted at today: the label's own when it is still in the panel, the
    // viewport's when it has already been moved into one.
    const int outerW = scroll_ ? scroll_->width() : message_->width();
    const int flatH  = message_->heightForWidth(outerW);

    if (!titleScrolls && flatH <= avail)
    {
        // IT FITS. Exactly the card this has always been — with the height the text actually needs.
        // A MINIMUM, not a fixed height: the layout still hands out the panel's spare pixels exactly as
        // it always did (one shipped card has a label 3px taller than its text and keeps it), it simply
        // can no longer hand out FEWER than the text needs.
        title_->setMinimumHeight(titleH);
        if (scroll_)   // a card that scrolled once and has since been relabelled shorter
        {
            setTitleScrolls(false);
            message_->setFixedWidth(outerW);
            message_->setFixedHeight(flatH);
            body_->setFixedWidth(outerW);
            body_->setFixedHeight(flatH);
            scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            scroll_->setFixedHeight(flatH);
        }
        else
            message_->setMinimumHeight(flatH);
        scrollable_ = false;
        return;
    }

    // IT DOES NOT FIT: scroll it.
    ensureScrollArea();
    setTitleScrolls(titleScrolls);
    const int sb    = scroll_->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scroll_);
    const int viewW = qMax(40, scroll_->width() - sb);   // the bar takes its width from the text, always

    int bodyH = 0;
    if (titleScrolls)
    {
        title_->setFixedWidth(viewW);
        title_->setFixedHeight(title_->heightForWidth(viewW));
        bodyH += title_->height() + sp;
    }
    else title_->setMinimumHeight(titleH);
    message_->setFixedWidth(viewW);
    message_->setFixedHeight(message_->heightForWidth(viewW));
    bodyH += message_->height();
    body_->setFixedWidth(viewW);
    body_->setFixedHeight(bodyH);

    // What is left over, and never more: this is the number that keeps the buttons on the card.
    const int viewH = qBound(line, avail, qMax(line, budget - pinned));
    scroll_->setFixedHeight(viewH);
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scroll_->verticalScrollBar()->setSingleStep(qMax(1, line));
    scroll_->verticalScrollBar()->setPageStep(qMax(line, viewH - line));
    scrollable_ = bodyH > viewH;
}

// ---------------------------------------------------------------- #349: every button says what it does
//
// A QHBoxLayout given less width than its items need does not refuse: it hands each of them a SHARE of
// what there is, and a QPushButton paints as much of its label as fits the share. That is how the button
// which starts a minutes-long compile came to read "Rebuild it wi" — 252px of the 478px its label needs —
// with no ellipsis and nothing to say a word had been dropped. The panel cannot simply grow: it is capped
// at the window's width less a margin, and on a 800x480 screen five buttons will not fit a line whatever
// the card does.
//
// So the row WRAPS. The buttons are packed, in order, into as many lines as they need, each line as wide
// as the card allows; every label is painted whole, and no action is hidden behind an ellipsis or a hover
// (this is a TV app driven by a pad — there is no hover, and an elided label is simply a lost one). A
// label too long for even a line of its own is broken at a space and painted on two lines of the button,
// which is still the whole label.
//
// WHEN THE ROW IS PACKED MATTERS AS MUCH AS HOW. It is packed against the widest the card can be — the
// window's clamp — BEFORE the card's width is measured, so the card is then sized from the row's real
// requirement: its widest line. Packing it against the card's CURRENT width instead measures the previous
// packing: the first layout left the card at the clamp with a band of nothing beside the lines, and the
// next relayout (a NavCountdown relays out once a second) shrank the card under them. Packed from the
// labels and the window alone, the same card lays out the same way every time.
//
// A ROW THAT FITS IS NOT TOUCHED: one line, one QHBoxLayout, the widths the layout always gave it, and
// Left/Right still handled by the ring's geometric step. That identity is the point — this widget is on
// the path of every confirmation in the app, including the ones that delete things.
void NavConfirm::fitPanelWidth(int maxPanelWidth)
{
    auto* v = qobject_cast<QVBoxLayout*>(panel()->layout());
    if (!v) return;
    // What a line of buttons really gets inside a card that wide: less the card's frame (its 1px stylesheet
    // border, which QFrame keeps in its contentsMargins) and the layout's own margins. Leaving the frame out
    // would pack a line 2px wider than it is ever laid out at — #347's border bug, on the other axis.
    const QMargins fm = panel()->contentsMargins();
    const QMargins cm = v->contentsMargins();
    fitButtonRow(qMax(40, maxPanelWidth - fm.left() - fm.right() - cm.left() - cm.right()));
}

void NavConfirm::fitButtonRow(int rowWidth)
{
    if (!buttonArea_ || buttons_.isEmpty()) return;
    const int sp = qMax(0, buttonArea_->spacing());

    // MEASURE THE LABEL, NEVER THE LAST MEASUREMENT (#347's rule, and this widget re-fits on every
    // relayout): put back the text as it was given before asking how wide it wants to be.
    for (int i = 0; i < buttons_.size(); ++i)
    {
        if (!buttons_[i]) continue;
        if (buttons_[i]->text() != buttonLabels_.at(i)) buttons_[i]->setText(buttonLabels_.at(i));
    }

    QVector<int> need;
    need.reserve(buttons_.size());
    for (QPushButton* b : buttons_) need << (b ? b->sizeHint().width() : 0);

    // A SINGLE LABEL WIDER THAN THE WHOLE LINE. Break it at the space nearest the middle and let the
    // button paint two lines — a button is allowed to be two lines tall; it is not allowed to lie about
    // what it does. (Shipped labels do not reach this at any tested size; a translation can.)
    for (int i = 0; i < buttons_.size(); ++i)
    {
        if (need.at(i) <= rowWidth || !buttons_[i]) continue;
        const QString label = buttonLabels_.at(i);
        const QStringList words = label.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (words.size() < 2) continue;                 // one long word: nothing to break, keep it whole
        int bestCut = -1, bestDelta = -1;
        for (int cut = 1; cut < words.size(); ++cut)
        {
            const int a = buttons_[i]->fontMetrics().horizontalAdvance(
                              QStringList(words.mid(0, cut)).join(QLatin1Char(' ')));
            const int b = buttons_[i]->fontMetrics().horizontalAdvance(
                              QStringList(words.mid(cut)).join(QLatin1Char(' ')));
            const int delta = qAbs(a - b);
            if (bestCut < 0 || delta < bestDelta) { bestCut = cut; bestDelta = delta; }
        }
        buttons_[i]->setText(QStringList(words.mid(0, bestCut)).join(QLatin1Char(' '))
                             + QLatin1Char('\n')
                             + QStringList(words.mid(bestCut)).join(QLatin1Char(' ')));
        need[i] = buttons_[i]->sizeHint().width();
    }

    // PACK, in order, greedily. Reading order is the order the caller listed the buttons in, which is the
    // order the ring walks them in, so the two never disagree about where "next" is.
    QVector<int> lines;
    int used = 0, count = 0;
    for (int i = 0; i < need.size(); ++i)
    {
        const int add = (count == 0) ? need.at(i) : sp + need.at(i);
        if (count > 0 && used + add > rowWidth) { lines << count; used = need.at(i); count = 1; }
        else { used += add; ++count; }
    }
    if (count > 0) lines << count;

    if (lines == buttonLines_) return;   // same packing as it already has: nothing to rebuild
    buttonLines_ = lines;

    // Rebuild the lines. The buttons are children of the panel, so taking them out of a layout and putting
    // them into another neither destroys nor hides them.
    while (QLayoutItem* it = buttonArea_->takeAt(0))
    {
        if (QLayout* old = it->layout())
            while (QLayoutItem* sub = old->takeAt(0)) delete sub;
        delete it;
    }
    int at = 0;
    for (int n : lines)
    {
        auto* line = new QHBoxLayout;
        line->setSpacing(sp);
        line->addStretch(1);
        for (int k = 0; k < n && at < buttons_.size(); ++k, ++at)
            if (buttons_[at]) line->addWidget(buttons_[at]);
        buttonArea_->addLayout(line);
    }
}

// The viewport, built the first time a message overflows and kept for the life of the card. Not resizable:
// QScrollArea's own widgetResizable path sizes its widget to minimumSizeHint(), which for a word-wrapped
// QLabel is a couple of lines — it would clip the very text this is here to show. We set the body's size
// ourselves, from a measurement, every relayout.
void NavConfirm::ensureScrollArea()
{
    if (scroll_) return;
    auto* v = qobject_cast<QVBoxLayout*>(panel()->layout());
    if (!v || !message_) return;
    const int idx = v->indexOf(message_);
    const int w   = message_->width();

    body_ = new QWidget(panel());
    body_->setAutoFillBackground(false);
    auto* bv = new QVBoxLayout(body_);
    bv->setContentsMargins(0, 0, 0, 0);
    bv->setSpacing(qMax(0, v->spacing()));
    v->removeWidget(message_);
    message_->setParent(body_);
    bv->addWidget(message_);
    message_->show();

    scroll_ = new QScrollArea(panel());
    scroll_->setObjectName(QStringLiteral("navConfirmScroll"));
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidgetResizable(false);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setFocusPolicy(Qt::NoFocus);   // the overlay drives it; no Qt focus fights (as NavMenu's list)
    scroll_->viewport()->setAutoFillBackground(false);
    // Scoped to this object name so a long NavMenu's list keeps the scrollbar it has always had. Wide and
    // in the theme's accent because the reader of this card is ten feet away with a pad in their hands.
    scroll_->setStyleSheet(QStringLiteral(
        "#navConfirmScroll, #navConfirmScroll > QWidget > QWidget { background: transparent; }"
        "#navConfirmScroll { border: none; }"
        "QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }"
        "QScrollBar::handle:vertical { background: %1; border-radius: 5px; min-height: 30px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }")
        .arg(themeColor("accent", "#4a79e8")));
    scroll_->setWidget(body_);
    scroll_->setFixedWidth(w);              // the width the message already had: the card does not move
    v->insertWidget(idx, scroll_);
    scroll_->show();
    body_->show();
}

void NavConfirm::setTitleScrolls(bool inside)
{
    if (!body_ || !title_ || titleInBody_ == inside) return;
    auto* v  = qobject_cast<QVBoxLayout*>(panel()->layout());
    auto* bv = qobject_cast<QVBoxLayout*>(body_->layout());
    if (!v || !bv) return;
    if (inside)
    {
        v->removeWidget(title_);
        title_->setParent(body_);
        bv->insertWidget(0, title_);
    }
    else
    {
        bv->removeWidget(title_);
        title_->setParent(panel());
        title_->setMinimumWidth(0);
        title_->setMaximumWidth(560);   // the ctor's cap, restored
        v->insertWidget(0, title_);
    }
    title_->show();
    titleInBody_ = inside;
}

// Up/Down scroll the message while it is scrolling. They are free to: the buttons on this card are walked
// with Left/Right, and Up/Down have never moved the selection here. Back/Escape and Enter are untouched,
// so a card that scrolls is still dismissed and still answered the way every other one is.
//
// AND WHILE THE ROW IS WRAPPED (#349), Left/Right walk the buttons in the order they were given, across
// the line break. The ring's geometric step cannot: the lines are right-aligned, so the first button of
// the second line sits DOWN AND TO THE LEFT of the last button of the first, and a Right press from the
// end of a line finds nothing to its right. Sequential is also simply what a row of actions means. For a
// row on ONE line the two orders are the same, so this is left to the ring there — unchanged.
bool NavConfirm::handleNavKey(int key)
{
    if (scrollable_ && scroll_ && (key == Qt::Key_Up || key == Qt::Key_Down))
    {
        QScrollBar* bar = scroll_->verticalScrollBar();
        const int step = qMax(1, bar->singleStep());
        bar->setValue(bar->value() + (key == Qt::Key_Down ? step : -step));
        return true;
    }
    if (buttonRowWraps() && (key == Qt::Key_Left || key == Qt::Key_Right))
    {
        QWidget* fw = QApplication::focusWidget();
        const int cur = fw ? buttons_.indexOf(qobject_cast<QPushButton*>(fw)) : -1;
        if (cur >= 0)
        {
            const int next = cur + (key == Qt::Key_Right ? 1 : -1);
            // No wraparound at the ends: exactly what the geometric step does on a single-line row.
            if (next >= 0 && next < buttons_.size() && buttons_.at(next))
            {
                buttons_.at(next)->setFocus(Qt::OtherFocusReason);
                return true;
            }
            return true;   // at an end of the row: consumed, and the selection stays where it is
        }
    }
    return NavOverlay::handleNavKey(key);
}

QString NavConfirm::describe() const
{
    QString base = NavOverlay::describe();
    // A label broken across two lines of its button (#349) is still reported as the label it was given.
    for (int i = 0; i < buttons_.size() && !base.isEmpty(); ++i)
        if (buttons_.at(i) && buttons_.at(i)->text() == base) { base = buttonLabels_.at(i); break; }
    if (!scrollable_ || !scroll_ || !scroll_->verticalScrollBar()) return base;
    const QScrollBar* bar = scroll_->verticalScrollBar();
    return base + QStringLiteral(" [scroll %1/%2]").arg(bar->value()).arg(bar->maximum());
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

void NavConfirm::setMessage(const QString& message)
{
    if (!message_) return;
    message_->setText(message);
    // RE-FIT THE PANEL AROUND IT. The card was sized when it was built, and a message that GROWS after that —
    // #137's lookup card opens on "Looking up…" and is relabelled seconds later with several lines of a
    // dictionary entry — kept the old size and had everything past the first line cut off. NavCountdown never
    // exposed this because it only ever swaps one digit into a message that was already its final size.
    // probe_nav section 10 pins it; a live drive of #137 against a real Wiktionary entry is what found it.
    relayoutPanel();
}

NavCountdown::NavCountdown(const QString& title, const QString& messageTmpl, const QStringList& buttons,
                          int seconds, int acceptIndex, int focusIndex, QWidget* window)
    : NavConfirm(title, messageTmpl.arg(seconds), buttons, focusIndex, window)
{
    // A live 1 Hz timer: relabel the remaining seconds each tick and auto-accept at zero. It's a child of this
    // overlay, so it dies with the card (Cancel / Play now / Back all dismiss the overlay, stopping the timer).
    // The timer fires inside ask()'s nested loop (the scout-verified property), so this animates while blocked.
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this,
            [this, timer, messageTmpl, acceptIndex, remaining = seconds]() mutable {
        if (--remaining <= 0) { timer->stop(); dismiss(acceptIndex); return; } // auto-accept at zero
        setMessage(messageTmpl.arg(remaining));                                 // relabel "… in N s"
    });
    timer->start(1000);
}

int NavCountdown::ask(const QString& title, const QString& messageTmpl, const QStringList& buttons,
                      int seconds, int acceptIndex, int focusIndex, int cancelIndex, QWidget* window)
{
    auto* card = new NavCountdown(title, messageTmpl, buttons, seconds, acceptIndex, focusIndex, window);
    int result = cancelIndex;
    QEventLoop loop;
    QObject::connect(card, &NavOverlay::closed, &loop, [&](int r) {
        result = (r < 0) ? cancelIndex : r;
        loop.quit();
    });
    loop.exec(); // pad polling keeps running (timers fire inside the nested loop)
    return result;
}
