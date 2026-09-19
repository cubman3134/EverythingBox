// Headless check of the classic queue list's confirm key (issue #415, src/ui/QueueListKeys.h).
//
// The bug: Enter/OK on a row of the classic player's queue list played that row PAUSED. QListWidget emits
// activated() for Return/Enter and then ignores the key, so Qt carried the same press up to
// MainWindow::keyPressEvent, whose player-page branch read "no button focused" as "toggle play/pause". One
// press did both: it reloaded the row, then paused it.
//
// This probe drives the REAL Qt propagation (QApplication::notify walking an ignored KeyPress up the parent
// chain) with a parent that stands in for MainWindow and counts every key that reaches it:
//   1. the premise: WITHOUT the filter a Return on a row both activates it AND reaches the parent (the
//      double action). If a Qt upgrade ever stops that propagation, this says so rather than passing on a
//      premise that no longer holds;
//   2. WITH eb::claimQueueConfirm in the list's filter: Return, Enter, keypad Enter and Select each
//      activate the current row exactly once, and the parent sees nothing;
//   3. what it must leave alone: a modified Enter, no current row, a disabled row, and every other key;
//   4. the wiring: MainWindow::eventFilter hands the queue list (playlist_) to the helper. Without that line
//      the helper is dead code and the bug is back, which no behavioural check above can see.
//
// Prints QUEUEKEYS-OK on success; any failure prints QUEUEKEYS-FAIL <cond> and exits non-zero.
#include "QueueListKeys.h"

#include <QApplication>
#include <QFile>
#include <QKeyEvent>
#include <QListWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "QUEUEKEYS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

namespace {

// Stands in for MainWindow: records every key press that propagates up to it.
struct Parent : QWidget
{
    int keys = 0;
    int lastKey = 0;
    void keyPressEvent(QKeyEvent* e) override { ++keys; lastKey = e->key(); e->accept(); }
};

// The list's event filter, reduced to the one line MainWindow::eventFilter gives the queue list.
struct Filter : QObject
{
    QListWidget* list = nullptr;
    int claimed = 0;
    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        if (obj == list && eb::claimQueueConfirm(list, ev)) { ++claimed; return true; }
        return false;
    }
};

