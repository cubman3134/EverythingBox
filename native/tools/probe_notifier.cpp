// Headless test for the Notifier overlay (the app's single user-feedback channel): show/hide/sticky/
// reposition invariants under the offscreen QPA. Prints NOTIFIER-OK when every assert holds.
#include <QApplication>
#include <QEventLoop>
#include <QLabel>
#include <QTimer>
#include <QWidget>
#include "../src/ui/Notifier.h"

// Wait by spinning a REAL event loop: the Notifier's auto-hide is a QTimer, so only a running loop advances it
// (and this probe links Qt6::Widgets, not Qt6::Test, so there is no QTest::qWait).
static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    QWidget host; host.resize(1280, 720); host.show();
    Notifier n(&host);

    n.notify(QStringLiteral("hello"), 4500);
    QLabel* notice = host.findChild<QLabel*>(QStringLiteral("mmvNotice"));
    CHECK(notice && notice->isVisible(), "notify shows the notice");
    CHECK(notice->geometry().center().x() > 400 && notice->geometry().center().x() < 880,
          "notice is horizontally centred");
    n.hideNotice();
    CHECK(!notice->isVisible(), "hideNotice hides it");

    n.notify(QStringLiteral("sticky"), 0);       // ms <= 0 = sticky (no auto-hide timer)
    CHECK(notice->isVisible(), "sticky notice shows");

    // notifyOverSticky: a timed message posted over a STILL-RUNNING sticky phase note must hand the label back
    // when it expires. One label serves the whole app, so a plain 3s toast fired mid-phase overwrites the phase
    // note and then hides it — a visible minutes-long wait becomes a blank one.
    n.notifyOverSticky(QStringLiteral("interrupt"), 50);
    CHECK(notice->isVisible() && notice->text() == QLatin1String("interrupt"),
          "notifyOverSticky shows its own text while it runs");
    spin(200);
    CHECK(notice->isVisible(), "the covered sticky is visible again once the transient expires");
    CHECK(notice->text() == QLatin1String("sticky"), "the covered sticky text is what came back");
    spin(200);                                   // restored STICKY, so a second wait must not hide it
    CHECK(notice->isVisible() && notice->text() == QLatin1String("sticky"),
          "the restored note is sticky and does not auto-hide");

    // Nothing sticky underneath (label hidden): ends hidden, exactly like notify().
    n.hideNotice();
    n.notifyOverSticky(QStringLiteral("alone"), 50);
    CHECK(notice->isVisible(), "notifyOverSticky with nothing under it still shows");
    spin(200);
    CHECK(!notice->isVisible(), "notifyOverSticky with nothing under it ends hidden");

    // A transient over a transient snapshots nothing — the covered message was already on its way out.
    n.notify(QStringLiteral("timed"), 60);
    n.notifyOverSticky(QStringLiteral("second"), 50);
    spin(200);
    CHECK(!notice->isVisible(), "a transient over a transient ends hidden, not restored");

    // An explicit hide is final: it drops the pending snapshot so a later expiry cannot undo it.
    n.notify(QStringLiteral("sticky2"), 0);
    n.notifyOverSticky(QStringLiteral("interrupt2"), 50);
    n.hideNotice();
    spin(200);
    CHECK(!notice->isVisible(), "hideNotice during the transient drops the snapshot");

    QWidget player(&host); player.setGeometry(0, 0, 1280, 720); player.show();
    n.setPlayerHost(&player, []{ return 60; });
    n.playerNotice(QStringLiteral("up next"), 6000);
    QWidget* pn = player.findChild<QWidget*>(QStringLiteral("mmvPlayerNotice"));
    CHECK(pn && pn->isVisible(), "playerNotice shows over the player");
    CHECK(n.playerNoticeVisible(), "playerNoticeVisible reports true");
    n.hidePlayerNotice();
    CHECK(!pn->isVisible(), "hidePlayerNotice hides it");

    host.resize(900, 500);
    n.reposition();                               // must not crash with both overlays live
    CHECK(true, "reposition survives a resize");

    if (fails == 0) printf("NOTIFIER-OK\n");
    return fails == 0 ? 0 : 1;
}
