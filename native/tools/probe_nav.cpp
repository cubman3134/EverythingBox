// Headless regression tests for the controller-navigation kit (src/ui/nav): the invariants every screen
// relies on — a selection always exists, arrows move it geometrically and clamp at edges, overlays own
// input while open and restore the selection when closed, Back always routes somewhere, the on-screen
// keyboard types/deletes/commits. Runs under the offscreen QPA in CI (see run-headless-probes.sh).
// Prints NAV-OK on success; any failure prints NAV-FAIL <what> and exits non-zero.
#include "nav/Nav.h"
#include "nav/NavOverlay.h"
#include "nav/Osk.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, what) do { \
    if (!(cond)) { std::fprintf(stderr, "NAV-FAIL %s (line %d)\n", what, __LINE__); ++failures; } \
} while (0)

static void pump() { QApplication::processEvents(); QApplication::processEvents(); }

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    QWidget win;
    win.resize(1280, 720);
    NavContext ctx(&win);
    win.show();
    win.activateWindow();
    pump();

    // ---------------------------------------------------------------- 1. vertical ring: step + clamp
    {
        auto* page = new QWidget(&win);
        auto* v = new QVBoxLayout(page);
        QVector<QPushButton*> rows;
        for (int i = 0; i < 4; ++i) { auto* b = new QPushButton(QStringLiteral("row%1").arg(i), page); v->addWidget(b); rows.push_back(b); }
        page->setGeometry(0, 0, 400, 400);
        page->show();
        pump();

        NavRing ring(page);
        ctx.setActiveRing(&ring);
        CHECK(ring.ensureSelection() == rows[0], "initial selection lands on the first row");
        ctx.routeKey(Qt::Key_Down);
        ctx.routeKey(Qt::Key_Down);
        CHECK(QApplication::focusWidget() == rows[2], "two Downs reach row2");
        ctx.routeKey(Qt::Key_Down);
        ctx.routeKey(Qt::Key_Down);   // extra press past the end
        CHECK(QApplication::focusWidget() == rows[3], "Down clamps at the last row (no wrap)");
        ctx.routeKey(Qt::Key_Up); ctx.routeKey(Qt::Key_Up); ctx.routeKey(Qt::Key_Up); ctx.routeKey(Qt::Key_Up);
        CHECK(QApplication::focusWidget() == rows[0], "Up clamps at the first row");

        // -------------------------------------------------------- 2. selection survives a deleted row
        rows[0]->setFocus();
        pump();
        delete rows[0]; // the focused row disappears (a list rebuild, an uninstalled item...)
        pump();
        ring.ensureSelection();
        CHECK(QApplication::focusWidget() == rows[1], "deleting the focused row recovers to the nearest survivor");

        // -------------------------------------------------------- 3. remember/restore across a screen swap
        rows[2]->setFocus();
        const QString memo = ring.rememberSelection();
        rows[1]->setFocus();
        ring.restoreSelection(memo);
        CHECK(QApplication::focusWidget() == rows[2], "restoreSelection returns to the remembered row");

        // -------------------------------------------------------- 4. Back falls through to the back action
        bool backRan = false;
        ctx.setBackAction([&backRan] { backRan = true; });
        CHECK(ctx.routeKey(Qt::Key_Backspace), "Backspace is consumed on a ring screen");
        CHECK(backRan, "Backspace runs the screen's back action");
        ctx.setBackAction(nullptr);
        ctx.setActiveRing(nullptr);
        delete page;
        pump();
    }

    // ---------------------------------------------------------------- 5. grid: geometric 2D navigation
    {
        auto* page = new QWidget(&win);
        auto* g = new QGridLayout(page);
        QPushButton* cell[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
            { cell[r][c] = new QPushButton(QStringLiteral("%1,%2").arg(r).arg(c), page); g->addWidget(cell[r][c], r, c); }
        page->setGeometry(0, 0, 400, 300);
        page->show();
        pump();

        NavRing ring(page);
        ctx.setActiveRing(&ring);
        cell[1][1]->setFocus();
        ctx.routeKey(Qt::Key_Right);
        CHECK(QApplication::focusWidget() == cell[1][2], "Right moves within the row");
        ctx.routeKey(Qt::Key_Down);
        CHECK(QApplication::focusWidget() == cell[2][2], "Down stays in the column");
        ctx.routeKey(Qt::Key_Left);
        ctx.routeKey(Qt::Key_Up);
        CHECK(QApplication::focusWidget() == cell[1][1], "Left+Up return to the centre");
        ctx.setActiveRing(nullptr);
        delete page;
        pump();
    }

    // ---------------------------------------------------------------- 6. menu overlay: choose + restore
    {
        auto* behind = new QPushButton(QStringLiteral("behind"), &win);
        behind->setGeometry(10, 10, 120, 30);
        behind->show();
        behind->setFocus();
        pump();

        int chosen = -2;
        auto* menu = new NavMenu(QStringLiteral("Actions"), { QStringLiteral("Play"), QStringLiteral("Favorite"), QStringLiteral("Uninstall") },
                                 [&chosen](int row) { chosen = row; }, &win);
        pump();
        CHECK(NavOverlay::topmost() == menu, "the menu is the topmost overlay");
        CHECK(ctx.routeKey(Qt::Key_Down), "overlay consumes nav keys");
        ctx.routeKey(Qt::Key_Return);
        pump();
        CHECK(chosen == 1, "Down+Enter chooses the second row");
        CHECK(NavOverlay::topmost() == nullptr, "the menu closed after choosing");
        CHECK(QApplication::focusWidget() == behind, "closing the overlay restores the previous selection");

        // -------------------------------------------------------- 7. stacked overlays + Back unwinding
        auto* menu2 = new NavMenu(QStringLiteral("Outer"), { QStringLiteral("a"), QStringLiteral("b") }, nullptr, &win);
        auto* confirm = new NavConfirm(QStringLiteral("Sure?"), QStringLiteral("Really do it?"),
                                       { QStringLiteral("Do it"), QStringLiteral("Cancel") }, 0, &win);
        pump();
        CHECK(NavOverlay::topmost() == confirm, "the confirm stacks on top of the menu");
        ctx.routeKey(Qt::Key_Backspace); // back out of the confirm
        pump();
        CHECK(NavOverlay::topmost() == menu2, "Back pops only the top overlay");
        ctx.routeKey(Qt::Key_Backspace); // back out of the menu
        pump();
        CHECK(NavOverlay::topmost() == nullptr, "Back unwinds the whole overlay stack");
        CHECK(QApplication::focusWidget() == behind, "the original selection is restored after unwinding");
        delete behind;
        pump();
    }

    // ---------------------------------------------------------------- 8. OSK: type, delete, commit, cancel
    {
        QString committed;
        bool ok = false;
        auto* osk = new Osk(QStringLiteral("Enter text"), QStringLiteral("ab"), QLineEdit::Normal,
                            [&](const QString& t, bool accepted) { committed = t; ok = accepted; }, &win);
        pump();
        CHECK(NavOverlay::topmost() == osk, "the OSK is the topmost overlay");
        ctx.routeKey(Qt::Key_Backspace);              // pad B deletes a character
        CHECK(osk->text() == QStringLiteral("a"), "pad Back deletes one character");
        ctx.routeKey(Qt::Key_Escape);                 // Start commits
        pump();
        CHECK(ok && committed == QStringLiteral("a"), "Start commits the buffer");
        CHECK(NavOverlay::topmost() == nullptr, "the OSK closed on commit");

        // Backing out with an empty buffer cancels.
        QString c2 = QStringLiteral("sentinel");
        bool ok2 = true;
        new Osk(QStringLiteral("t"), QString(), QLineEdit::Normal,
                [&](const QString& t, bool accepted) { c2 = t; ok2 = accepted; }, &win);
        pump();
        ctx.routeKey(Qt::Key_Backspace);
        pump();
        CHECK(!ok2 && c2.isNull(), "Back on an empty OSK cancels (null result)");
        CHECK(NavOverlay::topmost() == nullptr, "the cancelled OSK closed");
    }

    // ---------------------------------------------------------------- 9. a text row opens the OSK on Enter
    {
        auto* page = new QWidget(&win);
        auto* v = new QVBoxLayout(page);
        auto* edit = new QLineEdit(page);
        edit->setPlaceholderText(QStringLiteral("Search"));
        v->addWidget(edit);
        v->addWidget(new QPushButton(QStringLiteral("Go"), page));
        page->setGeometry(0, 0, 300, 120);
        page->show();
        pump();

        NavRing ring(page);
        ctx.setActiveRing(&ring);
        ring.ensureSelection();
        CHECK(QApplication::focusWidget() == edit, "the text row is arrow-selectable");
        ctx.routeKey(Qt::Key_Return);
        pump();
        CHECK(NavOverlay::topmost() != nullptr, "Enter on a text row opens the on-screen keyboard");
        ctx.routeKey(Qt::Key_Escape); // commit (empty)
        pump();
        CHECK(NavOverlay::topmost() == nullptr, "the OSK closes back to the screen");
        ctx.setActiveRing(nullptr);
        delete page;
        pump();
    }

    // ---------------------------------------------------------------- 10. no overlay text is ever cut off
    {
        auto fits = [](NavOverlay* o, const char* what) {
            pump(); pump(); // the deferred showEvent sizing must run first
            const QStringList bad = o->clippedTexts();
            for (const QString& b : bad)
                std::fprintf(stderr, "NAV-FAIL %s: %s\n", what, b.toUtf8().constData());
            failures += bad.size();
        };
        // A confirm card with a long word-wrapped message (the uninstall warning shape) — this is the case
        // adjustSize() used to under-measure, cutting the text off.
        auto* confirm = new NavConfirm(
            QStringLiteral("Permanently remove My Media Vault and all of its data?"),
            QStringLiteral("This deletes the whole app folder. That includes your settings, cloud sign-in, "
                           "downloaded games and music, emulator saves and save states, and installed "
                           "emulators and cores, plus the cache and crash logs. This cannot be undone. If you "
                           "want to keep any downloads, copy them out of that folder first, then run this "
                           "again once you have everything you care about safely backed up somewhere else."),
            { QStringLiteral("Uninstall everything now"), QStringLiteral("Cancel") }, 1, &win);
        fits(confirm, "confirm(long message)");
        confirm->dismiss(-1);
        pump();

        // A menu with a long title and long rows (a long game name in the Recent menu).
        auto* menu = new NavMenu(
            QStringLiteral("Super Ultra Mega Fighting Legends II: The Definitive Championship Edition (USA, Rev 2)"),
            { QStringLiteral("▶   Play"), QStringLiteral("☆   Favorite"),
              QStringLiteral("➕   Add to the playlist named after my very favourite childhood memories…"),
              QStringLiteral("🗑   Uninstall (delete the downloaded file from disk)") },
            nullptr, &win);
        fits(menu, "menu(long title+rows)");
        menu->dismiss(-1);
        pump();

        // The on-screen keyboard itself.
        auto* osk = new Osk(QStringLiteral("Enter the name of the playlist you would like to create"),
                            QStringLiteral("some text"), QLineEdit::Normal, nullptr, &win);
        fits(osk, "osk");
        osk->dismiss(0);
        pump();
    }

    // ---------------------------------------------------------------- 11. rows "act right" under the ring
    {
        auto* page = new QWidget(&win);
        auto* v = new QVBoxLayout(page);
        auto* check = new QCheckBox(QStringLiteral("Enable the thing"), page);
        auto* combo = new QComboBox(page);
        combo->addItems({ QStringLiteral("Any genre"), QStringLiteral("Action"), QStringLiteral("Puzzle") });
        auto* slider = new QSlider(Qt::Horizontal, page);
        slider->setRange(0, 100); slider->setValue(50);
        auto* spin = new QSpinBox(page);
        spin->setRange(0, 99); spin->setValue(7);
        auto* btn = new QPushButton(QStringLiteral("Apply"), page);
        v->addWidget(check); v->addWidget(combo); v->addWidget(slider); v->addWidget(spin); v->addWidget(btn);
        page->setGeometry(0, 0, 360, 320);
        page->show();
        pump();

        NavRing ring(page);
        ctx.setActiveRing(&ring);

        // The compound-row rule: the spinbox's internal line edit must not be a second ring stop.
        CHECK(!ring.widgets().contains(spin->findChild<QLineEdit*>()),
              "a spinbox's internal line edit is not its own ring stop");

        // Walking over a dropdown/spinner NEVER changes its value.
        check->setFocus(); pump();
        ctx.routeKey(Qt::Key_Down); // onto the combo
        CHECK(QApplication::focusWidget() == combo, "Down lands on the dropdown");
        ctx.routeKey(Qt::Key_Down); // over it, onto the slider
        CHECK(combo->currentIndex() == 0, "walking over a dropdown does not change its value");
        CHECK(QApplication::focusWidget() == slider, "Down moves past the dropdown");
        ctx.routeKey(Qt::Key_Down); // onto the spin
        CHECK(QApplication::focusWidget() == spin, "Down lands on the spinner");
        ctx.routeKey(Qt::Key_Down); // over it
        CHECK(spin->value() == 7, "walking over a spinner does not change its value");

        // A slider row: Left/Right edit the value (the ring hands them through).
        slider->setFocus(); pump();
        CHECK(!ring.handleKey(Qt::Key_Left), "the ring hands Left through to a slider");
        { QKeyEvent left(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier); QApplication::sendEvent(slider, &left); }
        CHECK(slider->value() < 50, "Left adjusts the slider value");

        // A checkbox row: Enter toggles it.
        check->setFocus(); pump();
        ctx.routeKey(Qt::Key_Return);
        CHECK(check->isChecked(), "Enter toggles a checkbox");

        // A dropdown row: Enter opens the popup (hover-then-select), and the value still didn't change.
        combo->setFocus(); pump();
        ctx.routeKey(Qt::Key_Return);
        pump();
        CHECK(combo->view() && combo->view()->isVisible(), "Enter pops a dropdown open");
        combo->hidePopup();
        pump();
        CHECK(combo->currentIndex() == 0, "opening the dropdown did not change its value");

        // A spinner row: Enter opens the OSK on the value; committing writes it back.
        spin->setFocus(); pump();
        ctx.routeKey(Qt::Key_Return);
        pump();
        auto* valueOsk = qobject_cast<Osk*>(NavOverlay::topmost());
        CHECK(valueOsk != nullptr, "Enter on a spinner opens the on-screen keyboard");
        if (valueOsk)
        {
            CHECK(valueOsk->text() == QStringLiteral("7"), "the OSK starts from the spinner's value");
            ctx.routeKey(Qt::Key_Backspace);           // delete the 7
            QKeyEvent four(QEvent::KeyPress, Qt::Key_4, Qt::NoModifier, QStringLiteral("42"));
            QApplication::sendEvent(valueOsk, &four);  // physical typing path
            ctx.routeKey(Qt::Key_Escape);              // commit
            pump();
            CHECK(spin->value() == 42, "committing the OSK writes the spinner value");
        }
        ctx.setActiveRing(nullptr);
        delete page;
        pump();
    }

    // ------------------------------------------- 12. settings-panel shape: scroll area + header Back
    {
        // Regression: QScrollArea holds StrongFocus by default, so it joined the ring as a "member" and
        // the compound-row filter then dropped every row inside it — the watchdog kept snapping the
        // selection back to the header's Back button whenever the user paused.
        auto* page = new QWidget(&win);
        auto* v = new QVBoxLayout(page);
        auto* back = new QPushButton(QStringLiteral("‹ Back"), page);
        v->addWidget(back);
        auto* scroll = new QScrollArea(page);
        scroll->setWidgetResizable(true);
        auto* content = new QWidget;
        auto* cv = new QVBoxLayout(content);
        QVector<QPushButton*> rows;
        for (int i = 0; i < 8; ++i)
        { auto* b = new QPushButton(QStringLiteral("setting %1").arg(i), content); cv->addWidget(b); rows.push_back(b); }
        scroll->setWidget(content);
        v->addWidget(scroll, 1);
        page->setGeometry(0, 0, 420, 340); // short enough that the rows actually scroll
        page->show();
        pump();

        NavRing ring(page);
        ctx.setActiveRing(&ring);
        const QVector<QWidget*> members = ring.widgets();
        CHECK(!members.contains(scroll), "a plain scroll container is not a ring stop");
        CHECK(members.contains(back) && members.contains(rows[0]) && members.contains(rows[7]),
              "the Back button and the scrolled rows are all ring stops");

        rows[0]->setFocus();
        pump();
        for (int i = 0; i < 7; ++i) ctx.routeKey(Qt::Key_Down); // walk to the bottom (scrolls on the way)
        CHECK(QApplication::focusWidget() == rows[7], "Down walks every row inside the scroll area");
        ctx.ensureFocus(); // the watchdog tick that used to snap the selection to Back
        pump();
        ctx.ensureFocus();
        CHECK(QApplication::focusWidget() == rows[7], "the focus watchdog never steals the selection");
        ctx.routeKey(Qt::Key_Down); // clamp at the end
        CHECK(QApplication::focusWidget() == rows[7], "Down clamps at the last row");
        for (int i = 0; i < 12; ++i) ctx.routeKey(Qt::Key_Up); // all the way back up
        CHECK(QApplication::focusWidget() == back, "Up walks out of the scroll area to the header Back");
        ctx.setActiveRing(nullptr);
        delete page;
        pump();
    }

    // ------------------------------------------- 13. profile-menu shape: selection NEVER lost
    {
        // Mirror of the Profiles screen: header Back, then rows of [wide pick | ✎ 36px | ✕ 36px],
        // a Create button and a Cancel button. Hammering arrows in any pattern must always leave the
        // focus on a live ring member — the selection can never vanish.
        auto* page = new QWidget(&win);
        auto* v = new QVBoxLayout(page);
        v->addWidget(new QPushButton(QStringLiteral("‹ Back"), page));
        for (int r = 0; r < 3; ++r)
        {
            auto* row = new QHBoxLayout;
            auto* pick = new QPushButton(QStringLiteral("🐱   Profile %1").arg(r), page);
            pick->setMinimumHeight(44);
            row->addWidget(pick, 1);
            auto* edit = new QPushButton(QStringLiteral("✎"), page); edit->setFixedWidth(36); row->addWidget(edit);
            auto* del  = new QPushButton(QStringLiteral("✕"), page); del->setFixedWidth(36);  row->addWidget(del);
            v->addLayout(row);
        }
        v->addWidget(new QPushButton(QStringLiteral("＋  Create New Profile"), page));
        v->addWidget(new QPushButton(QStringLiteral("Cancel"), page));
        v->addStretch(1);
        page->setGeometry(0, 0, 420, 460);
        page->show();
        pump();

        NavRing ring(page);
        ctx.setActiveRing(&ring);
        ring.ensureSelection();
        static const int walk[] = { Qt::Key_Down, Qt::Key_Down, Qt::Key_Down, Qt::Key_Right, Qt::Key_Down,
                                    Qt::Key_Down, Qt::Key_Down, Qt::Key_Down, Qt::Key_Down, Qt::Key_Left,
                                    Qt::Key_Up, Qt::Key_Right, Qt::Key_Right, Qt::Key_Down, Qt::Key_Down,
                                    Qt::Key_Down, Qt::Key_Up, Qt::Key_Up, Qt::Key_Up, Qt::Key_Up,
                                    Qt::Key_Up, Qt::Key_Up, Qt::Key_Up, Qt::Key_Down };
        int step = 0;
        for (int key : walk)
        {
            ctx.routeKey(key);
            QWidget* fw = QApplication::focusWidget();
            if (!fw || !ring.widgets().contains(fw) || !fw->isVisible())
            {
                std::fprintf(stderr, "NAV-FAIL selection lost after step %d (key %d): focus=%p\n",
                             step, key, static_cast<void*>(fw));
                ++failures;
                break;
            }
            ++step;
        }
        ctx.ensureFocus(); // and the watchdog can't lose it either
        CHECK(QApplication::focusWidget() && ring.widgets().contains(QApplication::focusWidget()),
              "the selection survives an arbitrary arrow-key hammering");
        ctx.setActiveRing(nullptr);
        delete page;
        pump();
    }

    // ------------------------------------------- 14. one Back rule: Escape == Backspace, everywhere
    {
        auto* page = new QWidget(&win);
        auto* v = new QVBoxLayout(page);
        for (int i = 0; i < 3; ++i) v->addWidget(new QPushButton(QStringLiteral("row%1").arg(i), page));
        page->setGeometry(0, 0, 300, 240);
        page->show();
        pump();
        NavRing ring(page);
        ctx.setActiveRing(&ring);

        int backs = 0;
        ctx.setBackAction([&backs] { ++backs; });
        // Both keys must run the SAME back action, and neither may leak (both consumed).
        CHECK(ctx.routeKey(Qt::Key_Backspace), "Backspace is consumed on a ring screen");
        CHECK(backs == 1, "Backspace runs the screen's back action");
        CHECK(ctx.routeKey(Qt::Key_Escape), "Escape is consumed on a ring screen");
        CHECK(backs == 2, "Escape runs the SAME back action as Backspace");
        // Arrows still move the selection (Back handling didn't swallow everything).
        const int before = ring.widgets().indexOf(QApplication::focusWidget());
        ctx.routeKey(Qt::Key_Down);
        CHECK(ring.widgets().indexOf(QApplication::focusWidget()) != before, "arrows still navigate");
        ctx.setBackAction(nullptr);
        ctx.setActiveRing(nullptr);
        delete page;
        pump();
    }

    if (failures) { std::fprintf(stderr, "NAV-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("NAV-OK\n");
    return 0;
}
