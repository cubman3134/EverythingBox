// Headless test for the Notifier overlay (the app's single user-feedback channel): show/hide/sticky/
// reposition invariants under the offscreen QPA. Prints NOTIFIER-OK when every assert holds.
#include <QApplication>
#include <QWidget>
#include "../src/ui/Notifier.h"

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
    QWidget* notice = host.findChild<QWidget*>(QStringLiteral("mmvNotice"));
    CHECK(notice && notice->isVisible(), "notify shows the notice");
    CHECK(notice->geometry().center().x() > 400 && notice->geometry().center().x() < 880,
          "notice is horizontally centred");
    n.hideNotice();
    CHECK(!notice->isVisible(), "hideNotice hides it");

    n.notify(QStringLiteral("sticky"), 0);       // ms <= 0 = sticky (no auto-hide timer)
    CHECK(notice->isVisible(), "sticky notice shows");

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
