#include "PasscodePad.h"
#include "Nav.h"
#include "../../core/ProfilePasscode.h"   // kLength — the pad and the policy agree on ONE number
#include "../../input/InputMode.h"        // which device is driving, so the footer can name its buttons

#include <QEventLoop>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

// The digit grid, laid out the way a phone/remote keypad is (1 top-left, 0 bottom-centre) rather than a
// calculator's, because that is the muscle memory a PIN entry borrows from.
static const char* const kDigitRows[] = { "123", "456", "789" };

PasscodePad::PasscodePad(const QString& title, const QString& message, const QStringList& extraActions,
                         const std::function<void(const QString&, bool, int)>& onDone, QWidget* window)
    : NavOverlay(window), onDone_(onDone)
{
    auto* v = new QVBoxLayout(panel());
    v->setContentsMargins(22, 18, 22, 18);
    v->setSpacing(12);

    auto* t = new QLabel(title, panel());
    t->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    t->setWordWrap(true);
    t->setMaximumWidth(420);
    v->addWidget(t);

    if (!message.isEmpty())
    {
        auto* m = new QLabel(message, panel());
        m->setStyleSheet(QStringLiteral("color: #9aa0ad; font-size: 12px;"));
        m->setWordWrap(true);
        m->setMaximumWidth(420);
        v->addWidget(m);
    }

    // The four boxes. QLabels, not QLineEdits: nothing here is typed INTO, they are a progress readout, and a
    // focusable edit would join the ring and put four dead stops between the user and the keys.
    auto* boxRow = new QHBoxLayout;
    boxRow->setSpacing(10);
    boxRow->addStretch(1);
    for (int i = 0; i < ProfilePasscode::kLength; ++i)
    {
        auto* b = new QLabel(panel());
        b->setFixedSize(44, 52);
        b->setAlignment(Qt::AlignCenter);
        b->setFocusPolicy(Qt::NoFocus);
        boxes_.push_back(b);
        boxRow->addWidget(b);
    }
    boxRow->addStretch(1);
    v->addLayout(boxRow);
    refreshBoxes();

    auto* grid = new QGridLayout;
    grid->setSpacing(8);
    auto makeKey = [this](const QString& label) {
        auto* b = new QPushButton(label, panel());
        b->setFixedSize(64, 48);
        b->setFocusPolicy(Qt::StrongFocus);
        return b;
    };
    for (int r = 0; r < 3; ++r)
    {
        const QString row = QString::fromLatin1(kDigitRows[r]);
        for (int c = 0; c < row.size(); ++c)
        {
            QPushButton* b = makeKey(row.mid(c, 1));
            const QString d = row.mid(c, 1);
            connect(b, &QPushButton::clicked, this, [this, d] { insertDigit(d); });
            grid->addWidget(b, r, c);
        }
    }
    // The symbol captions here and the bullet in refreshBoxes() are built with QString::fromUtf8 and never
    // QStringLiteral: QStringLiteral wraps the source bytes in a UTF-16 literal, so what they mean depends on
    // the compiler and on /utf-8 being in force (it is here, from native/CMakeLists.txt — but CI also builds
    // this file into probe_nav with GCC, where the same bytes decay into separate code units and the key
    // renders as mojibake). Same note as the label table in PadGlyphs.cpp and the OSK's two symbol keys.
    // U+232B erase-to-the-left.
    { QPushButton* b = makeKey(QString::fromUtf8("\xe2\x8c\xab"));
      connect(b, &QPushButton::clicked, this, [this] { backspaceDigit(); });
      grid->addWidget(b, 3, 0); }
    { QPushButton* b = makeKey(QStringLiteral("0"));
      connect(b, &QPushButton::clicked, this, [this] { insertDigit(QStringLiteral("0")); });
      grid->addWidget(b, 3, 1); }
    { QPushButton* b = makeKey(QString::fromUtf8("\xe2\x9c\x95"));   // U+2715 multiplication X (cancel)
      b->setToolTip(tr("Cancel"));
      connect(b, &QPushButton::clicked, this, [this] { finish(false, -1); });
      grid->addWidget(b, 3, 2); }
    v->addLayout(grid);

    // Recovery rows, full width under the grid so they read as a different KIND of thing than a digit.
    for (int i = 0; i < extraActions.size(); ++i)
    {
        auto* b = new QPushButton(extraActions.at(i), panel());
        b->setMinimumHeight(38);
        b->setFocusPolicy(Qt::StrongFocus);
        connect(b, &QPushButton::clicked, this, [this, i] { finish(false, i); });
        v->addWidget(b);
        extras_.push_back(b);
    }

    // The footer names whichever device is driving — the same treatment, and the same reasoning, as the OSK's
    // (Osk.cpp): this pad sits one action away from that keyboard in the profile flows, and a hard-coded "B"
    // read as nonsense to a mouse user. Rebuilt on InputMode::changed() so picking up a pad mid-entry re-words
    // it. `hint` is the connection's CONTEXT object, so the subscription dies with the label — InputMode is a
    // process-wide singleton that outlives every overlay, which is exactly what a context-free connect() would
    // leave dangling.
    auto* hint = new QLabel(panel());
    hint->setStyleSheet(QStringLiteral("color: #9aa0ad; font-size: 11px;"));
    hint->setWordWrap(true);
    auto relabelHint = [hint] {
        InputMode& im = InputMode::instance();
        // Delete rides the BACK verb ("Esc"), NOT a literal "B": handleNavKey() deletes on Key_Backspace, and
        // the nav table sends Key_Backspace from RetroPad A — resolved through Gamepad::binding(), so a remap
        // moves it. hintText() resolves the same id the same way, so the two cannot drift. There is no "done"
        // arm to name here, unlike the OSK: this pad AUTO-SUBMITS on the last digit and has no commit button.
        hint->setText(im.padMode()
            //: Passcode pad footer, controller wording. %1 is a controller button name (e.g. "B", or "○" on
            //: a PlayStation pad).
            ? tr("%1: delete   (a real keyboard's number keys type directly)")
                  .arg(im.hintText(QStringLiteral("Esc")))
            // A keyboard user's number keys ARE typing directly, so the parenthetical would only tell them
            // what they are already doing. Backspace is the one key keyPressEvent() handles for deletion.
            //: Passcode pad footer, keyboard wording. This is a physical key name.
            : tr("Backspace: delete"));
    };
    relabelHint();
    connect(&InputMode::instance(), &InputMode::changed, hint, relabelHint);
    v->addWidget(hint);
}

