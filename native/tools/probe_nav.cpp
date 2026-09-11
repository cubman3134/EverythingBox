// Headless regression tests for the controller-navigation kit (src/ui/nav): the invariants every screen
// relies on — a selection always exists, arrows move it geometrically and clamp at edges, overlays own
// input while open and restore the selection when closed, Back always routes somewhere, the on-screen
// keyboard types/deletes/commits. Runs under the offscreen QPA in CI (see run-headless-probes.sh).
// Prints NAV-OK on success; any failure prints NAV-FAIL <what> and exits non-zero.
#include "nav/Nav.h"
#include "nav/NavGraph.h"
#include "nav/NavThemeGraph.h"   // §20: the ONE builder of the themed surface's zones/edges (both cats shapes)
#include "nav/NavOverlay.h"
#include "nav/Osk.h"
#include "nav/PasscodePad.h"
#include "ProfileDialog.h"
#include "ProfileStore.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cstdio>
#include <deque>     // §20: the arrow-reachability walk over both themed graph shapes
#include <set>
#include <utility>

static int failures = 0;
// NAV_VERBOSE=1 also prints every check that PASSES, so two runs can be compared check by check (#351's DPI
// table). Off by default: the gate reads NAV-OK / NAV-FAIL only.
static const bool navVerbose = qEnvironmentVariableIsSet("NAV_VERBOSE");
#define CHECK(cond, what) do { \
    if (!(cond)) { std::fprintf(stderr, "NAV-FAIL %s (line %d)\n", what, __LINE__); ++failures; } \
    else if (navVerbose) std::fprintf(stderr, "NAV-PASS %s (line %d)\n", what, __LINE__); \
} while (0)

// Stand-in for a themed page's QML root item (ThemeEngine::buildView exposes the real one through the
// widget's "mmvQuickRoot" property): dismissing an overlay must invoke forceActiveFocus() on it, because
// widget focus alone does not revive a QQuickWidget scene's active-focus item after the keyboard grab.
class FakeQuickRoot : public QObject
{
    Q_OBJECT
public:
    int kicks = 0;
    Q_INVOKABLE void forceActiveFocus() { ++kicks; }
};

// §26: a stand-in for MainWindow's key routing, and the ONLY piece of it that matters here — a key that
// reaches the window is handed to NavContext::routeKey (MainWindow::keyPressEvent, the panelPage_ branch).
// The probe needs a real window with that one line, because issue #305's loop is closed by it: the ring
// synthesises a Return, the focused widget leaves it unaccepted, QApplication::notify propagates it up the
// parent chain to the window, and the window routes it straight back into the ring. A bare QWidget host
// would never see the bounce and the probe would pass against the broken code.
class RoutingWindow : public QWidget
{
public:
    int entries = 0;      // how many key presses reached this window at all
    int depth = 0;        // current re-entrancy depth of keyPressEvent
    int maxDepth = 0;     // the high-water mark — 1 means "no re-entry", >1 means the loop is alive
    void reset() { entries = depth = maxDepth = 0; }

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        ++entries;
        ++depth;
        if (depth > maxDepth) maxDepth = depth;
        // A BAIL-OUT, not the behaviour under test: against the pre-fix code this recursion is unbounded,
        // and a probe that reproduced it faithfully would die of the very c00000fd it exists to prevent
        // (the shipped crash reached 387 levels). 8 is far past anything legitimate and far short of that.
        if (depth <= 8 && NavContext::instance() && NavContext::instance()->routeKey(e->key())) e->accept();
        else QWidget::keyPressEvent(e);
        --depth;
    }
};

static void pump() { QApplication::processEvents(); QApplication::processEvents(); }

// The Profiles-screen shape, built identically by §13 and by §13b's DPI child runs (#351): header Back, then
// rows of [wide pick | ✎ 36px | ✕ 36px], a Create button and a Cancel button. The pick button's height is
// pinned (44) but the side buttons' and the header's come from the FONT, so one shape is a different geometry
// at every font DPI — which is why it is run at several.
struct ProfilePage
{
    QWidget* page = nullptr;
    QPushButton* back = nullptr;
    QPushButton* pick0 = nullptr;
    QPushButton* edit0 = nullptr;
    QPushButton* del0 = nullptr;
};

static ProfilePage buildProfilePage(QWidget* parent)
{
    ProfilePage p;
    p.page = new QWidget(parent);
    auto* v = new QVBoxLayout(p.page);
    p.back = new QPushButton(QStringLiteral("‹ Back"), p.page);
    v->addWidget(p.back);
    for (int r = 0; r < 3; ++r)
    {
        auto* row = new QHBoxLayout;
        auto* pick = new QPushButton(QStringLiteral("🐱   Profile %1").arg(r), p.page);
        pick->setMinimumHeight(44);
        row->addWidget(pick, 1);
        auto* edit = new QPushButton(QStringLiteral("✎"), p.page); edit->setFixedWidth(36); row->addWidget(edit);
        auto* del  = new QPushButton(QStringLiteral("✕"), p.page); del->setFixedWidth(36);  row->addWidget(del);
        v->addLayout(row);
        if (r == 0) { p.pick0 = pick; p.edit0 = edit; p.del0 = del; }
    }
    auto* create = new QPushButton(QStringLiteral("＋  Create New Profile"), p.page);
    v->addWidget(create);
    v->addWidget(new QPushButton(QStringLiteral("Cancel"), p.page));
    v->addStretch(1);
    p.page->setGeometry(0, 0, 420, 460);
    p.page->show(); p.page->activateWindow(); // offscreen QPA does not auto-activate subsequent windows
    pump();
    return p;
}

// Intuitive geometry on the profile rows (the reported bug: Down from a profile row dropped into the tiny ✎ edit
// button instead of the row below it; #351: at QT_FONT_DPI=72, Right from the row landed on the header Back
// instead of its ✎). Down from the wide "pick" button lands on the row DIRECTLY below, never the narrow side
// button; the ✎/✕ are reached with Right, not Down, and Left from ✎ goes back to its own row.
static void checkProfileRowGeometry(NavContext& ctx, const ProfilePage& p)
{
    p.pick0->setFocus(); pump();
    ctx.routeKey(Qt::Key_Right);
    CHECK(QApplication::focusWidget() == p.edit0, "Right from a profile row reaches its ✎ edit button");
    p.edit0->setFocus(); pump();
    ctx.routeKey(Qt::Key_Right);
    CHECK(QApplication::focusWidget() == p.del0, "Right from ✎ reaches the same row's ✕");
    p.edit0->setFocus(); pump();
    ctx.routeKey(Qt::Key_Left);
    CHECK(QApplication::focusWidget() == p.pick0, "Left from ✎ returns to its own profile row");
    p.pick0->setFocus(); pump();
    ctx.routeKey(Qt::Key_Down);
    CHECK(QApplication::focusWidget() != p.edit0, "Down from a profile row does NOT drop into the ✎ button");
    // Walk straight down the wide column: Profile0 -> Profile1 -> Profile2 -> Create -> Cancel, never a
    // side button, and clamping at Cancel.
    p.back->setFocus(); pump();
    ctx.routeKey(Qt::Key_Down); // Profile 0
    for (int i = 0; i < 5; ++i)
    {
        ctx.routeKey(Qt::Key_Down);
        auto* now = qobject_cast<QPushButton*>(QApplication::focusWidget());
        CHECK(now && now->width() > 60, "Down stays on full-width rows (never a 36px side button)");
    }
    auto* bottom = qobject_cast<QPushButton*>(QApplication::focusWidget()); // reached the bottom
    CHECK(bottom && bottom->text() == QStringLiteral("Cancel"), "walking Down lands on Cancel and clamps there");
}

// §13b's child mode (#351). QT_FONT_DPI is read once, when the QGuiApplication starts, so the only honest way to
// run the profile rows at another font DPI is another process: the parent sets the variable and relaunches this
// exe with --profile-row-only; the child builds the same page, runs the same checks, and reports the DPI it
// actually got (logical DPI x device pixel ratio: above 96 Qt turns the font DPI into a scale factor instead),
// so the parent can refuse a run where the variable was silently ignored.
static int profileRowChild()
{
    QWidget win;
    win.resize(1280, 720);
    NavContext ctx(&win);
    win.show();
    win.activateWindow();
    pump();
    const ProfilePage p = buildProfilePage(&win);
    NavRing ring(p.page);
    ctx.setActiveRing(&ring);
    checkProfileRowGeometry(ctx, p);
    std::printf("PROFILEROW-DPI %d\n", qRound(p.page->logicalDpiY() * p.page->devicePixelRatioF()));
    ctx.setActiveRing(nullptr);
    delete p.page;
    return failures == 0 ? 0 : 1;
}

