// Headless regression tests for the controller-navigation kit (src/ui/nav): the invariants every screen
// relies on — a selection always exists, arrows move it geometrically and clamp at edges, overlays own
// input while open and restore the selection when closed, Back always routes somewhere, the on-screen
// keyboard types/deletes/commits. Runs under the offscreen QPA in CI (see run-headless-probes.sh).
// Prints NAV-OK on success; any failure prints NAV-FAIL <what> and exits non-zero.
#include "nav/Nav.h"
#include "nav/NavOverlay.h"
#include "nav/Osk.h"

#include <QApplication>
#include <QGridLayout>
#include <QLineEdit>
#include <QPushButton>
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

    if (failures) { std::fprintf(stderr, "NAV-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("NAV-OK\n");
    return 0;
}
