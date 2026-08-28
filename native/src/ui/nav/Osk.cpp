#include "Osk.h"
#include "Nav.h"
#include "../../input/InputMode.h"   // which device is driving, so the footer can name its buttons

#include <QAbstractSpinBox>
#include <QApplication>
#include <QEventLoop>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputMethod>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

// True where the OS provides its own virtual keyboard (iOS/Android): text entry pops THAT instead of
// the controller-oriented key grid — native typing is what touch users expect, and the grid is wider
// than a phone screen anyway. Desktop/TV keep the grid (their input is a pad/remote, not a screen).
static bool nativeKeyboardPreferred()
{
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
    return true;
#else
    return false;
#endif
}

// The native-input drop-in for the OSK on mobile: a scrimmed IN-WINDOW card holding a REAL QLineEdit —
// giving it focus summons the system keyboard (Qt's platform input context handles show/hide). It must
// live inside the main window: iOS presents exactly one window, so a separate QDialog never becomes
// visible. The card sits in the upper third so the OS keyboard (bottom half) can't cover it. Blocking
// like Osk::getText's nested loop; a null return means cancelled.
static QString nativeTextPrompt(const QString& title, const QString& initial,
                                QLineEdit::EchoMode echo, QWidget* window)
{
    QWidget* host = window ? window->window() : QApplication::activeWindow();
    if (!host) return QString();

    QWidget scrim(host);
    scrim.setAttribute(Qt::WA_StyledBackground, true);
    scrim.setStyleSheet(QStringLiteral("background:rgba(0,0,0,150);"));
    scrim.setGeometry(host->rect());

    auto* card = new QWidget(&scrim);
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setStyleSheet(QStringLiteral("background:#161A20; border-radius:12px;"));
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(16, 16, 16, 16);
    auto* label = new QLabel(title, card);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color:#E6ECF3; font-size:16px; font-weight:bold; background:transparent;"));
    v->addWidget(label);
    auto* edit = new QLineEdit(initial, card);
    edit->setEchoMode(echo);
    edit->setStyleSheet(QStringLiteral(
        "background:#0F1216; color:#E6ECF3; border:1px solid #2A3540; border-radius:6px; padding:10px; font-size:16px;"));
    v->addWidget(edit);
    auto* row = new QHBoxLayout;
    auto* cancel = new QPushButton(QObject::tr("Cancel"), card);
    auto* done = new QPushButton(QObject::tr("Done"), card);
    const QString btnCss = QStringLiteral(
        "QPushButton{color:#E6ECF3; background:%1; border:none; border-radius:8px; padding:10px 18px; font-size:15px;}");
    cancel->setStyleSheet(btnCss.arg(QStringLiteral("#243244")));
    done->setStyleSheet(btnCss.arg(QStringLiteral("#3A6FB0")));
    row->addStretch(1); row->addWidget(cancel); row->addWidget(done);
    v->addLayout(row);

    const int w = qMin(host->width() - 32, 480);
    card->setFixedWidth(w);
    card->adjustSize();
    card->move((host->width() - w) / 2, qMax(16, host->height() / 6));

    QEventLoop loop;
    bool accepted = false;
    QObject::connect(cancel, &QPushButton::clicked, &loop, [&] { loop.quit(); });
    QObject::connect(done, &QPushButton::clicked, &loop, [&] { accepted = true; loop.quit(); });
    QObject::connect(edit, &QLineEdit::returnPressed, &loop, [&] { accepted = true; loop.quit(); });

    scrim.show();
    scrim.raise();
    edit->setFocus();
    edit->selectAll();
    if (QGuiApplication::inputMethod()) QGuiApplication::inputMethod()->show();
    loop.exec();
    if (QGuiApplication::inputMethod()) QGuiApplication::inputMethod()->hide();
    return accepted ? edit->text() : QString();
}

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
            // "oskPos", NOT "pos": QWidget ALREADY declares a `pos` property (a QPoint), so setProperty("pos",
            // int) is swallowed by the real property instead of creating a dynamic one, and relabel() then
            // read back 0 for every key — after Shift or #+= the whole grid relabelled to "1", and typing
            // inserted "1". That made every uppercase letter and symbol unreachable on a pad or remote.
            // A dynamic-property name must not collide with any Q_PROPERTY up the widget's meta-object chain.
            b->setProperty("oskPos", r * 100 + c); // row/col into the current page's tables (see relabel)
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
    // The two symbol captions are built with QString::fromUtf8, never QStringLiteral: QStringLiteral wraps the
    // source bytes in a UTF-16 literal, so what they mean depends on the compiler and on /utf-8 being in force
    // (it is here, from native/CMakeLists.txt — but CI also builds this file into probe_nav/probe_navqml with
    // GCC, where the same bytes decay into separate code units and the key renders as mojibake). See the same
    // note over the label table in PadGlyphs.cpp. U+21E7 upwards-white-arrow (shift), U+232B erase-to-the-left.
    actions->addWidget(makeAction(QString::fromUtf8("\xe2\x87\xa7"), 56, [this] { shift_ = !shift_; relabel(); }), 0, 0);
    actions->addWidget(makeAction(QStringLiteral("#+="), 56, [this] { symbols_ = !symbols_; relabel(); }), 0, 1);
    actions->addWidget(makeAction(QStringLiteral("Space"), 140, [this] { insert(QStringLiteral(" ")); }), 0, 2);
    actions->addWidget(makeAction(QString::fromUtf8("\xe2\x8c\xab"), 56, [this] { backspaceChar(); }), 0, 3);
    actions->addWidget(makeAction(QStringLiteral("Cancel"), 80, [this] { dismiss(0); }), 0, 4);
    actions->addWidget(makeAction(QStringLiteral("Done"), 80, [this] { accept(); }), 0, 5);
    v->addLayout(actions);

    // The footer names whichever device is driving. It was controller-worded unconditionally, which read as
    // nonsense to a mouse user typing into it. Rebuilt on InputMode::changed() so picking up a pad while the
    // keyboard is open re-words it under the user's hands. `hint` is the connection's CONTEXT object, so the
    // subscription dies with the label — and InputMode is a process-wide singleton that outlives every OSK,
    // which is exactly the case a context-free connect() would leak into a dangling call.
    auto* hint = new QLabel(panel());
    hint->setStyleSheet(QStringLiteral("color: #9aa0ad; font-size: 11px;"));
    hint->setWordWrap(true);
    auto relabelHint = [hint] {
        InputMode& im = InputMode::instance();
        // BOTH arms of the pad wording go through hintText(), so neither can drift from the button the user
        // actually has to press. Delete rides the BACK verb ("Esc") — the nav table sends Key_Backspace from
        // RetroPad A, which handleNavKey() deletes on. Done rides the MENU verb ("Start") — the nav table
        // sends Key_Escape from RetroPad START, which handleNavKey() commits on ("Start commits, the console
        // OSK convention"). Naming the Confirm verb ("Enter") for done would be a WRONG button: on this
        // overlay Confirm presses whichever key of the grid has focus instead of finishing.
        //
        // Both arms therefore follow a REMAP as well as a brand: pollMenuPad resolves both ids through
        // Gamepad::binding(), and so does hintText(). A footer that spelled either as a literal would go
        // stale the moment a user rebound that row in the input panel — which is exactly what the literal
        // "Start" here used to do while the delete arm beside it tracked the remap correctly.
        hint->setText(im.padMode()
            //: On-screen keyboard footer, controller wording. %1 and %2 are controller button names
            //: (e.g. "B" and "Menu", or "○" and "Options" on a PlayStation pad).
            ? tr("%1: delete   %2: done   (a real keyboard types directly)")
                  .arg(im.hintText(QStringLiteral("Esc")), im.hintText(QStringLiteral("Start")))
            // A keyboard user IS typing directly, so the parenthetical would be telling them what they are
            // already doing. Their two keys are the ones keyPressEvent() actually handles.
            //: On-screen keyboard footer, keyboard wording. These are physical key names.
            : tr("Backspace: delete   Enter: done"));
    };
    relabelHint();
    connect(&InputMode::instance(), &InputMode::changed, hint, relabelHint);
    v->addWidget(hint);
}

void Osk::relabel()
{
    const char** rows = symbols_ ? kSymbolRows : kLetterRows;
    for (QPushButton* b : charKeys_)
    {
        const int pos = b->property("oskPos").toInt();
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
    if (nativeKeyboardPreferred())
        return nativeTextPrompt(title, initial, echo, window);
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
    if (nativeKeyboardPreferred())
    {
        const QString t = nativeTextPrompt(QStringLiteral("Enter a value"), initial,
                                           QLineEdit::Normal, spin->window());
        if (!t.trimmed().isEmpty()) spin->setProperty("value", t.trimmed());
        return;
    }
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
    if (nativeKeyboardPreferred())
    {
        const QString t = nativeTextPrompt(title, edit->text(), edit->echoMode(), edit->window());
        if (t.isNull()) return; // cancelled
        edit->setText(t);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(edit, &press);
        return;
    }
    QPointer<QLineEdit> target(edit);
    new Osk(title, edit->text(), edit->echoMode(), [target](const QString& t, bool ok) {
        if (!ok || !target) return;
        target->setText(t);
        // Fire the edit's own submit path (search boxes act on returnPressed).
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(target, &press);
    });
}