// Every ring member that arrow keys can NEVER land on, starting from the ring's own initial selection.
// This is a CLOSURE, not a walk: from each reached widget it presses all four arrows and follows wherever
// focus goes, until nothing new appears. A hand-written walk can only prove the paths its author thought
// of — which is how §22's first draft passed while shipping a dead-end corner (see there). A closure has no
// such blind spot: anything it does not reach genuinely cannot be reached with a D-pad.
static QVector<QWidget*> navUnreachable(NavRing& ring, NavContext& ctx)
{
    const QVector<QWidget*> members = ring.widgets();
    QVector<QWidget*> seen;
    QWidget* start = ring.ensureSelection();
    if (!start) return members;
    seen.push_back(start);
    for (int i = 0; i < seen.size(); ++i)   // grows as the frontier expands
        for (int key : { Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right })
        {
            seen[i]->setFocus(Qt::OtherFocusReason);
            pump();
            ctx.routeKey(key);
            pump();
            QWidget* now = QApplication::focusWidget();
            if (now && members.contains(now) && !seen.contains(now)) seen.push_back(now);
        }
    QVector<QWidget*> out;
    for (QWidget* w : members) if (!seen.contains(w)) out.push_back(w);
    return out;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    // §13b's DPI child (#351): only the profile-row checks, at this process's font DPI.
    if (QCoreApplication::arguments().contains(QStringLiteral("--profile-row-only"))) return profileRowChild();

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
        page->show(); page->activateWindow(); // offscreen QPA does not auto-activate subsequent windows
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
        page->show(); page->activateWindow(); // offscreen QPA does not auto-activate subsequent windows
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

    // -------------------------------------------------- 8b. Shift/symbols relabel the grid, not flatten it
    {
        // The key grid stores each button's row/col in a DYNAMIC property that relabel() reads back. Naming it
        // "pos" collided with QWidget's own QPoint `pos` property: the int write was swallowed, every read
        // returned 0, and one Shift relabelled all 40 keys to "1" — so typing anything uppercase or symbolic
        // inserted "1" instead. Controller/TV users could not type a capital letter at all.
        //
        // Counting the keys captioned "1" pins it without depending on the grid's internals: exactly one key
        // (the digit) may ever read "1", on any page and in any shift state.
        auto* osk = new Osk(QStringLiteral("Enter text"), QString(), QLineEdit::Normal,
                            [](const QString&, bool) {}, &win);
        pump();
        auto onesAndUppers = [osk](int* ones, int* uppers) {
            *ones = 0; *uppers = 0;
            for (QPushButton* b : osk->findChildren<QPushButton*>())
            {
                const QString t = b->text();
                if (t == QStringLiteral("1")) ++*ones;
                if (t.size() == 1 && t.at(0).isLetter() && t.at(0).isUpper()) ++*uppers;
            }
        };
        int ones = 0, uppers = 0;
        onesAndUppers(&ones, &uppers);
        CHECK(ones == 1, "the unshifted letter page shows exactly one \"1\" key");
        CHECK(uppers == 0, "the unshifted letter page is lowercase");

        // Press Shift, then the symbols page, via their own buttons (the same path a pad Enter drives).
        auto clickLabel = [osk](const QString& label) {
            for (QPushButton* b : osk->findChildren<QPushButton*>())
                if (b->text() == label) { b->click(); return true; }
            return false;
        };
        CHECK(clickLabel(QString::fromUtf8("\xE2\x87\xA7")), "the Shift key exists");
        pump();
        onesAndUppers(&ones, &uppers);
        CHECK(ones == 1, "Shift relabels the letters — it does NOT turn every key into \"1\"");
        CHECK(uppers > 20, "Shift actually uppercases the letter keys");

        CHECK(clickLabel(QStringLiteral("#+=")), "the symbols key exists");
        pump();
        onesAndUppers(&ones, &uppers);
        CHECK(ones == 1, "the symbols page relabels too — still exactly one \"1\" key");

        osk->dismiss(0);
        pump();
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
        page->show(); page->activateWindow(); // offscreen QPA does not auto-activate subsequent windows
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
            QStringLiteral("Permanently remove EverythingBox and all of its data?"),
            QStringLiteral("This deletes the whole app folder. That includes your settings, cloud sign-in, "
                           "downloaded games and music, emulator saves and save states, and installed "
                           "emulators and cores, plus the cache and crash logs. This cannot be undone. If you "
                           "want to keep any downloads, copy them out of that folder first, then run this "
                           "again once you have everything you care about safely backed up somewhere else."),
            { QStringLiteral("Uninstall everything now"), QStringLiteral("Cancel") }, 1, &win);
        fits(confirm, "confirm(long message)");
        confirm->dismiss(-1);
        pump();

        // A confirm card whose message GROWS after it opened — the shape #137's lookup card has, and the one
        // the whole "relabel it live" facility exists for. The card opens saying "Looking up…", the answer
        // arrives seconds later and is several lines long, and setMessage() used to set the label text without
        // re-fitting the panel around it: the panel kept the size it was built at, so everything past the
        // first line was cut off. NavCountdown never noticed because it only ever swaps one digit. A live
        // drive of #137 found it against a real Wiktionary entry; this is that, headless.
        auto* growing = new NavConfirm(QStringLiteral("Define — ineffable (en)"),
                                       QStringLiteral("Looking up…"),
                                       { QStringLiteral("Language…"), QStringLiteral("Close") }, 1, &win);
        fits(growing, "confirm(before the answer arrives)");
        growing->setMessage(QStringLiteral(
            "Symbol 1. ISO 639-2 and ISO 639-3 language code for a language spoken in Nevada.\n"
            "Verb 1. First-person singular simple past indicative of be.\n"
            "Verb 2. Third-person singular simple past indicative of be.\n"
            "Verb 3. Second-person singular simple past indicative of be, chiefly in dialects that keep "
            "the older form, and in a good deal of poetry besides."));
        fits(growing, "confirm(message grown after it opened)");
        growing->dismiss(-1);
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

        // A menu with MORE ROWS THAN FIT — the subtitle picker's shape (0..n API results), and the case
        // every other NavMenu caller avoids by being a short fixed menu. NavMenu used to size itself to the
        // sum of its rows and never scroll, so the surplus ran off the bottom of the panel, which clamps
        // itself to the window: the ring walked onto rows that were not on screen. clippedTexts() missed it
        // because it measures rows against the LIST's viewport and the list claimed to be tall enough — so
        // this case asserts the ring DIRECTLY, not just the clip report.
        QStringList many;
        for (int i = 0; i < 20; ++i)
            many << QStringLiteral("en · Some.Movie.1982.Final.Cut.2160p.UHD.BluRay.x265-GROUP.v%1 · %2 downloads")
                        .arg(i + 1).arg(9000 - i * 37);
        auto* longMenu = new NavMenu(QStringLiteral("Choose a subtitle"), many, nullptr, &win);
        fits(longMenu, "menu(20 rows)");
        auto* lv = longMenu->findChild<QListWidget*>();
        CHECK(lv != nullptr, "the long menu has a list");
        if (lv)
        {
            CHECK(lv->verticalScrollBar()->isVisible(), "a menu too tall to fit scrolls");
            for (int i = 0; i < 19; ++i) ctx.routeKey(Qt::Key_Down);
            pump();
            CHECK(lv->currentRow() == 19, "the ring reaches the last row");
            // The real defect: selectable but INVISIBLE. The current row must be inside the viewport.
            CHECK(lv->viewport()->rect().intersects(lv->visualItemRect(lv->currentItem())),
                  "the selected row is actually on screen");
        }
        longMenu->dismiss(-1);
        pump();

        // The on-screen keyboard itself.
        auto* osk = new Osk(QStringLiteral("Enter the name of the playlist you would like to create"),
                            QStringLiteral("some text"), QLineEdit::Normal, nullptr, &win);
        fits(osk, "osk");
        osk->dismiss(0);
        pump();
    }


    // --------------------------------------------- 10b. a confirmation never loses a line (issue #347)
    //
    // A confirm card used to render its message in whatever height its LAYOUT worked out, and the layout
    // works that out at a width the text is never painted at:
    //
    //   * QLayout::heightForWidth measures at the panel's full width, but the panel's 1px stylesheet border
    //     is invisible to it, so the text is wrapped 2px wider than the label ever is — one line lost on any
    //     message whose last line was close to full; and
    //   * the message label is capped at 560px while the panel is as wide as its BUTTON ROW, so the #248
    //     rebuild card measured its message at 1552px and then painted it at 560px — and lost 17 lines.
    //
    // Both are silent: no ellipsis, no scrollbar, nothing. The button row simply sits where the rest of the
    // sentence should have been. Nine shipped confirmations were doing this at EVERY window size, 1920x1080
    // included; the audit is in the issue. What follows pins the three things that make it not happen:
    // the message gets the height its own text needs, an overflow is reachable with the pad AND says so,
    // and the buttons stay on the card at every size — while a message that fits builds nothing new at all.
    {
        // The card from the issue: #248's rebuild confirmation. Its button row is what makes the panel wide.
        const QString kLongMsg = QStringLiteral(
            "A newer version of this port is available: the catalogue offers build 1.2.0 and the copy on "
            "this computer is 1.1.1.\n\nThe build that was working before the last rebuild is still being "
            "kept, at 2.1 GB, in the \u201ckept\u201d folder beside the new one. It is removed the first time "
            "the new build launches.\n\nA recompilation of the retail game into a native program for this "
            "computer, with widescreen, high frame rates and gyro aiming.\n\nIt is built with the "
            "recompilation engine, which is licensed MIT. EverythingBox does not include or redistribute "
            "the recompilation engine \u2014 it comes from the project's own release, onto this computer, "
            "when you ask for a build.\n\nBuild tools found. Visual Studio 2022 (v143), CMake 3.31 and "
            "Ninja are all present.\n\nIt would be built from your own copy at C:/Games/ROMs/Nintendo 64/"
            "The Legend of Zelda - Majora's Mask (USA).z64. That file is read where it is; it is never "
            "copied, moved or changed.");
        const QStringList kBuildButtons = { QStringLiteral("Cancel"), QStringLiteral("Play (native)"),
                                            QStringLiteral("Rebuild it with the newer version"),
                                            QStringLiteral("Go back to the previous build"),
                                            QStringLiteral("Open homepage") };

        auto labelWith = [](NavOverlay* o, const QString& text) -> QLabel* {
            for (QLabel* l : o->findChildren<QLabel*>()) if (l->text() == text) return l;
            return nullptr;
        };
        auto panelOf = [](NavOverlay* o) {
            return o->findChild<QFrame*>(QStringLiteral("navOverlayPanel"));
        };

        // The size this was found at, and two smaller ones (a 1024x600 set-top panel, a 800x480 handheld).
        const QVector<QPair<int, int>> sizes = { { 1280, 760 }, { 1024, 600 }, { 800, 480 } };
        int scrolledAt = 0;   // a probe that never reaches the scrolling path would pass on nothing
        for (const QPair<int, int>& sz : sizes)
        {
            auto* host = new QWidget;
            host->resize(sz.first, sz.second);
            host->show();
            host->activateWindow();   // offscreen QPA does not auto-activate subsequent windows
            pump();

            auto* card = new NavConfirm(QStringLiteral("Zelda 64: Recompiled"), kLongMsg,
                                        kBuildButtons, 2, host);
            pump(); pump();
            QLabel* msg = labelWith(card, kLongMsg);
            QFrame* panel = panelOf(card);
            CHECK(msg && panel, "the long confirm card has a message and a panel");
            if (msg && panel)
            {
                // (1) THE DEFECT ITSELF. The label is at least as tall as its own text at its own width.
                CHECK(msg->height() >= msg->heightForWidth(msg->width()),
                      "the message label is as tall as the text it has to paint");

                // (2) THE BUTTONS ARE ON THE CARD. A card that grew until its actions left the screen would
                // have replaced one bug with a worse one.
                bool allIn = true;
                for (QPushButton* b : card->findChildren<QPushButton*>())
                    if (!panel->rect().contains(b->geometry())) allIn = false;
                CHECK(allIn, "every button is inside the card");
                CHECK(card->rect().contains(panel->geometry()), "the card is inside the window");

                // (3) THE WHOLE MESSAGE IS REACHABLE. Either it all fits, or it scrolls — and when it
                // scrolls the scrollbar is SHOWN (that is the "there is more" the issue asks for) and the
                // pad reaches the last line. Pressing Down is the only way offered; if it did not work the
                // text would be exactly as lost as it was before.
                auto* area = card->findChild<QScrollArea*>();
                if (area)
                {
                    ++scrolledAt;
                    CHECK(area->verticalScrollBar() && area->verticalScrollBar()->isVisible(),
                          "a scrolling message shows its scrollbar");
                    const int bottomBefore = msg->mapTo(area->viewport(), QPoint(0, msg->height() - 1)).y();
                    CHECK(bottomBefore > area->viewport()->height(),
                          "the case is real: the last line is off the viewport to start with");
                    for (int i = 0; i < 400 && area->verticalScrollBar()->value()
                                                   < area->verticalScrollBar()->maximum(); ++i)
                        ctx.routeKey(Qt::Key_Down);
                    pump();
                    const int bottomAfter = msg->mapTo(area->viewport(), QPoint(0, msg->height() - 1)).y();
                    CHECK(bottomAfter <= area->viewport()->height() + 1,
                          "the pad scrolls the message to its last line");
                    // Back up again: a reader who overshot must be able to return to the first line.
                    for (int i = 0; i < 400 && area->verticalScrollBar()->value() > 0; ++i)
                        ctx.routeKey(Qt::Key_Up);
                    pump();
                    CHECK(area->verticalScrollBar()->value() == 0, "Up scrolls back to the first line");

                    // (4) THE SCROLL DOES NOT STEAL THE BUTTONS' KEYS. Every button is in one horizontal
                    // row, so Left/Right are the keys the buttons need and they still move the selection;
                    // Enter still answers the card.
                    QWidget* was = QApplication::focusWidget();
                    ctx.routeKey(Qt::Key_Left);
                    pump();
                    CHECK(QApplication::focusWidget() != was && qobject_cast<QPushButton*>(
                              QApplication::focusWidget()) != nullptr,
                          "Left still moves between the buttons of a scrolling card");
                }
                else
                    CHECK(msg->height() >= msg->heightForWidth(msg->width()),
                          "a message that is not scrolled is shown whole");
            }
            int answered = -2;
            QObject::connect(card, &NavOverlay::closed, card, [&answered](int r) { answered = r; });
            ctx.routeKey(Qt::Key_Return);
            pump();
            CHECK(answered >= 0, "Enter still answers a card whose message scrolls");
            delete host;
            pump();
        }
        CHECK(scrolledAt >= 1, "at least one tested size really had to scroll (this is not vacuous)");

        // A TITLE LONG ENOUGH TO EAT THE CARD. Clipping it would be the same defect, and starving the
        // message to two lines is that defect wearing a hat — so the title scrolls with the message and the
        // buttons stay pinned. Unreachable with today's strings; a user-named playlist can get there.
        {
            auto* host = new QWidget;
            host->resize(800, 480);
            host->show();
            host->activateWindow();
            pump();
            QString hugeTitle;
            for (int i = 0; i < 40; ++i)
                hugeTitle += QStringLiteral("Delete the playlist named after my very favourite "
                                           "childhood memories, part %1? ").arg(i + 1);
            auto* card = new NavConfirm(hugeTitle, kLongMsg,
                                        { QStringLiteral("Delete"), QStringLiteral("Cancel") }, 1, host);
            pump(); pump();
            QFrame* panel = panelOf(card);
            CHECK(panel != nullptr, "the huge-title card has a panel");
            if (panel)
            {
                bool allIn = true;
                for (QPushButton* b : card->findChildren<QPushButton*>())
                    if (!panel->rect().contains(b->geometry())) allIn = false;
                CHECK(allIn, "the buttons stay on the card even under a title that fills it");
                CHECK(card->rect().contains(panel->geometry()),
                      "the huge-title card is still inside the window");
                QLabel* t = labelWith(card, hugeTitle);
                CHECK(t && t->height() >= t->heightForWidth(t->width()),
                      "the title is as tall as the text it has to paint");
            }
            card->dismiss(-1);
            delete host;
            pump();
        }

        // THE REGRESSION THAT WOULD MATTER: a short message must render exactly as it always has. No
        // viewport is built, the label is still a child of the panel, the card is no taller than its own
        // content, and Up/Down are still the ring's (they move nothing on a one-row button set) rather than
        // a scroll's. Every confirmation in the app is this shape, including the ones that delete files.
        {
            const QString shortMsg = QStringLiteral("This removes the downloaded game file.");
            auto* card = new NavConfirm(QStringLiteral("Uninstall game"), shortMsg,
                                        { QStringLiteral("Yes"), QStringLiteral("No") }, 1, &win);
            pump(); pump();
            QLabel* msg = labelWith(card, shortMsg);
            QFrame* panel = panelOf(card);
            CHECK(card->findChild<QScrollArea*>() == nullptr,
                  "a message that fits builds no scroll area");
            CHECK(msg && msg->parentWidget() == panel,
                  "a message that fits is still a direct child of the card");
            CHECK(card->clippedTexts().isEmpty(), "nothing on the short card is clipped");
            if (msg && panel)
            {
                CHECK(msg->height() >= msg->heightForWidth(msg->width()),
                      "the short message is shown whole");
                // No slack invented: the card is its content plus the border headroom relayoutPanel adds.
                int content = 0;
                if (QLayout* pl = panel->layout())
                {
                    const QMargins m = pl->contentsMargins();
                    content = m.top() + m.bottom() + 2 * qMax(0, pl->spacing());
                    for (QWidget* c : { static_cast<QWidget*>(msg) })
                        content += c->height();
                    if (QLabel* t = labelWith(card, QStringLiteral("Uninstall game"))) content += t->height();
                    if (QPushButton* b = card->findChild<QPushButton*>()) content += b->height();
                }
                CHECK(panel->height() <= content + 8,
                      "the card is no taller than the content it holds");
                QWidget* was = QApplication::focusWidget();
                ctx.routeKey(Qt::Key_Down);
                pump();
                CHECK(QApplication::focusWidget() == was,
                      "Down on a card that fits is still the ring's, not a scroll's");
            }
            card->dismiss(-1);
            pump();
        }
    }

    // --------------------------------------------- 10c. a button always says what it does (issue #349)
    //
    // The other half of #347, on the same card and just as silent. The buttons live in ONE horizontal row
    // sharing the card's width, and the card is capped at the window's width less a margin. A QHBoxLayout
    // given less than its items need does not refuse — it hands each of them a SHARE, and a QPushButton
    // paints as much of its label as the share holds. That is how the button which starts a minutes-long
    // compile came to read "Rebuild it wi": 252px of the 478px its label needs, at 1280x760, with no
    // ellipsis and nothing to say a word had been dropped. #347's audit measured ten card/size pairs doing it.
    //
    // The card cannot answer this the way #347 answered the message. A message can scroll; a button cannot,
    // and eliding one is simply losing it — this is a TV app driven by a pad, so there is no hover to read
    // the rest with. So the row WRAPS: the buttons are packed, in order, into as many lines as the widest
    // card the window allows needs, every label painted whole, and Left/Right walk them across the line
    // break. What follows pins that, the ten pairs from the audit, that the same card lays out the same way
    // every time — and, the thing that matters most, that a row which fits is exactly the row it always was.
    //
    // THE LABELS ARE THE AUDITED ONES, not today's shorter copy on the rebuild card. This pins the LAYOUT;
    // a label shortened until it happened to fit would let a layout that still squeezes pass here, and
    // clip again for the next long word or the next translation.
    {
        auto panelOf349 = [](NavOverlay* o) {
            return o->findChild<QFrame*>(QStringLiteral("navOverlayPanel"));
        };
        // What a button needs to paint its label: the widest of its lines, plus the frame allowance
        // clippedTexts() uses — the expression the issue's numbers were measured with.
        auto needOf = [](QPushButton* b) {
            int adv = 0;
            for (const QString& ln : b->text().split(QLatin1Char('\n')))
                adv = qMax(adv, b->fontMetrics().horizontalAdvance(ln));
            return adv + 16;
        };
        // The card's buttons in the order they were given: findChildren walks the children in creation
        // order, and moving a button from one line's layout to another never re-parents it.
        auto buttonsOf = [](NavOverlay* o) {
            QVector<QPushButton*> out;
            for (QPushButton* b : o->findChildren<QPushButton*>()) out << b;
            return out;
        };
        // How many lines the buttons sit on, read from where they ARE rather than from anything the widget
        // reports about itself — so this section reads the pre-#349 widget just as honestly.
        auto linesOf = [](const QVector<QPushButton*>& bs) {
            QSet<int> ys;
            for (QPushButton* b : bs) ys.insert(b->y());
            return int(ys.size());
        };
        auto flat = [](const QString& s) { return QString(s).replace(QLatin1Char('\n'), QLatin1Char(' ')); };
        // Everything a card owes its buttons, in one place for every case below. Each button is at least as
        // wide as the layout asks for its label — so it is not SQUEEZED, which is the defect's own mechanism
        // — and at least as wide as the label's ink plus frame, so nothing is CLIPPED; no two overlap; all
        // are on the card and the card is in the window; and clippedTexts(), the CI text-fit contract, agrees.
        auto rowIsWhole = [&](NavConfirm* card, const QByteArray& at) {
            QFrame* panel = panelOf349(card);
            const QVector<QPushButton*> bs = buttonsOf(card);
            bool fit = !bs.isEmpty(), inside = panel != nullptr, apart = true;
            for (int i = 0; i < bs.size(); ++i)
            {
                QPushButton* b = bs.at(i);
                if (b->width() < b->sizeHint().width() || b->width() < needOf(b)) fit = false;
                if (!panel || !panel->rect().contains(b->geometry())) inside = false;
                for (int j = i + 1; j < bs.size(); ++j)
                    if (b->geometry().intersects(bs.at(j)->geometry())) apart = false;
            }
            CHECK(fit, (at + ": every button is as wide as its own label asks").constData());
            CHECK(inside, (at + ": every button is on the card").constData());
            CHECK(apart, (at + ": no button is laid over another").constData());
            CHECK(panel && card->rect().contains(panel->geometry()), (at + ": the card is in the window").constData());
            CHECK(card->clippedTexts().isEmpty(), (at + ": the card reports nothing clipped").constData());
        };

        const QStringList kAuditedRebuildRow = { QStringLiteral("Cancel"), QStringLiteral("Play (native)"),
                                                 QStringLiteral("Rebuild it with the newer version"),
                                                 QStringLiteral("Go back to the previous build"),
                                                 QStringLiteral("Open homepage") };
        // A message long enough that the card is at its full height at the smaller sizes: the row is
        // measured against a card that is already as tight as it gets, and next to a message that scrolls.
        const QString kMsg349 = QStringLiteral(
            "A newer version of this port is available: the catalogue offers build 1.2.0 and the copy on "
            "this computer is 1.1.1.\n\nThe build that was working before the last rebuild is still being "
            "kept, at 2.1 GB, in the \u201ckept\u201d folder beside the new one.\n\nA recompilation of the "
            "retail game into a native program for this computer, with widescreen, high frame rates and gyro "
            "aiming.\n\nIt would be built from your own copy at C:/Games/ROMs/Nintendo 64/The Legend of "
            "Zelda - Majora's Mask (USA).z64. That file is read where it is; it is never copied, moved or "
            "changed.");

        // (A) THE CARD IN THE ISSUE, at every size the audit measured.
        const QVector<QPair<int, int>> sizes349 = { { 1920, 1080 }, { 1280, 760 }, { 1280, 720 },
                                                    { 1024, 600 }, { 800, 480 } };
        int wrappedAt = 0;   // a probe that never reaches the wrapping path would be pinning nothing
        for (const QPair<int, int>& sz : sizes349)
        {
            const QByteArray at = QByteArray("rebuild card at ") + QByteArray::number(sz.first) + "x"
                                  + QByteArray::number(sz.second);
            auto* host = new QWidget;
            host->resize(sz.first, sz.second);
            host->show();
            host->activateWindow();   // offscreen QPA does not auto-activate subsequent windows
            pump();

            auto* card = new NavConfirm(QStringLiteral("Zelda 64: Recompiled"), kMsg349, kAuditedRebuildRow,
                                        2, host);
            pump(); pump();
            const QVector<QPushButton*> bs = buttonsOf(card);
            CHECK(bs.size() == kAuditedRebuildRow.size(), (at + ": every button asked for is on the card").constData());
            rowIsWhole(card, at);

            // NOTHING IS SHORTENED ON THE USER'S BEHALF. A label that had to break is, put back together,
            // exactly the label that was asked for.
            bool asGiven = bs.size() == kAuditedRebuildRow.size();
            for (int i = 0; asGiven && i < bs.size(); ++i)
                if (flat(bs.at(i)->text()) != kAuditedRebuildRow.at(i)) asGiven = false;
            CHECK(asGiven, (at + ": every label is the label it was given, whole").constData());
            if (linesOf(bs) > 1) ++wrappedAt;

            // THE ROW WRAPS EXACTLY WHEN ONE LINE CANNOT HOLD IT. Measured with this platform's own font, so
            // it says the same thing on a runner whose font is narrower than this machine's (CI's is: there
            // the five labels fit one line at 1280 wide). What a line can hold is relayoutPanel's window
            // clamp, less the card's frame and the layout's margins; what one line needs is every button's
            // own size hint, 10px apart.
            if (QFrame* panel = panelOf349(card); panel && panel->layout())
            {
                const QMargins fm = panel->contentsMargins();
                const QMargins cm = panel->layout()->contentsMargins();
                const int lineRoom = qMax(300, sz.first - 120) - fm.left() - fm.right() - cm.left() - cm.right();
                int oneLine = 10 * (int(bs.size()) - 1);
                for (QPushButton* b : bs) oneLine += b->sizeHint().width();
                CHECK((linesOf(bs) > 1) == (oneLine > lineRoom),
                      (at + ": the row wraps exactly when one line cannot hold it").constData());
            }

            // EVERY ACTION IS REACHABLE WITH A PAD, IN THE ORDER GIVEN. Left to the start of the row, then
            // Right to its end: the walk has to be exactly the buttons as listed, or wrapping the row would
            // have hidden the very action it was widening (on a wrapped row that is the card's sequential
            // walk; on a single line it is the ring's own geometric step).
            for (int i = 0; i < 12; ++i) ctx.routeKey(Qt::Key_Left);
            pump();
            QVector<QPushButton*> walk;
            auto note = [&walk] {
                if (auto* f = qobject_cast<QPushButton*>(QApplication::focusWidget()))
                    if (walk.isEmpty() || walk.last() != f) walk << f;
            };
            note();
            for (int i = 0; i < int(bs.size()) + 3; ++i) { ctx.routeKey(Qt::Key_Right); pump(); note(); }
            CHECK(walk == bs, (at + ": Left, then Right, walks every button in the order given").constData());

            // ENTER ANSWERS THE BUTTON THAT IS SELECTED — here the third, the rebuild, which after a wrap
            // may sit on a different line from where it started.
            for (int i = 0; i < 12; ++i) ctx.routeKey(Qt::Key_Left);
            ctx.routeKey(Qt::Key_Right);
            ctx.routeKey(Qt::Key_Right);
            pump();
            int answered = -2;
            QObject::connect(card, &NavOverlay::closed, card, [&answered](int r) { answered = r; });
            ctx.routeKey(Qt::Key_Return);
            pump();
            CHECK(answered == 2, (at + ": Enter answers the button that is selected").constData());
            delete host;
            pump();
        }
        CHECK(wrappedAt >= 1, "the rebuild card really wraps at one tested size or more (this is not vacuous)");

        // (B) THE TEN PAIRS FROM #347'S AUDIT, each at the size it was measured clipping at, with the labels
        // the source has. The message is short on purpose: what squeezes the row is the card's WIDTH — the
        // row's own requirement clamped to the window — so these reproduce the defect exactly without
        // carrying paragraphs of shipped copy into a probe that is not about the copy.
        {
            struct Pair349 { int w, h; const char* what; QStringList row; };
            const QStringList unsent = { QStringLiteral("Send 14 plays, then remove"),
                                         QStringLiteral("Remove and discard 14 plays"), QStringLiteral("Cancel") };
            const QVector<Pair349> pairs = {
                { 1280, 760, "MainWindowRecomps.cpp:350 (recomp rebuild)", kAuditedRebuildRow },
                { 1280, 720, "MainWindowRecomps.cpp:350 (recomp rebuild)", kAuditedRebuildRow },
                { 1024, 600, "MainWindowRecomps.cpp:350 (recomp rebuild)", kAuditedRebuildRow },
                { 800, 480,  "MainWindowRecomps.cpp:350 (recomp rebuild)", kAuditedRebuildRow },
                { 1024, 600, "HomeView.cpp:6552 (music server with unsent plays)", unsent },
                { 800, 480,  "HomeView.cpp:6552 (music server with unsent plays)", unsent },
                { 800, 480,  "MainWindow.cpp:15288 (no parental PIN is set)",
                  { QStringLiteral("Set a parental PIN first"), QStringLiteral("Continue without one") } },
                { 800, 480,  "MainWindow.cpp:16576 (the game didn't stay open)",
                  { QStringLiteral("Open game folder"), QStringLiteral("Choose a different .exe"),
                    QStringLiteral("Close") } },
                { 800, 480,  "MainWindow.cpp:18354 (native port offer)",
                  { QStringLiteral("Cancel"), QStringLiteral("Install and play"),
                    QStringLiteral("Open homepage"), QStringLiteral("Remove") } },
                { 800, 480,  "MainWindowOpdsPse.cpp:152 (couldn't finish reading online)",
                  { QStringLiteral("Try again"), QStringLiteral("Download the volume instead"),
                    QStringLiteral("Not now") } },
            };
            for (const Pair349& p : pairs)
            {
                const QByteArray at = QByteArray(p.what) + " at " + QByteArray::number(p.w) + "x"
                                      + QByteArray::number(p.h);
                auto* host = new QWidget;
                host->resize(p.w, p.h);
                host->show();
                host->activateWindow();
                pump();
                auto* card = new NavConfirm(QString::fromUtf8(p.what),
                                            QStringLiteral("This is what the card explains before it asks."),
                                            p.row, 0, host);
                pump(); pump();
                rowIsWhole(card, at);
                card->dismiss(-1);
                delete host;
                pump();
            }
        }

        // (C) THE CARD IS SIZED FROM ITS ROW'S REAL REQUIREMENT, AND THE SAME CARD LAYS OUT THE SAME WAY
        // EVERY TIME. Packing the row against the width the card already had would measure the previous
        // packing: the first layout left the card at the window clamp with a band of nothing beside its
        // lines, and the next relayout shrank it under them — and a NavCountdown relays out once a second.
        {
            auto* host = new QWidget;
            host->resize(800, 480);
            host->show();
            host->activateWindow();
            pump();
            const QString shortMsg = QStringLiteral("A newer version of this port is available.");
            auto* card = new NavConfirm(QStringLiteral("Zelda 64: Recompiled"), shortMsg, kAuditedRebuildRow,
                                        2, host);
            pump(); pump();
            QFrame* panel = panelOf349(card);
            const QVector<QPushButton*> bs = buttonsOf(card);
            CHECK(linesOf(bs) > 1, "the rebuild card wraps at 800x480 (the cases below are real)");
            if (panel && panel->layout() && linesOf(bs) > 1)
            {
                // The widest line, from where the buttons are. The title and this message are both far
                // narrower, so the row is what the card's width has to answer to.
                QHash<int, QPair<int, int>> span;   // y -> (left, right)
                for (QPushButton* b : bs)
                {
                    auto it = span.find(b->y());
                    if (it == span.end()) span.insert(b->y(), { b->x(), b->x() + b->width() });
                    else it.value() = { qMin(it->first, b->x()), qMax(it->second, b->x() + b->width()) };
                }
                int widest = 0;
                for (const QPair<int, int>& s : span) widest = qMax(widest, s.second - s.first);
                const QMargins fm = panel->contentsMargins();
                const QMargins cm = panel->layout()->contentsMargins();
                // relayoutPanel's own sizing, applied to the row: content + margins + frame + 4px headroom.
                CHECK(panel->width() <= widest + fm.left() + fm.right() + cm.left() + cm.right() + 4,
                      "a wrapped card is as wide as its widest line of buttons, not left at the window clamp");

                const QRect panelBefore = panel->geometry();
                QVector<QRect> before;
                for (QPushButton* b : bs) before << b->geometry();
                for (int k = 0; k < 3; ++k) { card->setMessage(shortMsg); pump(); }
                QVector<QRect> after;
                for (QPushButton* b : bs) after << b->geometry();
                CHECK(panel->geometry() == panelBefore && after == before,
                      "relaying out a wrapped card moves nothing (a NavCountdown relays out every second)");
                rowIsWhole(card, "the wrapped card after three relayouts");
            }
            card->dismiss(-1);
            delete host;
            pump();
        }

        // (D) A SINGLE LABEL WIDER THAN THE WHOLE CARD. Wrapping the row cannot help — there is no narrower
        // line to put it on — so the label breaks across two lines of the button itself. No shipped string
        // reaches this at any tested size; a translation can, and an elided label would be a lost action.
        //
        // The card is sized FROM THE LABEL, measured with this platform's font: a line three quarters as
        // wide as the label needs on one line, which no font can fit it into and either half of it fits
        // comfortably. A fixed window size made this case depend on the runner's font — at 800px CI's
        // narrower font fitted the whole label on one line and there was nothing to break.
        {
            const QString monster = QStringLiteral("Delete every downloaded file for this game and "
                                                   "forget where it came from");
            const QStringList row = { monster, QStringLiteral("Cancel") };
            int oneLine = 0, frameAndMargins = 0;
            {
                auto* wideHost = new QWidget;
                wideHost->resize(3000, 1000);
                wideHost->show();
                wideHost->activateWindow();
                pump();
                auto* wide = new NavConfirm(QStringLiteral("Remove the download"),
                                            QStringLiteral("This cannot be undone."), row, 1, wideHost);
                pump(); pump();
                for (QPushButton* b : buttonsOf(wide))
                    if (b->text() == monster) oneLine = b->sizeHint().width();
                if (QFrame* panel = panelOf349(wide); panel && panel->layout())
                {
                    const QMargins fm = panel->contentsMargins();
                    const QMargins cm = panel->layout()->contentsMargins();
                    frameAndMargins = fm.left() + fm.right() + cm.left() + cm.right();
                }
                wide->dismiss(-1);
                delete wideHost;
                pump();
            }
            CHECK(oneLine > 0 && frameAndMargins > 0, "the over-long label was measured on one line first");
            const int hostW = oneLine * 3 / 4 + frameAndMargins + 120;   // + relayoutPanel's window margin
            auto* host = new QWidget;
            host->resize(hostW, 480);
            host->show();
            host->activateWindow();
            pump();
            auto* card = new NavConfirm(QStringLiteral("Remove the download"),
                                        QStringLiteral("This cannot be undone."), row, 1, host);
            pump(); pump();
            QPushButton* big = nullptr;
            for (QPushButton* b : buttonsOf(card))
                if (flat(b->text()) == monster) big = b;
            CHECK(big != nullptr, "the over-long button is on the card, its label whole");
            if (big)
            {
                CHECK(big->text().contains(QLatin1Char('\n')),
                      "a label too wide for the card breaks onto a second line of the button");
                CHECK(big->height() >= 2 * big->fontMetrics().lineSpacing(),
                      "...on a button tall enough to hold both lines");
                big->setFocus(Qt::OtherFocusReason);
                pump();
                CHECK(card->describe() == monster,
                      "...and the UI-test channel still reads it as the one label it was given");
            }
            rowIsWhole(card, QByteArray("the over-long label, on a card ") + QByteArray::number(hostW)
                                 + "px wide");
            card->dismiss(-1);
            delete host;
            pump();
        }

        // (E) THE REGRESSION THAT WOULD MATTER, and it is the one #347 pinned for the message: a row that
        // FITS is untouched. One line; each button exactly the width its label asks for, which is what a
        // QHBoxLayout behind a leading stretch has always given it; 10px apart; right-aligned against the
        // card's margin — the geometry of the single row this card has always had. Every confirmation in
        // the app is this shape, including the ones that delete things.
        auto asAlways = [&](NavConfirm* card, const QByteArray& at) {
            QFrame* panel = panelOf349(card);
            const QVector<QPushButton*> bs = buttonsOf(card);
            CHECK(panel && panel->layout() && !bs.isEmpty(), (at + ": the card has its buttons").constData());
            if (!panel || !panel->layout() || bs.isEmpty()) return;
            const QMargins fm = panel->contentsMargins();
            const QMargins cm = panel->layout()->contentsMargins();
            bool oneLine = true, hinted = true, spaced = true;
            for (int i = 0; i < bs.size(); ++i)
            {
                if (bs.at(i)->y() != bs.first()->y()) oneLine = false;
                if (bs.at(i)->width() != bs.at(i)->sizeHint().width()) hinted = false;
                if (i > 0 && bs.at(i)->x() != bs.at(i - 1)->x() + bs.at(i - 1)->width() + 10) spaced = false;
            }
            CHECK(oneLine, (at + ": a row that fits is still one line").constData());
            CHECK(hinted, (at + ": ...each button exactly as wide as its label asks").constData());
            CHECK(spaced, (at + ": ...10px apart").constData());
            CHECK(bs.last()->x() + bs.last()->width() == panel->width() - fm.right() - cm.right(),
                  (at + ": ...right-aligned against the card's margin").constData());
            CHECK(card->clippedTexts().isEmpty(), (at + ": ...and nothing clipped").constData());
        };
        {
            auto* card = new NavConfirm(QStringLiteral("Uninstall game"),
                                        QStringLiteral("This removes the downloaded game file."),
                                        { QStringLiteral("Yes"), QStringLiteral("No") }, 1, &win);
            pump(); pump();
            asAlways(card, "the two-button card at 1280x720");
            card->dismiss(-1);
            pump();
        }
        {
            // Five long labels that DO fit, at 1920x1080 — the same card that wraps at every smaller size.
            auto* host = new QWidget;
            host->resize(1920, 1080);
            host->show();
            host->activateWindow();
            pump();
            auto* card = new NavConfirm(QStringLiteral("Zelda 64: Recompiled"), kMsg349, kAuditedRebuildRow,
                                        2, host);
            pump(); pump();
            asAlways(card, "the rebuild card at 1920x1080");
            card->dismiss(-1);
            delete host;
            pump();
        }
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
        page->show(); page->activateWindow(); // offscreen QPA does not auto-activate subsequent windows
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
        page->show(); page->activateWindow(); // offscreen QPA does not auto-activate subsequent windows
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
        // Mirror of the Profiles screen (buildProfilePage). Hammering arrows in any pattern must always leave the
        // focus on a live ring member — the selection can never vanish.
        const ProfilePage prof = buildProfilePage(&win);
        QWidget* page = prof.page;

        NavRing ring(page);
        ctx.setActiveRing(&ring);

        // At this process's font DPI; §13b runs the same checks again at 72, 96, 120 and 144.
        checkProfileRowGeometry(ctx, prof);

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

    // ------------------------------------------- 13b. #351: Left/Right find the row's own neighbour, at any font DPI
    {
        // (a) The rule itself, over rect pairs (NavRing::besideInRow). `row` is §13's wide "pick" button as it
        // laid out at QT_FONT_DPI=72 on the machine that found the bug, and the first "header" is that run's Back
        // button: WIDER than the row (the row shares its width with ✎ and ✕), so its centre sits right of the
        // row's although it is above it — and at 72 DPI it is short enough that the old centre-based "more
        // sideways than in-direction" filter let it through, where it outscored ✎ (172 against 186). These are
        // fixed rects, not measurements: nothing here depends on a font.
        const QRect row(8, 27, 328, 44);
        const int R = Qt::Key_Right, L = Qt::Key_Left;
        CHECK(NavRing::besideInRow(row, QRect(340, 41, 36, 15), R),
              "beside: a SHORTER target within the row's vertical span is to its right (the ✎ button)");
        CHECK(NavRing::besideInRow(QRect(0, 0, 36, 20), QRect(50, 0, 100, 400), R),
              "beside: a TALLER target whose extent covers the current widget is to its right");
        CHECK(NavRing::besideInRow(QRect(0, 0, 100, 400), QRect(110, 380, 36, 20), R),
              "beside: a short target at the foot of a tall current widget is to its right");
        CHECK(!NavRing::besideInRow(row, QRect(8, 8, 408, 15), R),
              "beside: the 72-DPI header Back (wider, above the row) is NOT to its right");
        // The next two are horizontally CLEAR of the row, so only the vertical half of the rule can reject them.
        CHECK(!NavRing::besideInRow(row, QRect(340, 8, 36, 15), R),
              "beside: a target clear to the right but with no vertical overlap at all is NOT to its right");
        CHECK(!NavRing::besideInRow(row, QRect(340, 12, 36, 15), R),
              "beside: a target exactly touching the row's top edge (no shared pixel row) is NOT to its right");
        CHECK(NavRing::besideInRow(row, QRect(340, 13, 36, 15), R),
              "beside: a single shared pixel row is overlap");
        CHECK(NavRing::besideInRow(QRect(0, 0, 100, 20), QRect(100, 0, 36, 20), R),
              "beside: a target exactly touching the right edge is beside it");
        CHECK(!NavRing::besideInRow(QRect(0, 0, 100, 20), QRect(50, 0, 100, 20), R),
              "beside: a target overlapping the current widget horizontally is not beside it");
        CHECK(!NavRing::besideInRow(row, QRect(340, 41, 36, 15), L), "beside: a target on the right is not to the LEFT");
        CHECK(NavRing::besideInRow(QRect(340, 41, 36, 15), row, L), "beside: Left from ✎ finds its own, taller row");
        CHECK(!NavRing::besideInRow(row, QRect(340, 41, 36, 15), Qt::Key_Down), "beside: answers Left/Right only");

        // (b) The profile rows themselves at several font DPIs, each in its own process (profileRowChild). Every
        // assertion there is a relationship — where the focus lands — never a pixel count, so it means the same
        // thing on any machine's fonts; only the geometry it runs against changes with the DPI.
        for (int dpi : { 72, 96, 120, 144 })
        {
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("QT_FONT_DPI"), QString::number(dpi));
            QProcess child;
            child.setProcessEnvironment(env);
            child.setProcessChannelMode(QProcess::MergedChannels);
            child.start(QCoreApplication::applicationFilePath(),
                        { QStringLiteral("-platform"), QApplication::platformName(),
                          QStringLiteral("--profile-row-only") });
            const bool finished = child.waitForFinished(60000);
            if (!finished) { child.kill(); child.waitForFinished(5000); }
            int got = -1;
            for (const QByteArray& raw : child.readAll().split('\n'))
            {
                const QByteArray line = raw.trimmed();
                if (line.startsWith("NAV-")) std::fprintf(stderr, "  [QT_FONT_DPI=%d] %s\n", dpi, line.constData());
                else if (line.startsWith("PROFILEROW-DPI ")) got = line.mid(15).toInt();
            }
            const QByteArray at = " (QT_FONT_DPI=" + QByteArray::number(dpi) + ")";
            CHECK(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                  ("the profile-row checks all pass" + at).constData());
            CHECK(got == dpi, ("the profile-row child really ran at that font DPI" + at).constData());
        }
    }

    // ------------------------------------------- 14. one Back rule: Escape == Backspace, everywhere
    {
        auto* page = new QWidget(&win);
        auto* v = new QVBoxLayout(page);
        for (int i = 0; i < 3; ++i) v->addWidget(new QPushButton(QStringLiteral("row%1").arg(i), page));
        page->setGeometry(0, 0, 300, 240);
        page->show(); page->activateWindow(); // offscreen QPA does not auto-activate subsequent windows
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

    // ------------------------------------------- 15. text boxes: select vs edit (two-state)
    {
        auto* page = new QWidget(&win);
        auto* v = new QVBoxLayout(page);
        auto* top = new QPushButton(QStringLiteral("Top"), page);
        auto* edit = new QLineEdit(QStringLiteral("hello"), page);
        auto* display = new QLineEdit(QStringLiteral("C:/roms"), page); // a read-only DISPLAY field (issue #3)
        display->setReadOnly(true);
        auto* bot = new QPushButton(QStringLiteral("Bottom"), page);
        v->addWidget(top); v->addWidget(edit); v->addWidget(display); v->addWidget(bot);
        page->setGeometry(0, 0, 320, 240);
        page->show(); page->activateWindow(); // offscreen QPA does not auto-activate subsequent windows
        pump();

        NavRing ring(page);
        ctx.setActiveRing(&ring);
        ring.widgets(); // triggers NavTextField::ensure on both line edits

        // Navigated to -> SELECTED: read-only outline, not a live cursor.
        edit->setFocus(Qt::OtherFocusReason);
        pump();
        CHECK(edit->isReadOnly() && !NavTextField::isInteracting(edit), "arrowing onto a text box selects it (read-only, not editing)");

        // A printable key while selected does NOT type into it.
        { QKeyEvent k(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a")); QApplication::sendEvent(edit, &k); }
        CHECK(edit->text() == QStringLiteral("hello"), "a printable key does not auto-type into a selected box");

        // A physical Enter starts EDITING (a live cursor).
        { QKeyEvent k(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QApplication::sendEvent(edit, &k); }
        CHECK(!edit->isReadOnly() && NavTextField::isInteracting(edit), "Enter starts editing (cursor, typeable)");

        // Escape leaves editing back to the SELECTION — it does NOT bubble to the screen's Back.
        int backs = 0; ctx.setBackAction([&backs] { ++backs; });
        { QKeyEvent k(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QApplication::sendEvent(edit, &k); }
        CHECK(edit->isReadOnly() && !NavTextField::isInteracting(edit), "Escape leaves editing back to the selection");
        CHECK(backs == 0, "Escape while editing does NOT go back a screen");

        // A controller (synthetic) Enter opens the on-screen keyboard instead of an inline cursor.
        {
            NavContext::SyntheticScope synth;
            QKeyEvent k(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(edit, &k);
        }
        pump();
        CHECK(qobject_cast<Osk*>(NavOverlay::topmost()) != nullptr, "a controller Enter opens the on-screen keyboard");
        if (NavOverlay::topmost()) NavOverlay::topmost()->dismiss(-1);
        pump();

        // Read-only DISPLAY field (issue #3): it's a selectable ring stop that does NOT trap the arrows and
        // never becomes editable — all textboxes behave the same.
        CHECK(ring.widgets().contains(display), "a read-only display field is a selectable ring stop");
        display->setFocus(Qt::OtherFocusReason);
        pump();
        CHECK(display->isReadOnly() && !NavTextField::isInteracting(display), "arrowing onto a display field selects it");
        { QKeyEvent k(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier); QApplication::sendEvent(display, &k); }
        CHECK(QApplication::focusWidget() == edit, "Up from a selected display field navigates away (not trapped)");
        display->setFocus(Qt::OtherFocusReason); pump();
        { QKeyEvent k(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier); QApplication::sendEvent(display, &k); }
        CHECK(QApplication::focusWidget() == bot, "Down from a selected display field navigates away");
        // Selecting into it (cursor mode) must never make it writable.
        display->setFocus(Qt::OtherFocusReason); pump();
        { QKeyEvent k(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QApplication::sendEvent(display, &k); }
        CHECK(NavTextField::isInteracting(display) && display->isReadOnly(),
              "a display field selected-into stays read-only (never typeable)");
        { QKeyEvent k(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QApplication::sendEvent(display, &k); }
        CHECK(!NavTextField::isInteracting(display), "Escape leaves the display field back to just selected");

        ctx.setBackAction(nullptr);
        ctx.setActiveRing(nullptr);
        delete page;
        pump();
    }

    // ------------------------------------------- 16. Debug-log shape: a scrollable text view is selectable,
    //                                                 not an arrow-key trap (github issue #1)
    {
        auto* page = new QWidget(&win);
        auto* v = new QVBoxLayout(page);
        auto* top = new QPushButton(QStringLiteral("Refresh"), page);
        auto* log = new QPlainTextEdit(page);
        log->setReadOnly(true);
        for (int i = 0; i < 200; ++i) log->appendPlainText(QStringLiteral("log line %1").arg(i));
        auto* bot = new QPushButton(QStringLiteral("Clear"), page);
        v->addWidget(top); v->addWidget(log, 1); v->addWidget(bot);
        page->setGeometry(0, 0, 360, 300);
        page->show(); page->activateWindow(); // offscreen QPA does not auto-activate subsequent windows
        pump();

        NavRing ring(page);
        ctx.setActiveRing(&ring);
        const QVector<QWidget*> ring0 = ring.widgets(); // triggers NavTextField::ensure on the log view
        CHECK(ring0.contains(log), "the read-only log view is a selectable ring stop");

        // Selected: arrows navigate to the surrounding buttons instead of scrolling the log.
        log->setFocus(Qt::OtherFocusReason);
        pump();
        CHECK(!NavTextField::isInteracting(log), "arrowing onto the log selects it (not scroll mode)");
        int backs = 0; ctx.setBackAction([&backs] { ++backs; });
        { QKeyEvent k(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier); QApplication::sendEvent(log, &k); }
        CHECK(QApplication::focusWidget() == top, "Up from the selected log moves to the button above it");
        log->setFocus(Qt::OtherFocusReason); pump();
        { QKeyEvent k(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier); QApplication::sendEvent(log, &k); }
        CHECK(QApplication::focusWidget() == bot, "Down from the selected log moves to the button below it");

        // Enter "selects into" it: scroll mode. Escape returns to just selecting it (never leaves the screen).
        log->setFocus(Qt::OtherFocusReason); pump();
        { QKeyEvent k(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QApplication::sendEvent(log, &k); }
        CHECK(NavTextField::isInteracting(log), "Enter selects into the log (scroll mode)");
        { QKeyEvent k(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QApplication::sendEvent(log, &k); }
        CHECK(!NavTextField::isInteracting(log) && QApplication::focusWidget() == log,
              "Escape leaves scroll mode back to just selecting the log");
        CHECK(backs == 0, "Escape in the log never leaves the Debug screen");

        ctx.setBackAction(nullptr);
        ctx.setActiveRing(nullptr);
        delete page;
        pump();
    }

    // ------------------------------------------- 17. dropdowns: select vs open (two-state, github issue #4)
    {
        auto* page = new QWidget(&win);
        auto* v = new QVBoxLayout(page);
        auto* top = new QPushButton(QStringLiteral("Top"), page);
        auto* combo = new QComboBox(page);
        combo->addItems({ QStringLiteral("Player 1"), QStringLiteral("Player 2"), QStringLiteral("Player 3") });
        auto* bot = new QPushButton(QStringLiteral("Bottom"), page);
        v->addWidget(top); v->addWidget(combo); v->addWidget(bot);
        page->setGeometry(0, 0, 320, 200);
        page->show(); page->activateWindow(); // offscreen QPA does not auto-activate subsequent windows
        pump();

        NavRing ring(page);
        ctx.setActiveRing(&ring);
        CHECK(ring.widgets().contains(combo), "a dropdown is a ring stop"); // also triggers NavCombo::ensure

        combo->setCurrentIndex(1);
        combo->setFocus(Qt::OtherFocusReason);
        pump();
        // Arrowing over a SELECTED (closed) dropdown must navigate away, NOT change its value.
        { QKeyEvent k(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier); QApplication::sendEvent(combo, &k); }
        CHECK(combo->currentIndex() == 1, "Up over a closed dropdown does not change its value");
        CHECK(QApplication::focusWidget() == top, "Up navigates away from the dropdown");
        combo->setFocus(Qt::OtherFocusReason); pump();
        { QKeyEvent k(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier); QApplication::sendEvent(combo, &k); }
        CHECK(combo->currentIndex() == 1 && QApplication::focusWidget() == bot,
              "Down navigates away without changing the value");
        // A scroll-wheel over the closed dropdown does nothing.
        combo->setFocus(Qt::OtherFocusReason); pump();
        { QWheelEvent w(QPointF(5, 5), combo->mapToGlobal(QPoint(5, 5)), QPoint(), QPoint(0, -120),
                        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
          QApplication::sendEvent(combo, &w); }
        CHECK(combo->currentIndex() == 1, "scrolling over a closed dropdown does not spin its value");
        // Enter opens the popup (select into it).
        { QKeyEvent k(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QApplication::sendEvent(combo, &k); }
        pump();
        CHECK(combo->view() && combo->view()->isVisible(), "Enter opens the dropdown popup");
        combo->hidePopup();
        ctx.setActiveRing(nullptr);
        delete page;
        pump();
    }

    // ------------------------------------------- 18. themed (QML) page: closing an overlay revives the scene
    {
        // The themed home/browse is a QQuickWidget: restoring widget focus on dismiss is not enough — the
        // scene's active-focus item stays dead after the overlay's keyboard grab, and every QML Keys
        // handler (arrow nav) goes deaf until something calls forceActiveFocus() on the root item (the bug:
        // item navigation froze after the search OSK closed). dismiss() must kick the object exposed via
        // the page's "mmvQuickRoot" property whenever it restores the page's focus.
        auto* page = new QWidget(&win);
        page->setGeometry(0, 0, 400, 300);
        page->setFocusPolicy(Qt::StrongFocus);
        FakeQuickRoot sceneRoot;
        page->setProperty("mmvQuickRoot", QVariant::fromValue<QObject*>(&sceneRoot));
        page->show(); page->activateWindow(); // offscreen QPA does not auto-activate subsequent windows
        page->setFocus();
        pump();

        auto* osk = new Osk(QStringLiteral("Search everything"), QString(), QLineEdit::Normal, nullptr, &win);
        pump();
        CHECK(NavOverlay::topmost() == osk, "the search OSK is the topmost overlay");
        CHECK(sceneRoot.kicks == 0, "no focus kick while the overlay is still open");
        ctx.routeKey(Qt::Key_Escape); // Start commits — the themed-search flow's close
        pump();
        CHECK(NavOverlay::topmost() == nullptr, "the OSK closed on commit");
        CHECK(QApplication::focusWidget() == page, "widget focus returns to the themed page");
        CHECK(sceneRoot.kicks == 1, "dismiss revives the themed page's scene (forceActiveFocus)");

        // Stacked overlays: closing onto an overlay BELOW hands input to that overlay, not the page — no
        // kick until the stack unwinds back to the page itself.
        new NavMenu(QStringLiteral("Outer"), { QStringLiteral("a"), QStringLiteral("b") }, nullptr, &win);
        new NavConfirm(QStringLiteral("Sure?"), QString(), { QStringLiteral("Do it"), QStringLiteral("Cancel") }, 0, &win);
        pump();
        ctx.routeKey(Qt::Key_Backspace); // pop the confirm — the menu below takes over
        pump();
        CHECK(sceneRoot.kicks == 1, "closing onto an overlay below does not kick the page");
        ctx.routeKey(Qt::Key_Backspace); // pop the menu — back to the page
        pump();
        CHECK(sceneRoot.kicks == 2, "unwinding the last overlay revives the scene again");
        delete page;
        pump();
    }

    // ------------------------------------------- 19. overlay-mirror levels on a NavGraph (one Back router)
    {
        // An overlay opened over a themed screen mirrors itself as a level on that screen's NavGraph
        // (setNavGraph): show pushes exactly one level, and EVERY close path — accept, Back/cancel, an
        // explicit double dismiss, the graph's own back() unwinding, and the OSK's nested QEventLoop —
        // pops it exactly once (the dismissed_ latch + the m_popping guard make dismiss and popLevel
        // compose without a double-pop or a double-dismiss).
        NavGraph graph;
        graph.registerZone(QStringLiteral("items"), 5, 0, 0);
        int pops = 0, depthNow = 0;
        QObject::connect(&graph, &NavGraph::levelsChanged,
                         [&](int d) { if (d < depthNow) ++pops; depthNow = d; });
        bool browsePopped = false;
        graph.pushLevel(QStringLiteral("browse"), [&] { browsePopped = true; }); // a drill beneath the overlays
        CHECK(graph.levelDepth() == 1, "baseline: one browse level on the stack");

        // a) accept path: choosing a row dismisses with a result — one pop, back to baseline.
        auto* menu = new NavMenu(QStringLiteral("Actions"), { QStringLiteral("Play"), QStringLiteral("Fav") },
                                 nullptr, &win);
        menu->setNavGraph(&graph);
        pump();
        CHECK(graph.levelDepth() == 2, "setNavGraph pushes the overlay's mirror level");
        ctx.routeKey(Qt::Key_Return); // choose row 0 -> dismiss(0)
        pump();
        CHECK(NavOverlay::topmost() == nullptr, "the menu closed on accept");
        CHECK(graph.levelDepth() == 1, "the accept path pops the mirror level back to baseline");
        CHECK(pops == 1, "…with exactly ONE pop");

        // b) Back/cancel path: a fresh overlay, closed by the Back key — one pop, back to baseline.
        auto* menu2 = new NavMenu(QStringLiteral("Actions"), { QStringLiteral("a"), QStringLiteral("b") },
                                  nullptr, &win);
        menu2->setNavGraph(&graph);
        pump();
        CHECK(graph.levelDepth() == 2, "the second overlay pushes its mirror level");
        ctx.routeKey(Qt::Key_Backspace); // the overlay's own Back handling -> dismiss(-1)
        pump();
        CHECK(NavOverlay::topmost() == nullptr, "the menu closed on Back");
        CHECK(graph.levelDepth() == 1 && pops == 2, "the Back/cancel path pops exactly once");

        // c) re-entrant dismiss guard: calling dismiss twice is ONE close and ONE pop.
        auto* menu3 = new NavMenu(QStringLiteral("Actions"), { QStringLiteral("x") }, nullptr, &win);
        menu3->setNavGraph(&graph);
        pump();
        CHECK(graph.levelDepth() == 2, "the third overlay pushes its mirror level");
        menu3->dismiss(-1);
        menu3->dismiss(-1);   // second call must be a latched no-op (deleteLater hasn't collected it yet)
        pump();
        CHECK(graph.levelDepth() == 1 && pops == 3, "a double dismiss pops the mirror level exactly once");

        // d) graph-side unwind: graph.back() pops the mirror level, whose onPop dismisses the overlay —
        //    and dismiss's own pop is a guarded no-op mid-onPop (no double-pop, no cascade into "browse").
        auto* menu4 = new NavMenu(QStringLiteral("Actions"), { QStringLiteral("y") }, nullptr, &win);
        menu4->setNavGraph(&graph);
        pump();
        CHECK(graph.levelDepth() == 2, "the fourth overlay pushes its mirror level");
        graph.back();
        pump();
        CHECK(NavOverlay::topmost() == nullptr, "graph.back() dismisses the mirrored overlay");
        CHECK(graph.levelDepth() == 1 && pops == 4, "…and pops exactly once (dismiss's re-entrant pop is guarded)");
        CHECK(!browsePopped, "the browse level beneath survives every overlay close untouched");

        // e) OSK nested-loop composition: getText(&graph) blocks in a nested QEventLoop; a Back injected
        //    inside that loop cancels the OSK — the loop quits AND the mirror level pops exactly once.
        QTimer::singleShot(0, [&] { ctx.routeKey(Qt::Key_Backspace); }); // fires inside the nested loop
        const QString r = Osk::getText(QStringLiteral("Search"), QString(), QLineEdit::Normal, &win, &graph);
        pump();
        CHECK(r.isNull(), "the nested-loop OSK backed out (null result)");
        CHECK(NavOverlay::topmost() == nullptr, "the OSK closed through the nested loop");
        CHECK(graph.levelDepth() == 1 && pops == 5, "the OSK's mirror level popped exactly once through the nested loop");

        // The drill level beneath is still fully live: one real Back pops it and runs its onPop.
        graph.back();
        CHECK(browsePopped && graph.levelDepth() == 0, "the surviving browse level pops normally afterwards");
    }

    // ---------------------------------------------------------- 22. the 4-digit passcode pad (issue #30)
    // The pad is a NavOverlay, so it inherits the stacking/back/focus contract asserted above. What is
    // SPECIFIC to it — and what a remote user would hit first — is asserted here: the digit keys are ring
    // members reachable by arrows, entry AUTO-SUBMITS at kLength with no Done key to find, Back deletes
    // before it leaves, and the recovery rows report an index rather than a code.
    {
        // a) auto-submit, and the code that comes back.
        QString got; bool gotOk = false; int gotExtra = -2;
        auto* pad = new PasscodePad(QStringLiteral("Enter the passcode"), QStringLiteral("locked"),
                                    {}, [&](const QString& c, bool ok, int x) {
                                        got = c; gotOk = ok; gotExtra = x; }, &win);
        pump();
        CHECK(NavOverlay::topmost() == pad, "the passcode pad opens as the topmost overlay");
        // Type through the PHYSICAL path (the overlay's keyboard grab) — a desktop user's number row.
        for (const char* d : { "7", "0", "0", "7" })
        {
            QKeyEvent ev(QEvent::KeyPress, Qt::Key_0 + (*d - '0'), Qt::NoModifier, QString::fromLatin1(d));
            QApplication::sendEvent(pad, &ev);
        }
        pump();
        CHECK(gotOk, "four digits auto-submit with no Done key");
        CHECK(got.size() == 4, "the submitted code is exactly four digits long");
        CHECK(gotExtra < 0, "an auto-submit is not a recovery choice");
        CHECK(NavOverlay::topmost() == nullptr, "the pad closes itself on auto-submit");

        // b) Back deletes a digit first, and only leaves once the entry is empty — so a mistyped digit
        //    never costs the user the whole prompt.
        int closes = 0; bool backOk = true; int backExtra = -2;
        auto* pad2 = new PasscodePad(QStringLiteral("Enter the passcode"), QString(), {},
                                     [&](const QString&, bool ok, int x) { ++closes; backOk = ok; backExtra = x; },
                                     &win);
        pump();
        { QKeyEvent ev(QEvent::KeyPress, Qt::Key_5, Qt::NoModifier, QStringLiteral("5"));
          QApplication::sendEvent(pad2, &ev); }
        pump();
        CHECK(pad2->describe().contains(QStringLiteral("1")), "describe() reports how many boxes are filled");
        // ...and never the digits themselves — this string reaches uitest transcripts and screenshots.
        CHECK(!pad2->describe().contains(QStringLiteral("5")), "describe() never leaks an entered digit");
        ctx.routeKey(Qt::Key_Backspace);
        pump();
        CHECK(NavOverlay::topmost() == pad2, "Back with a digit entered deletes it instead of closing");
        CHECK(closes == 0, "…and does not fire the completion");
        ctx.routeKey(Qt::Key_Backspace);
        pump();
        CHECK(NavOverlay::topmost() == nullptr, "Back on an empty pad backs out");
        CHECK(closes == 1 && !backOk && backExtra < 0, "backing out reports neither a code nor a recovery row");

        // c) The digit grid is arrow-navigable, and the recovery row is reachable by pressing Down from EVERY
        //    column — not just the middle one. This assertion is written per-column because the first draft
        //    walked down from ONE column, passed, and shipped a pad whose bottom-left key was a dead end:
        //    NavRing::pickNext scores by centres and rejects a candidate that is more sideways than it is
        //    in-direction, so a full-width recovery button (centre under the MIDDLE column) loses from the
        //    outer two. A user who has forgotten their code arrives at exactly that corner.
        for (int startCol = 0; startCol < 3; ++startCol)
        {
            int chosen = -2;
            auto* pad3 = new PasscodePad(QStringLiteral("Enter the passcode"), QString(),
                                         { QStringLiteral("Use the parental PIN") },
                                         [&](const QString&, bool, int x) { chosen = x; }, &win);
            pump();
            QWidget* first = QApplication::focusWidget();
            CHECK(first != nullptr, "the pad always has a selection when it opens");
            for (int i = 0; i < startCol; ++i) ctx.routeKey(Qt::Key_Right);
            pump();
            CHECK(startCol == 0 || QApplication::focusWidget() != first,
                  "arrows move the selection across the digit grid");
            auto onRecoveryRow = [] {
                auto* b = qobject_cast<QPushButton*>(QApplication::focusWidget());
                return b && b->text().contains(QStringLiteral("parental"));
            };
            for (int i = 0; i < 12 && !onRecoveryRow(); ++i) ctx.routeKey(Qt::Key_Down);
            pump();
            CHECK(onRecoveryRow(), "the recovery row is reachable by pressing Down from this column");
            ctx.routeKey(Qt::Key_Return);
            pump();
            CHECK(chosen == 0, "choosing a recovery row reports its index");
            CHECK(NavOverlay::topmost() == nullptr, "…and closes the pad");
            (void)pad3;
        }
        // ...and Up from a recovery row goes back into the grid, so the hop is not one-way.
        {
            auto* pad4 = new PasscodePad(QStringLiteral("Enter the passcode"), QString(),
                                         { QStringLiteral("Use the parental PIN") },
                                         nullptr, &win);
            pump();
            for (int i = 0; i < 6; ++i) ctx.routeKey(Qt::Key_Down);
            pump();
            ctx.routeKey(Qt::Key_Up);
            pump();
            auto* b = qobject_cast<QPushButton*>(QApplication::focusWidget());
            CHECK(b && b->text().size() == 1, "Up from the recovery row lands back on a digit key");
            pad4->dismiss(-1);
            pump();
        }
    }

    // ------------------------------------------ 23. the themed surface's TWO categories shapes (issue #38)
    {
        // The themed home reaches its `categories` zone two ways, and both are built by the ONE shared builder
        // (buildThemedNavGraph, NavThemeGraph.h) so a probe can never assert a shape the app no longer ships:
        // the XMB CROSS (a horizontal axis co-located with the item column) and the SIDEBAR (a vertical list
        // beside a grid, the non-XMB route). This section pins the property that made the second one necessary
        // — entering and leaving the sidebar must not disturb the grid cursor — and the reachability both
        // shapes owe Invariant 2. The detailed key-by-key routing lives in probe_navqml §21, which drives the
        // real ThemeView.qml on top of these graphs; here we pin the pure model both of them stand on.
        auto reachable = [](NavGraph& g) {
            std::set<QString> seenZ;
            std::set<std::pair<QString, int>> seen;
            std::deque<std::pair<QString, int>> q;
            g.select(QStringLiteral("items"), 0);
            q.push_back({ g.zone(), g.index() });
            seen.insert({ g.zone(), g.index() });
            seenZ.insert(g.zone());
            static const Qt::Key arr[] = { Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right };
            while (!q.empty())
            {
                auto [z, i] = q.front(); q.pop_front();
                for (Qt::Key k : arr)
                {
                    g.select(z, i);
                    g.move(k);
                    auto st = std::make_pair(g.zone(), g.index());
                    if (!seen.count(st)) { seen.insert(st); seenZ.insert(st.first); q.push_back(st); }
                }
            }
            return seenZ;
        };

        NavGraph gs;
        buildThemedNavGraph(gs, 12, {}, CategoriesNav::Sidebar);
        gs.setZoneCount(QStringLiteral("categories"), 5);
        gs.setZoneCount(QStringLiteral("buttons"), 2);
        QString why;
        CHECK(gs.validate(&why), "themed(sidebar): the Sidebar-shaped graph passes validate()");
        const std::set<QString> rs = reachable(gs);
        CHECK(rs.count(QStringLiteral("items")) && rs.count(QStringLiteral("categories"))
              && rs.count(QStringLiteral("buttons")),
              "themed(sidebar): arrows alone reach items + categories + buttons from the default zone");

        // THE property: a sidebar visit is non-destructive to the grid. Park the grid on cell 7, go into the
        // sidebar, walk it, come back — the grid must be exactly where it was left (the model's per-zone
        // memory, reached because the Left/Right legs are CROSS-axis for their Vertical target and so carry
        // no fused step).
        gs.select(QStringLiteral("categories"), 0);
        gs.select(QStringLiteral("items"), 7);
        gs.move(Qt::Key_Left);
        CHECK(gs.zone() == QStringLiteral("categories"), "themed(sidebar): Left off the grid enters the sidebar");
        for (int i = 0; i < 7; ++i) gs.move(Qt::Key_Down);   // past the last row of 5: the strip WRAPS
        CHECK(gs.zone() == QStringLiteral("categories"),
              "themed(sidebar): the sidebar's Up/Down step the list and WRAP — they never fall through into "
              "the button bar a row below (which would be a one-way trapdoor)");
        gs.move(Qt::Key_Right);
        CHECK(gs.zone() == QStringLiteral("items") && gs.index() == 7,
              "themed(sidebar): Right comes back to the grid's remembered cell (7) — the round trip costs nothing");

        // The CROSS shape is untouched by all of this (the regression bar for Triple, whose home is an XMB).
        NavGraph gc;
        buildThemedNavGraph(gc, 12);                      // default = CategoriesNav::Cross
        gc.setZoneCount(QStringLiteral("categories"), 5);
        gc.setZoneCount(QStringLiteral("buttons"), 2);
        CHECK(gc.validate(&why), "themed(cross): the XMB-shaped graph still passes validate()");
        const std::set<QString> rc = reachable(gc);
        CHECK(rc.count(QStringLiteral("items")) && rc.count(QStringLiteral("categories"))
              && rc.count(QStringLiteral("buttons")),
              "themed(cross): arrows alone still reach items + categories + buttons");
        gc.select(QStringLiteral("categories"), 2);
        gc.select(QStringLiteral("items"), 4);            // leave categories (memory = 2)
        CHECK(gc.move(Qt::Key_Right) && gc.zone() == QStringLiteral("categories") && gc.index() == 3,
              "themed(cross): one Right from the column still switches + steps the axis (2 -> 3, fused)");
        gc.move(Qt::Key_Down);
        CHECK(gc.zone() == QStringLiteral("items"),
              "themed(cross): Down from the category axis still crosses back into the item column");
    }

    // ------------------------------------------- 24. the classic Edit-Profile picker: no D-pad orphans
    // The REAL ProfileDialog picker page, inside a replica of the panel that hosts it. Not a stand-in
    // layout: the defect this covers was pure geometry (a left-aligned button under a wide icon grid), and
    // a stand-in would drift from the page it is supposed to be guarding until it stopped describing it.
    //
    // This is also the page that shows why the kit resolves Up/Down by ROW and not by a single distance
    // score. Its shape — an 8-column grid of 42px icons, then a left-aligned "Passcode…", then a
    // right-aligned OK/Cancel — puts three consecutive rows under each other with no two of them sharing a
    // centre. Scoring by centres alone, every column of the grid rejected the row beneath it as "more
    // sideways than down", and the page came apart: Down did nothing at all from seven of eight columns.
    {
        // An overlay left open by a FAILING earlier section would swallow every key routed here (routeKey
        // hands the topmost overlay input first), turning this section's report into "nothing is reachable"
        // no matter what the ring does. Start from a clean stack so a failure here names its own cause.
        for (int i = 0; i < 8 && NavOverlay::topmost(); ++i) NavOverlay::topmost()->dismiss(-1);
        pump();

        ProfileStore::add(QStringLiteral("Kid"), QString::fromUtf8("🐱"));
        ProfileStore::add(QStringLiteral("Grown-up"), QString::fromUtf8("🦊"));
        const QString kidId = ProfileStore::list().first().id;

        // MainWindow::showDialogPanel's shape, replicated: a dark header (‹ Back + title) over a
        // widget-resizable QScrollArea whose content carries the embedded dialog. The ring's container is
        // the whole panel page, header included — so the Back button is a ring member here exactly as it is
        // in the app, and the geometry the picker is laid out into is the real one.
        auto* host = new QWidget(&win);
        auto* pv = new QVBoxLayout(host);
        pv->setContentsMargins(0, 0, 0, 0);
        pv->setSpacing(0);
        auto* header = new QWidget(host);
        auto* phl = new QHBoxLayout(header);
        phl->setContentsMargins(16, 10, 16, 10);
        auto* hdr = new QPushButton(QStringLiteral("‹ Back"), header);
        hdr->setStyleSheet(QStringLiteral("QPushButton{padding:10px 18px;font-size:16px;font-weight:bold;}"));
        phl->addWidget(hdr);
        phl->addSpacing(12);
        phl->addWidget(new QLabel(QStringLiteral("Profiles"), header), 1);
        pv->addWidget(header);
        auto* scroll = new QScrollArea(host);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* content = new QWidget;
        auto* cv = new QVBoxLayout(content);
        cv->setContentsMargins(28, 24, 28, 24);
        cv->setSpacing(14);
        auto* dlg = new ProfileDialog(false, [](const QString&) { return true; }, content);
        dlg->setWindowFlags(Qt::Widget);
        cv->addWidget(dlg);
        cv->addStretch(1);
        scroll->setWidget(content);
        pv->addWidget(scroll, 1);
        host->setGeometry(0, 0, 1280, 760);
        host->show(); host->activateWindow();
        pump();

        // Reach the picker: click the ✎ on the first row.
        for (QPushButton* b : dlg->findChildren<QPushButton*>())
            if (b->text() == QString::fromUtf8("✎")) { b->click(); break; }
        pump(); pump();

        NavRing ring(host);
        ctx.setActiveRing(&ring);
        pump();

        auto named = [&](const QString& t) -> QWidget* {
            for (QWidget* w : ring.widgets())
                if (auto* b = qobject_cast<QAbstractButton*>(w); b && b->text() == t) return w;
            return nullptr;
        };
        auto press = [&](QWidget* from, int key) -> QString {
            if (!from) return QStringLiteral("<missing>");
            from->setFocus(Qt::OtherFocusReason);
            pump();
            ctx.routeKey(key);
            pump();
            QWidget* now = QApplication::focusWidget();
            if (now == from) return QStringLiteral("<unchanged>");
            auto* b = qobject_cast<QAbstractButton*>(now);
            return b ? b->text() : QStringLiteral("<non-button>");
        };

        QWidget* pcBtn = named(QStringLiteral("Passcode…"));
        QWidget* okBtn = named(QStringLiteral("OK"));
        CHECK(pcBtn && okBtn && named(QStringLiteral("Cancel")),
              "the picker page really is up (Passcode… / OK / Cancel are ring members)");

        // a) NOTHING on this page is a D-pad orphan. The closure, not a walk: the defect that prompted this
        //    section hid from a walk that only ever pressed Down and Up. (It was NOT strictly unreachable —
        //    a Left from the bottom-LEFT icon did land on it — which is exactly why the assertion below is
        //    about ORDER, not just reachability. Reachable-by-a-press-nobody-would-try is not reachable.)
        const QVector<QWidget*> orphans = navUnreachable(ring, ctx);
        for (QWidget* w : orphans)
        {
            auto* b = qobject_cast<QAbstractButton*>(w);
            std::fprintf(stderr, "NAV-FAIL picker: no arrow key reaches %s '%s'\n",
                         w->metaObject()->className(),
                         (b ? b->text() : w->objectName()).toUtf8().constData());
        }
        failures += orphans.size();

        // b) Down off the BOTTOM ROW OF THE ICON GRID reaches the row beneath it — from every one of the
        //    eight columns, not just the one whose centre happens to line up. This is the ProfileDialog twin
        //    of §22(c): "Passcode…" is left-aligned in an HBox (its centre sits under column 0), so scoring
        //    by centres alone rejected it from all eight columns — seven of which had nothing else below and
        //    simply dead-ended, while the eighth leapt past it to OK. Rows, then columns.
        QVector<QWidget*> icons;
        for (QWidget* w : ring.widgets())
            if (auto* b = qobject_cast<QPushButton*>(w); b && b->size() == QSize(42, 42)) icons.push_back(w);
        CHECK(icons.size() == 24, "the icon grid is the expected 24 buttons");
        const int cols = 8;
        for (int c = 0; c < cols && icons.size() == 24; ++c)
            CHECK(press(icons[icons.size() - cols + c], Qt::Key_Down) == QStringLiteral("Passcode…"),
                  "Down from the icon grid's bottom row reaches Passcode… from THIS column");

        // c) …and the rows below it keep going, in order, both ways. Without this the Passcode… row is a
        //    pocket: you can get in, but OK/Cancel are only reachable by going back UP into the grid first.
        CHECK(press(pcBtn, Qt::Key_Down) == QStringLiteral("OK"), "Down from Passcode… reaches the OK/Cancel row");
        CHECK(press(okBtn, Qt::Key_Right) == QStringLiteral("Cancel"), "Right from OK reaches Cancel");
        CHECK(press(okBtn, Qt::Key_Up) == QStringLiteral("Passcode…"),
              "Up from OK comes back to Passcode… — the hop is not one-way, and it skips no row");
        CHECK(press(pcBtn, Qt::Key_Up) == QString::fromUtf8("🐝"),
              "Up from Passcode… lands on the icon row directly above, not the one above that");

        ctx.setActiveRing(nullptr);
        (void)kidId;
        delete host;
        pump();
    }

    // -------------------------------- 25. the on-screen keyboard's Done key COMMITS (issue: search box)
    // Two ways to finish typing, and they must do the same thing. Start/Escape commits (§8 covers that);
    // this covers the OTHER one — the Done KEY, which a mouse user clicks and a pad user activates through
    // the ring. It is the only finish a touch/mouse user has, so a Done that closes the keyboard without
    // committing is the whole feature failing silently.
    {
        // a) Done on a free-standing OSK hands the typed text back, accepted.
        QString committed = QStringLiteral("sentinel");
        bool ok = false, called = false;
        auto* osk = new Osk(QStringLiteral("Search"), QStringLiteral("master"), QLineEdit::Normal,
                            [&](const QString& t, bool accepted) { called = true; committed = t; ok = accepted; },
                            &win);
        pump();
        auto oskKey = [](Osk* o, const QString& caption) -> QPushButton* {
            for (QPushButton* b : o->findChildren<QPushButton*>())
                if (b->text() == caption) return b;
            return nullptr;
        };
        QPushButton* done = oskKey(osk, QStringLiteral("Done"));
        CHECK(done != nullptr, "the OSK has a Done key");
        if (done) done->click();
        pump();
        CHECK(called && ok && committed == QStringLiteral("master"), "the Done key commits the typed text");
        CHECK(NavOverlay::topmost() == nullptr, "the OSK closes on Done");

        // b) …and over a SEARCH BOX, Done must run the box's search, not merely fill it in.
        //
        // The defect: the commit was delivered as a synthetic Key_Return, and NavTextField's two-state
        // filter owns this widget's keys — in the SELECTED state it reads Return as "select into the field"
        // and SWALLOWED it. The query landed in the box with nothing run behind it, and the field was left
        // in the editing state, so the user's own next Enter passed through and searched. Exactly the
        // reported "pressing Done doesn't search, it only searches if I press Enter".
        auto* host = new QWidget(&win);
        host->setGeometry(0, 0, 400, 200);
        auto* hv = new QVBoxLayout(host);
        auto* box = new QLineEdit(host);
        box->setPlaceholderText(QStringLiteral("Search…"));
        hv->addWidget(box);
        hv->addWidget(new QPushButton(QStringLiteral("Go"), host));
        NavTextField::ensure(box);          // the guard every text row on the kit carries
        NavRing boxRing(host);
        ctx.setActiveRing(&boxRing);
        host->show();
        pump();
        int searches = 0, finishes = 0;
        QObject::connect(box, &QLineEdit::returnPressed, [&] { ++searches; });
        QObject::connect(box, &QLineEdit::editingFinished, [&] { ++finishes; });

        box->setFocus(Qt::OtherFocusReason);
        pump();
        ctx.routeKey(Qt::Key_Return);       // a controller Enter on the box opens the OSK over it
        pump();
        auto* boxOsk = qobject_cast<Osk*>(NavOverlay::topmost());
        CHECK(boxOsk != nullptr, "Enter on a search box opens the on-screen keyboard");
        if (boxOsk)
        {
            QKeyEvent k(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
            QApplication::sendEvent(boxOsk, &k);            // type into it
            if (QPushButton* d2 = oskKey(boxOsk, QStringLiteral("Done"))) d2->click();
            pump();
        }
        CHECK(box->text() == QStringLiteral("a"), "Done writes the typed query into the box");
        CHECK(searches == 1, "…and RUNS it: Done fires the box's returnPressed exactly once");
        CHECK(finishes == 1, "…and its editingFinished, which a settings row commits on");
        // A box left in the editing state is the fingerprint of the swallowed Return: the commit was
        // consumed as "select into the field" instead.
        CHECK(!NavTextField::isInteracting(box), "committing leaves the box SELECTED, not mid-edit");

        ctx.setActiveRing(nullptr);
        delete host;
        pump();
    }

    // -------------------------------- 26. Enter on a ring member must not re-enter the router (issue #305)
    // The crash: pressing Enter on a row of the CLASSIC Appearance theme list killed the app with c00000fd
    // — a stack overflow, 387 identical levels of
    //     MainWindow::keyPressEvent -> NavContext::routeKey -> NavRing::handleKey -> (activate) sendEvent
    //     -> QCoreApplication::notifyInternal2 -> QApplication::notify -> notify_helper -> QWidget::event
    //     -> MainWindow::keyPressEvent -> …
    // NavRing::activate's fallback synthesised the Return into the focused widget; QAbstractItemView answers
    // Return by emitting activated() and then IGNORING the event (Qt's own source warns that re-delivering
    // it "start[s] an endless loop"); QApplication::notify propagates an unaccepted key up the parent chain
    // to the window; and the window's keyPressEvent routes it back into the same ring. Nothing bounded it.
    //
    // What this section actually covers: the RE-ENTRY, not the theme list. The list is the reported gesture,
    // but the loop belongs to the kit, so the invariant asserted is the general one — one press enters the
    // window's routing at most once — over both widget classes that leave Return unaccepted (an item view,
    // and a QSlider, which reaches the generic fallback) and over both key paths (a physical key, which Qt
    // delivers to the focused widget first, and a controller/injected key, which is routed straight in).
    {
        auto* win2 = new RoutingWindow();
        win2->resize(500, 400);
        auto* host = new QWidget(win2);
        host->setGeometry(0, 0, 500, 400);
        auto* v = new QVBoxLayout(host);
        auto* list = new QListWidget(host);
        for (const char* t : { "Triple", "Night", "Channels" }) new QListWidgetItem(QString::fromLatin1(t), list);
        list->setCurrentRow(0);
        v->addWidget(list);
        auto* slider = new QSlider(Qt::Horizontal, host);   // the OTHER widget class that ignores Return
        v->addWidget(slider);
        v->addWidget(new QPushButton(QStringLiteral("Browse community themes…"), host));
        NavRing ring(host);
        ctx.setActiveRing(&ring);
        win2->show();
        win2->activateWindow();
        pump();

        int activated = 0;
        QObject::connect(list, &QListWidget::itemActivated, [&](QListWidgetItem*) { ++activated; });

        // a) THE PHYSICAL KEY PATH — exactly what Qt does with a real Enter: deliver it to the focus widget
        //    and let it propagate if unaccepted. Pre-fix this never came back.
        list->setCurrentRow(0);
        list->setFocus(Qt::OtherFocusReason);
        pump();
        win2->reset();
        activated = 0;
        {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(list, &press);
        }
        pump();
        CHECK(win2->maxDepth <= 1, "Enter on a list row enters the window's key routing at most once (#305)");
        CHECK(activated == 1, "…and activates the row exactly once — one press, one activation");

        // b) …and it is not a one-shot: five presses in a row each behave the same. A guard that forgot to
        //    reset would pass (a) and then silently swallow every Enter after it, which is the failure mode
        //    a re-entrancy flag is most likely to ship with.
        for (int i = 0; i < 5; ++i)
        {
            win2->reset();
            activated = 0;
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(list, &press);
            pump();
            CHECK(win2->maxDepth <= 1, "repeated Enter on the same row keeps not re-entering");
            CHECK(activated == 1, "…and each press still activates the row exactly once");
        }

        // c) THE CONTROLLER / INJECTED PATH (sendNavKey step 6): the key never touches the view, so the ring
        //    is the only thing that can activate the row — and it must, or picking a theme with a pad commits
        //    nothing. SyntheticScope is what tells the two paths apart, exactly as sendNavKey sets it.
        win2->reset();
        activated = 0;
        {
            NavContext::SyntheticScope synth;
            ctx.routeKey(Qt::Key_Return);
        }
        pump();
        CHECK(activated == 1, "a controller Enter activates the current row (the pad's only route in)");
        CHECK(win2->maxDepth <= 1, "…without bouncing back through the window's routing");

        // d) THE GENERIC FALLBACK, which is where a QSlider lands: no branch of activate() claims it, so it
        //    gets the synthesised Return — and QAbstractSlider ignores anything that is not an arrow/page key,
        //    so this is the same loop with a different widget. It is bounded now, whatever the widget does.
        slider->setFocus(Qt::OtherFocusReason);
        pump();
        win2->reset();
        {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(slider, &press);
        }
        pump();
        CHECK(win2->maxDepth <= 2, "Enter on a slider is bounded too — the fallback cannot feed itself (#305)");

        ctx.setActiveRing(nullptr);
        delete win2;
        pump();
    }

    if (failures) { std::fprintf(stderr, "NAV-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("NAV-OK\n");
    return 0;
}

#include "probe_nav.moc"