void PasscodePad::refreshBoxes()
{
    for (int i = 0; i < boxes_.size(); ++i)
    {
        const bool filled = i < entered_.size();
        // A BULLET, never the digit. The pad is used on a TV in a shared room; echoing the glyph would show
        // the passcode to everyone in it, which is the one thing the lock is actually for.
        boxes_[i]->setText(filled ? QString::fromUtf8("\xe2\x97\x8f") : QString());
        boxes_[i]->setStyleSheet(QStringLiteral(
            "QLabel { background: #0d0f14; color: #e8eaf0; font-size: 22px;"
            "         border: 2px solid %1; border-radius: 8px; }")
            .arg(filled ? QStringLiteral("#5b8cff") : QStringLiteral("#2c2f3a")));
    }
}

void PasscodePad::insertDigit(const QString& d)
{
    if (done_ || entered_.size() >= ProfilePasscode::kLength) return;
    entered_ += d;
    refreshBoxes();
    // AUTO-SUBMIT on the last digit. There is no Done key by design: with a fixed length there is nothing
    // left to decide, and a Done the user has to arrow down to is one more press on every single entry.
    if (entered_.size() == ProfilePasscode::kLength) finish(true, -1);
}

void PasscodePad::backspaceDigit()
{
    if (done_ || entered_.isEmpty()) return;
    entered_.chop(1);
    refreshBoxes();
}