struct Rig
{
    Parent       parent;
    QListWidget* list = nullptr;
    int          activations = 0;
    int          activatedRow = -1;
    Rig()
    {
        auto* lay = new QVBoxLayout(&parent);
        list = new QListWidget(&parent);
        lay->addWidget(list);
        for (const char* t : { "Runway Lights", "Cruising Altitude", "Tray Tables Up" })
            list->addItem(QString::fromLatin1(t));
        QObject::connect(list, &QListWidget::itemActivated, list, [this](QListWidgetItem* it) {
            ++activations;
            activatedRow = list->row(it);
        });
        parent.resize(320, 240);
        parent.show();
        list->setFocus();
    }
    void reset() { activations = 0; activatedRow = -1; parent.keys = 0; parent.lastKey = 0; }
    // The same delivery sendNavKey uses for the pad / remote / uitest route, and the one a physical key
    // takes through QApplication::notify: straight at the focused widget.
    void press(int key, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        QKeyEvent ev(QEvent::KeyPress, key, mods);
        QCoreApplication::sendEvent(list, &ev);
    }
};

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // 1. The premise, with NO filter installed: the row is chosen AND the parent receives the same press.
    {
        Rig r;
        r.list->setCurrentRow(1);
        r.reset();
        r.press(Qt::Key_Return);
        CHECK(r.activations == 1);
        CHECK(r.activatedRow == 1);
        CHECK(r.parent.keys == 1);                 // the press MainWindow read as "toggle pause"
        CHECK(r.parent.lastKey == Qt::Key_Return);
    }

    // 2. With the claim: every confirm spelling activates the current row once and stops there.
    {
        Rig r;
        Filter f;
        f.list = r.list;
        r.list->installEventFilter(&f);
        r.list->setCurrentRow(1);

        struct Case { int key; Qt::KeyboardModifiers mods; };
        const Case cases[] = { { Qt::Key_Return, Qt::NoModifier }, { Qt::Key_Enter, Qt::NoModifier },
                               { Qt::Key_Enter, Qt::KeypadModifier }, { Qt::Key_Select, Qt::NoModifier } };
        for (const Case& c : cases)
        {
            r.reset();
            f.claimed = 0;
            r.press(c.key, c.mods);
            CHECK(f.claimed == 1);
            CHECK(r.activations == 1);             // chosen exactly once: not zero, and not twice
            CHECK(r.activatedRow == 1);            // and it is the CURRENT row that was chosen
            CHECK(r.parent.keys == 0);             // nothing reaches the player page's play/pause fallback
        }

        // The playing row too (row 0 here): choosing it again is a reload, never a reload-then-pause.
        r.list->setCurrentRow(0);
        r.reset();
        r.press(Qt::Key_Return);
        CHECK(r.activations == 1);
        CHECK(r.activatedRow == 0);
        CHECK(r.parent.keys == 0);

        // 3a. A modified Enter is not "choose this row". It is left exactly as before.
        r.reset();
        f.claimed = 0;
        r.press(Qt::Key_Return, Qt::AltModifier);
        CHECK(f.claimed == 0);

        // 3b. Every other key is untouched. The list's own arrows still move the row, and M (the queue menu
        //     filter beside this one) is not ours.
        for (int k : { Qt::Key_Down, Qt::Key_Up, Qt::Key_M, Qt::Key_Space, Qt::Key_Escape, Qt::Key_Backspace })
        {
            QKeyEvent ev(QEvent::KeyPress, k, Qt::NoModifier);
            CHECK(!eb::claimQueueConfirm(r.list, &ev));
        }
        r.list->setCurrentRow(0);
        r.press(Qt::Key_Down);
        CHECK(r.list->currentRow() == 1);

        // 3c. A release is never claimed: the press already did the choosing.
        {
            QKeyEvent rel(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
            CHECK(!eb::claimQueueConfirm(r.list, &rel));
        }

        // 3d. A disabled row (a group header in an IPTV list) is not activatable, so the key is not claimed.
        r.list->setCurrentRow(2);
        r.list->item(2)->setFlags(r.list->item(2)->flags() & ~Qt::ItemIsEnabled);
        {
            QKeyEvent ev(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            CHECK(!eb::claimQueueConfirm(r.list, &ev));
        }

        // 3e. No current row: nothing to choose, so nothing is claimed and nothing is activated.
        r.list->setCurrentItem(nullptr);
        r.reset();
        {
            QKeyEvent ev(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            CHECK(!eb::claimQueueConfirm(r.list, &ev));
        }
        CHECK(r.activations == 0);
        CHECK(!eb::claimQueueConfirm(nullptr, nullptr));
    }

    // 4. The wiring. MainWindow::eventFilter has to hand the queue list to the helper.
    {
        QFile mw(QStringLiteral(EB_QUEUEKEYS_SRC_DIR) + QStringLiteral("/ui/MainWindow.cpp"));
        CHECK(mw.open(QIODevice::ReadOnly));
        // A Windows checkout (core.autocrlf) has CRLF line ends and a Linux one has LF; compare on LF.
        const QString src = QString::fromUtf8(mw.readAll()).remove(QLatin1Char('\r'));
        const qsizetype start = src.indexOf(QStringLiteral("bool MainWindow::eventFilter(QObject* obj, QEvent* event)"));
        CHECK(start >= 0);
        const qsizetype end = src.indexOf(QStringLiteral("\n}\n"), start);
        CHECK(end > start);
        const QString body = src.mid(start, end - start);
        CHECK(body.contains(QStringLiteral("eb::claimQueueConfirm(playlist_, event)")));
    }

    if (failures) { std::fprintf(stderr, "QUEUEKEYS-FAIL %d check(s)\n", failures); return 1; }
    std::printf("QUEUEKEYS-OK\n");
    return 0;
}
