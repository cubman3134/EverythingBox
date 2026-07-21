#include "Osk.h"
#include "Nav.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QEventLoop>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

// The letter pages (lowercase; shift uppercases) and the symbols page, laid out per row.
// Both pages are exactly 10 keys per row — relabel() maps key -> caption by position, so the tables
// must stay rectangular.
static const char* kLetterRows[] = { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,._" };
static const char* kSymbolRows[] = { "1234567890", "!@#$%^&*()", "-+=/\\:;\"'?", "<>[]{}|~`_" };

int Osk::s_keyW = 46;          // desktop identity (see setKeyMetrics)
int Osk::s_keyH = 40;          // desktop identity
int Osk::s_previewFontPx = 15; // desktop identity

void Osk::setKeyMetrics(int keyW, int keyH, int previewFontPx)
{
    s_keyW = keyW;
    s_keyH = keyH;
    s_previewFontPx = previewFontPx;
}

Osk::Osk(const QString& title, const QString& initial, QLineEdit::EchoMode echo,
         const std::function<void(const QString&, bool)>& onDone, QWidget* window)
    : NavOverlay(window), onDone_(onDone)
{
    auto* v = new QVBoxLayout(panel());
    v->setContentsMargins(22, 18, 22, 18);
    v->setSpacing(12);

    auto* t = new QLabel(title, panel());
    t->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    t->setWordWrap(true);        // long prompts wrap over the keyboard, never clip
    t->setMaximumWidth(540);
    v->addWidget(t);

    preview_ = new QLineEdit(initial, panel());
    preview_->setEchoMode(echo);
    preview_->setReadOnly(true);          // the grid + physical keys edit it; the caret never owns input
    preview_->setFocusPolicy(Qt::NoFocus); // keep it out of the ring
    preview_->setMinimumWidth(430);
    // Preview font + key box size ride the form-factor tokens (s_previewFontPx / s_keyW / s_keyH, pushed by
    // applyFormFactorWidgets); defaults are today's 15px / 46×40 so desktop is a pixel-for-pixel no-op.
    preview_->setStyleSheet(QStringLiteral(
        "QLineEdit { background: #0d0f14; color: #e8eaf0; border: 1px solid #2c2f3a;"
        "            border-radius: 6px; padding: 8px 10px; font-size: %1px; }").arg(s_previewFontPx));
    v->addWidget(preview_);

    auto* grid = new QGridLayout;
    grid->setSpacing(6);
    auto makeKey = [this](const QString& label) {
        auto* b = new QPushButton(label, panel());
        b->setFixedSize(s_keyW, s_keyH);
        b->setFocusPolicy(Qt::StrongFocus);
        return b;
    };
    for (int r = 0; r < 4; ++r)
    {
        const QString row = QString::fromLatin1(kLetterRows[r]);
        for (int c = 0; c < row.size(); ++c)
        {
            QPushButton* b = makeKey(row.mid(c, 1));
            b->setProperty("pos", r * 100 + c); // row/col into the current page's tables (see relabel)
            connect(b, &QPushButton::clicked, this, [this, b] { insert(b->text()); });
            charKeys_.push_back(b);
            grid->addWidget(b, r, c);
        }
    }
    v->addLayout(grid);

    // Action row: Shift, symbols page, Space, delete, Cancel, Done.
    auto* actions = new QGridLayout;
    actions->setSpacing(6);
    // Width from the label itself (never below `least`): a fixed pixel budget clipped "Cancel"/"Done".
    auto makeAction = [this](const QString& label, int least, const std::function<void()>& fn) {
        auto* b = new QPushButton(label, panel());
        b->setFixedHeight(s_keyH); // match the char keys' height (desktop identity: 40)
        b->setMinimumWidth(qMax(least, b->fontMetrics().horizontalAdvance(label) + 30));
        b->setFocusPolicy(Qt::StrongFocus);
        connect(b, &QPushButton::clicked, this, fn);
        return b;
    };
    actions->addWidget(makeAction(QStringLiteral("⇧"), 56, [this] { shift_ = !shift_; relabel(); }), 0, 0);
    actions->addWidget(makeAction(QStringLiteral("#+="), 56, [this] { symbols_ = !symbols_; relabel(); }), 0, 1);
    actions->addWidget(makeAction(QStringLiteral("Space"), 140, [this] { insert(QStringLiteral(" ")); }), 0, 2);
    actions->addWidget(makeAction(QStringLiteral("⌫"), 56, [this] { backspaceChar(); }), 0, 3);
    actions->addWidget(makeAction(QStringLiteral("Cancel"), 80, [this] { dismiss(0); }), 0, 4);
    actions->addWidget(makeAction(QStringLiteral("Done"), 80, [this] { accept(); }), 0, 5);
    v->addLayout(actions);

    auto* hint = new QLabel(QStringLiteral("B: delete   Start: done   (a real keyboard types directly)"), panel());
    hint->setStyleSheet(QStringLiteral("color: #9aa0ad; font-size: 11px;"));
    hint->setWordWrap(true);
    v->addWidget(hint);
}

void Osk::relabel()
{
    const char** rows = symbols_ ? kSymbolRows : kLetterRows;
    for (QPushButton* b : charKeys_)
    {
        const int pos = b->property("pos").toInt();
        QString ch = QString::fromLatin1(rows[pos / 100]).mid(pos % 100, 1);
        if (!symbols_ && shift_) ch = ch.toUpper();
        b->setText(ch);
    }
}

void Osk::insert(const QString& s)
{
    preview_->setText(preview_->text() + s);
    if (shift_ && !symbols_) { shift_ = false; relabel(); } // shift is one-shot, like a phone keyboard
}

void Osk::backspaceChar()
{
    QString t = preview_->text();
    if (!t.isEmpty()) { t.chop(1); preview_->setText(t); }
}

void Osk::accept()
{
    const QString t = preview_->text();
    const auto done = onDone_;
    dismiss(1);
    if (done) done(t, true);
}

bool Osk::handleNavKey(int key)
{
    switch (key)
    {
    case Qt::Key_Backspace:
        // Pad B deletes a character; once the text is empty it backs out (cancel).
        if (!preview_->text().isEmpty()) { backspaceChar(); return true; }
        { const auto done = onDone_; dismiss(0); if (done) done(QString(), false); }
        return true;
    case Qt::Key_Escape:
        accept(); // Start commits, the console OSK convention
        return true;
    default:
        return NavOverlay::handleNavKey(key); // arrows + Enter drive the key grid
    }
}

void Osk::keyPressEvent(QKeyEvent* e)
{
    // Physical keyboard: type straight into the buffer.
    switch (e->key())
    {
    case Qt::Key_Backspace: backspaceChar(); e->accept(); return;
    case Qt::Key_Return: case Qt::Key_Enter:
        // A physical Enter means "done" (the user is typing, not driving the grid); a synthetic one is
        // the controller pressing the focused key button and goes to the ring.
        if (!NavContext::syntheticKey()) { accept(); e->accept(); return; }
        break;
    // Key_Back is Android's hardware/gesture/remote Back — cancel the OSK exactly like Escape, so the
    // keyboard is dismissable by the OS Back rather than swallowing it (an uncancellable OSK on a remote).
    case Qt::Key_Escape: case Qt::Key_Back:
    { const auto done = onDone_; dismiss(0); if (done) done(QString(), false); e->accept(); return; }
    default: break;
    }
    const QString txt = e->text();
    if (!txt.isEmpty() && txt.at(0).isPrint())
    {
        insert(txt);
        e->accept();
        return;
    }
    NavOverlay::keyPressEvent(e); // arrows etc.
}

QString Osk::getText(const QString& title, const QString& initial, QLineEdit::EchoMode echo, QWidget* window,
                     NavGraph* graph)
{
    QString result;   // null = cancelled
    QEventLoop loop;
    auto* osk = new Osk(title, initial, echo,
                        [&](const QString& t, bool ok) { if (ok) result = t; }, window);
    osk->setNavGraph(graph); // mirror as a level on a themed screen's back stack (null = classic behaviour)
    QObject::connect(osk, &NavOverlay::closed, &loop, [&loop](int) { loop.quit(); });
    loop.exec();
    return result;
}

// Declared on NavOverlay so NavRing (Nav.cpp) can trigger these without depending on Osk directly.

// A spinner's value edits through the OSK (numeric entry) — arrows only ever MOVE over spin rows, so a
// value can't change just by walking past it. Works via the "value" property, which QVariant converts
// from the typed string for both QSpinBox (int) and QDoubleSpinBox (double); the widget re-clamps to its
// own min/max on write.
void NavOverlay::editSpinBox(QAbstractSpinBox* spin)
{
    if (!spin) return;
    const QString initial = spin->property("value").toString();
    QPointer<QAbstractSpinBox> target(spin);
    new Osk(QStringLiteral("Enter a value"), initial, QLineEdit::Normal, [target](const QString& t, bool ok) {
        if (!ok || !target || t.trimmed().isEmpty()) return;
        target->setProperty("value", t.trimmed());
    });
}

void NavOverlay::editLineEdit(QLineEdit* edit)
{
    if (!edit) return;
    const QString title = edit->placeholderText().isEmpty()
                              ? QStringLiteral("Enter text") : edit->placeholderText();
    QPointer<QLineEdit> target(edit);
    new Osk(title, edit->text(), edit->echoMode(), [target](const QString& t, bool ok) {
        if (!ok || !target) return;
        target->setText(t);
        // Fire the edit's own submit path (search boxes act on returnPressed).
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(target, &press);
    });
}