void PasscodePad::finish(bool accepted, int extraIndex)
{
    if (done_) return;   // auto-submit and a queued keypress must not both dispatch
    done_ = true;
    const QString code = accepted ? entered_ : QString();
    const auto cb = onDone_;
    entered_.clear();    // don't leave the digits sitting in the object past its usefulness
    dismiss(accepted ? 1 : 0);
    if (cb) cb(code, accepted, extraIndex);
}

QString PasscodePad::describe() const
{
    // The UI-test channel reads this. It reports PROGRESS, never content — a describe() that leaked the
    // digits would put the passcode into every uitest transcript and screenshot review.
    return tr("Passcode: %1 of %2 entered").arg(entered_.size()).arg(int(ProfilePasscode::kLength));
}

bool PasscodePad::handleNavKey(int key)
{
    switch (key)
    {
    case Qt::Key_Backspace:
        // Pad B deletes a digit; once empty it backs out — the same two-stage Back the OSK uses, so a remote
        // user never has to hunt for a Cancel target to leave.
        if (!entered_.isEmpty()) { backspaceDigit(); return true; }
        finish(false, -1);
        return true;
    case Qt::Key_Escape:
        finish(false, -1);
        return true;
    // NOTE: the bottom edge of the digit grid hopping to the recovery rows used to be handled HERE, because
    // NavRing::pickNext scored by widget centres and dropped any candidate more sideways than in-direction —
    // and a full-width recovery button's centre sits under the MIDDLE grid column, so from "⌫" or "✕" it was
    // ~89px sideways against ~60px down and lost. Down did nothing and the selection dead-ended in exactly
    // the corner a user who has forgotten their code arrives at. The ring now resolves Up/Down by ROW first
    // and column second, so the hop falls out of the general rule and the special case is gone. probe_nav
    // §22(c) still walks Down from every column — it is now a regression test of that rule.
    default:
        return NavOverlay::handleNavKey(key);   // arrows + Enter drive the key grid
    }
}

void PasscodePad::keyPressEvent(QKeyEvent* e)
{
    switch (e->key())
    {
    case Qt::Key_Backspace: backspaceDigit(); e->accept(); return;
    // Key_Back is Android's hardware/gesture/remote Back; Escape is Start on a pad. Both leave.
    case Qt::Key_Escape: case Qt::Key_Back: finish(false, -1); e->accept(); return;
    case Qt::Key_Return: case Qt::Key_Enter:
        // A SYNTHETIC Enter is the controller pressing the focused grid key — let the ring have it. A
        // physical one has nothing to submit (the pad auto-submits), so swallow it rather than letting it
        // activate whatever happens to hold focus.
        if (!NavContext::syntheticKey()) { e->accept(); return; }
        break;
    default: break;
    }
    const QString txt = e->text();
    if (txt.size() == 1 && txt.at(0) >= QLatin1Char('0') && txt.at(0) <= QLatin1Char('9'))
    {
        insertDigit(txt);
        e->accept();
        return;
    }
    NavOverlay::keyPressEvent(e);   // arrows etc.
}

QString PasscodePad::ask(const QString& title, const QString& message, const QStringList& extraActions,
                         int* extraChosen, QWidget* window, NavGraph* graph)
{
    if (extraChosen) *extraChosen = -1;
    QString result;   // stays null unless a full code was entered
    QEventLoop loop;
    auto* pad = new PasscodePad(title, message, extraActions,
                                [&](const QString& code, bool ok, int extra) {
                                    if (ok) result = code;
                                    else if (extraChosen) *extraChosen = extra;
                                }, window);
    pad->setNavGraph(graph);   // mirror as a level on a themed screen's back stack (null = classic)
    QObject::connect(pad, &NavOverlay::closed, &loop, [&loop](int) { loop.quit(); });
    loop.exec();
    return result;
}
