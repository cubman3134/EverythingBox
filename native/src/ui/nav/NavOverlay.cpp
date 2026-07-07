#include "NavOverlay.h"
#include "Nav.h"

#include <QApplication>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

QVector<QPointer<NavOverlay>> NavOverlay::s_stack;

// ---------------------------------------------------------------- NavOverlay

NavOverlay::NavOverlay(QWidget* window)
    : QWidget(window ? window : (NavContext::instance() ? NavContext::instance()->window() : nullptr))
{
    QWidget* host = parentWidget();
    Q_ASSERT(host); // overlays only exist inside the main window
    setGeometry(host->rect());
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("NavOverlay { background: rgba(8,10,16,150); }")); // dim the page behind

    panel_ = new QFrame(this);
    panel_->setObjectName(QStringLiteral("navOverlayPanel"));
    panel_->setStyleSheet(QStringLiteral(
        "#navOverlayPanel { background: #14161d; border: 1px solid #2c2f3a; border-radius: 10px; }"
        "QLabel { color: #e8eaf0; }"
        "QPushButton { background: #1d2029; color: #e8eaf0; border: 1px solid #2c2f3a;"
        "              border-radius: 6px; padding: 8px 18px; }"
        "QPushButton:focus { background: #2f5fd0; border-color: #4a79e8; }"
        "QListWidget { background: transparent; border: none; color: #e8eaf0; outline: none; }"
        "QListWidget::item { padding: 9px 14px; border-radius: 6px; }"
        "QListWidget::item:selected { background: #2f5fd0; }"));
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
        prevFocus_->setFocus(Qt::OtherFocusReason); // restore the selection from before we opened
    else if (NavContext::instance())
        NavContext::instance()->ensureFocus();      // its widget died: land somewhere valid
    emit closed(result_);
    deleteLater();
}

bool NavOverlay::handleNavKey(int key)
{
    switch (key)
    {
    case Qt::Key_Backspace: case Qt::Key_Escape:
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
        handleNavKey(e->key());
        e->accept();
        return;
    default:
        e->accept(); // swallow; subclasses (the OSK) override to accept typed text
        return;
    }
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
    // Centre the panel once its layout knows its size, and land the selection.
    QTimer::singleShot(0, this, [this] {
        if (dismissed_) return;
        panel_->adjustSize();
        panel_->move((width() - panel_->width()) / 2, (height() - panel_->height()) / 2);
        ring_->ensureSelection();
    });
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
    t->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    v->addWidget(t);
    list_ = new QListWidget(panel());
    list_->addItems(items);
    list_->setFocusPolicy(Qt::NoFocus);       // the overlay drives it; no Qt focus fights
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Show every row without scrolling (the menus are short); width fits the longest row.
    int h = list_->frameWidth() * 2, w = 260;
    for (int i = 0; i < list_->count(); ++i)
    {
        h += list_->sizeHintForRow(i) + 2;
        w = qMax(w, list_->sizeHintForColumn(0) + 40);
    }
    list_->setFixedSize(w, h + 6);
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
    t->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    v->addWidget(t);
    if (!message.isEmpty())
    {
        auto* m = new QLabel(message, panel());
        m->setWordWrap(true);
        m->setMaximumWidth(520);
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
